// Регрессионные тесты по находкам ревью безопасности (docs/DEV.md, раздел «Безопасность: что исправлено»):
// F1 права на каталог установки, F3 время входа неизвестного пользователя, F5 журнал аудита и заблокированные входы.
// F2 (operator и backup.run) проверяется по HTTPS в tools/testservers/web_client_tests.py и таблицей ролей в tests_web.cpp.
#include "autotest.h"
#include "tests_svc.h"
#include "webauth.h"
#include "applog.h"
#include <sddl.h>
#include <aclapi.h>

namespace vm2b {

namespace {

i64 g_secNow = 2000000;
i64 SecNow() { return g_secNow; }

struct AceInfo { Str sid; DWORD mask; BYTE type; BYTE flags; };

// Разбирает SDDL и возвращает ACE защищённого DACL.
bool ParseSddl(const wchar_t* sddl, Vec<AceInfo>& aces, bool& protectedDacl)
{
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &sd, nullptr)) return false;
    SECURITY_DESCRIPTOR_CONTROL ctl = 0; DWORD rev = 0;
    GetSecurityDescriptorControl(sd, &ctl, &rev); protectedDacl = (ctl & SE_DACL_PROTECTED) != 0;
    BOOL present = FALSE, def = FALSE; PACL acl = nullptr;
    bool ok = GetSecurityDescriptorDacl(sd, &present, &acl, &def) && present && acl;
    for (DWORD i = 0; ok && i < acl->AceCount; ++i) {
        void* ace = nullptr; if (!GetAce(acl, i, &ace)) { ok = false; break; }
        ACCESS_ALLOWED_ACE* a = (ACCESS_ALLOWED_ACE*)ace; wchar_t* ss = nullptr;
        if (!ConvertSidToStringSidW((PSID)&a->SidStart, &ss)) { ok = false; break; }
        AceInfo ai; ai.sid = FromWide(ss); ai.mask = a->Mask; ai.type = a->Header.AceType; ai.flags = a->Header.AceFlags; aces.push_back(ai); LocalFree(ss);
    }
    LocalFree(sd);
    return ok;
}

const DWORD kWriteBits = FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES | FILE_DELETE_CHILD | DELETE | WRITE_DAC | WRITE_OWNER;

bool CheckInstallAcl(Ctx& c_, const wchar_t* sddl, bool inherit)
{
    Vec<AceInfo> a; bool prot = false;
    T_CHECK(ParseSddl(sddl, a, prot));
    T_CHECK(prot);                                           // не наследует права корня тома
    T_CHECK(a.size() == 3);
    bool sy = false, ba = false, bu = false;
    for (size_t i = 0; i < a.size(); ++i) {
        T_CHECK(a[i].type == ACCESS_ALLOWED_ACE_TYPE);
        if (a[i].sid == "S-1-5-18") { sy = true; T_CHECK((a[i].mask & FILE_ALL_ACCESS) == FILE_ALL_ACCESS); }
        else if (a[i].sid == "S-1-5-32-544") { ba = true; T_CHECK((a[i].mask & FILE_ALL_ACCESS) == FILE_ALL_ACCESS); }
        else if (a[i].sid == "S-1-5-32-545") {               // Users: только чтение и выполнение
            bu = true;
            T_CHECK((a[i].mask & kWriteBits) == 0);
            T_CHECK((a[i].mask & FILE_EXECUTE) && (a[i].mask & FILE_READ_DATA));
        } else { c_.note("unexpected SID in the install ACL: %s", a[i].sid.c_str()); T_CHECK(false); }   // ни Authenticated Users, ни Everyone
        T_CHECK(((a[i].flags & (OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE)) != 0) == inherit);
    }
    T_CHECK(sy && ba && bu);
    return true;
}

} // namespace

// F1 (ревью безопасности): каталог установки и файлы получают защищённый DACL: SYSTEM и администраторы - полный доступ,
// "Пользователи" - только чтение и выполнение, никакой записи ни для кого другого.  На настоящей Windows права проверяет фаза installer-acl
// (docs/WINDOWS-TESTS.md); под Wine проверяются построение дескрипторов и то, что вызов проходит.
VM2B_TEST("security", install_dir_acl_is_protected_read_only_for_users)
{
    T_CHECK(CheckInstallAcl(c_, kInstallDirSddl, true));
    T_CHECK(CheckInstallAcl(c_, kInstallFileSddl, false));
    Tmp tmp(c_, "instacl");
    Str dir = tmp.sub("VM2Backup"), e; T_CHECK(MakeDirs(dir, e));
    Str f1 = JoinPath(dir, "VM2BackupService.exe"), f2 = JoinPath(dir, "VM2Backup.exe"); Bytes b = Random(c_, 100);
    T_CHECK(WriteBytes(f1, b)); T_CHECK(WriteBytes(f2, b));
    Str foreign = JoinPath(dir, "other.txt"); T_CHECK(WriteBytes(foreign, b));
    Wide wd(dir), w1(f1), w2(f2), w3(JoinPath(dir, "missing.exe"));
    const wchar_t* files[] = { w1.c_str(), w2.c_str(), w3.c_str() };       // a missing file is skipped
    bool applied = SecureInstallDir(wd.c_str(), files, 3);
    c_.note("SecureInstallDir under Wine: %s", applied ? "applied" : "not applied (error see below)");
    if (!applied) c_.note("last error %lu", GetLastError());
    T_CHECK(applied);
    // читаем назад: если файловая система Wine хранит дескриптор, он защищённый, а чужой файл не затронут
    BYTE buf[2048]; DWORD need = 0;
    if (GetFileSecurityW(wd.c_str(), DACL_SECURITY_INFORMATION, (PSECURITY_DESCRIPTOR)buf, sizeof(buf), &need)) {
        SECURITY_DESCRIPTOR_CONTROL ctl = 0; DWORD rev = 0; GetSecurityDescriptorControl((PSECURITY_DESCRIPTOR)buf, &ctl, &rev);
        c_.note("read back: dacl protected = %d", (ctl & SE_DACL_PROTECTED) != 0);
    }
    Bytes back; T_CHECK(ReadWholeFile(foreign.c_str(), back, e) && back.size() == 100);     // чужой файл на месте
    // повторное применение (обновление поверх существующей установки) тоже проходит
    T_CHECK(SecureInstallDir(wd.c_str(), files, 3));
    return true;
}

// Подсчёт времени ответа входа.
static double MedianMs(Vec<double>& v) { for (size_t i = 1; i < v.size(); ++i) { double x = v[i]; size_t j = i; while (j > 0 && v[j - 1] > x) { v[j] = v[j - 1]; --j; } v[j] = x; } return v[v.size() / 2]; }
static double NowMs() { LARGE_INTEGER f, t; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t); return 1000.0 * (double)t.QuadPart / (double)f.QuadPart; }

// F3: неизвестный пользователь стоит столько же, сколько известный с неверным паролем (контрольный хеш 32 байта, PBKDF2 всегда выполняется),
// а ответ не отличает «нет пользователя» от «неверный пароль».
VM2B_TEST("security", login_time_does_not_reveal_unknown_users)
{
    Tmp tmp(c_, "logintime"); WebAuth a; Str err;
    g_secNow = 3000000; a.setClock(SecNow); T_CHECK(a.open(tmp.sub("d"), err, false));
    a.setKdfIterationsForTests(60000);                        // ~ десятки мс: заметно на фоне шума, но тест остаётся быстрым
    Json s = Json::object(); s["name"] = "alice"; s["role"] = "operator"; s["password"] = "correct horse battery"; s["twofa"] = "optional";
    T_CHECK(a.usersSave(s, err));
    Vec<double> known, unknown; Str msgKnown, msgUnknown; int stKnown = 0, stUnknown = 0;
    for (int i = 0; i < 7; ++i) {
        g_secNow += 5000;                                     // блокировки по имени и адресу истекли
        Str ipA = Str::fmt("10.1.%d.1", i), ipB = Str::fmt("10.2.%d.1", i);   // разные исходные адреса
        double t0 = NowMs();
        WebAuth::LoginResult rk = a.login(ipA, "alice", "wrong password!", "", "");
        double t1 = NowMs();
        WebAuth::LoginResult ru = a.login(ipB, Str::fmt("nosuchuser%d", i), "wrong password!", "", "");
        double t2 = NowMs();
        known.push_back(t1 - t0); unknown.push_back(t2 - t1);
        stKnown = rk.status; stUnknown = ru.status; msgKnown = rk.message; msgUnknown = ru.message;
        T_CHECK(rk.status == 401 && ru.status == 401);
    }
    T_CHECK(stKnown == stUnknown && msgKnown == msgUnknown);   // ни кода, ни текста, которые различают случаи
    double mk = MedianMs(known), mu = MedianMs(unknown);
    c_.note("median ms: known user %.1f, unknown user %.1f", mk, mu);
    T_CHECK(mk > 3.0);                                        // хеш действительно считался
    T_CHECK(mu * 3.0 > mk && mu < mk * 3.0);                  // отношение медиан < 3 в обе стороны (до исправления было 1:50)
    return true;
}

static i64 FileSize(const Str& p) { WIN32_FILE_ATTRIBUTE_DATA fa; if (!GetFileAttributesExW(Wide(p).c_str(), GetFileExInfoStandard, &fa)) return -1; return ((i64)fa.nFileSizeHigh << 32) | fa.nFileSizeLow; }

// F5: поток заблокированных входов не вымывает журнал аудита: строка раз в минуту на адрес, потом одна сводка; обычные события пишутся.
VM2B_TEST("security", audit_log_survives_a_flood_of_blocked_logins)
{
    Tmp tmp(c_, "auditflood"); WebAuth a; Str err;
    g_secNow = 4000000; a.setClock(SecNow); T_CHECK(a.open(tmp.sub("d"), err, false));
    a.setKdfIterationsForTests(1000);
    Json s = Json::object(); s["name"] = "alice"; s["role"] = "operator"; s["password"] = "correct horse battery"; s["twofa"] = "optional";
    T_CHECK(a.usersSave(s, err));
    Str audit = JoinPath(tmp.sub("d"), "audit.log");
    a.login("10.0.0.1", "alice", "wrong password!", "", "");          // один неверный пароль: имя и адрес заблокированы на секунду
    i64 before = FileSize(audit);
    // 200 запросов с ОДНОГО адреса
    for (int i = 0; i < 200; ++i) { WebAuth::LoginResult r = a.login("10.0.0.1", "alice", "x", "", ""); T_CHECK(r.status == 429); }
    i64 afterOne = FileSize(audit);
    c_.note("200 blocked requests from one address: the audit log grew by %lld bytes", (long long)(afterOne - before));
    T_CHECK(afterOne - before < 400);                                // раньше ~54 байта на запрос = 10 КБ
    // 200 запросов с 200 РАЗНЫХ адресов (блокировка по имени)
    for (int i = 0; i < 200; ++i) { WebAuth::LoginResult r = a.login(Str::fmt("10.9.%d.%d", i / 250, i % 250 + 1), "alice", "x", "", ""); T_CHECK(r.status == 429); }
    i64 afterMany = FileSize(audit);
    c_.note("200 blocked requests from 200 addresses: +%lld bytes", (long long)(afterMany - afterOne));
    T_CHECK(afterMany - afterOne < 20 * 120 + 400);                  // не больше kBlockedLinesPerMinute строк
    // обычные события пишутся, а сводка появляется после минуты
    g_secNow += 130;
    WebAuth::LoginResult ok = a.login("10.0.0.7", "alice", "correct horse battery", "", ""); T_CHECK(ok.status == 200);
    Str tail = a.auditTail(500);
    T_CHECK(tail.find("login_ok") >= 0);
    T_CHECK(tail.find("login_failed") >= 0);
    T_CHECK(tail.find("blocked login attempts from 10.0.0.1 in last minute") >= 0);
    T_CHECK(tail.find("201 blocked login attempts") >= 0 || tail.find("200 blocked login attempts") >= 0);
    int lines = 0; for (size_t i = 0; i < tail.size(); ++i) lines += tail[i] == '\n';
    T_CHECK(lines < 60);
    return true;
}

// F5: ротация держит несколько поколений (audit.log, audit.1.log ... audit.4.log), один поток событий не стирает всю историю.
VM2B_TEST("security", audit_rotation_keeps_several_generations)
{
    Tmp tmp(c_, "auditrot"); WebAuth a; Str err;
    g_secNow = 5000000; a.setClock(SecNow); T_CHECK(a.open(tmp.sub("d"), err, false));
    Str d = tmp.sub("d");
    Bytes big; big.resize((5u << 20) + 100); memset(big.data(), 'x', big.size());
    Bytes m1; m1.resize(5); memcpy(m1.data(), "gen-1", 5); Bytes m2; m2.resize(5); memcpy(m2.data(), "gen-2", 5); Bytes m3; m3.resize(5); memcpy(m3.data(), "gen-3", 5);
    T_CHECK(WriteBytes(JoinPath(d, "audit.log"), big)); T_CHECK(WriteBytes(JoinPath(d, "audit.1.log"), m1));
    T_CHECK(WriteBytes(JoinPath(d, "audit.2.log"), m2)); T_CHECK(WriteBytes(JoinPath(d, "audit.3.log"), m3));
    a.audit("test_event", "u", "1.2.3.4", "after the rotation");
    Bytes r; Str e;
    T_CHECK(ReadWholeFile(JoinPath(d, "audit.4.log").c_str(), r, e) && r.size() == 5 && memcmp(r.data(), "gen-3", 5) == 0);
    T_CHECK(ReadWholeFile(JoinPath(d, "audit.3.log").c_str(), r, e) && r.size() == 5 && memcmp(r.data(), "gen-2", 5) == 0);
    T_CHECK(ReadWholeFile(JoinPath(d, "audit.2.log").c_str(), r, e) && r.size() == 5 && memcmp(r.data(), "gen-1", 5) == 0);
    T_CHECK(FileSize(JoinPath(d, "audit.1.log")) > (5 << 20));
    T_CHECK(a.auditTail(10).find("after the rotation") >= 0);
    return true;
}

} // namespace vm2b
