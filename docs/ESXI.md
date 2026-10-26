# VMware ESXi / vCenter as a source (VM2Backup)

Files: `src/xml.*` (XML parser/writer), `src/esxi.*` (the source, `NewEsxiSource`), `src/tests_esxi.cpp` (group `esxi`),
`tools/testservers/esxi_mock.py` + `esxi_launch.py` (the emulators), `tools/run-esxi-tests.sh`.

No VMware SDK: the vSphere Web Services API (SOAP at `/sdk`) over OpenSSL TLS (`tls.cpp`) + our own HTTP client, and
datastore file access over HTTPS (`/folder`). Supported: ESXi 6.0 / 6.5 / 6.7 / 7.0 / 8.0 / 9.0 and vCenter of the same versions.

## Server config (JSON, see `esxi.h`)

`type` esxi|vcenter, `host`, `port` (443), `user`, `password`, `verify`, `fingerprint` ("sha256:..", a self-signed host is pinned),
`tls_min` ("1.0" for ESXi 6.0 / 6.5 hosts that offer nothing newer, default "1.2"), `transport` "api" (default) | "ssh" (**not available
yet**: `NewEsxiSource` fails with a clear text), `snapshot` {quiesce: true, memory: false} (the service copies it from the task element),
`datacenter` (vCenter: only this datacenter; without it every datacenter is searched and a name that exists twice is refused with a
message asking for it). Tuning keys: `timeout` (idle seconds of a network step), `task_timeout` (600 s, snapshot creation),
`remove_timeout` (3600 s, a consolidation can be long), `poll_ms` (first task polling interval, doubles up to 1.5 s).

## What a backup does

1. `begin()`: reads `RetrieveServiceContent` (SOAP version string: tries `urn:vim25/6.0`, then 6.5 ... 9.0 when the server answers with a fault /
   HTTP error to it; the XML namespace is always `urn:vim25`), logs in (`SessionManager.Login`, cookie `vmware_soap_session`; a
   `NotAuthenticated` fault or a 401 on `/folder` makes one transparent re-login), finds the VM (a `ContainerView` of the datacenter +
   `RetrievePropertiesEx`; falls back to `RetrieveProperties` when a server refuses the Ex call), refuses templates and inaccessible VMs.
2. Snapshot `VM2Backup-<10 hex>` (`CreateSnapshot_Task`, quiesce / memory per config, only for powered-on VMs). Task polling through the
   property collector. A quiesce failure (VMware Tools missing: `ToolsUnavailable`, `ApplicationQuiesceFault`, ...) retries once without
   quiescing and adds a warning. A refused write API (`RestrictedVersion`, "current license ... prohibits", `NoPermission`: free ESXi) is a
   permanent error that names the SSH transport / a licensed host. A hung task is cancelled after `task_timeout`.
3. File list AFTER the snapshot, from `layoutEx`: the configuration files (`config` .vmx, `extendedConfig` .vmxf, `nvram`, `snapshotList`
   .vmsd, `namespaceData`), snapshot data/memory (.vmsn/.vmem), suspend state, and for every snapshot of the VM the disk chains it froze
   (`layoutEx.snapshot[].disk[].chain[]`: descriptors and base/flat/delta extents of earlier snapshots). **Excluded**: swap (.vswp,
   uwswap), logs, stats, cores, screenshots, digest files, lock files, and the delta/sesparse file and descriptor of the running point
   (the snapshot VM2Backup just made: it is in no snapshot's chain). A disk the snapshot does not cover (independent disk) is copied live
   with a warning. Files on another datastore go to `<vm>/_other-datastores/<datastore>/<path>`. Types the source does not know are not
   copied but listed in the meta file (`ignored_files`).
   Sizes and times come from a `HEAD /folder/...` per file (the layout's size is the fallback when HEAD is refused).
4. Fingerprint = SHA-256 of (uuid, instance uuid, snapshot MoRef + name, the ordered list of path|size); token = `<snapshot MoRef>|<name>`.
5. `pack()`: `vm2backup-meta.json` (source, host, API version, uuid, vmx path, snapshot, per-file name/size/type, the disks with their
   descriptor at snapshot time, ignored files), then every file as `<vm>/<path relative to the VM folder>` streamed with
   `GET /folder/<path>?dcPath=<dc>&dsName=<ds>` (`ha-datacenter` on a host). `tar.skippable()` > 0: no request when the whole file is
   stored, `Range: bytes=N-` otherwise (206 expected; a 200 is read and discarded). Thin / sparse disks are just a stream (compression takes
   care of the zeros). Transient network errors (reset, short body, timeout) are `transient` for the engine's retry; 4xx/5xx on a file is
   permanent with the datastore path in the text (500 / 502 / 503 / 504 are tried 3 times first). A configuration file (.vmx, .vmsd, ...) that shrank is
   padded with zeros, one that grew is cut; both give a warning. A frozen binary file (disk extent, snapshot data / memory, suspend) that comes back
   short, an HTML page (Content-Type text/html) instead of a binary file, or a HEAD size smaller than the layout's, is an error, never a padded copy.
6. `end(keepForResume)`: `RemoveSnapshot_Task(consolidate = true)` unless the backup will continue; abort or failure removes it always (the
   abort flag never prevents the removal). If even that fails the snapshot name is in `EsxiSource::endProblem()` and the NEXT `begin()` of that VM
   removes every `VM2Backup-<10 hex digits>` snapshot it finds (warning "removed N old VM2Backup snapshot(s)"); a user's own snapshots (any other name,
   also "VM2Backup-before-upgrade") are never touched. A running backup marks its snapshot description `[live <unix time>]` (renewed every 2 minutes,
   removed when it ends or pauses): a snapshot with a fresh mark belongs to a run that is working and is left alone (warning). The mark names the computer, the process id and a per-run token: a mark of this computer whose process (or, inside this process, run) is gone is a crash leftover and is removed at once; a mark of another computer is trusted for 10 minutes.
   `begin()` with a `resumeToken` re-attaches to that snapshot when it still exists (same name); otherwise it makes a new one and the engine
   sees a different fingerprint and starts over. Two backups of the same VM at the same time: neither removes the other's snapshot, but the second
   also copies the first one's snapshot files (the VM's history) and fails cleanly (HTTP 404) if the first removes them meanwhile; different VMs, also on
   one server, run in parallel.

## Test emulators and what is verified how

`esxi_mock.py` (python3 + `cryptography`) implements the SOAP calls above with the XML written from the vSphere API reference, per profile:
version strings, TLS floor (6.0 / 6.5: 1.0-1.2, 6.7 / 7.0: 1.2, 8.0 / 9.0: 1.2-1.3; one more 6.0 host speaks TLS 1.0 only), `capacityInBytes` only
from 6.7 (older profiles give only `capacityInKB`), delta file kind (`-delta.vmdk` up to 6.5, `-sesparse.vmdk` later), unknown extra fields (8.0+),
`instanceUuid` missing on the 6.0 host, 9.0 refusing the `6.0` SOAP version string, vCenter with two datacenters (`DC1`, `DC2`), session cookies and
expiry, `/folder` with Range / HEAD / PUT. Snapshot semantics are real: a frozen base flat file, a delta that takes guest writes, consolidation on
removal. Faults on demand (`/__mock/faults`): free-license refusal (at the call or in the task), quiesce failure (two kinds), task error, hung task,
`remove_fail`, 503 on a file, connection drop after N bytes, no Range, no HEAD, no `RetrievePropertiesEx`, layout that lags behind the snapshot, slow
server, session expiry after N calls. `/__mock/state` and `/__mock/log` give the live snapshots, the sequence of calls and the create/remove events.

**Verified only against the mock** (nothing real was available): every XML shape, i.e. that real ESXi / vCenter answer exactly like the emulator
(namespaces, `xsi:type` names, element order is not relied on; unknown elements are ignored), the fault type names the code recognises
(`NotAuthenticated`, `InvalidLogin`, `RestrictedVersion`, `NoPermission`, `ToolsUnavailable`, `ApplicationQuiesceFault`, `ManagedObjectNotFound`,
...; the code also matches on the message text), that `layoutEx.snapshot[].disk[].chain[]` of a real host lists the frozen chain the way the emulator does,
that `/folder` on real hosts honours `Range` and `HEAD` and accepts the session cookie (the code falls back when not), what exactly ESXi 8 / 9 and vCenter
7 / 8 / 9 accept as SOAP version (9.0 is entirely an assumption, including its refusing 6.0), TLS behaviour of the real stacks (OpenSSL against the emulator
here; ESXi 6.0 needs `tls_min` 1.0, which also lowers the OpenSSL security level), and the license texts of the free hypervisor. First thing to do with a real host: `list`, then a backup of a small powered-off VM, then a powered-on one.

## Odd hosts and VMs (round 2)

* `/folder` answers may be chunked, close-delimited, `Connection: close`, slow to start, or a 301/302/303/307/308 to this same host (followed, 5 hops; another host is a clear error).
* A 503 on a SOAP call is retried 3 times. Sessions that idle out are logged in again (SOAP, GET and PUT paths).
* Encrypted VMs (`config.keyId`, API 6.5+) are refused unless the server config has `allow_encrypted` (ciphertext copied, warning). RDM disks are skipped with a warning (the LUN is not readable over HTTP). `-ctk.vmdk` files are not copied (the restored VM has change tracking off).
* The archive path of every file is made Windows-safe (`EsxiArchivePrefix`); the meta file keeps the real VM name (`vm`) and the original datastore name of a renamed file (`orig_path`); restore-to-ESXi maps back.
* The new .vmx text escapes `"`, `|` and control characters as `|XX`.

## Odd hosts and VMs (round 3)

* A task that outlives `task_timeout` and cannot be cancelled is waited for up to 30 s more, so a snapshot it makes is found and removed. "Not found" from `RemoveSnapshot` is checked against the snapshot tree.
* A disk chain naming a file key without a file entry in `layoutEx` is asked for again (up to 5 times), then refused (never silently skipped).
* Restore: the VM's own datastore folder must be new (`MakeDirectory` "already exists" = someone else's folder: refused); a same-named folder on any other datastore the backup uses also counts as taken. Every restored file gets its place up front (`buildMap`): folders other than the VM's own are folded into the VM folder, but two files that would clash keep their datastore + folder in the name. Disks of another folder are matched in the .vmx by their full `[ds] path`; an independent disk (copied live) is repointed to its restored copy. A PUT answered 201 whose file is absent (HEAD 404) is retried.
* Branching snapshot trees, deep chains (34), 0-byte / unaligned disks, 60 disks, suspended VMs, datacenter names with spaces / unicode / `%` are covered by the emulator tests.

## Known gaps

* SSH transport (a later phase). Restore TO a host is the API transport only (see Restore below).
* Abort during `begin()` / `list()` needs the caller to call `EsxiSource::requestAbort()` (the engine passes no Progress to `begin`).
* vSAN / vVol / NFS-specific layouts, linked clones, RDM and shared disks, encrypted VMs (vSphere VM encryption) and VMs in a folder with `/` or `%` in the *datastore folder* name beyond what the tests draw are not specially handled.
* Restored disk descriptors are byte-exact: a linked clone whose `parentFileNameHint` is an absolute path (View / Horizon replicas) still points at the original parent after a restore (the parent's files are copied too, but not wired in). Not verified against a real host.
* Not verified with real vSphere: the thin-disk `size` of `layoutEx` against the HEAD size, the `RenameSnapshot` call that carries the "[live]" mark, > 4 GiB files (the emulator tests draw small disks; the 1 GiB thin disk test is the largest).
* CBT / incremental backups are not used: every backup is a full copy (the stream is compressed).
* A snapshot of a VM with more than ~30 existing snapshots hits vSphere's own limit and fails with the server's message.


## Restore TO an ESXi host / vCenter (phase 2)

`RestoreToEsxi(target, archive, server, EsxiRestoreOptions, password, ReadOptions, progress, result, err)` in `esxi.h`; the dialog calls
`EsxiRestoreTargets(server, datacenter, json, err)` for the choices. Service commands: `esxi.restore_options`, `backups.restore_esxi` (see the report / `svccore.cpp`). The restore is started from the web site (Backup Explorer, Restore..., "Restore to: ESXi host or vCenter": server, Load, datacenter / datastore with free space / VM folder / resource pool / host, new VM name - `<name>-restored` when the name is taken -, power on, archive password) and follows the job in the running-jobs panel and the history (`restored_vm`, `vmx`, message). Both commands are admin-only over the web (operator and viewer get 403); `tools/run-esxi-tests.sh --only svcweb` covers it against the mock.

What it does:
1. `vm2backup-meta.json` (the first tar entry) is read: the backup must come from this source (otherwise "not made from an ESXi ..."). The name is the
   option `vmName`, else the original, else `<name>-restored`; a VM or datastore folder of that name that exists is **never overwritten** (the restore stops
   before it writes anything). The datastore is the original one (every file goes back where it came from, a second datastore's files too) or the one
   chosen (then everything goes into it). Folder, resource pool and host are resolved from the inventory; the default pool is the root pool of the first
   compute resource; the default folder is the datacenter's root VM folder.
2. Folders are made with `FileManager.MakeDirectory(createParentDirectories)`, files are uploaded with `PUT /folder/<path>?dcPath=..&dsName=..` (session
   cookie, `Expect: 100-continue`, Content-Length from the tar entry, the host's size is checked with a HEAD afterwards). The archive's files for the running
   point (.vmsn / .vmem of the VM2Backup snapshot) are not uploaded.
3. `.vmx` and `.vmsd` are buffered and written last, edited: the disk entries (`scsiX:Y.fileName`...) point at the base disks (`disks[].snapshot_descriptor` of
   the meta file; the original .vmx pointed at the running delta, which is not in the archive), `snapshot.*`, `*ctkEnabled*`, `sched.swap.derivedName` lines are
   removed, `displayName` is the new name, `uuid.action = "keep"` is set (no "moved or copied" question); the .vmsd loses the VM2Backup snapshot
   (a VM's own earlier snapshots stay in it). Then `Folder.RegisterVM_Task(path, name, pool, host)`; a registration whose answer got lost is found again by its
   path. `powerOn` runs `PowerOnVM_Task`; a failure there is a warning, not a failed restore.
4. After a final failure or a cancel the folders the restore created are deleted (`DeleteDatastoreFile_Task`) and the state is removed. A pause
   (`Progress::keepOnAbort`) or a connection that did not come back within the retries keeps the uploaded files and the state.

**Resuming (honest limit).** `RestoreWriter::canResumeInsideFile()` is false for HTTP PUT (it cannot append). The engine therefore keeps its state
(`ReadOptions::stateDir`, default the Windows temp folder; a file named by a hash of archive + root + server + options) only at the START of each file, and after
a lost connection or in a later run it re-reads the archive from that file's beginning and uploads the file again from byte 0. A failure in the middle of a
100 GB disk re-reads and re-uploads that disk. (A local folder resumes inside the file.) A resumed run needs the same server, options and archive.

**Verified only against the mock** (like everything else here): `FileManager.MakeDirectory` / `DeleteDatastoreFile_Task`, `Folder.RegisterVM_Task`, `PowerOnVM_Task`,
the property paths used for the inventory (`Datacenter.vmFolder/datastore`, `Datastore.summary.*`, `ComputeResource.resourcePool/host`, `ResourcePool.parent`,
`HostSystem.parent`, `Folder.childType/parent`), `PUT /folder` semantics on a real host (201/200 answers, the folder must exist, 507 / 403 meanings, whether the
datastore folder listing `GET /folder/<dir>` exists, which the code uses to detect a taken folder name), that ESXi / vCenter accept the edited .vmx without a
question, and VMs with several user snapshots (the mock registers a VM only with its disk chain, not with snapshot objects). Not implemented: overwriting an existing
VM, restoring as a template, vApp / cluster-DRS placement choices beyond folder + pool + host, VM encryption policies.
