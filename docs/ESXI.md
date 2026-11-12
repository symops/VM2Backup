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

## Холодный (по умолчанию) и горячий бэкап

Что хочет пользователь: «если машина была включена, бэкап начат, машину выключили, связь пропала — машина не включится, пока бэкап не доделается или его явно не отменят». Это **холодный режим, он включён по умолчанию**. Горячий режим (прежний: снимок VM2Backup, клоны дисков через `CopyVirtualDisk`, `quiesce` / `memory`, `live_disk`) включается флажком задачи
«Back up while the VM is running (hot backup)» (`snapshot.hot_backup = true`); под флажком стоит предупреждение (на английском, 16 переводов): *Hot backup: the VM keeps running during the copy. The copy is crash-consistent (as after a power failure) unless VMware Tools can quiesce the guest; databases and mail servers may need recovery after a restore.
The VM runs slower while the snapshot exists, the snapshot grows on the datastore, and nothing stops other administrators from changing, powering off or deleting the VM meanwhile. Without this option the VM is shut down and locked for the whole backup, and started again afterwards.*
Для источника «папка с ВМ» опция не применяется. Отсутствие ключа `hot_backup` везде = холодный режим (миграции нет: продукт не выпущен).

### Схема холодной копии одной ВМ

1. Исходное состояние питания (`poweredOn` / `poweredOff` / `suspended`) запоминается в **идентификаторе удержания** (hold) `VM2Backup-<10 hex>-<on|off|sus>-<unix-время начала>` и в состоянии возобновления (токен `cold|<hold>/<время выключения>|<состояние>`).
2. Удержание ставится в два слоя (причина — факты настоящего хоста ниже): **метка** (всегда работает: ключ `vm2backup.hold` в `extraConfig` ВМ = идентификатор удержания и видимая строка `[VM2Backup hold ...: this VM is shut down for a backup, do not power it on or change it]` в аннотации ВМ,
   `ReconfigVM_Task`) и **блокировка** (`AuthorizationManager.DisableMethods(entity = ВМ, methods, sourceId = идентификатор удержания)`, где сервер её поддерживает, то есть на vCenter). Заблокированы: PowerOn, Reset, Suspend, RebootGuest, StandbyGuest, ReconfigVM, CreateSnapshot, RemoveAllSnapshots, RevertToCurrentSnapshot,
   Relocate, Migrate, CloneVM, MarkAsTemplate, UnregisterVM, Destroy, Customize, Upgrade, ConsolidateVMDisks, PromoteDisks, InstantClone, ExportVm, Rename, AcquireTicket; PowerOff заблокирован, если ВМ не работала (у приостановленной он уничтожил бы память `.vmss`). `ShutdownGuest` и (у работавшей ВМ) PowerOff остаются открыты: они нужны самой копии.
   Если сервер блокировку не поддерживает (отдельный ESXi), продукт **пишет это прямо**, но не как предупреждение каждого запуска (это постоянное свойство хоста, а не проблема копии): строка в журнале службы и поле `lock` результата в истории («not locked: the server does not support DisableMethods (a standalone ESXi host); the VM stays powered off during the backup and is marked as held, but another administrator can still power it on or change it»; на vCenter «locked (DisableMethods)»),
   плюс пояснение под флажком на странице задачи («A vCenter locks the VM ... A standalone ESXi host does not allow that ...»). `EsxiSource::lockInfo()` отдаёт ту же строку.
3. Работающая ВМ выключается гостевым выключением `ShutdownGuest` (нужны VMware Tools); ждём до `shutdown_timeout` (по умолчанию 300 с; ключ задачи `snapshot.shutdown_timeout` или ключ сервера). Ответ `ShutdownGuest` ничего не решает сам по себе: настоящий ESXi 7.0.3 отвечает `GenericVmConfigFault` («Invalid fault»), когда ЕГО ожидание выключения гостя (около 30 с) истекло, хотя гость уже останавливается; решает состояние питания (опрос `runtime.powerState` до `shutdown_timeout`). Определённо «нет Tools» только `ToolsUnavailable`.
   Нет Tools или гость не выключился: копия **не** выключает ВМ жёстко молча. Элемент завершается ошибкой
   *the guest cannot be shut down / VMware Tools is not running in the guest, so it cannot be shut down; ... / the guest did not shut down within N s; install VMware Tools, shut the VM down yourself, tick "Back up while the VM is running" or tick "Power off the VM if the guest does not shut down"*, блокировка и метка снимаются, ВМ остаётся такой, как была.
   Второй флажок «If the guest does not shut down, power it off (like pulling the plug)» (`snapshot.force_off`, виден при снятом hot; предупреждение: данные гостя, не сохранённые на диск, теряются) вместо ошибки делает `PowerOffVM_Task` и пишет предупреждение. Выключенная ВМ просто помечается и блокируется, приостановленная копируется как есть (память `.vmss`), питание не меняется.
4. Копирование: ВМ выключена, диски не заняты процессом ВМ, поэтому читаются **на месте** по `/folder`: без снимка VM2Backup и без клонов. Собственные снапшоты ВМ копируются по правилам `snapshot.history` (как у выключенной ВМ); читается вся текущая цепочка дисков вместе с точкой работы.
   Отпечаток: uuid, instance uuid, идентификатор удержания + время выключения, список `путь|размер` замороженных файлов и **время изменения** каждого диска (ВМ, которая побывала включённой между запусками, пишет в диск: отпечаток меняется, копия начинается заново). `.vmx` / `.nvram` / `.vmxf` / `.vmsd` читаются один раз и хранятся в состоянии, как у горячей копии; метка удержания из сохранённого `.vmx` вырезается, поэтому ни архив, ни восстановленная ВМ её не несут.
5. Завершение. **Успех**: блокировка снята (`EnableMethods`), метка снята, работавшая ВМ включена (`PowerOnVM_Task`; отказ включения — предупреждение с текстом в истории, не провал копии). **Отмена пользователем**: то же сразу. **Окончательная ошибка** (сервер отказал, ВМ удалена, учётные данные отвергнуты, копия невозможна): то же, с понятным сообщением.
   **Пауза, обрыв связи, остановка службы**: ВМ остаётся выключенной и помеченной (и заблокированной, где блокировка есть), журнал сохраняется **даже если ни один том ещё не готов** (`VmSource::holdsVm()`), список ВМ на сайте показывает «held for backup since <время>». Следующий запуск того же задания продолжает копию (`pr.resumes`), а потом возвращает питание.
6. Аварийное снятие. Метка, не принадлежащая ни одной возобновляемой копии (служба погибла до первого тома, журнал удалён, копию отбросили), снимается следующим запуском копии этой ВМ: метка и блокировка с нашим идентификатором снимаются, питание возвращается по исходному состоянию, записанному в идентификаторе; предупреждение «removed an old VM2Backup hold of this VM (left by a backup that was interrupted), the VM was powered on again».
   Чужие блокировки и метаданные не трогаются никогда (EnableMethods вызывается только с нашим sourceId). Явная команда администратора: служба `esxi.unlock_vm {server, vm}` (см. docs/WEB.md) и кнопка «Release the VM (unlock and restore power)» (с подтверждением) в окне «Virtual machines...» страницы Datacenter.
   **Вручную, если продукт недоступен**: на ESXi в консоли `vim-cmd vmsvc/getallvms`, затем изменить `.vmx` (или в клиенте «Edit Settings → VM Options → Advanced → Configuration Parameters»): удалить ключ `vm2backup.hold` и строку `[VM2Backup hold ...]` из аннотации, включить ВМ (`vim-cmd vmsvc/power.on <id>`); на vCenter (PowerCLI / govc / API): `AuthorizationManager.EnableMethods(entity, methods, sourceId)` с идентификатором из ключа `vm2backup.hold`.
7. Несколько ВМ в одной задаче идут по одной (каждая простаивает, пока копируется сама). Простой (время выключения → включения) записывается в результат элемента (`downtime`, `downtime_from`, `downtime_to`, секунды и unix-время) и, как предупреждение, отказ включения. Шаблоны и недоступные ВМ отвергаются, как раньше.

### Взаимодействие с другими настройками

`quiesce` / `memory` / `live_disk` относятся только к горячему режиму (в интерфейсе недоступны при снятом флажке). `history` действует в обоих режимах. `shutdown_timeout` / `force_off` — только холодный. Два одновременных бэкапа одной ВМ в холодном режиме не поддерживаются: второй увидит удержание первого и,
если оно не принадлежит работающей в этом процессе копии, снимет его как «осиротевшее».

### Проверено на настоящем ESXi 7.0.3 (холодный режим, 03.11.2026)

Стенд тот же (ESXi 7.0 U3n build 21930508 под qemu/KVM, `tools/esxi7/vm.sh`), гость `vm2b-guest` = Alpine Linux с **open-vm-tools** (пакеты Alpine v3.24 main/community, подпись индекса и контрольные суммы проверены, `tools/esxi7/guest/build-iso.sh tools`, `GUEST_MODE=tools guest-setup.sh`), пишет на диск данных. Сценарии: `tools/esxi7/cold-scenario.sh`, этапы `esxireal_cold_stage_*` (`src/tests_esxi_real.cpp`), ответы SOAP-пробы `tools/esxi7/probe-lock.py`.

**Факты о блокировке на ОТДЕЛЬНОМ хосте (то, ради чего выбрана схема «метка + блокировка там, где она есть»):**
* `AuthorizationManager.DisableMethods` / `EnableMethods` / `QueryDisabledMethods` на настоящем отдельном ESXi 7.0.3 отвечают `NotSupported` («The operation is not supported on the object.») для любой сущности (ВМ, папка, датацентр, хост), для одного метода и для списка. (`EnableMethods` ещё и требует параметр `method`.) Эти вызовы работают на vCenter; на настоящем vCenter 7.0.3 они проверены 07.11.2026 (раздел ниже).
* `SetEntityPermissions` с ролью «No access» для `root` на ВМ отвечает «The requested change cannot be completed because it could leave the system without full administrative privileges»: права `root` отнять нельзя, а на отдельном хосте все администраторы работают под `root`.
* Что остаётся: **метка** (`ReconfigVM_Task` с `extraConfig vm2backup.hold` и строкой в аннотации; проверено: ключ читается свойством `config.extraConfig`, значение `""` его снимает, прежняя аннотация пользователя сохраняется и возвращается; в архив и в восстановленную ВМ метка не попадает). Метка НЕ мешает другому администратору: `ReconfigVM_Task` под `root` по SOAP проходит, `vim-cmd vmsvc/power.on` включает ВМ.
* `HEAD /folder/<диск>-flat.vmdk` не даёт `Last-Modified`: время изменения диска от хоста неизвестно (его нельзя класть в отпечаток); о том, что ВМ включали и выключали между запусками, говорят её файлы журнала (`vmware.log`, ротация `vmware-N.log`: их список и размеры входят в отпечаток холодной копии).
* `ShutdownGuest`: если гость останавливается дольше примерно 30 с, ESXi отвечает `GenericVmConfigFault` («Invalid fault») и задача завершается ошибкой, хотя гость уже останавливается; решает состояние питания (опрос). Гость должен выключиться через ACPI: у busybox `shutdown -h` (сценарий open-vm-tools) только останавливает процессор, ВМ остаётся «включённой»; в Alpine гостя `/etc/vmware-tools/poweroff-vm-default` заменён на `exec /sbin/poweroff`. Сразу после загрузки гостя Tools какое-то время отвечают `ToolsUnavailable` (пульс запаздывает): продукт повторяет вызов до `tools_wait` секунд (30 по умолчанию) и только потом говорит «нет VMware Tools».

**Что показал прогон (сценарии `main`, `offrestore`, `offvm`, `aftercrash`, `unlock`, `notools`; сырые журналы в `.winvm-results/esxi7-cold-20261103/`; сценарии `cancel` и `limits` описаны ниже):**
* (а) холодный бэкап по умолчанию работающей ВМ: гость выключен через Tools (17 с на свободном хосте, 17 минут на загруженном), ВМ помечена, копия идёт БЕЗ снимка и клонов, sha256 `-flat.vmdk` в копии = sha256, снятый на хосте при выключенной ВМ (`e25c4f72...530d`), ВМ включена снова, метка снята, гость загрузился; простой записан (1262 с на загруженном хосте).
* (б) пауза посреди копии: ВМ осталась выключенной и помеченной (`list` показывает `HELD since`), продолжение шло по тому же удержанию (`возобновлений 1`), побитово верный результат; ВМ, включённая мимо продукта, делает продолжение невозможным: копия начинается заново (предупреждение).
* (д) осиротевшая метка (журнал потерян): следующий запуск снял метку «removed an old VM2Backup hold of this VM (left by a backup that was interrupted)», включил ВМ, которая работала до копии, и подождал загрузки Tools; перезагрузка самого хоста посреди этапа восстановления дала тот же след (ВМ выключена, метка стояла) — подчистил следующий запуск.
* (в) отмена пользователем: на настоящем хосте проверена и для выключенной ВМ (метка снята, питание не менялось), и для РАБОТАЮЩЕЙ (07.11.2026: пауза, `HELD since …`, отмена: «отменён (aborted)», ВМ включена, метка снята, возобновляемых копий 0; `esxireal_cold_stage_cancel` PASS 144 с); на настоящем vCenter то же (PASS 157 с, блокировка снята).
* (ж) ограничения: `ReconfigVM_Task` под `root` по SOAP проходит на настоящем хосте (меняет аннотацию). **`vim-cmd vmsvc/power.on` из SSH под удержанием (07.11.2026, выключенная `vm2b-off`, отдельный ESXi 7.0.3): `Powering on VM:` и ВМ включается, метка в `.vmx` остаётся; продолжение копии после этого отказывает понятной ошибкой «VMware Tools is not running in the guest, so it cannot be shut down; install VMware Tools, shut the VM down yourself, tick "Back up while the VM is running" …»** (ВМ теперь работает, у неё нет Tools; метка снята, ВМ остаётся включённой). Блокировки на отдельном хосте нет. Прямые операции с файлами датастора и консоль/SSH `root` не блокируются и на vCenter (блокировка действует на методы vCenter).
* (г) горячий режим (`hot_backup: true`): все прежние тесты `esxireal` идут с этим ключом.
* Восстановление холодной копии на хост: ВМ зарегистрирована и включается (`Powered on`).

### Регрессия горячего режима на настоящих ESXi 7.0.3 и vCenter 7.0.3 (сборка 1.0.311.407, после неё менялись только номер сборки, тесты и документы, 07.11.2026)

Сырые журналы: `.winvm-results/esxi7-cold-20261107/` (`real-hot.log`, `vcreal.log`, `grun.log`). Все тесты идут с `hot_backup: true`.

| Проверка | Результат |
|---|---|
| `esxireal` (отдельный хост): connect/list, неверный пароль, матрица 4 режимов (в том числе `vm2b-on` через клон), verify, неверный пароль архива, восстановление в папку, продолжение по `Range`, восстановление на хост, `vm2b-hist`, служба, живые сценарии (пауза/продолжение/отмена) | 11 из 11 PASS; остатков `VM2Backup-*` 0; sha256 восстановленных на хост дисков совпал у 7 файлов, не совпал у 0 |
| `vcreal` (vCenter): те же сценарии через vCenter, включая копию ВКЛЮЧЁННОЙ `vm2b-on` и восстановление в vCenter | 6 из 6 PASS |
| `guest-run.sh all` (гость пишет на диск во время копии): xz1; пароль + xz5 + тома 100 МБ (9,1 мин); восстановление на ХОСТ, загрузка восстановленной ВМ «VERIFY OK seq<=S» (M=41174, violations=0); пауза при работающей ВМ (A); выключение/включение посреди паузы (B); выключение посреди передачи (C) | 6 из 6 `verdict=OK`, 65535 блоков проверено в каждом, violations=0 |

### Проверено на настоящем vCenter 7.0.3 (холодный режим, блокировка, 07.11.2026)

Стенд: VCSA 7.0.3 (build 24927011) + вложенный ESXi 7.0.3 с датацентром `vm2b-dc`, гость `vm2b-guest` (Alpine + open-vm-tools), выключенная `vm2b-off`. Сценарии: `COLD_KEY=vcenter_real tools/esxi7/cold-scenario.sh vcoff|vcguest|crash|cancel|unlock`, пробы `tools/esxi7/probe-vc.py`, `probe-priv.py`, `probe-lock.py`; те же этапы `esxireal_cold_stage_*`, только через vCenter. Сырые ответы: `tests/fixtures/vcenter7-real/` (`DisableMethods-ok`, `DisableMethods-NoPermission`, `EnableMethods-NoPermission`, `QueryDisabledMethods-held`, `PowerOnVM_Task-MethodDisabled.fault`).
* `DisableMethods` принимает все 24 метода нашего списка (+ `PowerOffVM_Task` для не работавшей ВМ): ни один не отклонён как неизвестный (`InstantClone_Task`, `ExportVm`, `AcquireTicket`, `PromoteDisks_Task`, `ConsolidateVMDisks_Task` тоже), т. е. `kHoldMethods` на 7.0.3 верен.
* `QueryDisabledMethods(vm)` отдаёт по одному `<returnval><method>…</method><sources><sourceId>VM2Backup-<id>-<on|off|sus>-<unix></sourceId><reasonId>VM2Backup: the VM is held for a backup</reasonId></sources></returnval>` на метод: по `sourceId` продукт узнаёт свои блокировки (в том числе чужие по составу, оставшиеся без метки). Свойство `disabledMethod` ВМ перечисляет и состояния питания, поэтому по нему свои блокировки не узнать.
* Чужая сессия vCenter (`administrator@vsphere.local`, отдельный вход) под удержанием: `PowerOnVM_Task`, `ReconfigVM_Task`, `CreateSnapshot_Task`, `UnregisterVM`, `Destroy_Task` отвечают `MethodDisabled` с текстом «The method is disabled by 'VM2Backup-<id>-<on|off|sus>-<unix>'» (`<source>` в детали тот же идентификатор). `ShutdownGuest` и (у работавшей ВМ) `PowerOffVM_Task` открыты, как задумано.
* **Блокировка действует только на уровне vCenter.** Вызов по SOAP прямо на ХОСТ под `root` (`ReconfigVM_Task`, `PowerOnVM_Task`, `vim-cmd vmsvc/power.on`) проходит мимо неё, как и файловые операции с датастором. Метка при этом остаётся единственной защитой. Если так стёрли метку, а блокировка осталась, следующий запуск (или `esxi.unlock_vm`) находит свою блокировку по `sourceId`, снимает её и возвращает питание («removed an old VM2Backup lock … whose marker was gone»); продолжение копии в таком состоянии сначала снимает собственную блокировку (она запрещает и `ReconfigVM`, метку иначе не поставить) и ставит метку и блокировку заново. Это было найдено прогоном: до правки продолжение падало с «The method is disabled by 'VM2Backup-…'».
* Права: пользователю нужны `Global.DisableMethods` и `Global.EnableMethods`, причём **на корневой папке** (`group-d1`, с наследованием): без них `DisableMethods` отвечает `NoPermission` с `privilegeId` и `Folder group-d1` в детали (фикстуры `*-NoPermission`); текст ошибки продукта называет право.
* Результаты (настоящий vCenter, холодный режим): пауза посреди копии (ВМ выключена, помечена и заблокирована, `HELD`); продолжение по тому же удержанию (`возобновлений 1`), **sha256 диска в копии = sha256 с хоста при выключенной ВМ** (`9fd90df2…8f18`), простой 281 с, ВМ включена, метка и блокировка сняты; гибель службы посреди копии (метка и блокировка остались) и следующий запуск снял обе и включил ВМ (PASS); отмена работающей ВМ (ВМ включена, блокировка снята) PASS; `esxi.unlock_vm` 4 с PASS; выключенная `vm2b-off` — удержание без выключения, чужие вызовы, продолжение PASS. Отдельный сценарий `aftercrash` (повтор на уже чистой ВМ) для проверки не нужен: он входит в `crash`.
* Прямой вызов на ХОСТ с включением ВМ под удержанием делает продолжение невозможным («Tools not running»): ограничение, не дефект (см. выше про отдельный хост).

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
4. Отпечаток (fingerprint) = SHA-256 от (uuid, instance uuid, MoRef + имя снимка, способ чтения дисков clone/direct, упорядоченный список `путь|размер` ТОЛЬКО замороженных файлов: диски, данные и память снимков, suspend). Живые файлы конфигурации (`.vmx`, `.vmxf`, `.nvram`, `.vmsd`, namespace data) в отпечаток НЕ входят: хост переписывает их при выключении и включении ВМ. Токен возобновления = `<MoRef снимка>|<имя>|<состояние>`; состояние (xz + base64; в зашифрованной копии весь токен ещё и шифруется AES-GCM ключом копии) хранит конфигурацию начала копии, см. раздел «Возобновление копии: что переживает продолжение».
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
* Не проверено на настоящем хосте: 8.0 / 9.0, шифрованные ВМ, RDM, vSAN, NFS-датасторы, лицензия Hypervisor (free). (Файлы больше 4 ГиБ, quiesce при работающих VMware Tools и несколько датасторов проверены в раунде 3: раздел «Проверено на настоящем ESXi 7.0.3 (раунд 3)» ниже.)
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
* Согласованность при записях: у ВМ `vm2b-on` без ОС писать некому (процесс ВМ только крутит BIOS), поэтому здесь проверялась только неизменность замороженной базы. Запись гостя ВО ВРЕМЯ копии проверена отдельно, на ВМ с Alpine Linux: раздел «Запись гостя во время копии — проверено на настоящем ESXi 7» ниже (эмулятор: `esxi_running_vm_disks_are_cloned`, `esxi_guest_writes_are_frozen_out`).

### Сценарий «ВМ включена → копия начата → ВМ выключили → связь пропала → ВМ включили → следующий запуск продолжает» на рабочем транспорте

`tools/esxi7/live-scenario.sh` (`vm2b-on`; тома по ~1 МБ, пауза на 40 МБ данных диска):
1. Копия начата и поставлена на паузу с сохранением (`keepOnAbort`): **40 томов** готовы, возобновляемая копия 1. На хосте: снимок `VM2Backup-<10 hex>` (`consolidationNeeded = false`) и папка клонов `vm2b-on/VM2Backup-<10 hex>/` с файлами `d0.vmdk`, `d0-flat.vmdk`, `ready`.
2. ВМ выключена `power.off` при существующем снимке: проходит, снимок и папка клонов остаются.
3. ВМ включена снова: снимок и клон те же.
4. Следующий запуск **продолжает**: источник присоединяется к существующему снимку (новый не создаётся), клон признаётся годным по файлу `ready` (повторного клонирования нет), отпечаток тот же (`resumes = 1`, 40 готовых томов не копируются заново), копия идёт дальше с места остановки ВНУТРИ flat-диска запросом `Range`. Копия закончена, `end()` удаляет папку клонов, потом снимок: на хосте остаётся только `vm2b-on-flat.vmdk vm2b-on.vmdk vm2b-on.vmsd`.
5. Явная отмена (прерывание без `keepOnAbort`) удаляет снимок, папку клонов и журнал: возобновляемых копий 0, снимков на ВМ нет.
Если ВМ к моменту продолжения выключена: способ чтения дисков закреплён за копией (он записан в состоянии возобновления), поэтому продолжение идёт ТЕМ ЖЕ способом, через клон замороженного состояния (клон лежит на датасторе и не нуждается в ВМ; если папку клонов удалили, клон делается заново из замороженного звена, байты те же), отпечаток тот же. Раздел «Возобновление копии: что переживает продолжение».
Пределы продолжения: внутри диска продолжение возможно, пока жив снимок и замороженные данные не изменились; если снимок убрали руками (или ВМ его схлопнула), продолжения нет: копия начинается заново с предупреждением «the interrupted backup could not be continued (...)». Клон делается ДО передачи первого байта диска: время чтения и записи диска хостом (на этом стенде 256 МБ: секунды; на реальных ВМ — минуты и часы, `clone_timeout`), и нужно место на датасторе под тонкий клон; ограничение скорости, пауза и отмена работают на этапе передачи, отмена во время клонирования отменяет задачу на хосте (`CancelTask`), папка удаляется.

## Возобновление копии: что переживает продолжение (02.11.2026)

Задача: копия включённой ВМ начата (снимок `VM2Backup-<hex>`, клон диска), поставлена на паузу или оборвана, ВМ **выключают (и потом включают)**, следующий запуск должен ПРОДОЛЖИТЬ тот же бэкап по тому же снимку, без перекачки готовых томов.
Настоящий ESXi 7.0.3 при первом выключении ВМ ПЕРЕПИСЫВАЕТ `.vmx` (1972 -> 1994 байта), меняет `.nvram` и время файлов; прежний отпечаток считался и по этим файлам, поэтому копия перезапускалась с нуля. Теперь:

**Что в отпечатке и в состоянии.**
* Отпечаток покрывает только ЗАМОРОЖЕННОЕ: uuid и instance uuid ВМ, MoRef и имя снимка, способ чтения дисков (клон / на месте), упорядоченный список `путь|размер` замороженных файлов (диски и их клоны, дескрипторы клонов, данные и память снимков, suspend). Он меняется (и копия начинается заново), если снимок пропал или пересоздан, изменились размеры замороженных файлов или клона, диск добавлен или убран, ВМ другая или восстановлена заново.
* «Живые» файлы конфигурации (`config` .vmx, `extendedConfig` .vmxf, `nvram`, `snapshotList` .vmsd, `namespaceData`; не больше 4 МБ каждый) при ПЕРВОМ запуске читаются один раз, в `begin()`, сразу после снимка. Их байты, `vm2backup-meta.json` как он был построен, список всех файлов с размерами и временами и способ чтения дисков кладутся в **состояние возобновления** — третью часть токена журнала `<archive>.resume` (xz + base64; около сотен байт — единицы КБ после сжатия; журнал переписывается с каждым томом).
  Журнал не шифруется, но токен зашифрованной копии (пароль задан) шифруется AES-256-GCM ключом копии (`ProtectToken` в `engine.cpp`: тот же PBKDF2-ключ, случайный nonce, свой AAD); без пароля архива конфигурацию из журнала не прочитать.
* Продолжение строит список файлов из СОХРАНЁННОГО состояния: замороженные файлы берутся с хоста (чтение, пути клона), конфигурационные — из состояния, **байт в байт тем, что уже лежат в готовых томах**; мета-файл тоже сохранённый (в нём есть `power_state`, `reused_snapshot`, `created`: пересборка меняла бы его длину и сдвигала бы всё дальнейшее содержимое потока). Поэтому продолжение выдаёт ТОТ ЖЕ поток, а файл конфигурации, лежащий на границе тома, не склеивается из двух разных версий.
* Архив описывает ВМ на **момент снимка**: диски — замороженная база (в клоне — вся цепочка); конфигурация — **версия начала копии** (то, что хост отдавал сразу после снимка), а не та, что ESXi записал при выключении. Восстановленная ВМ получает записи дисков из `disks[].snapshot_descriptor` и состояние на момент снимка; позднее изменённый хостом `.vmx` в архив не попадает.

**Что переживает продолжение.**
* Перезапись и изменение `.vmx`, `.vmxf`, `.nvram`, `.vmsd`, логов между запусками; выключение, включение, несколько циклов.
* Смена `powerState`. Включённая ВМ при первом запуске: копия идёт через клон и продолжается через клон и тогда, когда ВМ к продолжению **выключена**. Способ чтения закреплён за копией: клон замороженного состояния лежит на датасторе (файл `ready`) и не зависит от питания ВМ; если папку клонов удалили, клон делается заново (`CopyVirtualDisk` читает замороженное звено выключенной ВМ без замка), байты те же, готовые тома совпадают. Читать «на месте» выключенной ВМ здесь не нужно и неправильно: замороженное звено может быть дельтой, и дескриптор «воспроизводимого» клона был бы другим.
  Выключенная ВМ при первом запуске (диски читаются на месте): продолжается тем же способом, пока ВМ выключена или её включили и выключили снова.
* Предупреждение при продолжении: `the VM was powered off/on and its configuration files changed while the backup was paused: the archive keeps the configuration from the start of the backup` (если сменился `powerState`; при изменении размера или времени конфигурационных файлов без смены состояния — то же без слов про питание).

**Что НЕ продолжается (копия начинается заново, старые снимок и клон убираются как остатки; в отчёте предупреждение `the interrupted backup could not be continued (<причина>): it was started over`).**
* Снимка нет: удалён рукой, схлопнут при выключении или консолидации, пересоздан (новый MoRef) — `the snapshot of the interrupted backup does not exist any more`.
* Замороженные данные изменились: размер клона или замороженного файла другой, диск добавлен/убран, ВМ другая или создана заново (uuid) — `the frozen state of the VM (the snapshot, its disks, the list of the disks) is not what the interrupted backup copied`.
* Копия начата на ВЫКЛЮЧЕННОЙ ВМ (диски читаются на месте, в архиве цепочки дельт), а к продолжению ВМ ВКЛЮЧЕНА: включённая ВМ держит замок на диски, читать их на месте нельзя, а клон выдаёт другой поток — `the VM was powered on while the backup was paused`.
* Журнал старого формата (до этого изменения): отпечаток другого вида, копия начинается заново один раз.
* Настройки копии (кодек, пароль, размер тома) изменились, том потерян на цели — как и раньше.

**Пределы.** Состояние хранится на цели рядом с томами и растёт с размером конфигурации (типичный `.nvram` ESXi около 270 КБ; файл больше 4 МБ не сохраняется, читается заново и считается замороженным: изменение его размера перезапускает копию). Само содержимое замороженных файлов при продолжении не перечитывается и не сверяется (проверяются имена и размеры): подмена содержимого клона с тем же размером не будет замечена. Для файла конфигурации, который хост меняет на лету, копия всегда «версия начала копии» (раньше при чтении «на лету» возможны были предупреждения «shorter / grew»; теперь файл читается один раз целиком).

**Тесты на эмуляторе** (`tools/testservers/esxi_mock.py` теперь воспроизводит поведение настоящего хоста: команда `power` при смене состояния переписывает `.vmx` (первый раз меняется размер, дальше только время), меняет `.nvram` и время `.vmxf`; диски выключенной ВМ читаются без блокировки): `esxi_resume_survives_power_cycles` (8 режимов: выключена; выключена и включена; три цикла; выключена и папка клонов потеряна — клон делается заново; замороженный клон изменён — перезапуск; выключенная ВМ включена до продолжения — перезапуск; выключенная ВМ с циклом включение-выключение — продолжение; замороженная база выключенной ВМ изменена — перезапуск; во всех режимах проверяются resumes, число снимков и клонов, предупреждения, побитовое совпадение восстановленных дисков, `.vmx` и `.nvram` с версией ПЕРВОГО запуска, отсутствие остатков, состояние журнала с паролем и без) и `esxi_resume_across_runs` (режим 2 теперь «замороженный клон изменён»). На коде до изменения `esxi_resume_survives_power_cycles` падает (resumes == 0, лишние снимки, `.vmx` и `.nvram` другой версии).

### Проверка на настоящем ESXi 7.0.3 (02.11.2026, сырые журналы `.winvm-results/esxi7-resume-20261102/`)

Стенд развёрнут заново по рецепту (qemu/KVM nested, 4 vCPU, 6 ГБ, изолированная сеть 10.213.77.0/24, evaluation-лицензия, ВМ `vm2b-guest` с Alpine, гость пишет непрерывно). Сценарии `tools/esxi7/guest-run.sh scenario` (`SCEN="B D E F G"`); копия идёт 20 Мбит/с, пауза после 100 МБ (102 готовых тома по 1 МБ), ВМ включена, гость пишет. Во всех сценариях после копии на хосте `VM2Backup-*` (папки и снимки): **0**, восстановленный диск проверен в 65 535 блоках: **0 нарушений** (`inflight` = 0).

| Сценарий (между паузой и продолжением) | возобновлений | готовых томов не перекопировано | M | stun гостя | предупреждение продолжения / перезапуска |
|---|---|---|---|---|---|
| B: ВМ выключена на 20 с и включена | **1** | 102 (104 448 000 байт потока) | 31 625 (запись 31 626 = stun, **M = stun - 1**); 3 950 записей гостя после снимка, в копии их нет | 31 626 (пауза 805 мс) | `continued with the snapshot of the interrupted run; the VM was powered off/on and its configuration files changed while the backup was paused: the archive keeps the configuration from the start of the backup` |
| B (первый прогон на этом стенде, до правки текста предупреждения) | 1 | 102 | 1 162 (запись, шедшая в момент заморозки, легла в базу: M = stun, допуск [stun - 2, stun]) | 1 162 | то же, без слов «powered off/on» |
| D: ВМ выключена и остаётся выключенной к продолжению (клон читается, ВМ потом включена) | **1** | 102 | 4 305 (M = stun, как выше) | 4 305 | то же |
| E: три цикла выключить/включить | **1** | 102 | 6 609 (**M = stun - 1**); 1 366 записей после снимка | 6 610 | то же, без слов «powered off/on» (прогон до правки текста: состояние питания в начале и в конце одно и то же) |
| F (негативный контроль): снимок удалён (`snapshot.removeall`) и создан новый `VM2Backup-0123456789` | **0** | 0 | 11 675 нового снимка (**M = stun - 1**) | 11 676 | `the interrupted backup could not be continued (the snapshot of the interrupted backup does not exist any more ...): it was started over` |
| G (негативный контроль): в замороженный клон `d0-flat.vmdk` дописан 1 байт (268 435 456 -> 268 435 457) | **0** | 0 | 20 375 (**M = stun - 1**: stun = запись 20 376 с задержкой 4 246 мс, снимок нового запуска после консолидации старого) | 20 376 | `... (the frozen state of the VM (the snapshot, its disks, the list of the disks) is not what the interrupted backup copied): it was started over` |

Что видно на самом хосте: при каждом выключении/включении `.nvram` (8 684 байта) получает новое время, `.vmx` (2 000 байт на этом стенде: размер уже «устоялся», время меняется) переписывается; продолжение при этом идёт по прежнему снимку (`resumes = 1`, новый снимок не создаётся), архив хранит конфигурацию начала копии. Снимок и папка клонов `d0.vmdk`, `d0-flat.vmdk`, `ready` переживают выключение и включение; при выключенной ВМ (D) клон читается без ВМ. Негативные контроли (F, G): копия НЕ склеивается, начинается заново с понятным предупреждением и дает согласованный диск нового снимка.
Что пошло не так по ходу прогона и исправлено в скриптах: (1) первая попытка E и F снята во время загрузки гостя (сценарий считал гостя «пишущим» по числу строк `BOOT`/`RUN start`, которые остаются от прежней загрузки): снимок пришёлся на простой (M = последняя запись до выключения, 0 нарушений, но не «в момент записи»), `wait_writing` теперь ждёт РОСТ числа строк `R seq` за 12 с; (2) в сценарии G `analyse.py` по часам гостя отбраковал результат (гостевые часы ушли на секунды после консолидации старого снимка: «запись 4,2 с, пауза 12,9 с»): добавлена проверка без часов (самая длинная пауза в окне ±12 с, M ровно перед ней; в разборе `clock_suspect`); (3) один раз загрузка гостя после серии включений зависла на ISOLINUX на 40 минут и была снята `power.reset` (к продукту не относится).
Предупреждение: при M = stun и M = stun - 1 оба исхода допустимы (запись, шедшая в момент заморозки, может лечь в базу), нарушений нет в обоих.

## Запись гостя во время копии — проверено на настоящем ESXi 7 (02.11.2026)

До этого прогона у тестовых ВМ не было ОС: писать на диск было некому, и на настоящем хосте проверялось только то, что замороженная база не меняется. Теперь у ВМ есть гость (Alpine Linux virt 3.24.3), который
**непрерывно пишет на диск данных во время копии**, и доказано: копия включённой ВМ = согласованное (crash-consistent) состояние диска РОВНО на момент снимка; записей гостя после снимка в ней нет; восстановленная ВМ загружается и сама подтверждает диск.

### Схема проверки

* **Стенд** (все скрипты в `tools/esxi7/`, воспроизводимо без кликов): `vm.sh install` / `start` (вложенный ESXi 7.0.3 под qemu/KVM, 4 vCPU, 6 ГБ, изолированная сеть 10.213.77.0/24), `setup-guest.sh` (датастор и прежние ВМ),
  `guest-setup.sh` (ВМ **vm2b-guest**: 512 МБ ОЗУ, 1 vCPU, диск данных 256 МБ thin на pvscsi, CD с Alpine, **без сети**, serial-порт ВМ в ФАЙЛ на датасторе `vm2b-serial/guest.log`; ISO и serial лежат вне папки ВМ),
  `guest-run.sh` (`copy` / `host` / `scenario` / `all`), `guest-negative.sh` (`neg`, `final`). Исходный ISO только читается; `tools/esxi7/guest/build-iso.sh` делает его КОПИЮ с `apkovl` (автозапуск через `/etc/local.d`, статический `vm2bio`, тихая загрузка).
  Загрузка Alpine во вложенном ESXi занимает 9 минут (при второй работающей ВМ до 25): параметры `pciBridge*` из `setup-guest.sh` нужны (без них pvscsi не получает слот); консоль ядра на serial отключена (`KCONSOLE=1` включает: каждый символ — выход в гипервизор, загрузка в разы дольше).
* **Писатель в госте** (`tools/esxi7/guest/vm2bio.c`, «сырой» блочный: без файловой системы). Диск = суперблок + блоки по 4 КиБ. Блок = заголовок {метка, номер записи `seq`, номер блока, CRC32, время гостя} + 4064 байта данных = поток splitmix64 от (seed, блок, seq).
  Сначала весь диск заполняется (`seq` 0), затем запись `k` = один блок, который определяется ТОЛЬКО функцией от (seed, k) (случайные блоки и последовательные участки по 64 записи), открыто `O_DIRECT|O_DSYNC`, одна запись в полёте.
  Следствие: любой согласованный по времени образ диска есть ровно «состояние после записей 1..M», а эталон для любого M вычисляется заново без журнала. Журнал гостя идёт на serial (`R seq=… real=…` каждые 25 записей, `STALL …` на записи с задержкой >100 мс или паузой >250 мс).
  Скорость во вложенном ESXi 15-30 записей/с (синхронная запись на thin-диск поверх qcow2).
* **Проверка образа** (два независимых кода: Си в госте и на хосте `vm2bio verify`, C++ в тесте `GuestScan`, Python/numpy `tools/esxi7/guest/check_disk.py`; все три совпали на всех образах): M = наибольший найденный `seq`; для КАЖДОГО из 65 535 блоков должна быть запись
  `последняя seq ≤ M` с верными CRC и данными. Нарушение = блок пуст / испорчен / «потерянная запись» (в блоке более старая запись, чем должна быть при M). Допустимо одно «окно незавершённых IO»: блок записи M+1 (она могла лечь частично или целиком).
* **Хронология ХОСТА, не зависящая от гостя** (`guest/esxi-watch.py` работает на самом ESXi, шаг ~50 мс, часы хоста): появление `*-sesparse.vmdk` = момент снимка, клон `VM2Backup-*/d0-flat.vmdk` и `ready`, рост дельты, исчезновение = снимок снят; плюс строки serial гостя с временем наблюдения.
  **Момент снимка в терминах гостя виден точно**: снимок «замораживает» гостя (stun) на 0,15-1,5 с, и писатель фиксирует это как `STALL` (пауза между записями или одна долгая запись). Запись M = последняя ДО паузы. Вторичная проверка по часам: M внутри окна ±2 с и ±4 с вокруг момента снимка
  (смещение часов гость/хост 0,35-0,4 с, задержка serial до ~1 с).
* **Разбор** (`guest/analyse.py`): сопоставляет образ, serial и журнал хоста и печатает числа; **негативные контроли** (`guest/negative.py`, `guest/final_compare.py`): см. ниже.

### Результаты (все на настоящем ESXi 7.0.3, build 21930508; сырые журналы `.winvm-results/esxi7-guest-20261102/`)

| Копия (включённая ВМ, гость пишет) | xz 1, без пароля | пароль + xz 5 + тома ~100 МБ | пауза посреди копии, продолжение (ВМ работает) |
|---|---|---|---|
| тест | `esxireal_guest_write_copy` | `esxireal_guest_write_copy` | `esxireal_guest_stage_pause` + `_resume` |
| копия от начала до конца (с verify) | 474,7 с (7,9 мин) | 680,9 с (11,3 мин) | пауза после 100 МБ, продолжение ~170 с |
| снимок прожил | 257 с | 463 с | 351 с |
| клон готов через (CopyVirtualDisk 256 МБ) | 9,2 с | 13,0 с | 14,4 с |
| дельта снимка (sesparse), рост | 6,3 МБ -> 56,6 МБ | -> 56,6 МБ | -> 23,1 МБ |
| **M** (записей в копии) | **2311** | **22263** | **94523** |
| stun гостя (seq записи после паузы) | 2312 (пауза 852 мс) | 22264 (запись 1944 мс) | 94524 (пауза 1456 мс) |
| M относительно stun | **ровно stun - 1** | **ровно stun - 1** | **ровно stun - 1** |
| окно по часам ±2 с | [2250, 2378] M внутри | [22224, 22271] внутри | [94514, 94530] внутри |
| проверено блоков / нарушений | 65 535 / **0** | 65 535 / **0** | 65 535 / **0** |
| записей гостя ПОСЛЕ снимка до конца копии | **11 421** | **10 832** | **3 471** |
| блоков копии, отличающихся от состояния в конце копии | 10 529 | 9 994 | 3 400 |

Итого по пункту 1-2: гость писал всё время (после снимка тысячи записей, дельта выросла в 4-9 раз), копия шла 4-11 минут, а восстановленный диск содержит ровно записи 1..M, где M — последняя запись ДО паузы снимка; ни одной записи после снимка
(в окно незавершённых IO за все прогоны не попало ни одного блока: inflight = 0). Режимы xz1 и пароль + xz 5 + тома 100 МБ (3 тома) дали одинаковый результат; verify и неверный пароль архива отвергнут, как для остальных ВМ.

### Восстановление на хост и загрузка восстановленной ВМ (пункт 3)

`esxireal_guest_restore_to_host` + `guest-run.sh host`: копия включённой ВМ при пишущем госте (xz 1, без ограничения скорости, 102 с; снимок M = **43169**, stun гостя на записи 43170) -> та же копия в папку (65 535 блоков, 0 нарушений) и на хост (`RestoreToEsxi`, ВМ `vm2b-rt-guest`).
Скрипт подменяет в восстановленной ВМ CD на **проверяющий** образ (`build-iso.sh verify <lo> <hi>`: окно [stun-2, stun] = [43168, 43170] вшито в apkovl) и serial на отдельный файл. Восстановленная ВМ включилась и загрузила Alpine, гость прочитал диск данных и напечатал на serial:
`VERIFYRESULT M=43169 blocks=65535 ok=65535 zero=0 torn=0 stale=0 inflight=0 violations=0` и **`VERIFY OK seq<=S: M=43169 in [43168,43170]`**. Тот же M, что у восстановления в папку; sha256 `-flat.vmdk` на самом хосте = sha256 файла, восстановленного в папку из того же архива
(`8b9ef241…cc298`). Загрузка со сканированием 256 МБ заняла в этом прогоне ~25 минут (на хосте шли чужие qemu, load average ~10); `guest-run.sh host` ждёт до 40 минут.

### Пауза, выключение ВМ посреди копии, выключение при передаче (пункт 4)

Копия идёт со скоростью 20 Мбит/с; снимок и папка клона остаются на хосте на паузе (`consolidationNeeded = false`, папка `VM2Backup-<hex>/` с `d0.vmdk`, `d0-flat.vmdk`, `ready`).

* **A: пауза (после 100 МБ), ВМ работает 40 с, продолжение** (`esxireal_guest_stage_pause` / `_resume`): возобновлений 1, «continued with the snapshot of the interrupted run»; **M = 94523 = stun - 1 ПЕРВОГО снимка**, 0 нарушений, 3471 запись гостя после снимка (из них 3400 блоков отличаются от состояния в конце копии).
* **B: пауза, ВМ ВЫКЛЮЧЕНА (`power.off`, гость умирает посреди работы), 20 с, включена, продолжение**: на хосте при выключении снимок и клон остаются. **История:** в первом прогоне (до изменения от 02.11.2026) первое выключение/включение переписывало `.vmx` (1972 -> 1994 байта), отпечаток менялся, и копия честно начиналась заново с НОВЫМ снимком (M = 99748 на момент выключения, 0 нарушений; повторная попытка при уже стабильном `.vmx` продолжалась по первому снимку, M = 101474 = stun - 1). Тогда в этом разделе стояла оговорка «первое выключение/включение посреди паузы может сделать продолжение невозможным». **Оговорка снята:** сценарий B заново, на новом коде, см. раздел «Возобновление копии» ниже («Проверка на настоящем ESXi 7.0.3»): продолжение по ПЕРВОМУ снимку, готовые тома не копируются заново.
* **C: ВМ ВЫКЛЮЧАЕТСЯ (hard `power.off`) ПОСРЕДИ ПЕРЕДАЧИ копии** (без паузы; `esxireal_guest_stage_full`, скрипт выключает ВМ через 60 с после готовности клона): клон принадлежит снимку и читается хостом независимо от питания ВМ, копия дошла до конца (снимок прожил 259 с), снимок и папка клонов удалены (остатков 0),
  verify прошёл; **M = 104146** (запись, шедшая в момент заморозки, легла в базу: stun на записи 104146, M = stun, допуск [stun - 2, stun]), 0 нарушений, 2704 записи гостя после снимка. Выключенная ВМ после этого осталась без снимков.
* Гость после трёх жёстких выключений (со снимком) каждый раз находил диск согласованным (`WRITER resume … violations=0`); итоговый «живой» диск ВМ — см. ниже.

### Негативный контроль: детектор МОЖЕТ ловить ошибку (пункт 5)

`guest-negative.sh neg` на реальных восстановленных образах (A: M = 2311, B: M = 22263, B снят позже на 19 952 записи): склейка «первая половина A + вторая B» → **10 851** нарушение (потерянные записи); один испорченный бит → 1 «испорчен»; один обнулённый блок → 1 «пуст»;
блок B, возвращённый к старому состоянию → 1 «потерянная запись»; образ A + ОДНА просочившаяся запись из будущего (seq M+4) → 3 нарушения (пропавшие записи M+1..M+3 видны); образ A против эталона «состояние в конце копии» (M = 22263): **17 402 блока отличаются** —
т.е. если бы в копию попали записи после снимка (или она была снята позже), проверка это бы показала. Сделать невидимой можно только запись M+1 сразу после последней: это и есть допустимое «окно незавершённых IO» (один блок).

Сверка с НАСТОЯЩИМ диском ВМ, снятым позже (`guest-negative.sh final`: ВМ выключена, `vm2b-guest-flat.vmdk` прочитан с хоста): живой диск сам согласован (M = 106 857, 0 нарушений), а каждая копия — согласованное СТАРШЕЕ состояние:
xz1 M = 2311 (отличается 52 935 блоков), пароль + xz 5 M = 22 263 (48 295), A M = 94 523 (11 325), B M = 101 474 (5 174), C M = 104 146 (2 660), на хост M = 43 169 (41 723). Нигде не «0 отличий»: проверка не пропускает записи, сделанные после снимка.

### Пределы

* **Crash-consistent, не application-consistent** (до раунда 3; quiesce с настоящими Tools проверен в раунде 3, см. ниже). В образе нет VMware Tools (open-vm-tools в образе Alpine нет, сети для установки нет), поэтому quiesce не проверялся: снимок делается без него (`quiesce: false`; при включённом quiesce без Tools источник повторяет без него и пишет предупреждение). Копия — как после внезапного отключения питания в момент снимка:
  данные, дошедшие до диска, целы и согласованы по порядку записей; кэш ОС гостя, не сброшенный на диск, в копии отсутствует. Здесь писатель синхронный (`O_DSYNC`), поэтому «префикс записей» полный; у гостя с файловой системой и журналом согласованность обеспечивает журнал ФС.
* **Окно незавершённых IO:** записи, начатые до снимка и не завершённые к его моменту, могут отсутствовать или быть записанными целиком; записи ПОСЛЕ снимка в копии быть не должны. Измеренное окно: одна запись (M+1); во всех прогонах M совпал с «последняя запись до stun» точно.
* Граница M определена по паузе гостя при снимке и по часам хоста (±2 с); «абсолютное» время снимка внутри ESXi недоступно (снимок идёт десятые доли секунды, момент stun = точка заморозки).
* Один гость, один диск данных 256 МБ, синхронный писатель 15-30 записей/с: не нагрузочный тест. Файловая система гостя, несколько дисков, Windows-гости и Tools/quiesce не проверялись.
* Каждая проверка `NoLeftovers` (тесты) сама делает снимок и клон включённой ВМ (несколько секунд) и удаляет их; остатков `VM2Backup-*` после каждой копии: 0.

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

## Проверено на настоящем ESXi 7.0.3 (раунд 3, 12.11.2026)

Стенд тот же (вложенный ESXi 7.0.3 build 21930508 под qemu/KVM, 6 ГБ, 4 vCPU, изолированная сеть 10.213.77.0/24), датасторы: `datastore1` (VMFS6, 64 ГиБ) и **`datastore2` (второй диск ESXi, VMFS6, 24 ГиБ)**; скрипты `tools/esxi7/`:
`guest-fs-build-disk.sh`, `guest-fs-setup.sh`, `fs-run.sh`, `fs-analyse.sh`, `fs-wait.sh`, `vm-guestinfo.py` (пункт A), `big-setup.sh`, `big-run.sh` (B), `multi-setup.sh`, `multi-run.sh` (C); тесты `esxireal_guest_fs_copy`, `esxireal_big_disks`,
`esxireal_multi_datastore` в `src/tests_esxi_real.cpp` (одно тело, параметры задаёт ключ `esxi_fs` файла servers.json). Сырые журналы: `.winvm-results/esxi7-round3-20261112/` (вне git).

### A. Гость с настоящей файловой системой и quiesce через VMware Tools

**Гость.** Alpine Linux virt 3.24.3, УСТАНОВЛЕННЫЙ НА ДИСК (`setup-disk -m sys`, автоматически: `guest-fs-build-disk.sh` ставит систему в локальном qemu по apkovl на КОПИИ образа, потом `guest-fs-setup.sh` кладёт диски на ESXi):
корень ext4 (1 ГиБ, SATA), **диск данных ext4 (512 МиБ, метка `vm2bdata`)**, open-vm-tools 13.0.0 **с пакетом `open-vm-tools-vmbackup`** (плагин quiesce; в образе его не было: пакеты скачаны с зеркала Alpine v3.24 вместе с зависимостями
скриптом `guest/fetch-apks.py` с проверкой подписи индекса `APKINDEX.tar.gz` (RSA-SHA1, ключи из initramfs образа) и контрольной суммы каждого пакета), сети нет. Скрипт quiesce `backupScripts.d/10-vm2b` пишет события freeze / thaw на serial.
Писатель `guest/fsw.c`: на каждую запись `seq` файл класса **s** (`write` + `fsync` + `rename` + `fsync` каталога: долговечен) и файл класса **l** (только `write` + `close`: лежит в кэше ОС), окно 400 последних записей, содержимое и размер псевдослучайны от `seq`, в файле CRC32; `fsw verify` проверяет все файлы.
Скорость записи во вложенном ESXi около 1-2 записей/с (fsync на thin-диске поверх qcow2).

**Что исправлено в самом стенде (находки по ходу).** (1) Установщик, оставленный в `/etc/local.d`, на КАЖДОЙ загрузке заново ставил систему и форматировал диск данных (`setup-disk -m sys` копирует /etc установщика): теперь `install.start` удаляется из установленной системы.
(2) Имена `sda` / `sdb` у SATA не детерминированы: диски находятся по размеру (установка) и по метке (`findfs LABEL=vm2bdata`, гость). (3) `/etc/vmware-tools/poweroff-vm-default` в Alpine СИМВОЛИЧЕСКАЯ ссылка на `statechange.sh`: запись «через» неё
превращала и `poweron-vm-default` в `poweroff`, гость выключал себя через 20 с после загрузки (в `vmware.log`: «The guest has requested that the virtual machine be powered off»); теперь ссылка сначала удаляется.

| Копия включённой ВМ (гость пишет) | quiesce = true (Tools работают) | quiesce = false |
|---|---|---|
| тест | `esxireal_guest_fs_copy` (`hot-q1`) | то же (`hot-q0`) |
| время копии / первый байт диска (клон) | 316,7 с / 80 с | 271,5 с / 35,6 с |
| объём / в архиве / томов | 1 610 661 888 / 66 376 580 / 7 | 1 610 661 888 / 76 021 460 / 7 |
| предупреждение продукта | нет | нет (quiesce не просили) |
| на хосте (`vmware.log`) | `TakeSnapshot ... quiesced=1`, `ToolsBackup: IDLE -> STARTED -> ... -> DONE`; в госте `QUIESCE freeze` -> `thaw` | без стадий ToolsBackup |
| `Filesystem features` в образе диска данных | **без `needs_recovery`**, состояние `clean` | **`needs_recovery`**, состояние `clean` |
| `e2fsck -fn` | 0 (140 файлов) | 0 (журнал не воспроизведён) |
| `mount -o ro,noload` (БЕЗ воспроизведения журнала) | s: 63 файла из 63 (1..63), **0 нарушений**; l: 63 из 63, **0 нарушений** | s: 402 файла, **1 пропуск, 3 повреждены**; l: 400, **6 повреждены** (один обрезан) |
| `mount -o rw` (воспроизведение) | в `dmesg` НЕТ «recovery complete» | **«EXT4-fs (loop4): recovery complete»** |
| после воспроизведения журнала | s: 63/63, l: 63/63, 0 нарушений | s: 401/401 (0 нарушений); **l: 3 файла из 400 пусты или повреждены** (данные кэша не дошли до диска) |

Итог: quiesce через настоящие Tools даёт **файловую систему без журнала-восстановления** и **все данные до снимка, включая не сброшенные гостем** (класс l: 63 из 63); без quiesce копия crash-consistent: журнал надо воспроизводить, часть данных кэша потеряна.
Заморозка в гостях такого стенда долгая: в `serial` гостя `freeze` -> `thaw` проходят за 6-18 секунд (синхронный диск поверх qcow2).

**Восстановленная копия загружается** (`hot-q1b`: та же копия с quiesce, восстановление на хост `RestoreToEsxi`, 1 312 с, затем ВМ включена): на `serial` восстановленной ВМ гость сам печатает `Filesystem features` диска данных **без `needs_recovery`**, `Filesystem state: clean`, `EXT4-fs (sdb): mounted filesystem ... r/w with ordered data mode` (**без «recovery»**),
`FSVERIFY s: n=400 min=5023 max=5422 gaps=0 bad=0 | l: n=400 min=5023 max=5422 gaps=0 bad=0` (все файлы до снимка целы и совпадают по CRC и содержимому, обеих классов), записей после снимка нет (следующая запись писателя 5423 = максимум + 1), `e2fsck -fn` без замечаний.

**(3) Ошибки quiesce, найденные на настоящем хосте.**
* **ВМ без VMware Tools** (`vm2b-on`, `quiesce=true`): настоящий ESXi 7.0.3 принимает запрос БЕЗ ошибки и делает снимок без заморозки; продукт не писал предупреждения (источник знал только об ошибке снимка), а копия оставалась crash-consistent под видом
  consistent. Исправлено: перед снимком с quiesce читается `guest.toolsRunningStatus`; если Tools не работают, в отчёте предупреждение «VMware Tools is not running in the guest: the snapshot is taken without quiescing (the host does not report an error for it), the copy is crash-consistent»
  (запрос quiesce всё равно отправляется: Tools могут просто опаздывать). **Свойство снимка `quiesced` на этом хосте ненадёжно**: у снимка, реально заморозившего гостя через vmbackup, оно `false` (проверено: `CreateSnapshot_Task quiesce=true` на ВМ с Tools -> `success`, в госте `freeze` / `thaw`, `quiesced = false`), поэтому по нему продукт не судит.
  Эмулятор: режим `quiesce_fail = silent` (хост без ошибки, Tools не работают), регрессия `esxi_snapshot_failure_paths`.
* **Сбой quiesce в госте / тайм-аут Tools** (под нагрузкой во вложенном ESXi: `msg.snapshot.quiesce.rpc_timeout`, «Failed to quiesce the virtual machine (29)»; повторялся при загруженном хосте): задача снимка заканчивается ошибкой, продукт повторяет БЕЗ quiesce и пишет «the quiesced snapshot failed (An error occurred while quiescing the virtual machine. See the virtual machine's event log for details.): taken without quiescing (...)»; копия
  crash-consistent: `needs_recovery`, «recovery complete» при монтировании, после воспроизведения s: 401/401 без нарушений, l: 54 файла потеряно (`hot-qfail2`). Остатков на хосте нет. Формулировка предупреждения дополнена («или quiesce не удался / вышел по времени в госте»).

**(4) Холодный режим на той же ВМ** (`cold-fs`): ВМ выключена через Tools (`ShutdownGuest`, скрипт `poweroff-vm-default`), копия по месту без снимка, 371,8 с, архив 80 496 436 байт; диск данных: **`needs_recovery` нет, state clean**, `e2fsck -fn` 0 (814 файлов), `mount -o ro,noload`: s 400/400, l 400/400, **0 нарушений** (файловая система размонтирована гостем при выключении).

### C. Восстановление на другой датастор, ВМ на двух датасторах

ВМ `vm2b-ms` (`multi-setup.sh`): `.vmx` и диск 1 (thin 128 МБ, 36 МБ случайных данных) на `datastore1`, диск 2 (zeroedthick 64 МБ, весь случайный) на `datastore2` (`scsi0:1.fileName = "/vmfs/volumes/datastore2/vm2b-ms/vm2b-ms_1.vmdk"`);
эталон sha256 снят на хосте. Холодная копия (ВМ выключена): 201 335 808 байт, в архиве 104 888 348 байт, 34-38 с; в архиве диск второго датастора лежит как `_other-datastores/datastore2/...`.
* **`esxi.restore_options`** (`EsxiRestoreTargets`, ключ теста `options`): `datastores: [{name: datastore1, free: 61 806 215 168, capacity: 68 451 041 280, type: VMFS, accessible: true}, {name: datastore2, free: 23 992 467 456, capacity: 25 501 368 320, type: VMFS, accessible: true}]`,
  `folders: [ha-folder-vm]`, `pools: [ha-root-pool "Resources"]`, `hosts: [ha-host]`, `datacenter: ha-datacenter`, `api_version 7.0.3.0`.
* **«На выбранный датастор»** (`restore_datastore = datastore2`): всё в него: `[datastore2] vm2b-ms-rtA/vm2b-ms.vmx`, оба диска там же; **sha256 обоих flat на хосте = эталону** (172,8 с).
* **«Как было»** (датастор не задан): `.vmx` и диск 1 в `[datastore1] vm2b-ms-rtB/`, диск 2 в **`[datastore2] vm2b-ms-rtB/`**, в `.vmx` `scsi0:1.fileName = "[datastore2] vm2b-ms-rtB/vm2b-ms_1.vmdk"`; sha256 обоих совпал (175,6 с).
* **Занятое имя** (`vm2b-ms-rtA` уже есть): отказ за 0,8 с «a VM or a datastore folder named "vm2b-ms-rtA" already exists on the server: choose another name (nothing is overwritten)»; ничего не записано.
* **Недостаточно места** (на `datastore2` оставлено 60 МБ: толстый файл-заполнитель): **найдена ошибка продукта**. Старый код не проверял свободное место: создавал папку, начинал загрузку и через **894 секунды** (повторы) получал от хоста пустой «HTTP 500 Internal Server Error» на втором файле
  («uploading [datastore2] vm2b-ms-rtC/vm2b-ms_1-flat.vmdk: HTTP 500 Internal Server Error»), понять причину по тексту нельзя. Исправлено: (1) перед созданием папки продукт считает по списку файлов копии, сколько байт попадёт на каждый датастор (каждый файл полного размера: PUT пишет и нули thin-диска) и сравнивает со свободным местом;
  (2) hostd держит свободное место датастора в кэше (после создания файла в 22 ГБ `summary.freeSpace` ещё минуты показывал 22 ГБ): перед проверкой вызывается `RefreshDatastore` для каждого датастора. Теперь: отказ за **0,9 с** «not enough free space on the datastore "datastore2": the restore needs 193 MB there and 60 MB are free (nothing was written): choose another datastore or free some space»,
  на хосте не остаётся ни папки, ни файла. Эмулятор: `RefreshDatastore`, переопределение `ds_free`, регрессия в `esxi_restore_refusals_and_clean_failures` (отказ до создания папок, `noTrace`).

### B. Диски больше 4 ГиБ, толстые диски, несколько дисков

ВМ `vm2b-big` (`big-setup.sh`, без ОС): диск 1 **thin 6 ГиБ** (`vm2b-big-flat.vmdk` 6 442 450 944 байт на VMFS; разреженный, семь случайных областей по 20 МиБ: в начале, на 1000, 2047, **4090 МиБ (ПЕРЕСЕКАЕТ границу 4 ГиБ)**, 4100, 5000 и 6100 МиБ; занято на VMFS 133 МБ),
диск 2 **zeroedthick (lazy-zeroed) 1 ГиБ** (2 x 40 МиБ случайных), диск 3 **eagerzeroedthick 1 ГиБ** (50 МиБ случайных). Эталон sha256 логического содержимого (flat-файлов) снят на самом хосте при выключенной ВМ (`sha256sum`, 8 ГиБ). Датастор 64 ГиБ, на 8,0 ГиБ дисков
(thin 6 ГиБ при восстановлении на хост занимает на VMFS все 6 ГиБ: PUT пишет нули) места хватало. Каждая копия: 8 589 945 856 ... 8 589 976 576 байт потока (2^33 + заголовки tar), в архиве 274 МБ. Все прогоны: **sha256 всех трёх flat-дисков после восстановления в папку совпал с эталоном хоста**.

| Режим | Время копии | В архиве / томов | Результат |
|---|---|---|---|
| холодная (ВМ выключена, диски по месту), xz 1, без пароля; verify 28,0 с; восстановление в папку 60,2 с | 977,2 с (16,3 мин) | 274 138 552 / 33 | sha256 6 442 450 944 + 1 073 741 824 + 1 073 741 824 = эталон |
| **восстановление на хост** (`RegisterVM_Task`, `PUT /folder` thin-диска 6 442 450 944 байт, Content-Length > 2^32, затем `HEAD`) | 6 913,2 с (115 мин: PUT нулей по виртуальной сети стенда около 1,3 МБ/с; на скорость продукта это не указывает) | — | ВМ `vm2b-big-rt1` зарегистрирована; **sha256 на хосте = эталон** у всех трёх, размеры 6 442 450 944 / 1 073 741 824 / 1 073 741 824 |
| холодная, **xz 7 + пароль + тома по 100 МБ** | 1 472,3 с (24,5 мин) | 273 902 336 / 82 | verify 41,4 с; sha256 = эталон |
| **горячая** (ВМ включена, `quiesce=false`: снимок + `CopyVirtualDisk` ТРЁХ дисков, клон 6 ГиБ thin за 23,0 с), xz 1 | 1 229,4 с (20,5 мин) | 274 143 200 / 33 | verify 28,9 с; sha256 = эталон; остатков `VM2Backup-*` нет |
| холодная, **пауза после 4 400 МБ потока** (внутри первого flat > 4 ГиБ; 624,6 с), затем продолжение | 559,2 с | 274 141 636 / 33 | `resumes = 1`, «continued: the VM stayed shut down and locked since the interrupted run»; sha256 = эталон |

32-битного переполнения нигде нет: размеры > 2^32 в manifest и tar (проверено побитовым совпадением), `Content-Length` и `HEAD` > 4 ГиБ на `PUT /folder` и `GET /folder`, `Range`-продолжение с позиции больше 4 ГиБ, `DatastoreFile`; ошибок продукта пункт B не нашёл.
Время копии определяется чтением хоста через HTTP (около 8 МБ/с во вложенном ESXi), а не размером дисков: толстые диски читаются целиком, разреженные нули читаются так же (HTTP `/folder` отдаёт нули thin-диска).

## Проверено на настоящем vCenter 7.0.3 (раунд 3, 13.11.2026): права ниже администратора

Стенд: VCSA 7.0.3.02500 (build 24927011, отдельная qemu-ВМ, 12 ГиБ) + ОДИН вложенный ESXi 7.0.3 (4 ГиБ, 2 vCPU), датацентр `vm2b-dc`; пользователи SSO `vm2b-viewer`, `vm2b-backup`, `vm2b-hot`, `vm2b-restore` (локальные, `dir-cli`), роли и назначения
`tools/esxi7/roles.py`, автоматический поиск `tools/esxi7/roles-find.py` (роль растёт по одной привилегии за прогон: продукт сам называет недостающую в ошибке), прогон `roles-try.sh`. Все роли назначены на КОРНЕВУЮ папку с наследованием (`propagate = true`);
ниже «где» — `Global.*` проверяются ТОЛЬКО на корневой папке (проверка блокировки от 07.11.2026), остальные права достаточно на корне с наследованием. Журналы: `.winvm-results/esxi7-round3-20261112/` (вне git).

### Роли-шаблоны (проверены на настоящем vCenter: каждая выполняла описанную работу без предупреждений о правах)

| Роль | Привилегии (идентификаторы vSphere) | Что умеет |
|---|---|---|
| **Viewer** (только чтение) | `System.Anonymous`, `System.View`, `System.Read` | вход и список ВМ (`vcreal_connect_and_list`: «VMware vCenter Server 7.0.3 build-24927011»); любая копия отказывает с названием права (ниже) |
| **Backup (cold)** — холодный режим, в том числе включённая ВМ | Viewer + `VirtualMachine.Config.AdvancedConfig`, `VirtualMachine.Config.Annotation` (метка «удерживается для копии» в `.vmx` и аннотации), `Datastore.Browse`, `Datastore.FileManagement` (чтение файлов ВМ через `/folder`), `VirtualMachine.Interact.PowerOff`, `VirtualMachine.Interact.PowerOn` (выключить и вернуть включённую ВМ), `Global.DisableMethods`, `Global.EnableMethods` (блокировка методов на ВМ; ТОЛЬКО на корневой папке, без них копия идёт с меткой без блокировки) | холодная копия выключенной и включённой ВМ (проверено на `vm2b-off`, `vm2b-hist` и работающей `vm2b-on` с принудительным выключением: 344 с, предупреждение только «VMware Tools is not running in the guest...») |
| **Backup (hot)** — горячий режим (снимок + клон диска) | Viewer + `Datastore.Browse`, `Datastore.FileManagement`, `VirtualMachine.State.CreateSnapshot`, `VirtualMachine.State.RemoveSnapshot` | горячая копия `vm2b-on` без `VirtualMachine.Config.*` и без привилегий `Provisioning`: снимок, `CopyVirtualDisk` клона (привилегии на хранилище достаточны: `Datastore.FileManagement`), чтение клона, удаление клона и снимка; 35 с; снимков и клонов после копии нет |
| **Restore** — восстановление в vCenter | Viewer + `Datastore.Browse`, `Datastore.FileManagement` (создание папки, `PUT /folder`), `VirtualMachine.Inventory.Register`, `Resource.AssignVMToPool` | `RegisterVM_Task` восстановленной ВМ в папку и пул (`vm2b-rtE`, ~30 с); `Datastore.AllocateSpace` не понадобился (проверено без него) |

Пользователь продукта для обоих режимов копии получает объединение Backup (cold) и Backup (hot); роль Restore обычно выдаётся отдельному учётному записи.

### Что отказывает без права и как об этом говорит продукт (опыт раунда: `roles-find.py`)

| Роль / шаг | Вызов | Ответ сервера | Текст ошибки продукта |
|---|---|---|---|
| только Viewer, холодная копия | `ReconfigVM_Task` (метка) | `NoPermission`, `privilegeId = VirtualMachine.Config.AdvancedConfig`, затем `VirtualMachine.Config.Annotation` | «the VM could not be marked as held for the backup: the user lacks the privilege "VirtualMachine.Config.AdvancedConfig"...» |
| без `Datastore.FileManagement` | `GET /folder` | **HTTP 401** (имя права не названо, повторный `Login` с cookie отвечает `InvalidLoginReasonRegisterFailed`) | **до раунда: «login failed: the user name or password was refused by the server» (неверная диагностика)**; теперь «the datastore file access was refused (HTTP 401) for the datastore "datastore1": the user most likely lacks the datastore privilege Datastore.FileManagement on it (the server names no privilege for file requests)...» |
| `Datastore.FileManagement` без `Datastore.Browse` | `HEAD /folder` | HTTP 404 (сервер прячет файлы) | «the file is missing on the datastore (or the user lacks the datastore privilege Datastore.Browse, with which the server hides files)» |
| нет `Global.EnableMethods` / `Global.DisableMethods` | `EnableMethods` / `DisableMethods` | `NoPermission`, `privilegeId = Global.*` | копия идёт; предупреждение «the lock "..." could not be removed (the user lacks the privilege "Global.EnableMethods"...)» |
| включённая ВМ, нет `PowerOff` | `ShutdownGuest` / `PowerOffVM_Task` | `NoPermission` | «the guest cannot be shut down (the user lacks the privilege "VirtualMachine.Interact.PowerOff"...)» |
| нет `PowerOn` | `PowerOnVM_Task` после копии | `NoPermission` | предупреждение «the VM could not be powered on again: the user lacks the privilege "VirtualMachine.Interact.PowerOn" ... (it was running before the backup)» (ВМ остаётся выключенной!) |
| горячая, нет `CreateSnapshot` | `CreateSnapshot_Task` | `NoPermission` | «creating the snapshot was refused by the server: the user lacks the privilege "VirtualMachine.State.CreateSnapshot" (...). Give the user a role with this privilege» |
| горячая, нет `RemoveSnapshot` | `RemoveSnapshot_Task` | `NoPermission` | **до раунда: ничего (копия «успешна», снимок молча остаётся на ВМ)**; теперь предупреждение «the snapshot VM2Backup-... could not be removed from the VM (removing the snapshot failed: the user lacks the privilege "VirtualMachine.State.RemoveSnapshot" ...): it stays on the VM and the next backup will try to remove it first» |
| восстановление, нет `Datastore.FileManagement` | `MakeDirectory` | `NoPermission` | «cannot create the folder [datastore1] vm2b-rtD on the datastore: the user lacks the privilege "Datastore.FileManagement"» |
| нет `Resource.AssignVMToPool` / `VirtualMachine.Inventory.Register` | `RegisterVM_Task` | `NoPermission` | «registering the VM failed: the user lacks the privilege "Resource.AssignVMToPool"» / «...VirtualMachine.Inventory.Register» (папка ВМ и загруженные файлы при этом удаляются) |

**Найдено и исправлено по ходу:** (1) `NoPermission` не называл право: теперь `privilegeId` из `detail` (SOAP-сбой) и из `info.error.fault` (задача) попадает в текст; (2) неверный «login failed» при HTTP 401 / 403 от `/folder` (и бесконечная рекурсия `readSmall` при повторном 401 после успешного входа); (3) молчаливый остаток снимка при отказе `RemoveSnapshot`;
(4) повторная попытка восстановления после отказа по правам (состояние возобновления сохранено, папка не была создана) говорила «the folder of the unfinished restore is gone...: remove the restore state and start again»: теперь, если прежний запуск ничего не создал, восстановление просто начинается заново.
Эмулятор и регрессии: `no_permission` (`esxi_snapshot_failure_paths`, kind 6: текст называет право и роль), `remove_fail` теперь проверяет предупреждение «could not be removed from the VM».
