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
(`vnet_hdr=off`: с включённым tap не передавал кадры гостя), ESXi 10.213.77.10 (статический, шлюза нет), VCSA (этап 2, не развёрнут) 10.213.77.11, .20-.99 свободны для служб продукта и тестовых серверов на хосте
(слушать на 10.213.77.1, не на 0.0.0.0). Нет `ip_forward`, masquerade, маршрутов в другие сети, DHCP; правил iptables/nftables стенд не добавляет. Снос: `tools/esxi7/vm.sh net-down` (`ip link del vm2besxi0t; ip link del vm2besxi0`).
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
2. **Диск ВКЛЮЧЁННОЙ ВМ по HTTP не читается, даже после снимка.** `GET /folder/.../vm2b-on-flat.vmdk` отвечает пустым HTTP 500 (в `hostd.log`: «Error creating Reader: Failed to lock file»,
   в `vmkernel.log`: «Failed to open file ... Busy», файл открыт процессом ВМ), а `.vmdk` (дескриптор) читается. `dd` и `cp` на хосте тоже получают «Device or resource busy»; читают такой диск только VDDK / NFC и `vmkfstools -i`.
   Копия ВЫКЛЮЧЕННОЙ ВМ со снимком работает. Что сделано: ошибка теперь говорит причину (диск удерживается запущенной ВМ, выключить ВМ на время копии), снимок после неудачи не остаётся (проверено тестом).
   **Не исправлено:** резервное копирование включённой ВМ по HTTP на ESXi 7.0.3 невозможно; нужен транспорт NFC (`VirtualMachineSnapshot.ExportSnapshot` / `HttpNfcLease`, поток stream-optimized VMDK с обратным преобразованием при восстановлении) или SSH (`vmkfstools`). Оба не реализованы.
   Эмулятор по-прежнему отдаёт такие диски (его тесты с включёнными ВМ остаются, но не отражают этого поведения настоящего хоста).
3. **Список папок восстановления** содержал корневую папку ВМ дважды (настоящий хост отдаёт `ha-folder-vm` среди найденных, эмулятор нет). Исправлено (`EsxiRestoreTargets`).
4. Прочие формы ответов совпали с эмулятором без правок: пространство имён `urn:vim25`, `xsi:type`, `layoutEx` с цепочками (7.0: `-sesparse.vmdk`, дескриптор снимка `-000001.vmdk`), задачи через `PropertyCollector`, `RenameSnapshot` с меткой `[live ...]`,
   `MakeDirectory` / `RegisterVM_Task`, Range и HEAD на `/folder`, TLS 1.2, самоподписанный сертификат с закреплением отпечатка, `Content-Range` в ответе 500. Запись образцов: `tests/fixtures/esxi7-real/` (тест `esxi_real_fixtures_parse` разбирает их тем же XML-кодом).

### Наблюдения и ограничения

* Восстановленный на хост thin-диск занимает на VMFS весь размер (512 МБ против 85 МБ у исходного): PUT пишет нули, `/folder` не умеет разреженные записи. Содержимое побитово то же.
* Эталонное время гостевого ESXi в этом стенде 2001 год (часы qemu); на работу не влияет.
* Не проверено на настоящем хосте: vCenter (этап 2 не выполнен, см. ниже), 8.0 / 9.0, шифрованные ВМ, RDM, vSAN, NFS-датасторы, файлы больше 4 ГиБ, quiesce при работающих VMware Tools, лицензия Hypervisor (free).
* Этап 2 (VCSA 7.0.3): не развёрнут. Установщик VCSA разворачивает OVA на хост ESXi с 12 ГБ ОЗУ для ВМ tiny (внутри вложенного ESXi на 8 ГБ это невозможно, трёхуровневая вложенность на 31 ГБ не нужна и не поместится),
  а запуск дисков OVA напрямую под qemu без служебных свойств OVF (`guestinfo.ovfEnv` из VMware Tools) оставляет устройство на первом этапе настройки. Нужно: ~16 ГБ свободной ОЗУ для отдельной ВМ VCSA и отдельный
  ESXi-хост (или второй вложенный ESXi на 4–6 ГБ), либо физический хост ESXi; адрес 10.213.77.11 для VCSA зарезервирован.

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
«the VM was backed up without its snapshots: its disks keep the parent chain, the snapshot tree is not restored» (в vSphere может понадобиться «Consolidate»). Для ВМ без снимков и для выключенных ВМ без снимков опция ничего не меняет.
Не делается (возможное развитие): консолидация на лету (CloneVM / экспорт плоского диска без цепочки родителей) для получения самостоятельного диска; это отдельная дорогая функция.

Проверено: эмулятор (`esxi_snapshot_history_option`: дерево s1 -> s2, s1 -> s3 со снимками с памятью; с историей 27 файлов, 4 `.vmsn`, 3 `.vmem`; без истории 17 файлов, один `.vmsn`, без `.vmem`; диски текущего состояния побитово те же;
восстановление на хост: ВМ регистрируется, снимков 0, предупреждение есть). Настоящий ESXi 7.0.3 (`esxireal_snapshot_history`, ВМ `vm2b-hist` с деревом s1 -> s2 и s1 -> s3): с историей 14 файлов (4 `.vmsn`, три дельты 000001..000003),
без истории 9 файлов (один `.vmsn`, две дельты: только предки текущей точки; ветка s2 не скопирована); verify, восстановление в папку и на хост, ВМ включается, цепочка согласована, sha256 базового диска совпал. С историей восстановленная ВМ
имеет все 3 своих снимка и тоже включается.

## Включённая ВМ: пауза, выключение, включение, продолжение, отмена (факты на настоящем ESXi 7.0.3)

Сценарий `tools/esxi7/live-scenario.sh` (результат: `.winvm-results/esxi7-<дата>/live-scenario.txt`), ВМ `vm2b-on` без ОС. Тело диска включённой ВМ не читается (см. расхождение 2), поэтому проверены те части, что не зависят от чтения диска:
1. Копия начата и поставлена на паузу с сохранением (`keepOnAbort`): три тома конфигурационных файлов готовы, возобновляемая копия 1, на хосте остаётся снимок `VM2Backup-<10 hex>` (`consolidationNeeded = false`).
2. ВМ выключена `power.off` при существующем снимке: выключение проходит, снимок остаётся.
3. ВМ включена снова: снимок остаётся (тот же).
4. Следующий запуск продолжает по тому же токену `<ссылка снимка>|<имя>`: источник заново присоединяется к существующему снимку (новый не создаётся; `resumes = 1`, три готовых тома не копируются заново), затем упирается в блокировку диска (HTTP 500)
   и при этой окончательной ошибке снимок удаляется (`end(false)`), дельта сворачивается, `consolidationNeeded = false`.
5. Явная отмена (прерывание без `keepOnAbort`) удаляет снимок и журнал: возобновляемых копий 0, снимков на ВМ нет.
Не проверено на настоящем хосте (нужно чтение диска включённой ВМ): продолжение внутри диска после выключения/включения ВМ, пока диск прочитать нельзя.
