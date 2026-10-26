// VM2Backup — the backup engine: VM source -> tar -> compression -> encryption -> hash -> target, plus verification,
// restore, listing, retention and RESUMING after a lost connection.
//
// A backup is a series of VOLUMES: the tar stream is cut every `segmentBytes` (default 256 MiB of tar data) and each piece is
// compressed, encrypted and written on its own, so every volume is a complete, independent file. Layout in a target:
//   <vm>_<YYYYMMDD-HHMMSS>.tar[.gz|.xz][.enc].00001, .00002, ...    the volumes (each written as .part, then renamed: atomic)
//   <archive>.resume                                                 the journal while the backup is being made (see below)
//   <archive>.json                                                   the manifest, written last: its presence = the backup is complete
// <archive> is the logical name "<vm>_<stamp>.tar[.gz|.xz][.enc]". The manifest lists every volume (size, SHA-256, raw bytes), so
// a checksum is verified volume by volume while the data is read (the SHA-256 state cannot be saved, per-volume hashes can).
//
// Resuming. After every finished volume the journal is rewritten. When the connection breaks, the engine reconnects (with growing
// pauses) and goes on with the first volume that is not finished: the VM source skips the data it already delivered (files are
// seeked, not read). The same happens across runs: a new BackupVm() of the same VM finds the journal, checks that the options and
// the VM (its fingerprint) are the same, and continues. Restore and verify reconnect the same way (a volume that broke off is
// opened again and the bytes already delivered are skipped), and a restore keeps a small state file in the destination so that it
// can continue after the program was stopped.
#pragma once
#include "codec.h"
#include "crypto.h"
#include "target.h"
#include "tar.h"
#include "throttle.h"

namespace vm2b {

extern const char* const kAbortedMessage;                  // the error text of a user abort (compare with ==)

struct RetryPolicy {
    int attempts;                                          // reconnect-and-continue attempts per failure series (0 = never retry)
    int baseDelayMs, maxDelayMs;                           // the pause doubles from base up to max
    RetryPolicy() : attempts(8), baseDelayMs(2000), maxDelayMs(60000) {}
};

struct Progress {
    virtual ~Progress() {}
    virtual void onBytes(u64 rawBytes) {}                  // bytes read from the VM while it is backed up
    virtual void onArchiveBytes(u64 stored) {}             // bytes read from the stored archive (verification, restore)
    virtual void onFile(const Str& path) {}
    virtual void onVolume(int done, int total) {}          // a volume is finished (total = -1 while unknown)
    virtual void onRetry(const Str& what, int attempt, int maxAttempts, int delayMs, const Str& reason) {}   // the connection broke: waiting to try again
    virtual void onResume(int volumesDone, u64 rawBytesDone) {}                                              // continuing from where an earlier run stopped
    virtual bool aborted() { return false; }               // polled often; true stops the job
    virtual bool keepOnAbort() { return false; }           // when it stops: keep the finished volumes so that the backup can go on later (a pause, not a cancel)
};

struct VmInfo { Str name, state; u64 size; Str extra; VmInfo() : size(0) {} };

struct VmSource {
    Str err; bool transient;
    VmSource() : transient(false) {}
    virtual ~VmSource() {}
    virtual const char* kind() const = 0;
    virtual bool list(Vec<VmInfo>& vms) = 0;
    // Prepares the copy of one VM (refuses a running one unless allowLive, takes a snapshot...). `fingerprint` stays the same
    // while the VM's layout and content are the same — a backup continues only if it matches the one of the interrupted run.
    // `token` is kept in the journal and given back as resumeToken when the backup continues (a snapshot to reuse).
    virtual bool begin(const Str& vm, bool allowLive, const Str& resumeToken, Str& fingerprint, Str& token, Str& warning) = 0;
    // Writes the whole VM into the tar. May be called again after a failure; use tar.skippable() / skip_data() to avoid reading
    // data that lies in the part of the stream that is already stored. kAbortedMessage after an abort.
    virtual bool pack(TarWriter& tar, Progress* pr, Str& warning) = 0;
    virtual void end(bool keepForResume) {}                // releases what begin() took (the snapshot), unless the backup will continue
protected:
    bool fail(const Str& e, bool temp = false) { err = e; transient = temp; return false; }
};

// A folder where every sub-folder is a VM (VMware Workstation / Player, VirtualBox, Hyper-V exports, KVM images, mounted datastores).
VmSource* NewDirectorySource(const Str& root);
// Builds a source from a server record of the configuration (type directory | esxi | vcenter); null + err when it cannot.
VmSource* MakeSource(const Json& server, Str& err);
VmSource* NewEsxiSource(const Json& server, Str& err);   // esxi.cpp

struct Segment { u64 size; u64 raw; Str sha256; Segment() : size(0), raw(0) {} };

struct Manifest {
    int version; Str vm, archive, source, sha256, stamp; double created; Str codec; int level; bool encrypted;   // codec: the name in the manifest, only "xz" can be restored
    u64 size, rawSize, segmentBytes; u32 kdfIterations; Vec<Segment> segments;
    Manifest() : version(3), created(0), codec("xz"), level(0), encrypted(false), size(0), rawSize(0), segmentBytes(0), kdfIterations(0) {}
    Str volumeName(size_t i) const { return archive + Str::fmt(".%05d", (int)i + 1); }
    Json toJson() const;
    static bool fromJson(const Json& j, Manifest& m);
};

enum { kDefaultSegmentMiB = 256 };

struct BackupOptions {
    CodecOptions codec;            // level (and threads) of the xz compression (default level 5)
    Str password;                  // empty = no encryption
    int keep;                      // keep this many newest backups of the VM (0 = all)
    bool verify;                   // re-read and check the written backup
    bool allowLive;                // copy a running VM (not consistent)
    u32 kdfIterations;             // PBKDF2 iterations for the key (stored in the archive; the tests use a small number)
    u64 segmentBytes;              // size of a volume in tar bytes, a multiple of 512 (0 = one volume: no resuming)
    bool resume;                   // continue an interrupted backup of this VM when there is one
    RetryPolicy retry;
    RateLimiter* limiter;          // the bandwidth limit of the transfer to the target (shared, may change while running); null = unlimited
    BackupOptions() : keep(0), verify(true), allowLive(false), kdfIterations(kEncIterationsDefault), segmentBytes((u64)kDefaultSegmentMiB << 20), resume(true), limiter(nullptr) {}
};

struct ReadOptions {               // for verify and restore
    bool resume;                   // restore: continue from the state file in the destination
    RetryPolicy retry;
    RateLimiter* limiter;          // the bandwidth limit of the transfer from the target; null = unlimited
    Str stateDir;                  // restore through a remote RestoreWriter: where its state file is kept (local; default: the Windows temp folder)
    bool* lostConnection;          // optional out: set to true when a verify / restore gave up because the connection to the target could not be kept (not because the data is bad)
    ReadOptions() : resume(true), limiter(nullptr), lostConnection(nullptr) {}
};

bool BackupVm(VmSource& src, const Str& vm, Target& target, const BackupOptions& o, Progress* pr, Manifest& out, Str& warning, Str& err);
bool ListBackups(Target& target, const Str* vm, Vec<Manifest>& out, Str& err);       // finished backups, oldest first
bool ReadManifest(Target& target, const Str& archive, Manifest& m, Str& err);

// Interrupted backups that can be continued by BackupVm().
struct ResumeInfo { Str vm, archive, stamp; double created; int volumesDone; u64 storedBytes, rawBytes; ResumeInfo() : created(0), volumesDone(0), storedBytes(0), rawBytes(0) {} };
bool ListResumable(Target& target, const Str* vm, Vec<ResumeInfo>& out, Str& err);
bool DiscardResumable(Target& target, const Str& archive, Str& err);                 // deletes the journal and the volumes

// Checks the volume sizes and SHA-256s; with a password (or without encryption) also decrypts / decompresses and reads every tar entry.
bool VerifyBackup(Target& target, const Str& archive, const Str& password, Progress* pr, Manifest* m, Str& err);
bool VerifyBackup(Target& target, const Str& archive, const Str& password, const ReadOptions& ro, Progress* pr, Manifest* m, Str& err);
// Extracts <vm>/... below destDir. A backup that fails its checksum is reported as an error after extraction (do not trust the files).
bool RestoreBackup(Target& target, const Str& archive, const Str& destDir, const Str& password, Progress* pr, Manifest* m, Str& err);
bool RestoreBackup(Target& target, const Str& archive, const Str& destDir, const Str& password, const ReadOptions& ro, Progress* pr, Manifest* m, Str& err);

// ---- restore writers -------------------------------------------------------------------------------------------------
// Where a restore puts the files: a local / network folder (LocalWriter, what RestoreBackup(.., destDir, ..) uses), an ESXi datastore (esxi.h).
// The engine reads the archive as one tar stream and hands every entry to the writer.
//   * Resuming. A restore keeps a small STATE (archive, root hash, volume + offset, the tar reader's state, plus the writer's own state
//     `saveState()`); a later run of the same restore (or a retry after a lost connection) continues from it.
//     - canResumeInsideFile() == true (local folder): the state is saved while a file is being written (every volume / 256 MiB), and
//       writeFile() may be asked to continue a file at `resumeAt` (the writer checks that the file holds that much, see canContinue()).
//     - canResumeInsideFile() == false (HTTP PUT cannot append): the state is saved only at the START of each file, and the file is
//       uploaded again from its first byte after a failure. LIMIT: a failure in the middle of a 100 GB disk re-reads the archive and
//       re-uploads that disk from its beginning.
//   * Errors: err + transient like Target: transient failures (lost connection, timeout, 5xx busy) are waited out by the engine (ReadOptions::retry),
//     reconnect() is called and the file is written again; permanent ones end the restore.
struct RestoreFile { Str rel; u64 size; i64 mtime; u64 resumeAt; RestoreFile() : size(0), mtime(0), resumeAt(0) {} };   // rel: a checked archive path, '/' separated

struct RestoreTick { virtual ~RestoreTick() {} virtual void chunk(size_t n) = 0; };  // the writer calls it after every chunk it wrote

struct RestoreWriter {
    Str err; bool transient;
    RestoreWriter() : transient(false) {}
    virtual ~RestoreWriter() {}
    virtual const char* kind() const = 0;
    virtual bool canResumeInsideFile() const = 0;
    virtual bool foldsCase() const { return false; }                    // true: the destination treats "A" and "a" as one name (a Windows folder): the restore refuses an archive with two such files
    // Where the engine keeps the state of this restore (a file name in a local folder; "" = no persistence).
    virtual Str stateFile(const ReadOptions& ro, const Str& archive, const Str& root) = 0;
    virtual Json saveState() { return Json(); }                         // the writer's own part of the state (null = none)
    virtual bool loadState(const Json& j) { (void)j; return true; }     // false: the saved state does not fit, the restore starts over
    // Creates the destination (called once, after the state was read). `resuming`: a state of an earlier run was accepted.
    virtual bool prepare(const Manifest& m, bool resuming) = 0;
    virtual bool makeDir(const Str& rel) = 0;
    // True when a file that was written up to `f.resumeAt` before can be continued there (the destination holds at least that much).
    virtual bool canContinue(const RestoreFile& f) { return f.resumeAt == 0; }
    // Writes the file's data: exactly f.size - f.resumeAt bytes come from `body` (body.read < 0 = the archive failed: return false,
    // the engine knows). The file keeps f.mtime. `tick` (may be null) is told about every chunk written.
    virtual bool writeFile(const RestoreFile& f, Source& body, RestoreTick* tick) = 0;
    virtual void checkpoint() {}                                        // make what was written so far durable (before the state is saved)
    virtual bool reconnect() { return true; }                           // after a transient failure
    // After the last file: whatever must happen at the end (ESXi: upload the edited .vmx, register the VM). May be retried when transient.
    virtual bool finish() { return true; }
    // true: on a final failure (not on a pause) the engine calls cleanup() and forgets the state; false: partial results stay (local folder)
    virtual bool cleansUpOnFailure() const { return false; }
    virtual void cleanup() {}
protected:
    bool fail(const Str& e, bool temp = false) { err = e; transient = temp; return false; }
};

RestoreWriter* NewLocalWriter(const Str& destDir);                      // a folder; the state file is <destDir>\.vm2b-restore.json
// Restores through a writer. `m` receives the manifest. Pause = Progress::keepOnAbort() keeps the state, cancel / permanent error cleans up (writers that clean up).
bool RestoreBackup(Target& target, const Str& archive, RestoreWriter& w, const Str& password, const ReadOptions& ro, Progress* pr, Manifest* m, Str& err);

bool DeleteBackup(Target& target, const Str& archive, Str& err);                     // the volumes and the manifest
// `protect`: an archive that is never removed and counts as the newest (the backup that was just made, whatever its "created" says: a clock that was set back must not delete the new backup).
bool PruneBackups(Target& target, const Str& vm, int keep, Vec<Str>& removed, Str& err, const Str& protect = Str());

} // namespace vm2b
