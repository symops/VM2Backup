# VMware ESXi / vCenter as a source (VM2Backup)

Files: `src/xml.*` (XML parser/writer), `src/esxi.*` (the source, `NewEsxiSource`), `src/tests_esxi.cpp` (group `esxi`),
`tools/testservers/esxi_mock.py` + `esxi_launch.py` (the emulators), `tools/run-esxi-tests.sh`.

No VMware SDK: the vSphere Web Services API (SOAP at `/sdk`) over OpenSSL TLS (`tls.cpp`) + our own HTTP client, and
datastore file access over HTTPS (`/folder`). Supported: ESXi 6.0 / 6.5 / 6.7 / 7.0 / 8.0 / 9.0 and vCenter of the same versions.

## Server config (JSON, see `esxi.h`)

`type` esxi|vcenter, `host`, `port` (443), `user`, `password`, `verify`, `fingerprint` ("sha256:..", a self-signed host is pinned),
`tls_min` ("1.0" for ESXi 6.0 / 6.5 hosts that offer nothing newer, default "1.2"), `transport` "api" (default; "ssh" is **not needed and not
implemented**: the disks of a running VM are cloned by the host itself through the API, see "Powered-on VMs" below; `NewEsxiSource` refuses "ssh" with that text), `snapshot` {quiesce: true, memory: false} (the service copies it from the task element),
`datacenter` (vCenter: only this datacenter; without it every datacenter is searched and a name that exists twice is refused with a
message asking for it). Tuning keys: `timeout` (idle seconds of a network step), `task_timeout` (600 s, snapshot creation),
`remove_timeout` (3600 s, a consolidation can be long), `poll_ms` (first task polling interval, doubles up to 1.5 s),
`live_disk` ("clone" (default) | "direct": how the disks of a powered-on VM are read, see below), `clone_timeout` (21600 s, one disk clone).

## Powered-on VMs: disk clones (since the real ESXi 7.0.3 run)

A powered-on VM keeps its disk files locked: a real ESXi answers `GET /folder/...-flat.vmdk` with an empty HTTP 500 ("Failed to lock file" in `hostd.log`),
also for the base disk a snapshot has just frozen. So for a powered-on VM (`live_disk` = "clone", the default) the source does this after the snapshot:
1. for every disk the snapshot froze, `VirtualDiskManager.CopyVirtualDisk_Task` (the API twin of `vmkfstools -i <link> -d thin <clone>`, `sourceName` = the descriptor of the
   NEWEST frozen link of the chain, so the whole chain is read by the host) makes a thin, parent-free clone `d<N>.vmdk` + `d<N>-flat.vmdk` in a temporary folder
   `<VM folder>/VM2Backup-<10 hex>/` (the name of the run's snapshot; a disk in another folder or datastore: `[datastore] VM2Backup-<10 hex>/`);
2. `ready` (a tiny file) is written into every folder after the last clone: only then are the clones trusted by a continuation;
3. the clone's flat file is read through `/folder` (Range works: a continuation goes on INSIDE the disk), and its descriptor is made reproducible (`CID=fffffffe`, no
   `ddb.uuid` / `longContentID`, the extent line names the archive's flat file, `ddb.adapterType` of the original) and put into the archive under the BASE
   link's name (`<disk>.vmdk` + `<disk>-flat.vmdk`), the meta file says `live_clone: true`, `disks[].cloned: true`;
4. the folders are deleted before the snapshot is removed (`end()`, a failed `begin()`, a cancel); a pause keeps snapshot and folders for the continuation. A leftover
   VM2Backup snapshot of a dead run is removed together with its folder; a `VM2Backup-<10 hex>` folder without a snapshot (the snapshot was removed by hand)
   is found in the VM folder listing and deleted; anything else is never touched (`VM2Backup-before-upgrade` stays).
Consequences, said honestly: (a) the datastore needs room for a thin copy of the disks' data for the duration of the copy; the clone is made before the first byte
streams (the time of a read plus a write of the disk on the host); (b) the VM's OWN earlier snapshots are not copied for a powered-on VM (the chain is consolidated
into the clone; `snapshot.history` is treated as off, a warning says so), the restored VM has one flat disk of the state at the moment of the snapshot and no snapshots;
(c) a restored disk is thin even when the original was thick; (d) an independent disk or an RDM is not cloned (as before: copied live / skipped); (e) a host that allows
reading in place (older hosts?) can be told `"live_disk": "direct"`; (f) the license: `CopyVirtualDisk` is a write call, the free Hypervisor license refuses it (clear message).
Not implemented, by choice: NFC export (`ExportSnapshot` answers NotSupported on a standalone ESXi 7.0.3, `ExportVm` needs the VM powered off, see the facts below) and SSH
(`vmkfstools`): both need more (a stream-optimized VMDK converter; credentials of the shell) and give nothing the API clone does not.

## What a backup does

1. `begin()`: reads `RetrieveServiceContent` (SOAP version string: tries `urn:vim25/6.0`, then 6.5 ... 9.0 when the server answers with a fault /
   HTTP error to it; the XML namespace is always `urn:vim25`), logs in (`SessionManager.Login`, cookie `vmware_soap_session`; a
   `NotAuthenticated` fault or a 401 on `/folder` makes one transparent re-login), finds the VM (a `ContainerView` of the datacenter +
   `RetrievePropertiesEx`; falls back to `RetrieveProperties` when a server refuses the Ex call), refuses templates and inaccessible VMs.
2. Snapshot `VM2Backup-<10 hex>` (`CreateSnapshot_Task`, quiesce / memory per config, only for powered-on VMs). Task polling through the
   property collector. A quiesce failure (VMware Tools missing: `ToolsUnavailable`, `ApplicationQuiesceFault`, ...) retries once without
   quiescing and adds a warning. A refused write API (`RestrictedVersion`, "current license ... prohibits", `NoPermission`: free ESXi) is a
   permanent error that names a licensed host. A hung task is cancelled after `task_timeout`.
3. File list AFTER the snapshot (task option `snapshot.history`, default true: see "Snapshots of the VM itself" below), from `layoutEx`: the configuration files (`config` .vmx, `extendedConfig` .vmxf, `nvram`, `snapshotList`
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

**Verified only against the mock** (until the real ESXi 7.0.3 run, see "Проверка на настоящем ESXi 7" below; what that run confirmed is listed there; the rest of this paragraph still holds for ESXi 6.x / 8 / 9 and every vCenter): every XML shape, i.e. that real ESXi / vCenter answer exactly like the emulator
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

* No SSH transport (not needed: the disks of a powered-on VM are cloned by the host through the API, see "Powered-on VMs"). Restore TO a host is the API transport only (see Restore below).
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


## Проверка на настоящем ESXi 7

Первая проверка на НАСТОЯЩЕМ хосте (до этого был только эмулятор): VMware ESXi 7.0 Update 3n, сборка 21930508 (образ `VMware-VMvisor-Installer-7.0U3n-21930508.x86_64.iso`,
образ только читается), вложенная ВМ под KVM/qemu на Linux-хосте, режим evaluation (60 дней; лицензия не вводилась: для снимков и `/folder` её не требуется,
бесплатная лицензия Hypervisor блокирует запись через API, и источник сообщает об этом отдельным текстом). Дата: 01.11.2026.

### Как поднят стенд (воспроизводимо, без кликов)

* `tools/esxi7/vm.sh install <iso>`: qemu (`-machine q35 -cpu host,+vmx -smp 4 -m 8192`, диск ОС 60 ГБ qcow2 thin, второй диск 40 ГБ под VMFS, сетевая карта vmxnet3),
  из образа во временной папке делается КОПИЯ с kickstart (`ks.cfg` в модуле `KS.TGZ`, параметры загрузки `ks=file:///ks.cfg`; `ks=cdrom:/KS.CFG` и `ks=http://` на этом образе
  в qemu не сработали). Установка около 8 минут, ещё ~3 минуты до готовности `hostd`. В kickstart: `vmaccepteula`, `rootpw`, `install --firstdisk=local --overwritevmfs --novmfsondisk`,
  в `%firstboot` включены SSH и ESXi Shell. Пароль root случайный, лежит только в `.winvm/esxi/secret` (права 600, в `.gitignore`), нигде не печатается; временная копия образа с ним удаляется.
* `tools/esxi7/setup-guest.sh`: VMFS-6 датастор `datastore1` на втором диске и две ВМ без ОС (только vmx + vmdk): `vm2b-off` (выключена; диск 1 thin 512 МБ с ~100 МБ случайных данных кусками, диск 2 zeroedthick 64 МБ целиком случайный)
  и `vm2b-on` (включена; thin 256 МБ). Эталонные sha256 всех `-flat.vmdk` снимаются на хосте (`sha256sum`) ДО копии.
* `tools/esxi7/run-real.sh`: пишет `servers.json` с ключом `esxi_real` (адрес, пользователь, ПУТЬ к файлу пароля, отпечаток самоподписанного сертификата, эталонные суммы) и запускает группу `esxireal`
  (`src/tests_esxi_real.cpp`, `src/tests_svc_esxi.cpp`). Без ключа группа пропускается; эмуляторы не затронуты. `tools/esxi7/ssh.sh`: SSH на стенд (`sshpass -f`, пароль не в командной строке).
  `VM2B_ESXI_TRACE=<папка>` у любого процесса с источником ESXi записывает SOAP-обмен (пароль Login заменён) для снятия образцов: `tools/esxi7/record_fixtures.py` превращает его в `tests/fixtures/esxi7-real/`.

### Сеть стенда (резерв для тестового стенда ESXi)

Изолированная host-only сеть **10.213.77.0/24** (диапазон ЗАРЕЗЕРВИРОВАН под стенд ESXi, не использовать для другого): Linux-мост `vm2besxi0` с адресом хоста 10.213.77.1, tap `vm2besxi0t` для qemu
(`vnet_hdr=off`: с включённым tap не передавал кадры гостя), ESXi 10.213.77.10 (статический, шлюза нет), VCSA 10.213.77.11 (отдельная ВМ qemu, tap `vm2besxi1t` на том же мосту; раздел «vCenter 7.0.3 на настоящем VCSA»), .20-.99 свободны для служб продукта и тестовых серверов на хосте
(слушать на 10.213.77.1, не на 0.0.0.0). Нет `ip_forward`, masquerade, маршрутов в другие сети, DHCP; правил iptables/nftables стенд не добавляет. Снос: `tools/esxi7/vcsa.sh delete` (tap `vm2besxi1t`), `tools/esxi7/vm.sh net-down` (`ip link del vm2besxi0t; ip link del vm2besxi0`).
Первая попытка была на qemu user-net (10.0.2.0/24); в ней TCP с гостя не проходил, пока на vmnic0 не выключены checksum/TSO offload (`esxcli network nic cso set -n vmnic0 -e 0`, закреплено в `/etc/rc.local.d/local.sh`; на мосту оставлено).

### Что проверено на настоящем ESXi 7.0.3 (все тесты группы `esxireal`)

* `esxireal_connect_and_list`: вход, `RetrieveServiceContent` (api 7.0.3.0), список ВМ (состояния stopped / running, размеры). Отпечаток самоподписанного сертификата закреплён.
* `esxireal_wrong_credentials`: неверный пароль и неверный пользователь дают `login failed` (не временная ошибка, пароль в тексте не повторяется), чужой отпечаток даёт отказ с показом настоящего. Не более двух неверных попыток за прогон
  (у ESXi свой счётчик блокировки root).
* `esxireal_backup_verify_restore_matrix`: бэкап выключенной ВМ (снимок, 600 МБ двух дисков по Datastore HTTP, снимок удалён) в четырёх режимах: без пароля / xz 1, пароль + xz 5 + тома по ~100 МБ, xz 9 + тома по 2 МБ, пароль + без сжатия;
  verify, неверный пароль архива отвергнут, восстановление в папку: **sha256 обоих flat-дисков совпал побитово во всех режимах**.
* `esxireal_resume_after_abort`: пауза с сохранением томов в середине копии, затем продолжение (`resumes >= 1`), результат побитово равен исходному.
* `esxireal_restore_to_host`: восстановление на сам ESXi (PUT в `/folder`, `RegisterVM_Task`), ВМ видна в списке и включается (проверено `power.on`), повтор с занятым именем отказан без перезаписи; **sha256 flat-дисков на хосте совпал с эталоном**.
* `esxireal_service_flow_and_lockout_guard`: тот же путь через службу: `servers.save` / `servers.test`, неверный пароль помечает сервер (`auth_blocked`, плановые запуски его больше не трогают; ручной запуск делает одну попытку), исправление пароля снимает метку,
  задача с шифрованием, verify, restore в папку (sha256 совпал), `esxi.restore_options`, `backups.restore_esxi`; пароли не найдены ни в ответах, ни в данных службы.

### Расхождения «эмулятор и настоящий хост» и что сделано

1. **SOAPAction определяет набор свойств.** Источник искал версию с «6.0» (первую принятую), и настоящий ESXi 7.0.3 отвечал HTTP 500 `InvalidProperty config.keyId` (свойство 6.5+): список ВМ не работал вообще.
   Исправлено в `negotiate()` (`src/esxi.cpp`): после `RetrieveServiceContent` берётся самая новая известная версия не выше `apiVersion` сервера; `config.keyId` запрашивается только при версии запроса 6.5+.
   Эмулятор (`esxi_mock.py`) теперь так же отвечает `InvalidProperty` на `config.keyId` при SOAPAction ниже 6.5 у любого профиля: регрессия в существующих тестах эмуляторов. Образец настоящего ответа: `tests/fixtures/esxi7-real/RetrievePropertiesEx-fault-InvalidProperty-keyId.resp.xml`.
2. **Диск ВКЛЮЧЁННОЙ ВМ по HTTP не читается, даже после снимка** (найдено первым прогоном, РЕШЕНО вторым: раздел «Включённая ВМ: исследование и выбранный транспорт»). `GET /folder/.../vm2b-on-flat.vmdk` отвечает пустым HTTP 500
   (в `hostd.log`: «Error creating Reader: Failed to lock file», в `vmkernel.log`: «Failed to open file ... Busy»). Эмулятор теперь воспроизводит это (`live_locked()` в `esxi_mock.py`), продукт читает такие диски из клона.
3. **Список папок восстановления** содержал корневую папку ВМ дважды (настоящий хост отдаёт `ha-folder-vm` среди найденных, эмулятор нет). Исправлено (`EsxiRestoreTargets`).
4. Прочие формы ответов совпали с эмулятором без правок: пространство имён `urn:vim25`, `xsi:type`, `layoutEx` с цепочками (7.0: `-sesparse.vmdk`, дескриптор снимка `-000001.vmdk`), задачи через `PropertyCollector`, `RenameSnapshot` с меткой `[live ...]`,
   `MakeDirectory` / `RegisterVM_Task`, Range и HEAD на `/folder`, TLS 1.2, самоподписанный сертификат с закреплением отпечатка, `Content-Range` в ответе 500. Запись образцов: `tests/fixtures/esxi7-real/` (тест `esxi_real_fixtures_parse` разбирает их тем же XML-кодом).

### Наблюдения и ограничения

* Восстановленный на хост thin-диск занимает на VMFS весь размер (512 МБ против 85 МБ у исходного): PUT пишет нули, `/folder` не умеет разреженные записи. Содержимое побитово то же.
* Эталонное время гостевого ESXi в этом стенде 2001 год (часы qemu); на работу не влияет.
* Не проверено на настоящем хосте: 8.0 / 9.0, шифрованные ВМ, RDM, vSAN, NFS-датасторы, файлы больше 4 ГиБ, quiesce при работающих VMware Tools, лицензия Hypervisor (free).
* vCenter 7.0.3: проверен отдельно, раздел «vCenter 7.0.3 на настоящем VCSA» ниже.

### Что нашла проверка на настоящем хосте (дополнение): порядок цепочки диска

6. **Порядок звеньев `layoutEx.snapshot[].disk[].chain[]`.** Настоящий ESXi 7.0.3 перечисляет звенья от СТАРШЕГО (база) к новому, эмулятор (6.x) отдавал от нового к старому. Источник брал `chain[0]` как «диск текущей точки», и у ВМ с собственными снимками
   восстановленный .vmx указывал на БАЗОВЫЙ диск (потеря всего, что лежит в дельтах), хотя файлы копировались полностью. Исправлено (`TopLink` в `src/esxi.cpp`: новое звено определяется по типу экстента, base = не `-delta` / `-sesparse`); эмулятор профилей 7.0+ теперь
   тоже отдаёт цепочки от старого к новому. На настоящем хосте: .vmx восстановленной ВМ указывает на `vm2b-hist-000002.vmdk`, `vmkfstools -e` даёт «Disk chain is consistent», ВМ включается, sha256 базы совпал.

## Снимки самой ВМ: опция `snapshot.history`

Ключ элемента задачи `snapshot` (рядом с `quiesce` и `memory`): `history` (bool), **по умолчанию true, в том числе для старых сохранённых задач без ключа**: поведение не меняется, копируется всё, что описано выше
(цепочки дисков каждого снимка ВМ, `.vmsn` / `.vmem`, `.vmsd`). Сайт: флажок «Also back up the existing snapshots of the VM» в блоке Snapshot (17 языков). `tasks.save` приводит значение к bool и отвергает не-логическое.

`history = false` отключает: (1) `.vmsn` / `.vmem` (состояние и память) ВСЕХ чужих снимков; (2) дисковые файлы веток дерева снимков, не входящих в цепочку текущего состояния (боковые ветки); (3) записи снимков в `.vmsd` при восстановлении.
**Остаётся и копируется всегда:** диски-предки текущего состояния (текущий диск есть дельта поверх цепочки родителей, без родителей он не читается). Поэтому вес копии ВМ с линейной историей без памяти почти не уменьшается; уменьшается у ВМ со снимками
с памятью и с ветвлением. В мета-файле: `snapshot_history: false`, `snapshots_not_copied: N`. Восстановление такой копии: .vmx указывает на диск текущей точки (родители по дескрипторам), `.vmsd` пустой, предупреждение
«the VM was backed up without its snapshots: its disks keep the parent chain, the snapshot tree is not restored» (в vSphere может понадобиться «Consolidate»). Для ВМ без снимков и для выключенных ВМ без снимков опция ничего не меняет. У ВКЛЮЧЁННОЙ ВМ опция всегда как «выключена» (цепочка свёрнута в клон, предупреждение в отчёте): раздел «Включённая ВМ».
Не делается (возможное развитие): консолидация на лету (CloneVM / экспорт плоского диска без цепочки родителей) для получения самостоятельного диска; это отдельная дорогая функция.

Проверено: эмулятор (`esxi_snapshot_history_option`: дерево s1 -> s2, s1 -> s3 со снимками с памятью; с историей 27 файлов, 4 `.vmsn`, 3 `.vmem`; без истории 17 файлов, один `.vmsn`, без `.vmem`; диски текущего состояния побитово те же;
восстановление на хост: ВМ регистрируется, снимков 0, предупреждение есть). Настоящий ESXi 7.0.3 (`esxireal_snapshot_history`, ВМ `vm2b-hist` с деревом s1 -> s2 и s1 -> s3): с историей 14 файлов (4 `.vmsn`, три дельты 000001..000003),
без истории 9 файлов (один `.vmsn`, две дельты: только предки текущей точки; ветка s2 не скопирована); verify, восстановление в папку и на хост, ВМ включается, цепочка согласована, sha256 базового диска совпал. С историей восстановленная ВМ
имеет все 3 своих снимка и тоже включается.

## Включённая ВМ: исследование и выбранный транспорт (настоящий ESXi 7.0.3, 02.11.2026)

Исследовано на `vm2b-on` (включена, диск thin 256 МБ с ~50 МБ случайных данных, эталон sha256 снят ДО снимка). После снимка:

| Путь | Результат на настоящем хосте |
|---|---|
| (a) чтение замороженной БАЗЫ в оболочке: `dd`, `cp`, `cat`, `sha256sum` по SSH (то же самое, что дали бы SCP/SFTP: файл один) | **нет**: «Device or resource busy». Не зависит от режима снимка: без памяти и с памятью (`.vmem`) одинаково. Файл, который держит процесс ВМ, читает только средство самого хоста |
| `GET /folder/...-flat.vmdk` базы или дельты (Datastore HTTP) | **нет**: пустой HTTP 500, `hostd.log`: «Failed to lock file» (имя файла в этом сообщении — именно БАЗА, замороженная снимком, а не «живая» точка: живая точка лежит в `-sesparse.vmdk`) |
| (b) `vmkfstools -i <база.vmdk> -d thin <папка>/c.vmdk` (метод ghettoVCB) | **да**: 9 → 100 %, sha256 клона **= эталону**. Нужен SSH на хосте |
| (b') то же самое через API: `VirtualDiskManager.CopyVirtualDisk_Task(sourceName, destName, destSpec thin)` | **да**: тот же результат, sha256 клона = эталону; клон отдаётся `GET /folder` (200, а с `Range` 206 `Content-Range: bytes 1000-2999/268435456`); SSH не нужен, лицензия — evaluation |
| (c) NFC: `VirtualMachineSnapshot.ExportSnapshot` | **нет**: `NotSupported` на самостоятельном хосте 7.0.3 (метод vCenter) |
| `VirtualMachine.ExportVm` у включённой ВМ | **нет**: `InvalidState` (нужна выключенная ВМ) |
| `AcquireTicket` | только билет консоли (mks / vmrc), к диску не ведёт |
| (d) другое: чтение дельты «живой» точки, `-ctk.vmdk` | живая точка меняется (это не копия), `-ctk` читается, но это не диск |

**Выбор: (b') клон средствами API** (`live_disk` = "clone", по умолчанию). Причины: побитно согласованная копия замороженного состояния (sha256 клона = эталон до снимка); работает по уже поддерживаемому транспорту (SOAP + `/folder`), без SSH, без VDDK/NFC, без лицензий кроме evaluation; клон читается по `Range`, поэтому продолжение идёт ВНУТРИ диска; остатки (`VM2Backup-<10 hex>` папка клонов) убираются теми же путями, что и остатки снимков (метка и имя, чужое не трогается). SSH в настройки сервера поэтому НЕ добавлялся (полей, интерфейса и перевода на 17 языков нет: не нужны). Не выбраны: (a) невозможно; NFC (`ExportSnapshot` недоступен на хосте, а при выгрузке через vCenter получился бы stream-optimized VMDK, для которого нужно писать преобразователь «поток в flat» и обратный при восстановлении: заметно больше кода и риска без выигрыша).

Что проверено на настоящем хосте (группа `esxireal`, тесты те же, что для выключенных ВМ, плюс новые):
* `esxireal_backup_verify_restore_matrix`: **включённая** `vm2b-on` в четырёх режимах (без пароля xz 1; пароль + xz 5 + тома ~100 МБ; xz 9 + тома 2 МБ; пароль + без сжатия): снимок, клон (`CopyVirtualDisk`), чтение клона, удаление клона и снимка, verify, неверный пароль архива отвергнут, восстановление в папку: **sha256 `vm2b-on-flat.vmdk` совпал побитно во всех четырёх режимах** (268 435 456 байт, в архиве 43 МБ). После копии на хосте нет ни одного остатка (`VM2Backup-*` в корне датастора и в папках ВМ: 0, снимков `VM2Backup-*`: 0; следующий `begin()` не находит старых снимков).
* `esxireal_resume_after_abort` и `esxireal_restore_to_host` теперь идут и для включённой ВМ (восстановление на хост: sha256 flat-диска на самом хосте совпал с эталоном).
* `esxireal_live_hist_running`: включённая `vm2b-hist` со СВОИМ деревом снимков (s1 -> s2, s1 -> s3): предупреждение «the VM is running: its disks are copied as one consolidated clone each, the 3 earlier snapshot(s) of the VM are not copied»; в копии один flat (128 МБ) + один дескриптор, ни одной дельты, sha256 совпал с эталоном; восстановление на хост.
* Согласованность при записях: у ВМ без ОС писать некому (процесс ВМ только крутит BIOS), «живой» гостевой записи нет, а менять диск включённой ВМ извне нельзя (файлы заперты). Проверено поэтому то, что можно проверить на настоящем хосте: замороженная база не меняется за время копии и побитно равна эталону, снятому ДО снимка; запись в гостя при копии (дельта) воспроизведена только на эмуляторе (`esxi_running_vm_disks_are_cloned`, `esxi_guest_writes_are_frozen_out`: ВМ пишет во время копии, копия остаётся состоянием снимка). Образ-гость (tinycore / alpine) в образах пользователя отсутствует, сети для загрузки из интернета в стенде нет (host-only).

### Сценарий «ВМ включена → копия начата → ВМ выключили → связь пропала → ВМ включили → следующий запуск продолжает» на рабочем транспорте

`tools/esxi7/live-scenario.sh` (`vm2b-on`; тома по ~1 МБ, пауза на 40 МБ данных диска):
1. Копия начата и поставлена на паузу с сохранением (`keepOnAbort`): **40 томов** готовы, возобновляемая копия 1. На хосте: снимок `VM2Backup-<10 hex>` (`consolidationNeeded = false`) и папка клонов `vm2b-on/VM2Backup-<10 hex>/` с файлами `d0.vmdk`, `d0-flat.vmdk`, `ready`.
2. ВМ выключена `power.off` при существующем снимке: проходит, снимок и папка клонов остаются.
3. ВМ включена снова: снимок и клон те же.
4. Следующий запуск **продолжает**: источник присоединяется к существующему снимку (новый не создаётся), клон признаётся годным по файлу `ready` (повторного клонирования нет), отпечаток тот же (`resumes = 1`, 40 готовых томов не копируются заново), копия идёт дальше с места остановки ВНУТРИ flat-диска запросом `Range`. Копия закончена, `end()` удаляет папку клонов, потом снимок: на хосте остаётся только `vm2b-on-flat.vmdk vm2b-on.vmdk vm2b-on.vmsd`.
5. Явная отмена (прерывание без `keepOnAbort`) удаляет снимок, папку клонов и журнал: возобновляемых копий 0, снимков на ВМ нет.
Если ВМ к моменту продолжения выключена (а не включена): источник читает диски на месте (выключенная ВМ не держит замок), набор файлов другой, отпечаток другой, движок начинает копию заново с новым снимком; папка клонов прерванного запуска при этом удаляется.
Пределы продолжения: внутри диска продолжение возможно, пока жив снимок и папка клонов с `ready`; если снимок убрали руками, продолжения нет (снимок и клоны исчезают как остатки при следующей копии, копия начинается заново). Клон делается ДО передачи первого байта диска: время чтения и записи диска хостом (на этом стенде 256 МБ: секунды; на реальных ВМ — минуты и часы, `clone_timeout`), и нужно место на датасторе под тонкий клон; ограничение скорости, пауза и отмена работают на этапе передачи, отмена во время клонирования отменяет задачу на хосте (`CancelTask`), папка удаляется.

## vCenter 7.0.3 на настоящем VCSA (02.11.2026)

Устройство `VMware-VCSA-all-7.0.3-24927011.iso` (vCenter Server Appliance 7.0.3.02500, build 24927011, режим evaluation, лицензия не вводилась) поднято ОТДЕЛЬНОЙ ВМ qemu/KVM (не внутри вложенного ESXi: размер «tiny» просит 12 ГБ ОЗУ), в той же изолированной сети: ESXi 10.213.77.10 (уменьшен до 4 ГБ ОЗУ / 2 vCPU: `ESXI_MEM=4096 ESXI_SMP=2 tools/esxi7/vm.sh start`), VCSA 10.213.77.11 (2 vCPU, 12 288 МБ, tap `vm2besxi1t` на том же мосту). Памяти хватило с запасом (занято 26 из 32 ГБ, доступно 5,8-6,2 ГБ по `free`, чужие процессы не трогались).

### Рецепт (воспроизводимо, без кликов; пароли случайные, только в файлах `.winvm/esxi/secret-vc-sso` и `secret-vc-root`, права 600, в `.gitignore`)
1. `tools/esxi7/vcsa.sh prepare <ISO>`: из образа (только чтение) достаётся OVA, диски `system` и `cloudcomponents` переводятся в qcow2 (`qemu-img convert` читает stream-optimized VMDK), добавляются 15 пустых тонких дисков размеров OVF «tiny» (25, 25, 10, 10, 15, 10, 1, 10, 10, 100, 50, 10, 5, 100, 150 ГБ: первая загрузка находит их по размеру), три контроллера LSI (в ядре устройства встроены; virtio и pvscsi там нет), сетевая карта vmxnet3.
2. **Параметры первой загрузки.** Под qemu нет VMware Tools, а `/bin/install-parameter` (читает `guestinfo.cis.*`) знает только три источника: `/etc/vmware/install-defaults/<ключ>`, `settings.json` и `vmtoolsd --cmd "info-get guestinfo.ovfEnv"` (канал VMware-гипервизора, которого в qemu нет; скрипт `/opt/vmware/bin/ovfenv` умеет читать ovf-env.xml с компакт-диска, но `install-parameter` им не пользуется: первая попытка с ISO оставила устройство на экране «Root password is not set»). Рабочий путь: до первой загрузки в диск устройства (qcow2, копия; образ и OVA не меняются) кладутся файлы `/etc/vmware/install-defaults/{appliance.net.addr, .mode, .prefix, .gateway, .dns.servers, .pnid, appliance.root.passwd, vmdir.password, vmdir.domain-name, vmdir.first-instance, db.type, deployment.node.type, deployment.autoconfig = True, ...}` (`tools/esxi7/vcsa-ovfenv.py --seed`). Со значением `deployment.autoconfig = True` устройство само проходит оба этапа: установка RPM (около 10 минут, этап 1) и `firstboot` (около 30 минут, этап 2, 49 шагов); REST API этапов развёртывания (`/rest/vcenter/deployment/install/initial-config`) не понадобился.
3. `tools/esxi7/vcsa.sh start`; доступность: ping .11 через ~3 минуты, затем `https://10.213.77.11:5480` (VAMI), прогресс шагов `ssh root@10.213.77.11 'cat /var/log/firstboot/firstbootStatus.json'` (`tools/esxi7/vcsa-ssh.sh`, пароль через `sshpass -f`).
4. `tools/esxi7/vc-setup.py`: датацентр `vm2b-dc` и хост 10.213.77.10 в нём (SOAP `CreateDatacenter`, `AddStandaloneHost_Task` с отпечатком SHA-1 сертификата хоста). Ограничение стенда, найденное здесь: **время на ESXi должно совпадать со временем vCenter** (часы гостевого ESXi стояли на 2001 год: «Unable to push signed certificate to host»); перед добавлением `esxcli system time set` и перезапуск `hostd`.
5. `tools/esxi7/run-real.sh` сам добавляет в `servers.json` ключ `vcenter_real` (если VCSA отвечает по 443); те же тела тестов идут группой `vcreal`.

Этап 2 завершился ОШИБКОЙ одного из последних шагов (`hvc_firstboot`, служба Hybrid Linked Mode: 503 от `invsvc` при регистрации в Lookup Service в медленной среде), поэтому остальные 15 шагов не выполнялись (в том числе `vsan-health`, `perfcharts`, `content-library`, `updatemgr`). Работают: vpxd, SSO (`administrator@vsphere.local`), Lookup Service, vsphere-ui, certificate, eam, analytics, vapi (`service-control --status`); этого достаточно для источника резервных копий (SOAP `/sdk`, `/folder`), подробности ниже.

### Что проверено на настоящем vCenter 7.0.3 (группа `vcreal`, 6 из 6)
Тот же код продукта, что для хоста, без единой правки по результатам: сервер `type: vcenter`, `apiType VirtualCenter`, `apiVersion 7.0.3.0`, `rootFolder group-d1`, `virtualDiskManager` называется `virtualDiskManager` (на хосте `ha-vdiskmanager`; эмулятор приведён к этому).
* `vcreal_connect_and_list`: вход, `RetrieveServiceContent`, список ВМ с состояниями и размерами; `vcreal_wrong_credentials`: неверный пароль и неверный пользователь дают `login failed` (не временная ошибка, пароль в тексте не повторяется), чужой отпечаток даёт отказ с показом настоящего.
* `vcreal_backup_verify_restore_matrix`: бэкап через vCenter выключенных `vm2b-off`, `vm2b-hist` и ВКЛЮЧЁННОЙ `vm2b-on` (клон диска через vCenter: `CopyVirtualDisk_Task` с `sourceDatacenter` / `destDatacenter`, чтение клона `GET /folder?dcPath=vm2b-dc&dsName=datastore1` через vCenter) в двух режимах; verify, неверный пароль архива отвергнут, восстановление в папку: sha256 всех flat-дисков совпал побитно. Остатков (`VM2Backup-*`, снимков) на хосте нет.
* `vcreal_resume_after_abort`: продолжение после паузы с сохранением; `vcreal_restore_to_host`: восстановление ТРЁХ ВМ (в том числе включённой) в vCenter (`Folder.RegisterVM_Task`, датацентр `vm2b-dc`); повтор с занятым именем отказан без перезаписи; **sha256 flat-дисков на хосте совпал с эталоном у всех восстановленных** (7 файлов).
* `vcreal_snapshot_history`: дерево снимков через vCenter с историей и без; восстановление в vCenter.
* `vcreal_service_flow_and_lockout_guard`: тот же путь через службу: `servers.save` / `servers.test` (4 ВМ найдено), неверный пароль помечает сервер (`auth_blocked`, плановые запуски его не трогают, ручной запуск делает одну попытку; у SSO своя блокировка учётной записи, прогон делает не больше двух неверных попыток подряд), исправление пароля снимает метку, задача с шифрованием, verify, restore в папку (sha256 совпал), `esxi.restore_options` (`kind: vcenter`, датацентр `vm2b-dc`, `vm_folder group-v4`, датасторы, папки, пулы, хосты), `backups.restore_esxi` в vCenter; пароли не найдены ни в ответах, ни в данных службы.
* Образцы настоящих ответов vCenter: `tests/fixtures/vcenter7-real/` (тест `esxi_real_fixtures_parse` разбирает их тем же XML-кодом).

### Ограничения и что НЕ проверено
Один хост и один датацентр (выбор датацентра / папки / пула / хоста на нескольких проверен только на эмуляторе: на настоящем списке по одному варианту), нет кластеров DRS / vSAN, вариантов с vCenter 8.0 / 9.0, шифрованных ВМ, VMware Tools quiesce, лицензии vCenter (серийный номер не вводился: для источника и восстановления evaluation хватило), служб после `hvc_firstboot` (см. выше). Стенд во вложенной виртуализации медленный: этап 2 длится около 30 минут, xz-сжатие 9 заметно тормозит.

### Расхождения «эмулятор и настоящий хост» этого прогона и что сделано
1. **Замок диска включённой ВМ** (главное): эмулятор отдавал такие диски, настоящий хост нет. Эмулятор теперь отвечает пустым HTTP 500 на `GET` диска (`-flat` / дельта / `-sesparse`) любой включённой ВМ (`live_locked()`; ошибка `no_live_lock` возвращает старое поведение), понимает `CopyVirtualDisk_Task` (клон = состояние диска на нужном звене цепочки, дескриптор в виде настоящего ответа хоста) и называет `virtualDiskManager` как настоящий (`ha-vdiskmanager` на хосте, `virtualDiskManager` у vCenter). Новый тест `esxi_running_vm_disks_are_cloned` падает на старом коде (чтение «на месте» даёт 500), в нём же: клон на каждый диск, отсутствие чтения запертого файла, `live_disk: direct` отказывает понятным текстом, отказ клонирования без остатков, продолжение с сохранённой папкой клонов, чистка остатков умершего запуска и «осиротевшей» папки (чужая `VM2Backup-before-upgrade` остаётся), ВМ со своим снимком.
2. Дескриптор диска в копии включённой ВМ новый (`CID=fffffffe`, без `ddb.uuid`), поэтому проверки «побайтно равно оригиналу» у включённых ВМ сравнивают размер и flat-файл, а дескриптор признаётся копией по размеру в секторах (`CloneDescriptorOf`). Тесты, которые проверяют сами цепочки снимков, ветвления дерева и исчезающие файлы цепочки, идут с ВЫКЛЮЧЕННЫМИ ВМ (цепочка включённой ВМ сворачивается в клон, это проверяет отдельный тест).
3. Малые файлы клона (дескриптор, листинг папки) читаются с проверкой: HTML вместо файла и ответ короче `Content-Length` — временная ошибка (нашёл `esxi_hostile_http_answers`: дескриптор в 6 байт после обрыва ответа); дескриптор клона обязан начинаться с `# Disk DescriptorFile` и содержать строку `RW `.
4. Найдено попутно (не связано с ESXi): тест `xz_encrypted_tiny_volumes_survive_connection_loss` обрывал связь внутри первого тома (при некоторых зёрнах `resumes == 0`); теперь обрыв не раньше двух томов.
5. Стенд: первый запуск ESXi с DHCP остаётся без адреса (в kickstart теперь статический 10.213.77.10), у `pvscsi` во вложенном ESXi нет слота PCIe без `pciBridge*` в `.vmx` (`setup-guest.sh` добавляет), часы вложенного ESXi (2001 год) мешают добавлению хоста в vCenter.
