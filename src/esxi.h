// VM2Backup — VMware ESXi 6.0 .. 9.0 and vCenter as a source of virtual machines (see docs/ESXI.md).
// vSphere Web Services API (SOAP at /sdk) for the inventory and snapshots, datastore file access over HTTPS (/folder) for the data.
// No VMware SDK, no external library: the TLS client (tls.h), the HTTP client (http.h) and the XML parser (xml.h) are our own.
#pragma once
#include "engine.h"

namespace vm2b {

// The source (kind() == "esxi"; static_cast a VmSource* to it after checking kind()). The caller that wants to cancel a running begin() /
// list() (waiting for a snapshot task, a slow server) calls requestAbort() from another thread. pack() polls Progress::aborted() instead.
struct EsxiSource : VmSource {
    virtual void requestAbort() = 0;
    virtual Str serverInfo() const = 0;      // "VMware ESXi 7.0.3 build-xxx (api 7.0.3.0)" after the first call, "" before
    virtual Str endProblem() const = 0;      // non-empty when end() could not remove the snapshot (its name); the next begin() cleans it up
};

// Server config (a JSON object): type "esxi"|"vcenter", host, port (443), user, password, verify, fingerprint "sha256:..", tls_min "1.0"|"1.2",
// transport "api" (default) | "ssh" (not available yet), snapshot {quiesce:true, memory:false}, datacenter (vCenter: only this one),
// and the tuning keys timeout (idle seconds of one network step), task_timeout (s, snapshot creation, default 600), remove_timeout (s, default 3600),
// poll_ms (first task polling interval).
VmSource* NewEsxiSource(const Json& server, Str& err);

// ---- helpers (pure functions, unit-tested) ---------------------------------------------------------------------
bool EsxiParseDsPath(const Str& s, Str& datastore, Str& path);          // "[ds 1] dir/x.vmx" -> "ds 1", "dir/x.vmx"
Str  EsxiUrlEscape(const Str& s);                                       // percent-escapes everything but unreserved characters and '/'
Str  EsxiFolderUrl(const Str& datastore, const Str& path, const Str& datacenter);   // /folder/<path>?dcPath=..&dsName=..
i64  EsxiParseHttpDate(const Str& s);                                   // "Sat, 24 Oct 2026 12:00:00 GMT" -> unix time, -1 when malformed
int  EsxiCompareVersion(const Str& a, const Str& b);                    // dotted numbers, "6.7.0" < "7.0": -1 / 0 / 1

// ---- restore TO an ESXi host / vCenter (docs/ESXI.md, "Restore") -----------------------------------------------------
struct EsxiRestoreOptions {
    Str vmName;                 // the new VM's name; "" = the original (or "<name>-restored" when that is taken). A name that is taken is refused: nothing is overwritten
    Str datacenter;             // vCenter: the datacenter (default: the first / the server's `datacenter` key); a host has exactly one
    Str datastore;              // "" = every file goes back to the datastore it came from; a name = everything into that datastore
    Str folder;                 // VM folder: id or path below the datacenter's root VM folder ("Servers/Linux"); "" = the root VM folder
    Str pool;                   // resource pool: id, full path ("host/Resources/Prod") or a unique name; "" = the root pool of the (first) host / cluster
    Str host;                   // optional host: id or name (vCenter)
    bool powerOn;               // power the VM on after it was registered (default off)
    RateLimiter* limiter;       // bandwidth limit of the UPLOAD (bytes that go to the host); RestoreToEsxi() fills it from ReadOptions::limiter when null
    EsxiRestoreOptions() : powerOn(false), limiter(nullptr) {}
};

struct EsxiRestoreResult { Str vmName, vmRef, vmxPath, datastore, folder, warning; };

// Uploads the backup's files (PUT /folder), edits the .vmx (the archive holds the base disks, not the running delta the original .vmx points
// at: the disk entries are redirected to the base disks, snapshot / change tracking lines are removed, the name is set), registers the VM.
// The restore is resumable (a lost connection: the file that was being uploaded starts again; a later run continues, see engine.h RestoreWriter)
// and cleans up after itself: on a final failure or a cancel the folders it created are deleted. `ro.stateDir` = where the state file lives.
bool RestoreToEsxi(Target& target, const Str& archive, const Json& server, const EsxiRestoreOptions& o, const Str& password, const ReadOptions& ro, Progress* pr,
                   EsxiRestoreResult* result, Str& err);
RestoreWriter* NewEsxiRestoreWriter(const Json& server, const EsxiRestoreOptions& o, EsxiRestoreResult* result, Str& err);   // for engine-level use / tests

// What a restore dialog can offer: {"datacenter":"..","datacenters":[..],"vm_folder":"id","datastores":[{"name","free","capacity","type","accessible"}],
// "folders":[{"id","path"}],"pools":[{"id","name","path"}],"hosts":[{"id","name"}],"vms":["existing VM names"],"api_version":"..","kind":"esxi|vcenter"}
bool EsxiRestoreTargets(const Json& server, const Str& datacenter, Json& out, Str& err);

// The pure parts (unit-tested): the new text of the .vmx / .vmsd.  `fromBase[i]` (a disk file name as the old .vmx refers to it, no folder) is
// replaced by `toValue[i]` in every  scsiX:Y.fileName / sataX:Y.fileName / ideX:Y.fileName / nvmeX:Y.fileName  line.
Str EsxiRewriteVmx(const Str& vmx, const Str& newName, const Vec<Str>& fromBase, const Vec<Str>& toValue);
// The first path component of every file of a backup: the VM name made safe for any restore target (characters Windows refuses become _, a trailing
// dot / space goes, CON / NUL / COM1... get a leading _, at most 200 bytes); the real name stays in the meta file.
Str EsxiArchivePrefix(const Str& vmName);
Str EsxiRewriteVmsd(const Str& vmsd, const Str& dropSnapshotName);                     // the snapshot with that display name is removed, the rest renumbered

} // namespace vm2b
