/* vm2bio: «сырой» блочный писатель и проверяющий для проверки бэкапа ВКЛЮЧЁННОЙ ВМ на настоящем ESXi (docs/ESXI.md, раздел «Запись гостя во время копии»).
 *
 *   vm2bio write  <устройство> [--rate N]   гость: заполняет диск (seq 0), затем пишет по одному блоку 4 КиБ (O_DIRECT|O_DSYNC, одна запись в полёте),
 *                                          seq = 1, 2, 3...; продолжает с места остановки, если на диске уже есть данные этого писателя
 *   vm2bio verify <устройство|файл> [lo hi] проверка «префикса»: диск обязан быть ровно состоянием после записей 1..M (M = наибольший найденный seq);
 *                                          lo/hi (необязательно): окно допустимых M из хронологии хоста
 *
 * Блок: [0:8] "VM2BBLK1", [8:16] seq, [16:20] номер блока, [20:24] CRC32 (байты 0..19 и 32..4095), [24:32] время записи (нс, CLOCK_REALTIME; в сверку не входит),
 * [32:4096] данные = splitmix64-поток от ключа (seed, блок, seq).  Блок 0 — суперблок «VM2BSUPR» + seed + число блоков.
 * Какой блок пишет запись k — функция только от (seed, k): по журналу можно восстановить эталон диска для любого M.  Все тексты журнала машинные (английские), без пароля и секретов.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <stdarg.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <linux/fs.h>

typedef uint64_t u64; typedef uint32_t u32; typedef uint8_t u8;
#define BS 4096
#define G 0x9E3779B97F4A7C15ULL
static u64 fin(u64 z) { z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL; z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL; return z ^ (z >> 31); }
static u64 mix(u64 x) { return fin(x + G); }
static u64 keyof(u64 seed, u32 idx, u64 seq) { return mix(seed + mix(mix(idx) ^ mix(seq ^ 0x5851F42D4C957F2DULL))); }
static u64 N;     /* число блоков, включая суперблок */
static u64 SEED;
static u32 blockof(u64 k) {
    u64 s = k >> 6;
    if ((mix(SEED ^ s) & 3) == 0) return (u32)(1 + (s * 64 + (k & 63)) % (N - 1));      /* последовательный участок из 64 записей */
    return (u32)(1 + mix(SEED ^ mix(k)) % (N - 1));                                     /* случайный блок */
}
static u32 crct[256];
static void crcinit(void) { for (u32 i = 0; i < 256; ++i) { u32 c = i; for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; crct[i] = c; } }
static u32 crcadd(u32 c, const u8* p, size_t n) { for (size_t i = 0; i < n; ++i) c = crct[(c ^ p[i]) & 255] ^ (c >> 8); return c; }
static u32 blkcrc(const u8* b) { u32 c = crcadd(0xFFFFFFFFu, b, 20); c = crcadd(c, b + 32, BS - 32); return ~c; }
static void fillblock(u8* b, u32 idx, u64 seq, u64 tns) {
    memcpy(b, "VM2BBLK1", 8); memcpy(b + 8, &seq, 8); memcpy(b + 16, &idx, 4); memcpy(b + 24, &tns, 8);
    u64 key = keyof(SEED, idx, seq);
    for (int j = 0; j < (BS - 32) / 8; ++j) { u64 w = fin(key + (u64)(j + 1) * G); memcpy(b + 32 + j * 8, &w, 8); }
    u32 c = blkcrc(b); memcpy(b + 20, &c, 4);
}
/* 0 = пустой (все нули), 1 = верный блок, 2 = испорчен (заголовок / CRC / данные не совпали) */
static int checkblock(const u8* b, u32 idx, u64* seq, u64* tns) {
    static const u8 z[BS] = { 0 };
    if (memcmp(b, z, BS) == 0) return 0;
    u64 s, t; u32 i, c; memcpy(&s, b + 8, 8); memcpy(&i, b + 16, 4); memcpy(&c, b + 20, 4); memcpy(&t, b + 24, 8);
    if (memcmp(b, "VM2BBLK1", 8) != 0 || i != idx || c != blkcrc(b)) return 2;
    u8 e[BS]; fillblock(e, idx, s, t);
    if (memcmp(e, b, BS) != 0) return 2;
    *seq = s; *tns = t; return 1;
}
static int logfd = -1;
static u64 nowns(clockid_t c) { struct timespec t; clock_gettime(c, &t); return (u64)t.tv_sec * 1000000000ULL + (u64)t.tv_nsec; }
static void logf_(const char* f, ...) {
    char buf[512]; va_list a; va_start(a, f); int n = vsnprintf(buf, sizeof buf - 2, f, a); va_end(a);
    buf[n++] = '\n'; if (logfd >= 0) { ssize_t r = write(logfd, buf, (size_t)n); (void)r; }
    ssize_t r2 = write(2, buf, (size_t)n); (void)r2;
}
static int rdfull(int fd, u8* p, size_t n, off_t off) { size_t g = 0; while (g < n) { ssize_t r = pread(fd, p + g, n - g, off + (off_t)g); if (r <= 0) return -1; g += (size_t)r; } return 0; }
static int wrfull(int fd, const u8* p, size_t n, off_t off) { size_t g = 0; while (g < n) { ssize_t r = pwrite(fd, p + g, n - g, off + (off_t)g); if (r <= 0) return -1; g += (size_t)r; } return 0; }
static u64 devsize(int fd) { struct stat st; if (fstat(fd, &st) != 0) return 0; if (S_ISBLK(st.st_mode)) { u64 s = 0; ioctl(fd, BLKGETSIZE64, &s); return s; } return (u64)st.st_size; }
static int readsuper(int fd) {
    u8* b = aligned_alloc(BS, BS); if (rdfull(fd, b, BS, 0) != 0) { free(b); return 0; }
    int ok = memcmp(b, "VM2BSUPR", 8) == 0; if (ok) { memcpy(&SEED, b + 8, 8); memcpy(&N, b + 16, 8); }
    free(b); return ok;
}
/* разбор диска; возвращает M и заполняет сводку.  verbose: печать нарушений */
struct Rep { u64 M, blocks, ok, zero, torn, stale, inflight, viol, tmin, tmax, tM; };
static int scan(int fd, struct Rep* r, int verbose) {
    memset(r, 0, sizeof *r);
    u32* seq = calloc(N, 4); u8* st = calloc(N, 1); u64* tn = calloc(N, 8);
    u8* buf = aligned_alloc(BS, 1 << 20);
    for (u64 b = 1; b < N;) {
        u64 cnt = N - b; if (cnt > 256) cnt = 256;
        if (rdfull(fd, buf, cnt * BS, (off_t)(b * BS)) != 0) { logf_("SCAN read error at block %llu", (unsigned long long)b); return -1; }
        for (u64 i = 0; i < cnt; ++i) { u64 s = 0, t = 0; int k = checkblock(buf + i * BS, (u32)(b + i), &s, &t); st[b + i] = (u8)k; if (k == 1) { seq[b + i] = (u32)s; tn[b + i] = t; if (s > r->M) r->M = s; } }
        b += cnt;
    }
    r->blocks = N - 1;
    u32* last = calloc(N, 4);
    for (u64 k = 1; k <= r->M; ++k) last[blockof(k)] = (u32)k;
    u32 infl = blockof(r->M + 1), infl2 = blockof(r->M);
    r->tmin = ~0ULL;
    for (u64 b = 1; b < N; ++b) {
        int bad = 0;
        if (st[b] == 1) { if (seq[b] == last[b]) { ++r->ok; if (tn[b] < r->tmin) r->tmin = tn[b]; if (tn[b] > r->tmax) r->tmax = tn[b]; if (seq[b] == r->M) r->tM = tn[b]; } else { ++r->stale; bad = 1; } }
        else if (st[b] == 0) { ++r->zero; bad = 1; } else { ++r->torn; bad = 1; }
        if (bad) {
            /* допустимое «окно незавершённых IO»: единственная запись M+1 (диск мог обновить её частично или полностью), но только в её же блоке */
            if (b == infl && (st[b] != 0)) ++r->inflight; else { ++r->viol; if (verbose && r->viol <= 20) logf_("VIOLATION block=%llu state=%d seq=%u expected=%u", (unsigned long long)b, st[b], seq[b], last[b]); }
        }
    }
    (void)infl2;
    free(seq); free(st); free(tn); free(last); free(buf); return 0;
}
static int do_verify(const char* dev, int hasw, u64 lo, u64 hi) {
    int fd = open(dev, O_RDONLY); if (fd < 0) { logf_("VERIFY BAD cannot open %s: %s", dev, strerror(errno)); return 2; }
    u64 sz = devsize(fd); N = sz / BS;
    if (!readsuper(fd)) { logf_("VERIFY BAD no superblock"); return 2; }
    if (N * BS > sz) { logf_("VERIFY BAD size"); return 2; }
    struct Rep r; if (scan(fd, &r, 1) != 0) return 2;
    int bad = r.viol != 0;
    logf_("VERIFYRESULT M=%llu blocks=%llu ok=%llu zero=%llu torn=%llu stale=%llu inflight=%llu violations=%llu tM=%llu tmax=%llu seed=%llx",
          (unsigned long long)r.M, (unsigned long long)r.blocks, (unsigned long long)r.ok, (unsigned long long)r.zero, (unsigned long long)r.torn, (unsigned long long)r.stale,
          (unsigned long long)r.inflight, (unsigned long long)r.viol, (unsigned long long)r.tM, (unsigned long long)r.tmax, (unsigned long long)SEED);
    if (hasw) {
        if (r.M < lo) { logf_("VERIFY BAD M=%llu < S_lo=%llu: writes finished before the snapshot are missing", (unsigned long long)r.M, (unsigned long long)lo); bad = 1; }
        else if (r.M > hi) { logf_("VERIFY BAD M=%llu > S_hi=%llu: writes made AFTER the snapshot are present", (unsigned long long)r.M, (unsigned long long)hi); bad = 1; }
        else logf_("VERIFY OK seq<=S: M=%llu in [%llu,%llu]", (unsigned long long)r.M, (unsigned long long)lo, (unsigned long long)hi);
    }
    if (!bad && !hasw) logf_("VERIFY OK prefix-consistent M=%llu", (unsigned long long)r.M);
    return bad ? 1 : 0;
}
static int do_write(const char* dev, int rate) {
    int fd = open(dev, O_RDWR | O_DIRECT | O_DSYNC); if (fd < 0) { logf_("WRITER cannot open %s: %s", dev, strerror(errno)); return 2; }
    u64 sz = devsize(fd); N = sz / BS; u8* b = aligned_alloc(BS, BS); u64 k = 1;
    if (readsuper(fd) && N * BS <= sz) {
        struct Rep r; if (scan(fd, &r, 0) != 0) return 2;
        logf_("WRITER resume: M=%llu violations=%llu", (unsigned long long)r.M, (unsigned long long)r.viol); k = r.M + 1;
    } else {
        SEED = mix(nowns(CLOCK_REALTIME) ^ 0xC0FFEE); memset(b, 0, BS); memcpy(b, "VM2BSUPR", 8); memcpy(b + 8, &SEED, 8); memcpy(b + 16, &N, 8);
        wrfull(fd, b, BS, 0);
        logf_("FILL start blocks=%llu seed=%llx", (unsigned long long)(N - 1), (unsigned long long)SEED);
        {   /* заполнение крупными записями (по 1 МиБ), O_DSYNC побочно только для случайных записей ниже; в конце fdatasync */
            u8* big = aligned_alloc(BS, 1 << 20);
            for (u64 i = 1; i < N;) {
                u64 cnt = N - i; if (cnt > 256) cnt = 256; u64 t = nowns(CLOCK_REALTIME);
                for (u64 q = 0; q < cnt; ++q) fillblock(big + q * BS, (u32)(i + q), 0, t);
                if (wrfull(fd, big, cnt * BS, (off_t)(i * BS)) != 0) { logf_("FILL write error"); return 2; }
                i += cnt; if ((i / 256) % 32 == 0) logf_("FILL %llu/%llu", (unsigned long long)i, (unsigned long long)(N - 1));
            }
            fdatasync(fd); free(big);
        }
        logf_("FILL done");
    }
    logf_("RUN start seq=%llu rate=%d real=%llu mono=%llu", (unsigned long long)k, rate, (unsigned long long)nowns(CLOCK_REALTIME), (unsigned long long)nowns(CLOCK_MONOTONIC));
    u64 t0 = nowns(CLOCK_MONOTONIC), n0 = k, nowm; u64 prevend = t0;
    for (;; ++k) {
        if (rate > 0) {                                            /* график: запись k не раньше t0 + (k-n0)/rate */
            u64 due = t0 + (k - n0) * 1000000000ULL / (u64)rate; nowm = nowns(CLOCK_MONOTONIC);
            if (due > nowm) { struct timespec ts; ts.tv_sec = (time_t)((due - nowm) / 1000000000ULL); ts.tv_nsec = (long)((due - nowm) % 1000000000ULL); nanosleep(&ts, 0); }
            else if (nowm - due > 500000000ULL) { t0 = nowm; n0 = k; }
        }
        u64 s = nowns(CLOCK_MONOTONIC), tr = nowns(CLOCK_REALTIME);
        fillblock(b, blockof(k), k, tr);
        if (wrfull(fd, b, BS, (off_t)((u64)blockof(k) * BS)) != 0) { logf_("WRITE error seq=%llu: %s", (unsigned long long)k, strerror(errno)); return 2; }
        u64 e = nowns(CLOCK_MONOTONIC); u64 lat = e - s, gap = s - prevend; prevend = e;
        if (lat > 100000000ULL || gap > 250000000ULL) logf_("STALL seq=%llu lat_ms=%llu gap_ms=%llu real=%llu", (unsigned long long)k, (unsigned long long)(lat / 1000000), (unsigned long long)(gap / 1000000), (unsigned long long)tr);
        if (k % 25 == 0) logf_("R seq=%llu real=%llu", (unsigned long long)k, (unsigned long long)tr);
    }
}
int main(int argc, char** argv) {
    crcinit();
    if (argc >= 3 && strcmp(argv[1], "write") == 0) {
        if (getenv("VM2B_TTY")) logfd = open("/dev/ttyS0", O_WRONLY | O_NOCTTY);
        int rate = 150;
        for (int i = 3; i + 1 < argc; ++i) if (strcmp(argv[i], "--rate") == 0) rate = atoi(argv[i + 1]);
        return do_write(argv[2], rate);
    }
    if (argc >= 3 && strcmp(argv[1], "verify") == 0) {
        if (getenv("VM2B_TTY")) logfd = open("/dev/ttyS0", O_WRONLY | O_NOCTTY);
        int hasw = argc >= 5; u64 lo = hasw ? strtoull(argv[3], 0, 10) : 0, hi = hasw ? strtoull(argv[4], 0, 10) : 0;
        return do_verify(argv[2], hasw, lo, hi);
    }
    fprintf(stderr, "vm2bio write DEV [--rate N] | verify DEV|FILE [lo hi]\n"); return 64;
}
