# Tests on a real Windows VM

Everything else in this project runs under Wine or against fakes. This harness builds a **real Windows** test VM from the user's ISOs, installs the package
(`dist/VM2Backup-Setup.exe`) on it and runs the whole plan: the service under the real Service Control Manager, the named pipe ACL, the panel on a real desktop,
the web site with Windows' own TLS clients, DPAPI, the native autotests, the network groups against servers on the Linux host, a **real SMB share**
(`WNetAddConnection2W`) and the installer's update / removal.

    tools/run-winvm.sh x86            # Windows 10 x86 (WIN10X86.iso), plain qemu inside the dockurr/windows image
    tools/run-winvm.sh w10            # Windows 10 x64 ru business 22H2 (dockur)      [одна команда с нуля проверена 27.10.2026: раздел «Раунд 3»]
    tools/run-winvm.sh w11            # Windows 11 5in1 25H2 (dockur)                 [одна команда с нуля, 21 фаза: раздел «Раунд 4: Windows 11», 27.10.2026]
    tools/run-winvm.sh x86            # ОДНА команда с нуля: создание -> ожидание установки -> все фазы -> удаление (раунд 2, проверено 26.10.2026)
    tools/run-winvm.sh x86 --phases sysinfo,web --keep      # only some phases; --keep leaves the VM
    tools/run-winvm.sh x86 --no-create --phases smb --keep  # the VM is already up (after --keep)

First `./package.sh` (the run refuses a package older than the sources). The run creates the VM, waits for the Windows installation (30-60 min for x86), runs the
phases, prints a summary, copies every result to `.winvm-results/<os>-<time>/` and then **deletes the container `vm2b-win`, its storage disk and the custom ISOs**
(`.winvm/destroy.sh`; the user's rule: no test VM stays, whatever the result). Only ONE VM at a time; the container name is exactly `vm2b-win` (the other projects own
`c2s-win`, `s2-win`, `ra2-*`: never touch them). RAM 3-4 GB, 4 cores, VNC `127.0.0.1:5908`, web console `8008` (dockur), qemu port forward `18443` (x86).

## The harness (`.winvm/`)

| File | Job |
|---|---|
| `run-os.sh <os>` | creates the VM (refuses when `vm2b-win` exists or `/home` has < 30 GB free) |
| `x86/qemu-run.sh`, `x86/answer/` | the x86 VM: plain qemu (IDE disk, e1000, std VGA, `-m 3G -smp 4`), the user's ISO as is, a second tiny CD (`answer.iso`) with `autounattend.xml` (DiskID 0, en-GB, autologon user `Docker` / `admin`, a copy of the executor bootstrap) |
| `oem/install.bat` | the same bootstrap for the dockur systems (runs once after the install) |
| `shared/` | the folder the guest sees as `\\10.0.2.4\qemu` (x86) or `\\host.lan\Data` (dockur): `jobs/` (host -> guest), `results/` (guest -> host), `pkg/` (payload), `srv/` (test servers' files) |
| `shared/poller.bat` | the **executor**: a scheduled task `VM2BPoller` at the logon of the autologon user (so it runs in the *interactive session*, elevated); takes `jobs/*.cmd` one by one, output -> `results/<job>.out`, exit code -> `results/<job>.rc` |
| `job.sh <name> <file|-> [timeout]` | runs one batch job on the guest and prints its output / exit code |
| `wait-ready.sh`, `shot.sh [png]`, `sync-pkg.sh <os>`, `destroy.sh [os]` | wait for the executor, screenshot through the qemu monitor, copy the payload, delete everything |
| `guest/*.ps1, *.cmd` | what runs on the guest: `sysinfo`, `install`, `service`, `pipeacl`, `panel`, `web`, `web-bind`, `dpapi`, `autotest`, `uninstall-update`, `kept`, `lib.ps1` (web client helpers), `shot.ps1` (screenshot from inside the desktop session) |
| `guest/esxilocal.ps1` | (раунд 2) эмулятор ESXi / vCenter ВНУТРИ гостя: python embeddable (`.winvm/pyembed/`, кеш с python.org), скрипты эмулятора и готовые сертификаты из `shared/pkg/esxi/`; данные датастора на локальном NTFS |
| `guest/acltest.ps1`, `guest/svcacct.ps1` | (раунд 2) права на папку данных от обычного пользователя; служба под другими учётными записями |
| `guest/smbsign.ps1`, `guest/restartjob.ps1` | (раунд 2) обязательная подпись SMB; перезапуск Windows посреди копии |
| `guest/aclreal.ps1`, `guest/aclcheck.ps1`, `guest/speedlimit.ps1` | (раунд 3, x64 ru) права папки данных / канала от обычного пользователя с кириллическим именем и проверка SDDL (`aclcheck.ps1 old|new` вызывается и из `uninstall-update.cmd`); ограничение скорости на реальных данных |
| `guest/signinst.ps1`, `leaks.ps1`, `ntfsav.ps1`, `defender-toggle.ps1`, `tzreal.ps1`, `tools/winvm-variants.sh` | (раунд 8) подписанный пакет и его варианты; утечки; NTFS / EFS / EICAR; Defender на время проверки; часы и пояса. Файлы `*.ps1` с кириллицей хранятся в UTF-8 С BOM (PowerShell 5 иначе ломает строки) |
| `destroy.sh` + файл `.winvm/hold` | пока файл `hold` существует, `destroy.sh` ничего не удаляет (чтобы дочинить / перепроверить фазы на той же ВМ после одной длинной команды); убрать файл и запустить `destroy.sh` ещё раз |
| `shared/smb-real.py` | runs inside the container: a password-protected Samba share + a control port (stop / start the server while a backup runs) |
| `tools/winvm-servers.sh` | the ftp / ssh / esxi test servers on THIS host, listening on the Docker bridge address, files under `.winvm/shared/srv/` |

The network (x86, qemu user mode): guest `10.0.2.15`; `10.0.2.2` = the container's loopback (the SMB control port `10.0.2.2:4456`); `10.0.2.4` = the exchange share; any other
address is NATed out of the container, so the Linux host is `172.17.0.1`; `10.0.2.50` / `10.0.2.51` (guestfwd) = the test Samba (two *names* for one server, because Windows allows one
credential set per server name); `10.0.2.77` = nothing (unreachable-server test). Container port `18443` -> guest `8443` (host curl to the site on the guest's real address).

### Network test servers on the host (`VM2B_*` variables)

The launchers (`tools/testservers/launch.py`, `esxi_launch.py`, `pm_server.py`, `tools/run-ssh-tests.sh`) take, with the defaults unchanged (127.0.0.1, `Z:` paths):
`VM2B_BIND_ADDR` (listen address), `VM2B_SERVER_HOST` (the address written to `servers.json`, and into certificates' subject alternative names; default = bind address),
`VM2B_GUEST_MAP=<host prefix>=<guest prefix>` (paths in `servers.json` as the guest sees them: the server folders are under `.winvm/shared/`, reached as UNC), `VM2B_SERVERS_ONLY=1`
(`run-ssh-tests.sh`: start the servers, write `servers.json`, wait to be killed; no wine, no tmpfs).

## Phases and what they verify

| Phase | What | Result means |
|---|---|---|
| `sysinfo` | `ver`, build / UBR, arch, session, adapters, TLS registry, PowerShell / .NET, curl | recorded in `01-sysinfo.out` |
| `autotest` | `vm2bautotest.exe` natively on the clean guest, fresh seed: `core,engine,resume,restore,xz,smb,web,svcweb,panel`; `service` (the tests that are SKIPPED under Wine run for real: `exe_scm_install_start_stop`, `exe_console_mode`, `default_pipe_security`); `gui` (`VM2B_GUI_TESTS=1`, interactive session) | every FAIL is a product or test problem on real Windows |
| `install` | silent install, files, `sc qc / qfailure / sdshow`, idle stop, **STOP_PENDING during a running job**, job resumed after the restart, crash -> SCM recovery restart (60 s), pipe ACL (standard user denied, SYSTEM and admin allowed, DACL read back) | |
| `panel` | `/selftest`, `/guitest` on the real desktop, screenshots at 100 / 125 / 150 % (`LogPixels` + restart) | `results/shots/*.png` |
| `web` | `SslStream` per protocol (TLS 1.0-1.3), `curl.exe` (Schannel), `Invoke-WebRequest`, `HttpClient`, login admin/admin, `/api/me`, state, `web.interfaces.list` vs `Win32_NetworkAdapterConfiguration`, bind to the real address + host curl, **DPAPI**: a server with a password is saved, `config.json` must not hold it, it must still work after a service restart and after a restart of Windows | |
| `net` | `ftp,tls,http`, `ssh,scp`, `esxi,svcesxi` on the guest against the host's servers | |
| `smb` | group `smbreal` (`src/tests_smb_real.cpp`) against the real Samba share + `net use Z:` and `net.drives` | |
| `installer` | update in place over the running service, `/remove /silent` (data kept), reinstall | |
| `defender` | (w11) Defender в конце прогона: защита в реальном времени включена, нет исключений и обнаружений | `16-defender.out` |
| `signing` | (w11) подписанный установщик (`tools/sign.sh`, тестовый центр из `tools/make-test-cert.sh`) с Mark of the Web, четыре сочетания: (c) без подписи, (a) подписан, корень не доверен, (b) подписан, корень в `Root` + издатель в `TrustedPublisher`, (d) подписан, доверен, файл изменён после подписи; снимки SmartScreen и UAC. На время фазы включается UAC (на dockur `EnableLUA=0`) с перезапуском и затем возвращается | `20-sign-*.out`, `signing-observed.txt`, `shots/signing/*.png` |
| `signinst` | (раунд 8) подписанный пакет на Windows 10: `Get-AuthenticodeSignature`, тихая установка (32-битный загрузчик на x86), подписи внутри Program Files, обновление поверх старой версии, файл с Mark of the Web, изменённые после подписи файлы (`guest/signinst.ps1`, варианты `tools/winvm-variants.sh`) | `21-signinst.out` |
| `leaks` | (раунд 8) 36 минут нагрузки, замер каждые 2 минуты счётчиков службы и долгоживущей панели (HandleCount, потоки, закрытая память, WorkingSet, GDI / USER); панель открыта и закрыта не меньше 200 раз, служба перезапущена 2 раза (`guest/leaks.ps1`) | `22-leaks.out`, `leaks.csv` |
| `ntfsav` | (раунд 8) файл под EFS, NTFS-сжатый и разреженный файлы, имя 255 символов, каталог с 5000 файлов, файл исчез посреди копии (`guest/ntfsav.ps1`) | `23-ntfsav.out` |
| `eicar` | (раунд 8) EICAR при ВКЛЮЧЁННОМ Defender (политика bootstrap.cmd на время снимается, два перезапуска) и «файл исчез по ходу» (`ntfsav.ps1 av`, `defender-toggle.ps1`) | `23b-eicar.out` |
| `ftprepeat` | (раунд 8) `ftp_large_streamed` 10 раз с журналом FTP-сервера DEBUG | `srv-ftprepeat/run-N/` |
| `tz` | (раунд 8) пояса с переходом на летнее время: родные тесты расписания в трёх поясах + настоящие службы и часы (`guest/tzreal.ps1`, w32time выключен, `SetSystemTime`) | `25-tz-*.out` |
| `sac` | (w11, ВСЕГДА ПОСЛЕДНЯЯ: необратима) включение Smart App Control через настоящий интерфейс «Безопасность Windows» (переключатель On, UI Automation), перезапуск, запуск вариантов установщика, службы и программ продукта без доверия к тестовому корню и с ним, пробы со свежесобранными неподписанными exe, события CodeIntegrity | `99-sac-*.out`, `shots/sac-*.png` |

### Group `smbreal` (servers json key `smb`; skipped without it, e.g. under Wine)

`{unc, user, password, wrong_password, alt_unc, unreachable_unc, control, drive}`: backup / verify / restore with xz (plain and encrypted) through `NewSmbTarget`; wrong password -> permanent
1326, no password in any message; unreachable server -> transient; unknown share -> permanent; the server is **stopped in the middle of a backup and started again** (control port) -> the
engine resumes; `net.browse` of the share and of the server with credentials; `net.drives` lists `Z:` (mapped with `net use` in the interactive session) with its UNC path; five
concurrent targets; the service's `servers.save / servers.test / tasks.run / backups.verify / backups.restore` over the real share, no secret in any answer.

## Other systems (w10 / w11): dockur wiring (tested on w10)

`run-os.sh w10|w11`: dockur/windows with the user's ISO (`-v ISO:/boot.iso`), `DISK_SIZE=32G`, `RAM_SIZE=4G`, share `\\host.lan\Data` (= `/shared`), x64 payload (`sync-pkg.sh` copies `build/x64`).
The install is unattended (dockur's `win10x64-enterprise.xml`, user `Docker`, autologon, `oem/install.bat` runs once and restarts): **19 min** to a working executor. Network: guest `172.30.0.2`,
gateway = the container `172.30.0.1` (`host.lan`), the container NATs to the Docker host `172.17.0.1`, so the test servers' addresses are as for x86. What differs from the qemu variant:

* **Real SMB (`smb-real.py`)**: dockur's own smbd owns `172.30.0.1:445`. The script adds secondary addresses `172.30.0.50` / `.51` to the bridge `docker` and forwards their :445 to the test smbd (127.0.0.1:4455) with a small
  TCP proxy; the control port listens on `172.30.0.1:4456`; `172.30.0.77` is the "unreachable" address. `run-winvm.sh` writes these into `servers-smb*.json` (`SMB1/SMB2/SMBX/SMBCTL`). The mapped drive test uses **`Y:`**
  (dockur maps `Z:` to `\\host.lan\Data` persistently: `net use Z:` waits for a Y/N answer and hangs the executor).
* **Host -> guest** (web bind test): the container is on the guest's subnet, so `curl https://172.30.0.2:8443` from inside the container (no port forward).
* **SMB client directory cache**: with dockur's Samba the guest sees a new `jobs\*.cmd` only after ~2 minutes. `00-init` and `oem/install.bat` set `DirectoryCacheLifetime / FileInfoCacheLifetime /
  FileNotFoundCacheLifetime = 0` (`Set-SmbClientConfiguration`, effective at once): then a job starts in seconds.
* **Defender stays ON** (real-time protection, no exclusions; `oem/install.bat` no longer sets `DisableAntiSpyware`). Never put `Add-MpPreference` / Defender policy keys into a job (see the Click2Screen notes).
* **Keyboard layout is Russian** (`РУС`): debugging through the monitor (`type.sh` types into the guest, `shot.sh` takes a screenshot: the dockur monitor socket is `/run/shm/monitor.sock`) needs `alt-shift` first.
* `reboot_guest`, the poller and the phases are the same. Order that gives the most valuable results first: `--phases sysinfo,install,panel,web` (each with `--no-create --keep`), then `/remove /silent` the product
  (the `autotest` phase wants a clean guest), `autotest`, `smb`, `unicode`, `installer,net`.

### Russian Windows: what the harness must not assume

`sc` / `net` / `systeminfo` output is **localized** (`Состояние`, `Имя_службы`, `перезапуск`): only the keywords `RUNNING`, `STOPPED`, `STOP_PENDING`, `AUTO_START (DELAYED)`, `LocalSystem`, `WIN32_OWN_PROCESS` stay English.
The parsers (`service.ps1`, `waitsvc.ps1`, `uninstall-update.cmd`, `install.cmd`) match the state names / counts, not the labels, and use WMI for the PID. The console is CP866: `job.sh` converts the output
with `iconv -f CP866` for display (the stored `.out` files are raw: read them with `iconv -f cp866 -t utf-8 -c`, grep them with `-a`). Group names are not used (SDDL `BA` / `SY`; `icacls ... '*S-1-1-0'`).
The phase `unicode` (`guest/unicode-user.ps1`) creates a local administrator with a Cyrillic name and runs the native `core` group under its profile `C:\Users\Юзер-Тест`.

## Known limits of the harness (what is NOT a product problem)

* **Active-mode FTP** (`ftp/ftp_passive_active_and_listing_variants`, the "active ftp*" parts) cannot work: the guest sits behind qemu's NAT (10.0.2.15), the server refuses a data
  connection to an address other than the client's ("501 Rejected data connection to foreign address"), and could not reach the guest anyway. Everything else of the FTP groups passes.
* **Five ESXi tests** (`esxi_guest_writes_are_frozen_out`, `esxi_resume_across_runs`, `esxi_user_snapshot_chain_and_middle_leftover`, `esxi_two_vms_with_the_same_archive_prefix`,
  `esxi_branching_snapshot_tree`) compare the server's datastore folder with the restored files and read that folder through the SMB share: (1) the Windows SMB client serves a file the
  server changed in place (same size) from its cache for ~10-20 s (measured: hash of a 1 MB file stays old; `FileInfoCacheLifetime=0`, `UtilizeNtCaching=0`, Samba `oplocks / smb2 leases = no`
  do not help), (2) names with characters Windows cannot hold (`pfx:vm`) arrive as mangled 8.3 names. They pass natively under Wine (62 of the 67 ESXi tests pass on the VM). The product's
  own paths (HTTP / SOAP to the mock) are not involved. A fix would be to run the mock datastore inside the guest.
* **(раунд 8, СНЯТО в раунде 9) Тесты `ssh/auth_guard_scp_*` на ВМ** читали журнал `sshd` через SMB, и кеш клиента SMB отдавал старый файл. Теперь журнал читается по TCP (`tools/testservers/logread.py`, `servers.json` `x.authlog_tcp`): тесты детерминированы и на госте (64 из 64). `scp_fault_drop_listing` тоже детерминирован (обрыв всегда внутри листинга).
* **(раунд 8) Defender на тестовых ВМ выключен политикой** `DisableAntiSpyware=1` (`bootstrap.cmd`): служба не запущена. Для EICAR фаза `eicar` снимает политику на время проверки (перезапуск) и возвращает.
* **(раунд 8) SmartScreen на тестовых ВМ выключен политикой** `EnableSmartScreen=0` (`bootstrap.cmd`): окна «Windows protected your PC» нет. Фаза `signing` на Windows 10 включает его на время фазы.
* **(раунд 8) Жёсткий `system_reset` гостя оставляет открытые сеансы SMB прежнего гостя**: qemu-smbd отвечает NT_STATUS_IO_TIMEOUT при открытии `current.txt` / `*.out`, исполнитель встаёт. Перезапускать только `shutdown /r` или `system_powerdown` + `docker start vm2b-win`.
* **(раунд 8) Windows возвращает время «с узла»** через 1 - 3 минуты после `SetSystemTime`, если не выключить службу w32time (событие `Kernel-General` 1, процесс `svchost.exe`): `tzreal.ps1 prep` её выключает и останавливает. **Раунд 9:** на dockur-ВМ (есть интернет) Windows САМА возвращала `w32time` в «Автоматически» через ~45 с, поэтому дополнительно закрывается исходящий UDP 123 (правило `vm2b-block-ntp` в `tzreal.ps1`).
* The guest sees the host as `172.17.0.1` through qemu's NAT: connection resets / half closes pass through qemu's user-mode TCP, so timing-sensitive network tests can differ from loopback.
* **Windows Update** is switched off by the bootstrap (`wuauserv`, policy): without that the guest installed a cumulative update by itself in the middle of the first run (19045.3803 -> 19045.6456) and
  spent 40 minutes in "Preparing to configure Windows" after a restart.
* The window test `gui/gui_service_stops_and_restarts` failed once on the FIRST run after new binaries were copied to the guest (3 of 3 first runs, never in 40 later runs; its checks of the button state were racy and are now waited for): treat a failure there on a fresh copy as a flake and run it again.

## Findings of the first run (Windows 10 Pro x86 22H2, build 19045.3803, later 19045.6456; 32-bit payload)

Timings: Windows installation 26 min; sysinfo 1 min; autotest core / service / gui 14 / 3 / 1 min; install + service + pipe 7 min; panel at three scalings 15 min (three restarts); web + DPAPI 20 min;
ftp / ssh / esxi 10 / 9 / 33 min; smbreal 3 min; installer end 3 min. The whole plan: about 3 hours plus the install.

| Area | Result |
|---|---|
| Installer (silent), files, SCM registration | PASS: `AUTO_START (DELAYED)`, LocalSystem, quoted path, 3 recovery actions (60 s, 60 s, 300 s), description, uninstall entry, shortcuts |
| Service | PASS: idle stop 0.5 s (STOP_PENDING -> STOPPED), stop with a running task job 1.4 s, the task continues after the start and ends Success, `net stop/start`, kill -> restarted by the SCM after 60 s (300 s from the third failure within a day) |
| Pipe ACL | PASS: DACL exactly `(A;;FA;;;BA)(A;;FA;;;SY)`; standard user DENIED, SYSTEM and administrator connect |
| Panel | PASS: `/selftest`, the window test (122 + 25 checks) on the real desktop at 100 / 125 / 150 %; screenshots in `results/shots/` |
| Web | PASS: TLS 1.2 (Schannel, `curl.exe`, `Invoke-WebRequest`, `HttpClient`); TLS 1.0 / 1.1 refused; TLS 1.3 refused by the Schannel client of this build (no TLS 1.3 client in 19045), login / session / CSRF / RPC, `web.interfaces.list` contains the real adapters (10.0.2.15, IPv6), bind to the real address reached from the host (HTTP 200), `/reset-web` recovery |
| DPAPI | PASS: the SMB password is `dpapi:...` in `config.json`, nowhere in clear (config, other files, `servers.list`), still works after a service restart and a restart of Windows |
| Autotests (clean guest) | core / engine / resume / restore / xz / smb / web / svcweb / panel / service / gui: 2 FAIL found real problems (below); the SKIPPED-under-Wine `exe_scm_install_start_stop`, `exe_console_mode`, `default_pipe_security` PASS |
| Network groups | ftp / tls / http 32 PASS (+1 active-mode limit); ssh / scp all PASS; esxi 62 PASS, 5 limits (above) |
| Real SMB | PASS: 7 + 1 tests (`smbreal`): round trip, 1326, unreachable (transient, ~55 s for Windows to give up), missing share 67, server stop / start mid backup, browse, `net.drives`, concurrency, service |
| Installer end | PASS: update in place over the running service (1297 -> 299.28, new PID, data kept), `/remove /silent` (service and files gone, data kept), reinstall |

Product problems found by the real Windows and fixed (each with a regression test):

1. `ReadWholeFile` / `WriteWholeFile` (`src/base.cpp`) opened paths of 240+ characters without the `\\?\` prefix: real Windows refuses them (Wine does not). Failed `engine/restore_destination_path_edge_cases` and
   `engine/tar_volume_boundaries_exhaustive` (250 of 250 combinations). Regression: `core/whole_file_helpers_accept_long_paths` (`src/tests_winreal.cpp`).
2. A share that does not exist on a Samba server is reported by Windows as error 53 (the same as "the server does not answer"), so the target classed it transient and retried for minutes with the wrong message
   ("cannot reach the server"). `SmbApi::probe` (TCP 445 reachable?) turns that 53 into 67 (permanent, "the share ... was not found"). Regressions: `smb/missing_share_on_a_live_server_is_permanent`
   (fake API) and `smbreal/real_unreachable_server_is_transient` (real).

Test problems fixed: `service/restart_continues_a_running_job` read the history right after the backup became visible (the entry is written a moment later: a race that only a fast real disk shows);
`ssh/scp_fault_*` used a hard-wired loopback address for the fault control port (now `servers.json` `host`); `gui/gui_service_stops_and_restarts` checked the button state at the instant the label changed (the buttons follow a moment later: now waited for).

## Findings of the second run (Windows 10 Enterprise ru x64 22H2, build 19045.3570, 32-bit launcher + 64-bit payload)

Timings: install 19 min; sysinfo 1; install+service+pipe 3; panel at three scalings 17 (three restarts); web + DPAPI + restart of Windows 6; autotest core / service / gui 21 total (core is slow
with Defender on); smbreal 12 (the 55 s unreachable-server test dominates), drive test 1; unicode 0.4; installer 1; ftp 2, ssh 8. esxi skipped. Whole run about 1 h 50 min including harness repairs.

| Area | Result |
|---|---|
| Installer (silent), Program Files (x64 payload), SCM | PASS: `C:\Program Files\VM2Backup`, `AUTO_START (DELAYED)`, LocalSystem, 3 recovery actions (60/60/300 s), service sdshow as on x86 |
| Service | PASS: idle stop, busy stop (STOP_PENDING), start, SCM recovery restart after kill (29 of 30 checks in the first pass: the one failure was the localized `RESTART` in the harness, fixed) |
| Pipe ACL | PASS: `D:(A;;FA;;;BA)(A;;FA;;;SY)`; standard user denied, `NT AUTHORITY\СИСТЕМА` and the administrator connect |
| Panel | PASS: `/selftest` and the window test at 100 / 125 / 150 %, screenshots `results/shots/` |
| Web | PASS (37 checks): TLS 1.2 via SslStream, `curl.exe`, `HttpClient`; TLS 1.0 / 1.1 refused; **TLS 1.3 refused by the Schannel client of 19045.3570** (`SEC_E_ALGORITHM_MISMATCH`, expected), login, `/api/me`, bind to `172.30.0.2` reached from the container (HTTP 200), DPAPI (service restart and Windows restart) |
| Autotests (clean guest) | core 96 pass, service 27, gui 4: 0 FAIL; 3 SKIP (need the servers files) |
| Real SMB | PASS: 7 `smbreal` tests (wrong password 1326, unreachable transient 53 after 55 s, missing share 67, stop / start mid backup, browse, concurrency, service) + `net.drives` (`Y:`) |
| Cyrillic profile | PASS: `core` under `C:\Users\Юзер-Тест` |
| Installer end | PASS: update in place (new service PID), `/remove /silent`, reinstall. Note: after `/remove` the copy `C:\Program Files\VM2Backup\VM2Backup-Setup.exe` was still in the folder (running uninstaller, removed later / on reboot) |
| ftp / tls / http | 32 PASS, 1 limit (`ftp_passive_active_and_listing_variants`: active mode behind the container's NAT, same as x86) |
| ssh / scp | 59 PASS, 0 FAIL |
| esxi | not run (time; known SMB-staleness failures on this harness) |
| Defender | real-time ON the whole run, 0 detections, no sharing violations seen, no exclusions |

No product problem was found by this system. Harness problems found and fixed: localized `sc` output (service / waitsvc / install / uninstaller parsers), SMB directory cache (jobs picked up after 2 min), `Z:` taken by dockur,
dockur monitor socket path, Defender disabling in `oem/install.bat`, smb-real addressing, host-to-guest curl, `grep` on CP866 output ("binary file matches"), `icacls Everyone`.

## Findings of the third run (Windows 11 25H2 "Enterprise" ru, build 26200.9445, 64-bit payload; 2026-10-26)

dockur detected the 5-in-1 ISO as "Windows 11 Orporativnaa (Russian)" (no ready answer file for that name, it still installed unattended): the edition is **Enterprise ru** (the registry
`ProductName` still says "Windows 10 Enterprise", a known quirk of 26200). No setting was needed; DISK_SIZE=32G, RAM 4G, 4 cores. dockur's answer file skips OOBE / the online account (local
administrator `Docker`, autologon), skips the TPM / Secure Boot checks (`Get-Tpm`: TpmPresent False, no SecureBoot variable) and the poller works as on Windows 10. Timings: **install 40 min**
(slower than Windows 10: 5 reboots), sysinfo + win11 info 2, autotest core / service / gui 21 total, install + service + pipe 3, web + DPAPI + restart 8, smbreal + drive + encrypted share 3,
installer 2, panel at 100 / 125 / 150 % 15, ftp / ssh ~10. `wmic.exe` is absent (24H2+): the harness never used it (PowerShell / CIM only).

| Area | Result |
|---|---|
| Installer, SCM, service (idle stop 0.5 s, busy stop 0.7 s, crash recovery restart), pipe ACL | PASS, 30 of 30 checks |
| Panel `/selftest` + window test at 100 / 125 / 150 % | PASS |
| Web (37 checks) | PASS. **TLS 1.3 is negotiated** by Schannel (.NET `SslStream`: Tls13, AES-256 / SHA384; `curl.exe --tlsv1.3` connects), TLS 1.2 works, TLS 1.0 / 1.1 refused. DPAPI after service restart and after a restart of Windows PASS |
| Autotests (clean guest) | core 96 pass 0 fail 3 skip; gui 4 pass; service 25 pass 2 FAIL (`exe_console_mode`, `scheduler_does_not_rewrite_state_every_tick`): both passed 3 of 3 when rerun alone, both are load races (see below) |
| Real SMB | 7 of 8 passed the first time: `real_unreachable_server_is_transient` FAILED = **product bug** (below); after the fix 8 of 8 + the drive test |
| Encrypted SMB3 (new) | PASS: share `vm2benc` (`smb encrypt = required`): `smbreal/real_encrypted_share_round_trip` (plain and encrypted archive); `Get-SmbConnection`: dialect **3.1.1**, `Encrypted=True` on `vm2benc`, `Encrypted=False` on the plain share |
| SMB signing | the client does not require signing here (`RequireSecuritySignature False`, `EnableInsecureGuestLogons True` set by the harness); connections show `Signed=False` on the test Samba (Samba does not sign unless asked); user + password logons work |
| Installer end (update in place, remove, reinstall) | PASS |
| ftp / tls / http | 31 pass, 2 FAIL: `ftp_passive_active_and_listing_variants` (the known active-mode limit), `ftp_abort_and_idle_connection` (550 on a read; passed when rerun alone: the host was busy building at that moment) |
| ssh / scp | 59 pass, 0 fail |
| SmartScreen | **Blocks the unsigned Setup that has the Mark of the Web** (as after a browser download): dialog "Windows protected your PC" (ru: "Система Windows защитила ваш компьютер"; "Microsoft Defender SmartScreen prevented an unrecognized app from starting"); the first screen offers only "Don't run" until the user clicks "More info" which then shows "Run anyway" (the "Подробнее" link is visible in `results/shots/smartscreen-1.png`). A file started from a network share / copied by `cmd` has no Mark of the Web and starts without any prompt (the harness installs that way). Defender real-time stayed ON, nothing was disabled |
| Smart App Control | state 2 (off) on this install: not tested while enforcing; with SAC on, an unsigned installer is blocked without a "run anyway" option: a code-signing certificate would be needed for such users |

Product problems found by Windows 11 and fixed (regression tests in `src/tests_winreal.cpp` and `tests_smb_real.cpp`):

1. **Windows 11 25H2 answers error 67 (`ERROR_BAD_NET_NAME`) after ~43 s when nothing answers on the server** (Windows 10: 53 after 55 s). The target took 67 for "the share was not found" (permanent), so a
   powered-off NAS was reported as a missing share and never retried. `target_smb.cpp` now decides by the TCP probe of port 445 for both 53 and 67 (answers: 67 permanent; silent: 53 transient).
   Regressions: `smb/windows11_67_for_a_silent_server_is_transient` (fake API) and the real `smbreal/real_unreachable_server_is_transient` (failed on Windows 11, passes after the fix).
2. `service.shutdown`: the core could be stopped before the answer left the pipe (`exe_console_mode` failed once on the loaded guest: "service.shutdown" call failed). The stop is now 150 ms later (`service.cpp`).
3. Test race `service/scheduler_does_not_rewrite_state_every_tick` (the state was read before a last tick; now read after the stop).

New harness pieces: phases `win11` (informational: protections, TPM, SMB client policy, wmic), `smartscreen` (Setup with Zone.Identifier started like a double click, screenshot), `big`
(the three 4 GiB tests; not run here), the encrypted share in `smb-real.py` + `smbinfo.ps1` (negotiated dialect / signing / encryption), `NETSETS` (default `ftp ssh`; `NETSETS='ftp ssh esxi'`),
`wait-ready` failure now detected (`PIPESTATUS`). The default `w11` phase list is `sysinfo,win11,autotest,install,installer,panel,web,net,smb,smartscreen`.
Note: **`hostile`, `svchard`, `deep`, `winreal` are source files, not groups**: their tests are registered in `core / engine / resume / restore / service / big / smb`, so the autotest phase
already ran them (`--list` shows the groups; only `big` was never in a phase). 
The run was driven as several `run-winvm.sh w11 --no-create --keep --phases ...` calls (to reorder phases against the deadline); the single-command
`run-winvm.sh w11` from nothing was NOT run end to end (the create / wait branch was exercised by hand: run-os.sh + wait-ready.sh, 40 min).

## Раунд 2: Windows 10 x86 (сборка 19045.3803 -> 6456, 32-битная нагрузка), 26.10.2026

Что нового в обвязке и тестах относительно раунда 1.

**Одна команда с нуля.** `tools/run-winvm.sh x86` сама делает всё: собирает нагрузку, поднимает контейнер `vm2b-win`, ждёт установки Windows (около 23-24 минут), прогоняет ВСЕ фазы
(`sysinfo, autotest, winreal, install, installer, panel, web, net, smb, smbsign, svcacct, restartjob, unicode, verify, big, winbig`) и удаляет ВМ вместе с диском. Чему это научило:
* bash читает скрипт по кускам, а прогон идёт часами: правка `run-winvm.sh` посреди прогона роняла его («syntax error near unexpected token `then`»). Теперь первое действие скрипта - запуск
  собственной временной копии (`VM2B_RUNWINVM_COPY`), правки файла безопасны.
* код возврата задания не равен числу упавших тестов (последняя команда пакетного файла), поэтому итог по каждой группе берётся из журналов `autotest-*.txt` (строки `log <имя>: N passed, M failed` в конце).
* `net` по умолчанию теперь `ftp ssh esxi`; `big` задаёт `VM2B_BIG=1`; фаза `winbig` - 6 ГиБ разреженный файл.

**ESXi / svcesxi внутри гостя.** В раунде 1 пять тестов падали из-за клиента SMB Windows (кеш содержимого и листингов, искажённые имена) - датастор эмулятора лежал на хосте и читался по SMB.
Теперь эмулятор (`tools/testservers/esxi_mock.py` + `esxi_launch.py`) работает ВНУТРИ гостя под python embeddable 3.12 (win32, скачивается один раз в `.winvm/pyembed/`), сертификаты делаются
заранее на хосте (`tools/testservers/make_premade_certs.py`, у гостя нет модуля `cryptography`), датастор лежит на локальном NTFS (`guest/esxilocal.ps1`). Правки эмулятора: `st_blocks` есть
только в POSIX (на Windows берётся `st_size`). Два теста пропускаются на NTFS-датасторе, потому что ВМ с именами `pfx:vm` / `a:b*c?` на NTFS не существуют как файлы
(`esxi_two_vms_with_the_same_archive_prefix`, `esxi_vm_names_windows_cannot_store`); под Wine они идут как раньше.

**Новые группы тестов** (`src/tests_winreal.cpp`, `src/tests_smb_real.cpp`; под Wine пропускаются или проходят как обычные):

| Тест | Что проверяет на настоящей Windows |
|---|---|
| `winreal/links_inside_a_vm_folder_are_not_followed` | junction на `C:\Windows`, на внешнюю папку, петля на саму папку, симлинки на папку и файл: в архив ничего извне не попадает, список ВМ ссылок не показывает, ВМ-ссылка отвергается с понятным текстом, содержимое цели цело |
| `winreal/source_names_windows_cannot_restore_are_refused` | имена `trail.`, `trail2 `, `NUL`, `con.txt`, `COM1` (создаются через `\\?\`): копия отказывает ДО записи с именем файла, манифеста нет |
| `winreal/source_file_locks_have_deliberate_behaviour` | файл с общим чтением копируется; монопольный на 2 с - копия ждёт; монопольный навсегда - ошибка с именем файла за ~8 с, копия НЕ пропускает файл молча; заблокированный диапазон байт (`LockFile`) - то же |
| `winreal/destination_files_held_by_a_scanner_are_waited_for` | процесс-«антивирус» берёт монопольно каждый файл папки назначения на 150 мс: запись, переименование .part, проверка и восстановление переживают это |
| `winreal/unicode_long_names_short_names_and_streams_on_ntfs` | кириллица, японский, эмодзи, двойные пробелы, путь около 400 знаков, имена 8.3 не попадают в архив, альтернативный поток `file:stream` не архивируется и не восстанавливается |
| `winreal/attributes_and_times_after_restore` | решено: время записи возвращается (допуск 2 с), атрибуты «только чтение / скрытый / системный» НЕ восстанавливаются; поверх существующего файла только для чтения восстановление останавливается с понятной ошибкой `cannot create ...: Access is denied (5)` |
| `winreal/unreadable_config_is_never_replaced_by_an_empty_one`, `undecryptable_saved_password_gives_a_clear_message`, `dpapi_machine_scope_round_trip_and_damaged_blobs` | конфигурация, DPAPI (см. «Найдено») |
| `smbreal/real_unicode_names_and_unc_paths_over_260` | настоящая шара: кириллица / японский / эмодзи, путь к томам 349 знаков, тот же файл виден по `\\?\UNC\` |
| `winbig/sparse_6gib_vmdk_whole_pipeline` | 6 ГиБ разреженный vmdk: xz уровня 1, проверка, восстановление, три контрольных блока побайтно; пик рабочего набора процесса |

Скрипты гостя: `svcacct.ps1` (служба под `.\vm2svc` администратором, под администратором, которого не знает Samba, под обычным пользователем; права на папку данных от обычного пользователя;
копия в папку, где права только у учётной записи службы; SMB с явной учётной записью и с ПУСТЫМ пользователем = права самой службы; для этого в Samba создан тот же пользователь `vm2svc`;
`secedit` выдаёт право «Вход в качестве службы»), `smbsign.ps1` (Samba `server signing = mandatory` + `RequireSecuritySignature`, команда `signing on|off|disabled` порта управления `smb-real.py`;
`smbreal` целиком по подписанной сессии; `Get-SmbConnection`: `Signed=True`, диалект 3.1.1), `restartjob.ps1` (`shutdown /r /f` на втором томе копии 1,2 ГБ: служба после загрузки продолжает задачу
по журналу «continuing after 4 finished volumes», ровно одна запись в истории, `Success`, проверка и восстановление бит в бит по SHA-256).

### Найдено и исправлено в продукте (регрессии - в тестах выше)

1. **Папка данных `%ProgramData%\VM2Backup` была открыта для чтения и записи ВСЕМ локальным пользователям** (`applog.cpp`: «пользователи пишут журналы» выдавалось на всю папку с наследованием: `BUILTIN\Users:(OI)(CI)(M)`
   на папке и на `config.json`). Секреты защищены DPAPI областью «компьютер» - её снимает любая учётная запись компьютера, поэтому обычный пользователь мог прочитать `config.json`, расшифровать пароли
   серверов, прочитать `web.json` и менять задачи. Теперь: данные - только `SYSTEM` и администраторы (защищённый DACL, применяется и к существующим файлам, в том числе при обновлении со старой версии),
   пользователям открыт только `logs` (панель пишет туда журнал). Проверка: `guest/acltest.ps1` от обычного пользователя + `icacls` до и после обновления (`08-installer.out`).
2. **Нечитаемый `config.json` затирался пустым** (`Config::open`: ошибка чтения молча считалась «файла нет», а потом `saveCfg` записывал пустую конфигурацию): служба под другой учётной записью без прав на папку или файл,
   занятый другим процессом, стирали все серверы и задачи. Теперь ошибка чтения `config.json` / `history.json` / `state.json` останавливает запуск с понятным текстом, файл не трогается; в журнале службы и
   в журнале событий Windows (Приложение) - причина (SCM сам показывает только «Incorrect function»).
3. Пароль, который не удалось расшифровать (конфигурация на другом компьютере), уходил серверу как строка `dpapi:...` и пользователь видел «неверный пароль». Теперь `MakeTarget` / `MakeSource` отвечают
   «the saved password cannot be decrypted on this computer ... enter it again».
4. Копия отказывает до записи, если в источнике есть имя, которое восстановление отвергнет (`CON`, `NUL`, `имя.`, `имя `): раньше копия «успешно» делалась и не восстанавливалась.
5. Папка ВМ - ссылка (junction / symlink) не копируется (раньше список ВМ её скрывал, но заданное вручную имя уводило копию за пределы папки).
6. Файл источника, занятый на мгновение, ждётся до 8 с (раньше копия падала сразу); файл папки назначения, занятый монопольно «антивирусом», ждётся при открытии на чтение / запись и при удалении до 10 с
   (раньше ждало только переименование `.part`).
7. `RemoveTree` разворачивал корневую ссылку: тест со ссылкой на `C:\Windows` стёр часть файлов Windows на отладочной ВМ (PowerShell перестал запускаться). Теперь корневая ссылка удаляется как ссылка.

### Поведение, которое теперь записано и проверяется

* **DPAPI**: область «компьютер» (`CRYPTPROTECT_LOCAL_MACHINE`). Смена учётной записи службы ничего не теряет (секрет, сохранённый под `LocalSystem`, работает под `.\vm2svc`), копия `config.json` на другом
  компьютере не расшифровывается (понятное сообщение), а от чтения другими пользователями защищают права на папку (п. 1).
* **Пустой пользователь в записи SMB-сервера** = права учётной записи службы. Windows держит один набор учётных данных на имя сервера в сеансе входа: запись с пустым пользователем и запись с явным
  пользователем на ОДНО имя сервера делят соединение (поэтому «свои права» проверяются на втором имени того же сервера). `LocalSystem` -> отказ «access denied ... no user name was given (5)»;
  учётная запись, которую Samba не знает -> 1326; копия на такой сервер `Failed`, не `Success`.
* **Служба под обычным пользователем** без прав на папку данных не стартует (STOPPED), причина в `service.log` и в журнале событий. Такому пользователю нужны права на папку данных (`icacls`) и право входа как служба.
* **Подпись SMB**: обязательна на обеих сторонах - копия / проверка / восстановление работают, `Get-SmbConnection` `Signed=True`, диалект 3.1.1. Samba не удаётся заставить отказаться от подписи SMB2
  (`server signing = disabled` всё равно подписывает при требовании клиента), поэтому случай «сервер не умеет подпись» воспроизвести нельзя.

### Итоги прогона одной командой `tools/run-winvm.sh x86` (10503 с = 2 ч 55 мин, включая установку Windows 1422 с)

Результаты лежат в `.winvm-results/x86-20261026-235553/` (`summary.txt`, `results/*.out`, `results/autotest-*.txt`). Отладочные ВМ того же дня (их результаты в `x86-20261026-192152`, `-210043`) удалены.

| Область | Результат |
|---|---|
| Чистый гость: `core` / `service` / `gui` | 97 / 27 / 4 прошли, 0 упали (3 пропуска: нужен servers.json) |
| `winreal` (новая группа) | 9 из 9 |
| Установка, SCM, служба, ACL канала | 30 из 30 |
| Установщик (обновление поверх работающей службы, удаление, повторная установка) + ACL до / после | PASS, в `08-installer.out` видно `BUILTIN\Users:(M)` у старой версии и закрытую папку после обновления |
| Панель 100 / 125 / 150 %, оконные тесты | PASS |
| Web (TLS 1.2, DPAPI после перезапуска службы и Windows) | 37 из 37 |
| ftp / tls / http | 32 прошли, 1 известный предел (`ftp_passive_active_and_listing_variants`: активный режим за NAT) |
| ssh / scp | 59 из 59 |
| esxi + svcesxi (эмулятор внутри гостя) | 64 прошли, 2 пропуска (имена с `:` на NTFS), **1 упал: `esxi_thin_big_disk`** - ошибка эмулятора на Windows (см. ниже), после исправления пройден на том же госте (`autotest-thinrecheck.txt`, 1 из 1) |
| Настоящая SMB (`smbreal` 9, `smbdrive` 1, `smbinfo`) | PASS, в том числе `real_unicode_names_and_unc_paths_over_260` |
| Подпись SMB обязательна с обеих сторон | `smbreal` 8 из 8, `Signed=True`, диалект 3.1.1 |
| `svcacct` | 34 из 35: последняя проверка упала по таймауту из-за мёртвых записей блокировок Samba после `kill -9` (см. ниже); повтор на очищенной Samba: 35 из 35 (`99-svcacct-recheck2.out`) |
| `restartjob` (Windows перезапущена посреди копии) | 12 из 12, копия продолжена по журналу, одна запись в истории, SHA-256 диска совпал |
| `unicode` (администратор с кириллическим именем) | 12 из 12 тестов `core`; одна проверка скрипта упала ложно (английская консоль печатает `????`): исправлено, повтор `99-unicode-recheck.out` PASS |
| `verify` (локальные группы при установленном продукте) | 83 из 83 |
| `big` (три теста по 4 ГиБ) | 3 из 3 (самый долгий `big_mixed_heavy_levels_memory`, 631 с) |
| `winbig` (6 ГиБ разреженный vmdk, xz уровня 1) | PASS: копия 87 с, 25 томов, 4,1 МБ в архиве, пик рабочего набора процесса 50 МиБ, восстановление и три контрольных блока совпали; весь тест 349 с |

### Что в обвязке ломалось и исправлено в этом раунде

* `esxi_mock.py` на Windows: `st_blocks` нет; `truncate()` CRT ДОПИСЫВАЕТ нули, ВМ с тонким диском 1 ГиБ создавалась больше минуты, клиент управления не дожидался -> файл помечается
  разреженным и увеличивается записью последнего байта (0,3 с).
* `smb-real.py`: `stop` убивает `smbd` через SIGKILL и в базах блокировок остаются аренды мёртвого процесса; следующий `smbd` ждёт по таймауту ("defer_open_done ... NT_STATUS_IO_TIMEOUT") и
  обращения гостя к шаре зависают на минуту и дольше. Теперь `stop` стирает базы блокировок. Новые команды управления: `signing on|off|disabled`; пользователь `vm2svc`.
* Клиент SMB гостя не сразу видит свежесозданную папку `pkg\esxi` (кеш каталога): `esxilocal.ps1` ждёт до 60 с.
* `unicode-user.ps1` проверял имя по тексту `net user` (в английской консоли кириллица превращается в `????`): теперь `Get-LocalUser`.

### Что не проверялось

* Доменная учётная запись службы (нужен контроллер домена), Smart App Control / SmartScreen (это Windows 11, фаза `smartscreen`), Windows 10 x64 и Windows 11 в этом раунде (следующие в цикле).
* Сервер SMB, который вообще не умеет подпись при требовании подписи у клиента: Samba такой режим для SMB2 не даёт.
* Ограничение скорости `speed_mbit` в записи задачи не повлияло на задачу в `restartjob.ps1` (`limit_mbit` остался 0): копия шла медленно сама (xz на несжимаемых данных, 3,6 МБ/с), поэтому
  перезапуск попал в середину; ограничение проверяется отдельно в существующих тестах.

## Раунд 3: Windows 10 Enterprise ru x64 22H2 (сборка 19045.3570, 32-битный загрузчик + 64-битная нагрузка), 27.10.2026

Одна команда `tools/run-winvm.sh w10` с нуля: создание, ожидание установки (**1151 с = 19 мин**), 18 фаз, итог. Весь прогон **8802 с (2 ч 27 мин)**. Последующие фазы `aclreal` и `winreal` и повтор одного теста
FTP выполнены на той же ВМ (`--no-create --keep`, файл `hold` не давал `destroy.sh` удалить ВМ), затем ВМ и диск удалены (`.winvm/destroy.sh`). Результаты: `.winvm-results/w10-20261027-024450/`
(`results/summary-main.txt`, `results/run-winvm-w10.log` = вывод основной команды; `results/*.out`, `results/autotest-*.txt`, `results/shots/`).

| Область | Результат |
|---|---|
| Чистый гость: `core` / `service` / `gui` | 97 / 28 / 4 прошли, 0 упали (3 пропуска: нужен servers.json); `gui` на 100 / 125 / 150 % по 4 из 4 |
| `winreal` | в основном прогоне 8 из 9: упал `attributes_and_times_after_restore` (тест, не продукт: проверял английский текст «denied» в системной ошибке, а русская Windows пишет «Отказано в доступе (5)»; теперь проверяется код `(5)`); повтор с новым тестом на той же ВМ: 9 из 9 |
| Установка, SCM, служба, ACL канала | 30 из 30 (остановка по простою 0,3 с, при работающей задаче 0,8 с, перезапуск SCM после kill) |
| Установщик: обновление со старой версии 1.0.298 поверх работающей службы, удаление, повторная установка | PASS. Права по SDDL (`aclcheck.ps1`): у старой версии `BU` имеет `0x1301bf` на папке данных, `config.json`, `state.json`, `logs`; после обновления у папки `D:PAI(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)`, у всех файлов только `SY` и `BA`, 10 из 10 проверок |
| Права и канал от обычного пользователя с кириллическим именем `Юзер-Обычный` (`aclreal`, 16 из 16) | `config.json`, `web.json`, `state.json`, `history.json` не читаются, запись в `config.json` запрещена, DPAPI не снять, `logs` доступна (`BU` по SID `S-1-5-32-545`), к каналу службы `DENIED`; группа определяется по SID, названия групп не используются |
| Панель 100 / 125 / 150 %, оконные тесты | PASS |
| Web | 37 из 37: TLS 1.2 (SslStream, `curl.exe`, `HttpClient`), TLS 1.0 / 1.1 отказ, TLS 1.3 клиентом Schannel этой сборки не поддерживается (`SEC_E_ALGORITHM_MISMATCH`), привязка к 172.30.0.2, DPAPI после перезапуска службы и Windows |
| ftp / tls / http | 31 прошёл, 2 упали: `ftp_passive_active_and_listing_variants` (известный предел: активный режим за NAT) и `ftp_abort_and_idle_connection` (таймаут управляющего соединения; в журнале тестового сервера `AttributeError: 'H' object has no attribute 'data_channel'` внутри pyftpdlib после оборванной передачи; на той же ВМ отдельно прошёл (42 с), на loopback 6 из 6; то же упало на Windows 11 в прошлом раунде: нестабильность тестового сервера, не продукта) |
| ssh / scp | 59 из 59 (1 пропуск) |
| esxi + svcesxi (эмулятор внутри гостя, python embeddable 32 бита под WOW64) | 65 прошли, 0 упали, 2 пропуска (имена с `:` на NTFS) |
| Настоящая SMB | `smbreal` 10 из 10 (в том числе новый `real_cyrillic_share_and_account_names`: шара `резерв` и учётная запись Samba `юзер-смб`), `smbdrive` 1, `smbinfo`; обязательная подпись с обеих сторон: 9 из 9, `Signed=True` |
| `svcacct` | 45 из 45: `.\vm2svc`, администратор неизвестный Samba, обычный пользователь (служба не стартует, причина в `service.log`), **кириллическое имя службы `Служба-ВМ`**, **`NT AUTHORITY\NetworkService`** (без прав на папку данных STOPPED, после выдачи прав по SID `*S-1-5-20` работает и расшифровывает секреты), возврат на LocalSystem |
| `speedlimit` (новая фаза) | 10 из 10 после исправления (см. ниже) |
| `restartjob` | 12 из 12 (Windows перезапущена посреди копии; одна запись в истории, SHA-256 диска совпал) |
| `unicode` (администратор `Юзер-Тест`) | 12 из 12 тестов `core` под кириллическим профилем |
| `verify` / `big` / `winbig` | 83 из 83 / 3 из 3 (самый долгий 512 с) / 6 ГиБ разреженный vmdk: копия 55 с, 25 томов, 4,3 МБ в архиве, пик рабочего набора 51 МиБ |
| Defender | работал в реальном времени весь прогон, без исключений и отключений; сбоев из-за нарушения совместного доступа в тестах не было (журнал Защитника отдельно не разбирался) |

### Найдено и исправлено в продукте

**Ограничение скорости элемента задачи (`speed_mbit`) не действовало при запуске ЗАДАЧИ** (`svccore.cpp`, `ServiceCore::backupElement`): `elementMbit` задавался только в `startElementJob` (запуск одного элемента из окна), а запуск
задачи (расписание, «Запустить», `tasks.run`) учитывал лишь глобальную настройку `speed_limit_mbit` и окно расписания, поэтому поле «Speed limit, Mbit/s» в элементе задачи молча игнорировалось (`limit_mbit` = 0): вот что
наблюдалось в `restartjob.ps1` в раунде 2. Теперь `backupElement` ставит `elementMbit` из элемента и пересчитывает предел (действует меньший из трёх: настройка, элемент, окно).
Регрессия: `service/task_element_speed_limit_is_applied` (запуск задачи с лимитом и без, `limit_mbit` 8, время записи: без лимита 3,6 с, с лимитом 6,7 с на 4,2 МБ под Wine; без исправления тест падает: `limit_mbit` 0).
На госте: `guest/speedlimit.ps1` (12 МиБ несжимаемых данных, xz 1, без проверки): без лимита, `speed_mbit=8`, настройка 8, настройка 64 + элемент 8, окно расписания 8 на ручном запуске, после снятия: лимитированная копия не быстрее 1,5 МиБ/с
и дольше 9 с, не менее чем вдвое медленнее свободной. Числа времени в выводе прогона не сохранились (функция возвращала строку `INFO` вместе с результатом: исправлено, `Write-Host`), остались только результаты проверок.

Других ошибок продукта эта система не нашла. Наблюдение без правки: источник событий `VM2BackupService` не зарегистрирован с файлом сообщений, поэтому Просмотр событий пишет «Не найдено описание для события с кодом 1», а текст причины виден только в строках вставки.

### Изменения обвязки и тестов раунда 3

* Новые фазы `aclreal`, `speedlimit` (в `run-winvm.sh` по умолчанию для x86 и w10), `installer` теперь печатает и считает проверки прав по SDDL (`aclcheck.ps1 old|new`).
* `svcacct.ps1`: группа администраторов добавляется по SID `S-1-5-32-544` (на русской Windows группа называется иначе), новые разделы про кириллическое имя службы и `NetworkService`.
* `smb-real.py`: шара `[резерв]` и пользователь `юзер-смб` (`useradd --badname`; если не создаётся, пользователь пропускается), конфигурация пишется в UTF-8; `servers-smb*.json` получают `cyr_unc`, `cyr_user`, `cyr_password`.
* Файлы `.ps1` с кириллицей в строках кода сохраняются с BOM (без BOM Windows PowerShell 5 читает их как ANSI и ломает разбор: `aclreal.ps1` в основном прогоне выдал 0 проверок из-за ошибки разбора, перепроверен на той же ВМ).
* `.winvm/hold` для `destroy.sh` (см. таблицу).

### Не проверялось

* gMSA и доменные учётные записи (нужен контроллер домена); проверены локальные записи, кириллическое имя, `NetworkService`.
* Windows 11 в этом раунде, Smart App Control / SmartScreen.
* Точные числа скорости записи при лимите (см. выше), повтор после исправления скрипта не делался (потребовал бы новой ВМ).

## Раунд 4: Windows 11 25H2 Enterprise ru (сборка 26200.9445, x64), второй раунд, 27.10.2026

Одна команда `tools/run-winvm.sh w11` с нуля: создание, ожидание установки (**2956 с = 49 мин**), 21 фаза
(`sysinfo, win11, autotest, winreal, install, installer, panel, web, net, smb, smbsign, svcacct, aclreal, speedlimit, restartjob, unicode, verify, big, winbig, defender, smartscreen`
плюс последняя фаза `sac`). Основной прогон до `smartscreen` занял **около 11 400 с (3 ч 10 мин)** вместе с установкой. Файл `.winvm/hold` не давал `destroy.sh` удалить ВМ: фаза `sac`
необратима (после включения Smart App Control его нельзя выключить без сброса Windows), поэтому основной запуск остановили перед ней (по PID), перепроверили на той же ВМ `smbinfo`, три раза
подряд набор FTP (с журналом сервера уровня DEBUG) и только потом вручную выполнили `--no-create --keep --phases sac`. Затем `hold` убран, ВМ и диск удалены `destroy.sh`.
Результаты: `.winvm-results/w11-20261027-064957/` (`results/summary-main.txt` и `results/run-winvm-w11.log` = вывод основной команды; `results/*.out`, `results/autotest-*.txt`,
`results/shots/` со снимками экрана, `results/ftp-debug-logs-reruns.tar.xz` = журналы FTP-серверов трёх повторов).

| Область | Результат |
|---|---|
| Чистый гость: `core` / `service` / `gui` / `winreal` | 97 / 28 / 4 / 9 прошли, 0 упали (3 пропуска: нужен servers.json); `gui` на 100 / 125 / 150 % по 4 из 4; панель `/selftest` на трёх масштабах |
| Установка, SCM, служба, ACL канала | 32 из 32 (в том числе новые проверки источника событий, см. ниже) |
| Установщик: обновление с 1.0.298, удаление, повторная установка; права по SDDL | 10 из 10 |
| Web | 41 из 41. **TLS 1.3 только**: `curl.exe --tlsv1.3 --tls-max 1.3` даёт HTTP 200, `SslStream` с `SslProtocols.Tls13` согласует `Tls13` (AES-256, SHA384), PowerShell `Invoke-WebRequest` при `SecurityProtocol = Tls13` даёт 200; клиент, ограниченный TLS 1.2, по-прежнему получает 200; TLS 1.0 / 1.1 отказ; привязка к 172.30.0.2, DPAPI после перезапуска службы и Windows |
| ftp / tls / http | основной прогон: 31 прошёл, 2 упали: `ftp_passive_active_and_listing_variants` (известный предел: активный режим за NAT) и **`ftp_large_streamed`** (одноразовая нестабильность, см. ниже); три повтора с журналом DEBUG: 33 из 34, падает только активный режим |
| ssh / scp | 59 из 59 (1 пропуск) |
| esxi + svcesxi (эмулятор внутри гостя) | 65 прошли, 0 упали, 2 пропуска (имена с `:` на NTFS) |
| Настоящая SMB | `smbreal` **11 из 11** (в том числе новый `real_readonly_user_reads_and_write_is_refused`), `smbdrive` 1 из 1; **обязательная подпись SMB с обеих сторон** (`smbsign`): `smbreal` 10 из 10, 6 проверок включения / выключения; шифрованная шара `vm2benc` (SMB 3.1.1, `Encrypted=True` по `Get-SmbConnection`) |
| Два пользователя на одной шаре (новое) | `vm2buser` пишет, `vm2bro` (Samba `read list`) только читает: чтение и список работают, запись даёт отказ `Отказано в доступе (5)`, постоянный (не повторяется), без пароля в тексте; создание новой папки: `cannot create folder ...: ... (5)`; `servers.test`: «the connection works, but a file cannot be created in the folder: cannot create ...\\.vm2b-test.tmp.part: ... (5)»; `net use R:` из PowerShell: запись и `New-Item` отказаны |
| `svcacct` | 46 из 46 (локальная учётная запись, неизвестная Samba, обычный пользователь, кириллическое имя службы, `NetworkService`); для обычного пользователя причина отказа запуска видна в журнале событий «Приложение» читаемым текстом |
| `aclreal` / `speedlimit` / `restartjob` | 16 из 16 / 10 из 10 (без лимита 3,5 с, с лимитом 8 Мбит/с около 15,4 с, 0,78 МиБ/с) / 12 из 12 (Windows перезапущена посреди копии, 20 мин) |
| `unicode` (администратор `Юзер-Тест`) | 12 из 12 тестов `core` |
| `verify` / `big` / `winbig` | 83 из 83 / 3 из 3 (самый долгий 439 с) / 6 ГиБ разреженный vmdk: копия 80 с, 25 томов, 4,3 МБ, пик рабочего набора 53 МиБ |
| `defender` (новая фаза) | 5 из 5: защита в реальном времени включена весь прогон и в конце (`RealTime=True`, `TamperProtected=True`), без исключений, `DisableRealtimeMonitoring=False`, `Get-MpThreatDetection` пуст, событий 1116 / 1117 нет; ничего не отключалось |
| `win11` (справочно) | `wmic.exe` отсутствует; новый Терминал Windows (`wt.exe`, appx 1.18) есть, терминал по умолчанию не задан; TPM и SecureBoot нет (dockur обходит проверки); клиент SMB: подпись не требуется, `EnableInsecureGuestLogons=True` (задан обвязкой) |
| SmartScreen | как в раунде 1: установщик с меткой Mark of the Web блокируется окном «Система Windows защитила ваш компьютер» с единственной кнопкой «Не выполнять» до нажатия «Подробнее» (`results/shots/smartscreen-1.png`) |
| Smart App Control | в начале состояние **2 = оценка** (Evaluation; в разделе раунда 1 оно названо «off», это неточно). Администратор смог записать `VerifiedAndReputablePolicyState = 1` напрямую в реестр без переустановки и сброса; после перезапуска значение осталось 1, журнал CodeIntegrity показал активацию политики `VerifiedAndReputableDesktop` (событие 3099, статус 0). **Блокировки не было**: неподписанный Setup (с меткой Mark of the Web и без) запустился и показал своё окно (`shots/sac-*.png`), служба продукта работает, `vm2bautotest.exe` запускается. Вывод: запись значения реестром не включает реальное принудительное применение (решение принимает облачная репутация, а у ВМ нет интернета); поведение установщика при настоящем режиме On **не проверено**. Для таких пользователей по-прежнему нужен подписанный установщик |

### Найдено и исправлено

**В продукте ошибок этой системой не найдено.** Доработка продукта (небольшая, по просьбе): **источник событий `VM2BackupService` получил файл сообщений**. Раньше Просмотр событий писал «Не найдено описание для события с кодом 1»
(текст причины был только в строках вставки). `service.cpp`: при `/install` и при неудачном старте службы пишется `HKLM\SYSTEM\CurrentControlSet\Services\EventLog\Application\VM2BackupService`
(`EventMessageFile = %SystemRoot%\System32\EventCreate.exe`, `TypesSupported = 7`; сообщения 1..1000 у `EventCreate.exe` имеют вид «%1», поэтому показывается переданный текст), при `/uninstall` ключ снимается.
Проверка на госте (`guest/service.ps1`): событие с кодом 1 из источника `VM2BackupService` читается с текстом, а не «описание не найдено»; (`guest/svcacct.ps1`) отказ запуска службы под обычным пользователем
виден в журнале «Приложение» как «VM2Backup service cannot start: cannot write C:\ProgramData\VM2Backup\config.json.tmp: error 5». Обе проверки прошли.

**Тестовый сервер FTP (`tools/testservers/ftp_server.py`), нестабильный `ftp_abort_and_idle_connection`: ошибка была в НАШЕМ обработчике.** Функция `cut_everything` (обрыв по `SITE FAULT`) закрывала управляющий канал
(`ch.close()`) прямо внутри обработчика канала данных, который вызван из кода pyftpdlib (`push_dtp_data`, `handle_read`). `FTPHandler.close()` удаляет атрибут `data_channel`, а код pyftpdlib после нашего исключения
продолжал читать его, отсюда в журнале сервера `AttributeError: 'H' object has no attribute 'data_channel'`; оборванное соединение оставалось в неопределённом состоянии, и тесты на том же сервере (порядок задаёт сид)
иногда получали ложные «550 ... is not retrievable» (ответ `SIZE`) или таймаут управляющего соединения. Исправление: теперь на сокеты только вешается RST при закрытии (`SO_LINGER 0`), а каналы закрываются
на следующем обороте цикла (`ioloop.call_later(0, ...)`); у класса обработчика добавлен `data_channel = None` по умолчанию. В журналах сервера `AttributeError` больше нет (до правки он был в каждом прогоне фаз `fault_injection`).
Проверка: на хосте 10 полных наборов `--only ftp` и 10 одиночных `ftp_abort_and_idle_connection` подряд (случайные сиды) 20 из 20; на госте три полных набора подряд с журналом DEBUG: `ftp_abort_and_idle_connection` и
`ftp_large_streamed` прошли все три раза. **Не объяснено:** в основном прогоне один раз упал `ftp_large_streamed` (FTPS, 134 МБ: данные ушли за ~11 с, ответ 226 не пришёл за 60 с, журнал сервера на уровне WARNING пуст),
на хосте и в трёх повторах на госте не воспроизвёлся. Для разбора при следующем появлении `run-winvm.sh` теперь копирует журналы серверов в `results/srv-<набор>/`, а уровень журнала FTP-сервера задаётся
переменной `VM2B_FTP_LOGLEVEL` (например `VM2B_FTP_LOGLEVEL=DEBUG NETSETS=ftp tools/run-winvm.sh w11 ...`).

### Изменения обвязки и тестов

* `run-winvm.sh w11`: набор фаз по умолчанию теперь полный (как у x86 / w10) плюс `win11`, `defender`, `smartscreen`, `sac`; `json_smb` передаёт `ro_user` / `ro_password`; `tally` для `smbinfo`; журналы тестовых серверов в результаты.
* Новый тест `smbreal/real_readonly_user_reads_and_write_is_refused` (`src/tests_smb_real.cpp`), поля `ro_user` / `ro_password` в `servers.json`; в `smb-real.py` пользователь `vm2bro` (`read list` шары `[vm2b]`).
* `guest/smbinfo.ps1`: подключён `lib.ps1` (до этого `Check` не был определён, проверки не считались); сведения о сессиях SMB собираются ПОКА идёт запись (Windows 11 закрывает бездействующую сессию сразу, `Get-SmbConnection` после ввода-вывода
  её уже не показывает): `vm2benc` 3.1.1, `Encrypted=True`; проверки второго пользователя с доступом только на чтение.
* `guest/web.ps1`: блок «TLS 1.3 only» (на сборках 22000 и выше обязателен, на старых справочно). `guest/win11.ps1`: Терминал Windows, источник событий. `guest/defender.ps1` (новая), `guest/sac.ps1` (новая: `info | enable | check`).
* `guest/service.ps1`, `guest/svcacct.ps1`: проверки источника событий.

### Не проверено

* Настоящее принудительное применение Smart App Control (запись в реестр его не включила, нужен путь через интерфейс «Безопасность Windows» и ВМ с интернетом).
* Поведение подписанных установщиков (подписи нет).
* Причина единичного `ftp_large_streamed` (см. выше); домен и gMSA; настоящие ESXi / vCenter.

## Раунд 5: настоящий домен Active Directory (две ВМ Windows Server 2022 ru), 27.10.2026

Раньше были только локальные учётные записи и Samba. Здесь: контроллер домена и рядовой сервер, VM2Backup на рядовом сервере. Запуск: `tools/run-winvm.sh domain` (то же, что `tools/run-domain.sh`; фазы `create,dc,join,install,domainreal,domainlock,auth,svc,web,installer`, ключи `--keep --no-create --phases --seed`). Обе ВМ и их диски удаляются при любом результате (`.winvm/dom/destroy-dom.sh`; файл `.winvm/hold` запрещает удаление). Результаты: `.winvm-results/domain-20261027-ad/` (`results-dc/`, `results-member/`, журналы запусков `rundomain*.log`).

### Топология

| | `vm2b-dc` | `vm2b-member` |
|---|---|---|
| ОС | Windows Server 2022 Standard (с рабочим столом, индекс 2 образа `ru-ru_windows_server_2022_updated_feb_2023`), build 20348.1547 | то же |
| Имя / адрес | `DC`, внутренняя карта `10.77.0.1` | `MEMBER`, `10.77.0.2` |
| Роль | AD DS + DNS, лес `vm2b.test` (NetBIOS `VM2B`), KDS root key (EffectiveTime -10 ч), файловые шары `Backups`, `Restricted` | член домена, на нём VM2Backup (одно-файловый установщик), шара `memberlocal` |
| Ресурсы | 3 ГБ, 2 ядра | 3 ГБ, 4 ядра |
| VNC | 127.0.0.1:5909 | 127.0.0.1:5910 |

Устройство стенда (`.winvm/dom/`): чистый qemu внутри образа `dockurr/windows` (как у x86: IDE-диск, e1000, std VGA, драйверы есть в самом образе Server 2022), ISO как есть, ответный файл `autounattend.xml.in` на втором CD (ru-RU, раскладка en-US, индекс 2, общедоступный общий ключ установки KMS-клиента только для выбора редакции, автовход Administrator). У каждой ВМ **две сетевые карты e1000**: user-режим (хост, общая папка `\\10.0.2.4\qemu`, как в прежних раундах) и внутренняя сеть домена `10.77.0.0/24` - L2-туннель `-netdev socket` между контейнерами по docker-сети `vm2b-domnet` (DC слушает `:7700`, member подключается к `vm2b-dc:7700`); MAC `52:54:00:77:00:0X` по ним скрипт находит карту. Карта хоста на обеих ВМ не регистрируется в DNS (иначе у обоих в домене появился бы адрес 10.0.2.15), DNS обеих карт = `10.77.0.1`, DNS-сервер слушает только `10.77.0.1`. Исполнитель заданий - ключ реестра `Run` (а не задача планировщика: после повышения локального Administrator на DC нет). Файлы: `run-dom.sh dc|member`, `job.sh <роль> <имя> <файл|-> [таймаут]`, `wait-dom.sh`, `shot-dom.sh`, `type-dom.sh` (ввод с монитора qemu), `destroy-dom.sh`; гость: `guest/domain-lib.ps1` (константы тестового домена: одноразовый стенд, пароли в скриптах - константы), `domain-dc1/dc2/dc3.ps1`, `domain-member1/member2.ps1`, `domain-svc.ps1`, `domain-gmsa.ps1`, `domain-auth.ps1`, `domain-badpwd.ps1`, `domain-unlock.ps1`.

Учётные записи домена: `svcbackup` (служба), `bkuser` (доступ к `\\dc\Backups`), `nouser` (пароль верный, прав нет), `lockuser` (PSO: блокировка после 5 неверных попыток, 30 мин), `chguser` / `expuser` (смена пароля при входе), `disuser` (отключена); группа `VM2B-Backup-Operators` (bkuser, svcbackup, lockuser, gMSA); gMSA `vm2bgmsa$` (интервал пароля 1 сутки, получать пароль может `MEMBER$`); у `MEMBER$` права на `Backups` выданы НАПРЯМУЮ, на `Restricted` нет ни у кого, кроме администраторов.

Время установки: Windows Server 2022 на каждой ВМ ~37 мин при двух параллельных установках; повышение до DC ~11 мин + перезагрузка ~11 мин; весь прогон тестов ~1 ч 15 мин с повторами.

### Результаты

| Область | Итог | Подробности |
|---|---|---|
| Время / Kerberos | PASS | разница часов DC и MEMBER < 1 с (`w32tm /monitor`), канал домена `Test-ComputerSecureChannel` True, `Test-ADServiceAccount` True |
| (1) LocalSystem, пустой пользователь = `MEMBER$` | PASS | копия + проверка + восстановление на `\\dc.vm2b.test\Backups` и `\\dc\Backups` (Kerberos компьютера); на `Restricted`: отказ «access denied», копия `Failed`; по IP (`\\10.77.0.1`) учётная запись компьютера тоже вошла (NTLM, событие 4624 `MEMBER$ NTLM`) |
| (2) служба под `VM2B\svcbackup` | PASS | RUNNING, владелец процесса доменный, пустой пользователь = права svcbackup: копия на `\\dc\Backups` и на шару на самом member, `Restricted` отказ; секреты DPAPI, записанные под LocalSystem, читаются |
| (3) служба под gMSA (`sc config obj= VM2B\vm2bgmsa$ password= ""`) | PASS | `domain-gmsa.ps1`: запуск, `servers.test`, копия, перезапуск службы, копия, `Test-ADServiceAccount`, возврат на LocalSystem. **Ротация пароля не подтверждена**: `Reset-ADServiceAccountPassword` для gMSA не работает (ошибка `NetAddServiceAccount`, команда для автономных MSA); сдвиг часов обеих ВМ на +2 суток (интервал 1 сутки) с перезапуском службы: служба RUNNING, DPAPI читается, но `msDS-ManagedPasswordId` не изменился (сам пересчёт не доказан); перезагрузка не потребовалась ни разу |
| (4) явный пользователь `VM2B\bkuser`, `bkuser@vm2b.test`, `bkuser` x `\\dc.vm2b.test`, `\\dc`, `\\10.77.0.1` | PASS (9 из 9 + копия/проверка/восстановление трёх сочетаний) | `domainreal/domain_user_forms_and_server_names`, `domain_backup_verify_restore_by_name_form` |
| Kerberos или NTLM | записано | события безопасности DC (`domain-auth.ps1`): по FQDN и короткому имени: 4768/4769 (билет `cifs`->`DC$`) и 4624 `Kerberos`; по IP: 4776 + 4624 `NTLM` (SPN нет). В продукте тот же `WNetAddConnection2`, что у `net use` |
| неверный пароль | PASS | постоянная ошибка 1326 без пароля в тексте; **одно обращение к Windows на операцию** (счётчик вызовов `WNetAddConnection2W` в тесте): целая копия с неверным паролем = 1 вызов, движок не повторяет |
| блокировка (1909) | PASS | `lockuser`: после 6 неверных попыток 1909, постоянная ошибка «the account is locked out», верный пароль тоже 1909, одна попытка на операцию, копия падает с 1909 без повторов |
| 1907 (смена пароля) / 1331 (отключена) / `nouser` -> 5 | PASS после правки | см. «Найдено»; 1330 (срок пароля истёк) **не проверен**: в свежем домене состояние «истёк» без ожидания 42 суток получить нельзя (`pwdLastSet` принимает только 0 или -1), `expuser` получил то же состояние, что `chguser` |
| (5) net.browse / net.drives | PASS / справочно | `\\dc.vm2b.test`, `\\dc`, `\\10.77.0.1`: 4 записи, шара `Backups` найдена и открывается; неверный пароль 1326; **корень сетевого окружения пуст** (0 записей: контроллер и шары на домене без обзора NetBIOS не перечисляются; до ввода в домен ответ был «не ответил за 15 с (1460)») |
| (6) DPAPI между LocalSystem, доменным пользователем и gMSA | PASS | секреты, записанные под любой из трёх записей, читаются под остальными (машинная область) |
| (7) папка данных | PASS | права только SYSTEM и Администраторы; обычный доменный пользователь (`nouser`, вход разрешён) не читает `config.json`, `web.json`, `state.json`, `history.json`, не пишет, не снимает DPAPI |
| (8) сайт и панель | PASS | `web.ps1` 16 из 16 (TLS 1.2 и 1.3 Schannel, вход admin/admin), `VM2Backup.exe /selftest` rc=0, окна `gui` 4 из 4 |
| (9) обновление / удаление / переустановка | PASS | `uninstall-update.cmd`: 9 из 9 |
| (10) служба под записью без прав на папку данных | PASS | STOPPED, в журнале «Приложение» (источник `VM2BackupService`): «cannot write ...config.json.tmp: error 5» |

Побочные находки обвязки: на Server 2022 гостевой вход на общую папку qemu по умолчанию запрещён (`AllowInsecureGuestAuth` теперь выставляется ответным файлом первым же действием); встроенный Administrator на русской ОС называется «Администратор», в домене учётная запись `Administrator` не находится (скрипты берут имя из кодов символов); автовход после повышения до DC и после ввода в домен не срабатывал: вход вручную (в логе обвязки отмечено, на тесты не влияет); сдвиг часов на +2 суток ломает WinRM Kerberos и подвешивает исполнитель (возврат часов делает хост заданиями, `run-domain.sh`); пустой пользователь у локального администратора member, не входящего в домен, прошёл на `Backups` потому, что имя и пароль совпали с доменным Администратором (NTLM сверяет только имя), это не ошибка продукта.

### Найдено и исправлено

1. **Коды 1907 и 1330 не имели английского текста** (`src/target_smb.cpp`, `SmbErrorText`): вместо понятного сообщения продукт показывал системный текст на языке Windows («Пароль пользователя необходимо изменить перед входом в систему (1907)»). Добавлены тексты «the password of this account must be changed before it can log on ...» и «the password of this account has expired ...», обе ошибки постоянные. Регрессия: `smb/error_texts_and_classes` (строки 1907, 1330), домен: `domainreal/domain_account_states_give_clear_permanent_errors`.

### Защита от блокировки учётной записи (исправлено; проверка на настоящем домене, 27.10.2026, второй прогон)

Риск, найденный в этом раунде (каждое обращение к серверу с неверным паролем стоит доменному пользователю `badPwdCount` +2, а планировщик перезапускает задачи), **устранён**: сервер, отказавший в учётных данных (SMB 86 / 1326 / 1909 / 1331 / 1330 / 1907 / 1385 / 1327 / 1328, FTP 530, SCP: ошибка аутентификации, ESXi `InvalidLogin`), помечается в `state.json`
(раздел `_auth_blocks`, отметка привязана к отпечатку учётных данных записи), и запуск по расписанию, а также второй элемент той же задачи к нему **не обращаются вообще** (результат `Failed` + `skipped`, текст «The credentials of server "X" were rejected on ...; not tried again until the server record is changed or "Try again" is pressed.»).
Отметку снимают: изменение логина / пароля / адреса / порта / ключа записи, команда `servers.unblock` («Try again», admin), успешное подключение. Ручной запуск и «Проверить соединение» делают одну попытку. Подробности: `docs/WEB.md`, раздел «Защита от блокировки учётной записи».

Новый тест группы `domainlock`: `domain_scheduled_task_with_wrong_password_is_tried_once` (`src/tests_domain_real.cpp`): служба в процессе теста, настоящий `WNetAddConnection2W` (счётчик обращений), запись сервера `\\dc.vm2b.test\Backups` с `lockuser` (порог блокировки 5) и НЕВЕРНЫМ паролем, задача по расписанию с шагом 10 минут и поддельными часами; 8 проходов планировщика.

| Что измерено на контроллере домена | Результат |
|---|---|
| 8 запусков планировщика с неверным паролем | **1** обращение к `WNetAddConnection2W` (раньше 8), `badPwdCount` `lockuser` 0 -> **1** (одно обращение; на `bkuser` то же обращение стоило +2: Kerberos + NTLM; у `lockuser` на этом контроллере +1) |
| учётная запись | **не заблокирована**: верный пароль входит, после серии `badPwdCount` ниже порога 5 |
| последний запуск | `Failed`, `skipped`, сообщение «The credentials of server "Domain share" were rejected on ... (the user name or the password is wrong (1326)); not tried again until ...» |
| новый пароль в записи сервера | отметка снята сама, следующий запуск дошёл до сервера (+1 обращение) и **копия `Success`** |
| контрольные измерения без защиты на уровне цели (старые тесты, не изменились) | одно обращение = `badPwdCount` +2 (`bkuser`, FQDN + UPN: connect + open_write = 2 обращения, 0 -> 4); целая копия с неверным паролем = 1 обращение, 0 -> 2 |

Полный прогон `tools/run-winvm.sh domain` (ВМ создавались заново, 7954 с = 2 ч 13 мин): `dc1` 3/0, `dc2` 5/0, `join1` 4/0, `join` 13/0, `domainreal` 6/0, `auth` 3/0, `svc` **53/0** (в прошлом прогоне было 8 FAIL из 53 по причине в самом скрипте; на этот раз все прошли, скрипт не менялся), `web` 16/0, `gui` 4/0, `installer` 10/0, `domainlock` 3 из 4: единственный отказ - **ошибка самого теста** (мой ожидаемый прирост `badPwdCount` «ровно 2»; на `lockuser` одно обращение стоит +1, а счётчик читался уже после включения планировщика и первого запуска). Тест исправлен (счётчик читается до включения планировщика, допустимо +1..+2), на тех же ВМ повторён: `domainlock` **4 из 4**. Результаты: `.winvm-results/domain-20261027-lockguard/`.
Обе ВМ после этого удалены (`destroy-dom.sh`, `docker ps -a` - без `vm2b-*`, диски и сеть `vm2b-domnet` удалены).

Замечание по обвязке: и на DC, и на member после перезагрузки автовход не сработал (скрипты задают имя `Administrator`, а на русской ОС учётная запись называется «Администратор»): исполнитель стоял до ручного входа (клавиши и мышь через монитор qemu: Ctrl+Alt+Del, плитка «Администратор», пароль через `type-dom.sh`; на DC ожидание перезагрузки истекло через 1500 с, прогон продолжился после входа). На результаты тестов это не влияет, но в `run-domain.sh` стоит заменить `DefaultUserName` на имя из кодов символов (как `AdminUser` в `domain-lib.ps1`).

### Не проверено

Срок действия пароля 1330 (см. выше); сам факт ротации пароля gMSA (`msDS-ManagedPasswordId` не менялся при сдвиге часов); `domain-svc.ps1` в первом полном прогоне дал 8 FAIL из 53 (во втором прогоне 27.10.2026 все 53 прошли, скрипт не менялся), все одной причины в самом скрипте (ошибка теста, не продукта): запись сервера с пустым пользователем на `\\dc.vm2b.test\Backups` не получила идентификатор (`six servers saved`; четыре копии по FQDN: LocalSystem, svcbackup, gMSA x2; проверки `all servers are still there` x2 следствие). Те же копии по FQDN проверены отдельно: LocalSystem и gMSA (до и после перезапуска и `Reset`) вручную на `diag`-сервере и в `domain-gmsa.ps1`; копии по короткому имени `\\dc\Backups` под LocalSystem и svcbackup прошли в самом `domain-svc.ps1`; копия под svcbackup по FQDN отдельно не перепроверена (скрипт надо исправить: уникальные имена записей); ESXi/vCenter, второй контроллер домена, репликация, доверия между лесами.

## Раунд 6: настоящий домен Active Directory, повторный прогон (пункты A-G), 28.10.2026

Те же две ВМ Windows Server 2022 ru (`vm2b-dc`, `vm2b-member`), прогон `tools/run-winvm.sh domain` (новые фазы `browse`, `reboot`, `ftp`, `rotate`; `svc` теперь два прохода подряд, третий после перезапуска ВМ).
Из-за правок продукта между этапами прогон шёл этапами на одних и тех же ВМ (`--no-create --keep --phases ...`): этап 1 (старый пакет, базовые результаты) - `dc,join,install,domainreal,domainlock,auth,browse,svc,reboot,web,installer,ftp`;
этапы 2-5 (новый пакет) - `install,domainreal,svc,rotate`. Результаты: `.winvm-results/domain-20261028-stage1` ... `-stage4`, `-round6` (последний этап). Обе ВМ, диски и сеть `vm2b-domnet` удалены (`destroy-dom.sh`).

| Пункт | Итог | Подробности |
|---|---|---|
| A. Автовход | подтверждено | имя встроенного администратора берётся из SID -500 (`Get-LocalUser` / `Win32_UserAccount`): `bootstrap.cmd.in` (локальная запись, `DefaultDomainName .`), `domain-dc1.ps1` (после повышения `VM2B\Администратор`), `domain-member1.ps1` (после ввода в домен то же); функции `Get-BuiltinAdminName`, `Set-AutoLogon` в `domain-lib.ps1`. Исполнитель поднимался сам, без входа через монитор: после установки (оба), после повышения до DC (365 с вместо прежнего таймаута 1500 с), после ввода member в домен (250 с), после перезапуска обеих ВМ фазой `reboot` (417 и 426 с, сеанс консоли `VM2B\Администратор`) |
| B. Ротация пароля gMSA | подтверждено способом Б | способ А (`Set-ADObject -Replace @{pwdLastSet=0}` у объекта gMSA): **отказано в доступе** (gMSA такую запись не принимает), идентификатор не менялся. Способ Б (синхронный сдвиг часов обеих ВМ на +45, +95, +200 суток, w32time выключен, абсолютное время хоста + N суток, разница между ВМ около 1 с): `msDS-ManagedPasswordId` и `pwdLastSet` **изменились** (например L1/L2-индексы `13/13 -> 1F/13`, `pwdLastSet` +45 суток; `ROTATION_B_CHANGED=True` в трёх этапах). `Test-ADServiceAccount` новый пароль не порождает: ротация происходит при первой ВЫДАЧЕ пароля, то есть при старте службы под gMSA. Служба, запущенная ДО ротации, без перезапуска продолжала копировать (`Success`); после ротации: `Test-ADServiceAccount` True, перезапуск службы, копия на шару `Success`, без перезагрузки |
| C. Срок пароля (1330) | подтверждено, исправлено | при сдвиге часов на +45 суток и более у `expuser` `PasswordExpired=True` (максимум 42 суток). **Windows возвращает НЕ 1330, а 1208 (`ERROR_EXTENDED_ERROR`)**; настоящий ответ - `WNetGetLastError`: код провайдера **2242 (NERR_PasswordExpired)**, провайдер `Microsoft Windows Network`, текст по-русски. Продукт этого кода не знал: ошибка не считалась отказом учётных данных, защита не включалась (6 проходов планировщика = 6 обращений). Исправлено (`SmbResolveExtendedError`, `SmbMapProviderError`: 2242 -> 1330, NTSTATUS -> Win32): тест `domainexpire/domain_expired_password_is_1330_guard_blocks_new_password_unblocks` проходит: постоянная английская ошибка 1330 без пароля, **1 обращение** на 6 проходов, сервер помечен, сообщение «not tried again...», после смены пароля пользователя и записи сервера с новым паролем метка снята, копия `Success` |
| D. Счётчик SCP | подтверждено, исправлено | новые тесты группы `ssh` (настоящий sshd с `LogLevel VERBOSE`, сервер `scp_authlog`, `src/tests_ssh_guard.cpp`): `auth_guard_scp_password_one_attempt` (6 проходов планировщика с неверным паролем = **1** «Failed password» и 1 соединение, затем верный пароль = 1 вход, затем снова неверный = 1), `auth_guard_scp_key_one_attempt` (неизвестный ключ: 1 «Failed publickey»), `auth_guard_scp_key_wrong_passphrase_one_attempt`. **Ошибка продукта:** неверная парольная фраза ключа (`LIBSSH2_ERROR_FILE`) не считалась отказом: 6 проходов = 6 соединений с сервером, метка не ставилась. Исправлено в `src/ssh.cpp` (теперь 1 соединение) |
| E. `ftp_large_streamed` | не воспроизведено | хост (Wine): 51 прогон подряд с журналом DEBUG (из них 39 FTPS, часть из них три параллельных потока при высокой загрузке процессора) - 0 отказов; гость member (Server 2022), 10 прогонов фазой `ftp` (6 FTPS, 60-191 МБ) - 10 из 10, журналы серверов в `results-member/srv-ftp/`. Причина не найдена, продукт и тестовый сервер не менялись. Добавлены: `tools/ftp-repeat.sh`, переменная `VM2B_FTP_KEY` (закрепить вид сервера), фаза `ftp` |
| F. 8 FAIL `domain-svc.ps1` | не воспроизведено, устойчивость повышена | три прохода `svc` (после установки, повтор, после перезапуска обеих ВМ) - по 48 из 48. Ответ `servers.save` без идентификатора при том, что запись потом видна в списке, объясняется кодом: `Config::saveServer` менял список в памяти до записи `config.json`, и при неудачной записи (временный файл `config.json.tmp` занят сканером: `CreateFileW` без повтора) запись оставалась «призраком». Исправлено: повтор создания временного файла (`WriteWholeFile`), откат списка серверов/задач в памяти при неудаче записи; тесты `smb/config_save_waits_for_a_held_tmp_file`, `smb/config_failed_save_leaves_no_ghost_record`. Скрипт `Save-Smb` теперь пишет в журнал `SAVE-NO-ID` с телом ответа |
| G. Обзор сети в домене | исправлено | на member: `WNetOpenEnum(GLOBALNET)` даёт только провайдеров, `NetServerEnum` - ошибки 87 / 6118 (служба «Браузер» отсутствует), а **`DsGetDcName` + LDAP (`wldap32`, вход под учётной записью процесса: пользователь и SYSTEM = `MEMBER$`) находят контроллер и все компьютеры домена** (`domain-browse.ps1`). Реализовано в `src/netbrowse.inc` (`enumDomain`): корень сетевого окружения в домене показывает контроллер и серверы домена (ответ с полем `domain`), без домена прежнее поведение с подсказкой; запрос к каталогу идёт первым, при зависшем обходе NetBIOS отвечает найденным, последний ответ каталога запоминается. На домене: корень = `DC.vm2b.test`, `MEMBER.vm2b.test` (тест `domainreal/domain_net_browse_server_share_and_network` теперь проверяет и это). Привязка LDAP на нагруженном стенде занимала до 48 с (весь корень до 60 с), поэтому добавлены ответ из памяти и частичный ответ; тесты `smb/netbrowse_root_*` (4 штуки) |

Полный прогон `tools/run-tests.sh --seed 9191` на Wine после всех правок: native ok, core 150/0, core32 65/0, ftp 33/0 (1 пропуск), ssh 64/0, esxi 67/0, web 1/0, gui 4/0, package ok, package2 ok (`.winvm-results/domain-20261028-round6/fulltests-summary.txt`).

Побочное: при возврате часов относительным `Set-Date -45 суток` часы member однажды вернулись только на 22,5 суток; теперь `run-domain.sh` выставляет ОБЕИМ ВМ абсолютное время хоста + N суток (`set_clocks`). Сдвиг часов без WinRM: всё делается заданиями хоста.

## Раунд 7: подписанный установщик, SmartScreen, UAC и Smart App Control в режиме «On» (Windows 11 25H2 ru, сборка 26200.9445), 28.10.2026

Одна ВМ `vm2b-win` (Windows_11_5in1_25H2, RAM 4 ГБ / 4 ядра, установка 3508 с). Выполнены фазы `install`, `installer`, `panel`, `web`, `smartscreen` (расширенная), `signing` (новая), `sac` (расширенная, последней); остальные пропущены (полный набор на этой ОС уже проверен в раундах 1 и 4). Прогон шёл по этапам на одной ВМ (`--keep --no-create --phases ...`), результаты: `.winvm-results/w11-20261028-signing/` (логи этапов `w11-run1.log` ... `w11-run4.log`, `results/*.out`, `results/shots/`). ВМ и диск удалены после прогона.

### Что сделано для подписи (в продукте и обвязке)

* `tools/sign.sh`: Authenticode SHA-256 через `osslsigncode` (PFX или пара PEM; пароль только из файла / переменной окружения / stdin, в командную строку и журнал не попадает; метка времени и `-n` / `-i` по параметрам; `--verify`). `package.sh` подписывает при заданной `VM2B_SIGN_PFX`: сначала все exe полезной нагрузки, потом итоговый одиночный `VM2Backup-Setup.exe` целиком. Без переменных пакет собирается как раньше. Документ `docs/SIGNING.md`.
* `tools/make-test-cert.sh`: тестовый корень + сертификат подписи кода (RSA 3072 / SHA-256, EKU 1.3.6.1.5.5.7.3.3) в `.winvm/shared/pki/` (в `.gitignore`: каталог, `*.pfx`, `*.p12`, `*.key`).
* **Найдено и исправлено в продукте:** подпись добавляет таблицу сертификатов ПОСЛЕ хвоста `VM2BPKG1` (его старый поиск «последние 16 байт файла» подписанный файл не находил), а `osslsigncode` выравнивает файл до 8 байт нулями. Теперь загрузчик (`src/vm2blauncher.cpp`) читает заголовок PE, берёт начало таблицы сертификатов за конец данных и пропускает до 7 нулей; `package.sh` выравнивает файл до 8 байт ДО подписи (нули между загрузчиком и cab), так что отступа за хвостом нет. Регрессионные тесты в `tools/test-package-wine.sh`: подписанный одиночный файл подписан и проверяется, хвост не в конце файла, ставится и удаляется под Wine, временная папка убрана, файл с изменённым байтом не проходит проверку подписи; сравнения файлов в Program Files учитывают подпись (`same`).
* Обвязка: `.winvm/guest/signing.ps1` (подпись, доверие, UAC, запуск «двойным щелчком» с Mark of the Web, нажатия в окне SmartScreen через UI Automation, состояние, события), расширенные `smartscreen.ps1` и `sac.ps1`, фаза `signing`. Отключено сочетание «Залипание клавиш» (снимки хоста посылают Shift) на время теста.

### Подпись и проверка на госте (`Get-AuthenticodeSignature`)

| Файл | Статус |
|---|---|
| не подписан | `NotSigned` |
| подписан, корень НЕ доверен | `UnknownError`: «Цепочка сертификатов обработана, но обработка прервана на корневом сертификате, у которого отсутствует отношение доверия с поставщиком доверия» (подпись математически верна) |
| подписан, корень в `LocalMachine\Root`, сертификат в `TrustedPublisher` | `Valid`: «Подпись проверена» |
| изменён один байт после подписи | `HashMismatch` (при доверенном и недоверенном корне) |

После установки подписанного пакета `VM2Backup.exe`, `VM2BackupService.exe` и копия установщика в `Program Files\VM2Backup\` подписаны (Valid при доверенном корне): UAC и SmartScreen видят издателя у внутренних файлов, а не только у внешней оболочки.

### Что видит пользователь: четыре сочетания (файл с Mark of the Web, ZoneId=3, двойной щелчок; снимки `shots/signing/<метка>-N-*.png`)

На dockur UAC выключен политикой (`EnableLUA=0`, `ConsentPromptBehaviorAdmin=5`): для теста включены `EnableLUA=1`, `ConsentPromptBehaviorAdmin=2` (запрос на безопасном рабочем столе) с перезапуском, после теста возвращены с перезапуском.

| | SmartScreen («Система Windows защитила ваш компьютер») | UAC после «Подробнее» -> «Выполнить в любом случае» |
|---|---|---|
| (c) без подписи | окно с единственной кнопкой «Не выполнять»; после «Подробнее»: приложение VM2Backup-Setup.exe, **Издатель: Неизвестный издатель**, кнопки «Выполнить в любом случае» / «Не выполнять» | жёлтая шапка: «Разрешить этому приложению **от неизвестного издателя** вносить изменения на вашем устройстве?»; имя файла VM2Backup-Setup.exe, «Издатель: Нет данных» |
| (a) подписан, корень не доверен | то же окно, **Издатель: Неизвестный издатель** (недоверенная подпись для SmartScreen как её отсутствие) | то же, что у (c): неизвестный издатель, «Нет данных» |
| (b) подписан, корень доверен, издатель в TrustedPublisher | **окно SmartScreen всё равно показывается** (репутации у тестового сертификата нет), но в нём **Издатель: Symo (TEST), Symo TEST Code Signing**; кнопка «Выполнить в любом случае» есть | синяя шапка, значок и имя программы «VM2Backup» (из подписи), «**Проверенный издатель**: Symo TEST Code Signing» |
| (d) подписан и доверен, файл изменён после подписи | как у (c): Издатель: Неизвестный издатель (подпись недействительна); «Выполнить в любом случае» доступно | неизвестный издатель, «Нет данных» |

Выводы по сочетаниям: подпись меняет **строку издателя** в SmartScreen и UAC, но **не убирает окно SmartScreen для нового файла** (репутацию даёт только настоящий сертификат и накопленные скачивания); файл с недействительной подписью **не блокируется** самой Windows (SmartScreen / UAC считают его неподписанным: блокируют его только политики SAC / WDAC / AppLocker, см. ниже). Окно «Открыть файл - предупреждение системы безопасности» в автоматических запусках не появлялось (в одном ручном запуске оно показывалось над окном установщика).

### Smart App Control в режиме «On» (выполнено последней фазой, необратимо)

* **Включение:** запись `VerifiedAndReputablePolicyState=1` в реестр (раунд 4) принудительного применения не включала. **Через интерфейс** «Безопасность Windows» (`windowsdefender://smartapp`; страница «Smart App Control», переключатели «On / Evaluation / Off»; UI Automation, `SelectionItemPattern.Select()` на «On») состояние 2 -> **1**, после перезапуска в журнале `Microsoft-Windows-CodeIntegrity/Operational` событие **3099**: активирована политика `VerifiedAndReputableDesktop` ({0283ac0f-...}); реальное принудительное применение работает (события 3077 / 3033 / 3118).
* У ВМ **есть интернет** (DNS, HTTPS www.microsoft.com = 200): облачная проверка репутации SAC доступна. Решение SAC по файлу **не оказалось детерминированным** (см. ниже): опытные факты записаны как есть.

Что запускалось при SAC = On (корень НЕ доверен / затем доверен):

| Что | Результат |
|---|---|
| неподписанный Setup (с меткой и без), первый и все повторные запуски | запустился (корень не доверен и доверен) |
| подписанный Setup (тестовым центром), с меткой и без | запустился (и без доверия к корню) |
| подписанный и изменённый (`HashMismatch`) с меткой Mark of the Web, **первый запуск** | **заблокирован**: «Политика управления приложениями заблокировала этот файл», события 3077 / 3033 (политика {0283ac0f-...}) / 3118 («Smart App Control Block Details») |
| то же изменённое: без метки, и далее три повтора с меткой, и после добавления корня в доверенные | запустилось |
| свежесобранный неподписанный консольный exe (`Add-Type`, никогда не виденный облаком): `fresh1` (с меткой и без) | **заблокирован** (Win32 «Политика управления приложениями заблокировала этот файл», 3077 / 3033 x2 на файл) |
| такой же `fresh2` собранный сразу после | запустился (код 7), **без блокировки** |
| `vm2bautotest.exe`, `VM2Backup.exe`, `VM2Backup-Setup.exe`, `VM2BackupService.exe` из тестовой сборки (не подписаны) из папки «Загрузки» | запустились |
| **служба VM2Backup** (подписана тестовым центром) | **запускается и работает** при SAC = On: после перезапуска 1 (корень НЕ доверен) была запущена (остановлена только тихой переустановкой в 18:14, `service.log`), после перезапуска 2 (корень доверен) запущена автоматически с отложенным стартом (18:18:32 и далее 18:22:12), `Running`, `VM2BackupService.exe`, `VM2Backup.exe`, `VM2Backup-Setup.exe` в Program Files со статусом Valid; тихая установка подписанного пакета (`/install /silent`): rc 0 |
| неподписанная служба (`VM2BackupService.exe` из тестовой сборки) | процесс запустился (вне SCM завершился кодом 1 как обычная консольная проверка) |

Честный итог: **на этой ВМ SAC в режиме On блокировал отдельные файлы (изменённый подписанный с меткой; свежий неподписанный exe), но не стабильно**: часть неподписанных и подписанных тестовым (недоверенным) центром файлов запускалась. Решение SAC зависит от облачной репутации и локальной политики (`VerifiedAndReputableDesktop` по хэшу файла, происхождению и подписи), его нельзя воспроизвести детерминированно в тесте; поэтому вывод «подписанный тестовым корнем код пройдёт / не пройдёт SAC» **сделать нельзя**. Надёжно установлено: (1) включить SAC можно только через интерфейс; (2) служба продукта с подписью тестового центра запускается и работает при состоянии 1 (с доверием к корню и без него); (3) SAC обращается к облаку, у ВМ интернет есть. Для пользователей с включённым SAC, у которых блокируется неподписанный установщик, нужен подписанный настоящим сертификатом (репутация по издателю), как описано в `docs/SIGNING.md`; проверить это без настоящего сертификата нельзя. Тестовый сертификат **не даёт репутации SmartScreen** и не принимается SAC как доверенный издатель (он не входит в программу доверенных корней Microsoft).

### Остальные фазы раунда (с подписанным пакетом, UAC выключен)

`install` 32 / 0, `installer` (обновление, удаление, переустановка, ACL) 10 / 0, `panel` (`/selftest`, `/guitest`, 100 / 125 / 150 %) 3 x rc 0, `gui` 4 / 0 на каждом масштабе, `web` 41 / 0 (в том числе DPAPI после перезапусков, подключение с хоста через проброс). Расширенная `smartscreen`: «Подробнее» нажимается через UI Automation (`shots/smartscreen-2-more-info.png`).

### Не проверено

* Репутация SmartScreen и допуск SAC для настоящего сертификата (OV / EV, облачная подпись): нужен настоящий сертификат.
* Метка времени подписи (RFC 3161): не использовалась (сети не нужно).
* ~~Windows 10 / x86 с подписанным пакетом~~ - проверено в раунде 8 (30.10.2026); не проверено на Windows 10 x64 ru (следующая ОС цикла).

## Раунд 8: Windows 10 Pro x86 (22H2, сборка 19045.3803, 32-битная нагрузка), цикл 3, 30.10.2026

Первая ОС третьего цикла (дальше по плану: Windows 10 x64 ru, Windows 11). Одна ВМ `vm2b-win` (ISO `WIN10X86.iso`, чистый qemu внутри `dockurr/windows`, RAM 3 ГБ / 4 ядра, установка 1812 с), удалена после прогона вместе с диском, тестовым центром сертификации (`.winvm/shared/pki`) и файлом `hold`. Результаты: `.winvm-results/x86-20261030-cycle3/` (`run1/` - первый полный прогон до правок продукта; `run2/` - перепроверки после правок: `winreal`, `signing` со включённым SmartScreen, `net` ftp / ssh, `tz`; `run3/` - `ntfsav` и `eicar`; `run4/` - последняя перепроверка `signinst` и осенний сценарий Lord Howe; `final-destroy-copy/` - копия, снятая `destroy.sh`; в каждом `results/*.out`, `results/autotest-*.txt`, снимки, журналы `r8-run*.log`). **Честная оговорка:** создание ВМ шло отдельной командой (`tools/run-winvm.sh x86 --keep --phases sysinfo`), пока писались новые фазы, потом `--no-create` с полным набором фаз; так что «одной командой с нуля» цикл 3 в этой ОС не повторялся (создание с нуля одной командой проверено в раундах 2 - 4).

### Базовые фазы (первый прогон, seed 872173946, до правок продукта)

| Фаза | Результат |
|---|---|
| `autotest` core / service / gui | 108 из 108 (3 пропуска) / 28 из 28 / 4 из 4 |
| `winreal` | 10 из 11: упал новый тест расписания через переход на летнее время (см. «Найдено и исправлено»); после правок 12 из 12 |
| `install` / `installer` (обновление, удаление, переустановка, ACL) / `unicode` | 32 / 0, 10 / 0, 2 / 0 |
| `panel` (`/selftest`, окна на 100 / 125 / 150 %) | rc 0 на всех трёх масштабах, `gui` 4 из 4 на каждом |
| `web` (TLS клиентов Windows, вход, DPAPI через перезапуск службы и Windows) | 38 / 0 |
| `net`: ftp / ssh / esxi (эмулятор внутри гостя) | 32 из 33 (известный предел: активный FTP за NAT qemu) / 59 из 63 (см. ниже) / 65 из 65 |
| `smb` (`smbreal`, диск `Y:`, `smbinfo`) / `smbsign` | 11 + 1 + 5 из 17 / 6 из 6 |
| `svcacct` / `aclreal` / `speedlimit` / `restartjob` | 46 / 16 / 10 / 12, без падений |
| `big` / `winbig` / `verify` | 3 из 3 (самый долгий 554 с) / 6 ГиБ разреженный vmdk: копия 147 с, 25 томов, 4,1 МБ, пик рабочего набора 50 МиБ / 94 из 94 |

Падения сети, не связанные с продуктом:

* `ftp_passive_active_and_listing_variants` - активный FTP (известный предел из раздела «Known limits»).
* `ssh`: `auth_guard_scp_password_one_attempt`, `auth_guard_scp_key_one_attempt`, `auth_guard_scp_key_wrong_passphrase_one_attempt` (тесты добавлены 28.10, на x86 запущены впервые). Они считают строки в журнале `sshd`, который хост дописывает, а гость читает через SMB: клиент SMB Windows отдаёт кеш (счётчики 0 при настоящих попытках в журнале хоста) - тот же предел, что у ESXi (раунд 2). Те же тесты под Wine проходят (64 из 64). `scp_fault_drop_listing` ("0 of 6 listings were cut": ни одно из шести обрывов не попало) нестабилен по времени, под Wine один раз упал так же, в полном прогоне Wine прошёл. Это границы обвязки, не продукта.
* Второй прогон `net`: `ftp_fault_injection_upload` один раз ("receive timed out", 74 с); в первом прогоне и в 10 повторах следующей проверки не воспроизвёлся.

### Новые проверки раунда

**(1) Подписанный установщик на Windows 10 x86** (`signing`, `signinst`; тестовый корень `tools/make-test-cert.sh`, варианты `tools/winvm-variants.sh`: unsigned / signed / tampered (байт в PE) / tampered-cab (байт внутри пакета), подписаны и все exe внутри пакета):

| Проверка | Результат |
|---|---|
| `Get-AuthenticodeSignature` | неподписанный `NotSigned`; подписанный без доверия к корню `UnknownError` («корень не доверен»), после доверия `Valid`; изменённый после подписи `HashMismatch` (оба варианта) |
| Загрузчик (32-битный PE, 0x14C) | тихая установка подписанного пакета `/install /silent`: rc 0; служба 32-битная (0x14C) |
| Подписи внутри Program Files | `VM2Backup.exe`, `VM2BackupService.exe`, копия `VM2Backup-Setup.exe`: `Valid` (корень доверен) |
| Обновление поверх старой версии (служба работала) | rc 0, новый процесс службы, данные (запись сервера) сохранены |
| Изменён байт в загрузчике | подпись `HashMismatch`, но установщик работает как обычно (проверку подписи делает Windows), rc 0 |
| Изменён байт внутри пакета | rc 1, без зависания, ничего не сломано (служба та же), временная папка убрана; **сообщение раньше было недоступно** (в тихом режиме окон нет, оставался только rc 1): теперь строка в `%TEMP%\VM2Backup-Setup.log`: «The installation package is damaged or incomplete (it was changed after it was built, or the download was cut short). Download Setup again (Windows error 13)»; без `/silent` то же в окне (по-русски на русской Windows) |
| Удаление и переустановка подписанного | rc 0, файлы `Valid` |

Что видит пользователь (снимки с включённым SmartScreen: `run2/results/shots/signing/<метка>-N-*.png`; первый прогон, SmartScreen выключен политикой bootstrap, показывает только UAC: `run1/results/shots/signing/`; англоязычная Windows 10 22H2; **на тестовых ВМ bootstrap выключает SmartScreen политикой `EnableSmartScreen=0`: на время фазы `signing` политика снимается, `SmartScreenEnabled=Warn`, потом возвращается**; интернет у ВМ есть):

* Файл с Mark of the Web (ZoneId=3), двойной щелчок: окно **«Windows protected your PC»** («Microsoft Defender SmartScreen prevented an unrecognised app from starting. Running this app might put your PC at risk.»), кнопка «More info» открывает строки Application / Publisher и кнопку «Run anyway». Для подписанного и доверенного файла: **Publisher: Symo TEST Code Signing** (у неподписанного и недоверенного строка Publisher закрыта посторонним системным окном «How do you want to open this?», которое на этой ВМ после ручных нажатий клавиш в госте появлялось поверх снимков; их UAC-окна снимались без помех в первом прогоне); окно показывается всё равно (у тестового сертификата нет репутации, как на Windows 11 в раунде 7).
* UAC после «Run anyway»: подписанный и доверенный - синяя шапка, значок и имя «VM2Backup», **«Verified publisher: Symo TEST Code Signing»**, «File origin: Hard drive on this computer»; неподписанный - жёлтая шапка «…from an unknown publisher…», «Publisher: Unknown»; подписанный недоверенный и изменённый - как неподписанный.
* Запуск файла с Mark of the Web через ShellExecute из скрипта (как делает проводник) показывает окно **«Open File - Security Warning»** («Do you want to run this file?», Publisher: Symo TEST Code Signing, кнопки «Run» / «Cancel») **даже при SmartScreen выключенном**, и вызывающий скрипт стоит, пока кто-то не нажмёт: **при тихой установке из средств развёртывания надо запускать установщик через CreateProcess** (так делают msiexec / PsExec / SCCM и `Start-Process` с `UseShellExecute=false`) - тогда запроса нет. Окно записано снимком `run1/shot-win10-open-file-security-warning.png` (первый прогон, когда SmartScreen на ВМ ещё выключен). В повторных прогонах автоматическое нажатие «Run» не сработало: поверх окна каждый раз появлялось системное окно «How do you want to open this?» (выбор приложения; на этой ВМ оно возникло после ручных нажатий клавиш в госте и больше не уходило), поэтому ShellExecute в `signinst.ps1` оставлен справочным блоком без проверок (`INFO ShellExecute start ...`).
* Окно SmartScreen не закрывается подписью тестового центра; пользователю по-прежнему нужны «More info» -> «Run anyway» (репутацию даёт настоящий сертификат и накопленные скачивания: `docs/SIGNING.md`).

**(2) Часовые пояса и перевод времени на настоящей Windows** (фаза `tz`, `guest/tzreal.ps1`; w32time выключен и остановлен, задачи «Time Synchronization» выключены, `NoSync`; часы ставятся `SetSystemTime`, дальше идут сами):

* Родные тесты `winreal/schedule_across_dst_transitions_matches_the_windows_clock` (каждая минута вокруг перехода сверена с эталоном Windows `SystemTimeToTzSpecificLocalTime`; ежедневно 02:30 двумя видами записи, каждые 30 минут, «один раз 02:30» в день перехода, окно 22:00-06:00, ежемесячно 31-го и в последний день) и `scheduler_runs_each_slot_once_across_dst_transitions` (настоящий планировщик службы на подменённых часах идёт минута за минутой через весенний и осенний переход; число и время запусков сверяются с ожиданием), пояса **W. Europe**, **Pacific**, **Lord Howe** (сдвиг 30 минут) и **GMT** (исходный пояс гостя): до правок падали, после правок **2 из 2 в каждом поясе**.
* Настоящая служба и настоящие часы (две задачи: ежедневно в 02:05 и каждые 30 минут от 00:00; наблюдение по истории запусков):
  * **Весна, W. Europe** (часы 2027-03-28 01:56:30; пропущенный час 02:00 -> 03:00): ежедневная в 02:05 (такой минуты нет) - **ровно один запуск в 03:00:00 по местному**, каждые 30 минут - один запуск в 03:00:00 (слоты 02:00 и 02:30 не накапливаются); 2 запуска за 18 минут, часы никто не трогал (`25-tz-spring-eu.out`, 8 проверок из 8).
  * **Весна, Pacific** (2027-03-14 01:56:30): так же, оба запуска в 03:00:01 по местному, по одному (`25-tz-spring-us.out`; проверка «ровно один запуск» в этом прогоне упала только потому, что считала записи истории предыдущего сценария: сценарий теперь фильтрует записи по времени начала и очищает историю). **До правок** (`run1/results/25-tz-spring-us.out`) на той же службе `tasks.next` для «ежедневно в 02:05» отвечал «2027-03-15 02:05»: запуск в день перехода пропускался целиком.
  * **Осень, Lord Howe** (сдвиг 30 минут, 2027-04-04 02:00 LHDT -> 01:30 LHST; часы ставятся на 01:43 ПЕРВОГО прохода): ежедневная в 01:45 (минута бывает дважды) - **один запуск в 01:45:02 первого прохода** (14:45 UTC), второй проход (01:45 LHST, 15:15 UTC) слот НЕ повторил; каждые 30 минут - запуск в 02:00:00 LHST (15:30 UTC), как положено после повторяющегося получаса; 2 запуска за 52 минуты, часы никто не трогал (`27-tz-autumn-lh.out`, 8 проверок из 8; первая попытка этого сценария началась уже ПОСЛЕ перехода из-за неоднозначного времени и в счёт не идёт: сценарий теперь берёт первый проход).
* Служба и смена пояса без перезапуска: `winreal/schedule_follows_a_time_zone_change_without_a_restart` (Tokyo, UTC, Pacific, W. Europe подряд через `tzutil /s`): после правок проходит.

**(3) Утечки ресурсов службы и панели** (`leaks`, `guest/leaks.ps1`, 36 минут нагрузки): 136 копий малой ВМ, 204 входа / выхода через сайт, 68 переключений порта сайта 8443 <-> 8444 (сайт перезапускался каждый раз), панель открыта и закрыта 816 раз (200 - порог), служба перезапущена 2 раза; замер каждые ~2 минуты (`results/leaks.csv`: HandleCount, потоки, PrivateMemorySize64, WorkingSet64, GDI / USER через `GetGuiResources`, которые на настоящей Windows возвращают значения).

| Процесс (после прогрева 4 минуты, последний отрезок жизни) | Дескрипторы | Потоки | Закрытая память | GDI / USER |
|---|---|---|---|---|
| служба (5 замеров, 28 - 36 мин) | 225 -> 228 (+3; порог < 50) | 0 | 4,3 -> 4,8 МБ (+0,5; порог < 30) | 0 / 0 |
| долгоживущая панель (16 замеров, 4 - 36 мин) | 190 -> 190 (0) | +2 | 1,9 -> 2,0 МБ | 30 / 35 -> 30 / 33 (не растёт) |

Рост не монотонный: дескрипторы службы колеблются 222 - 245 в течение всех трёх отрезков (после перезапуска возвращаются к 225). **Утечек нет** (14 проверок, 14 прошли).

**(4) NTFS и антивирус** (`ntfsav`, `eicar`, `guest/ntfsav.ps1`; служба работает под SYSTEM, задание ставит пользователь `Docker` из группы администраторов):

| Случай | Что сделал продукт |
|---|---|
| (a) файл под EFS (`cipher /e` от того же пользователя; `Archive, Encrypted`) | задание **Failed** за 0,016 с: «cannot read enc.vmdk: Access is denied (5)» (имя файла и причина названы; служба продолжает работать; копия ВМ целиком не создаётся: один нечитаемый файл = неудача копии ВМ) |
| (b) NTFS-сжатый файл (`compact /c`, 5,2 : 1) и разреженный файл (256 МБ логических, 2 МБ выделено, два диапазона) | **Success** (сырых 269 МБ, архив 2,15 МБ); восстановление побитно (SHA-256) совпало у обоих. Восстановленный разреженный файл **не разреженный** (на диске 256 МБ): содержимое верное, но место не экономится |
| (c) EICAR (Defender запущен: политика `DisableAntiSpyware` на время снята, сигнатуры 1.303.25.0 от 2019 года достаточны) | Defender обнаружил `Virus:DOS/EICAR_Test_File` (события 1116, `Get-MpThreatDetection`: действие выполнено) уже после первых секунд (через 12 с файл ещё лежал); задание **Failed** за 0,08 с: «cannot read eicar.com.txt: Operation did not complete successfully because the file contains a virus or potentially unwanted software (225)»; без зависания, готовой копии не осталось |
| (d) файл с именем из 255 символов и каталог с 5000 файлов | **Success**, все 5000 файлов и файл с длинным именем восстановлены с тем же содержимым (SHA-256 по всему набору) |
| (e) файл исчез посреди копии (большой файл с ограничением 12 Мбит/с, через 6 с удалён ещё не прочитанный `z-vanish.vmx`, создан EICAR) | задание **Failed** через 14 с: «cannot read z-vanish.vmx: The system cannot find the file specified (2)»; незавершённой копии, считающейся успешной, нет (0 готовых копий); Defender удалил и второй EICAR |

Журнал Defender записан в `results/23b-eicar.out` (события 1116, `Get-MpThreatDetection`); ничего в Defender не отключалось, политика `DisableAntiSpyware=1` возвращена после проверки.

**(5) Повтор `ftp_large_streamed` 10 раз** на госте с журналом сервера DEBUG (`VM2B_FTP_LOGLEVEL`, свои серверы на каждый прогон): **10 из 10 прошли** (6,9 - 18,8 с; журналы `results/srv-ftprepeat/run-N/`). Закономерность одна: тест сам выбирает обычный FTP или FTPS по зерну: 7 прогонов шли через FTPS (в `ftps.log` 6 - 15 тысяч строк DEBUG на записи TLS, 7,1 - 18,8 с), 3 через обычный FTP (`ftp.log` 75 строк, 6,9 / 10,1 / 13,9 с); времени и размеру журнала сбой не соответствует, обрывов в журналах нет, в двух обычных прогонах `net` тест тоже прошёл. Редкий сбой не воспроизвёлся.

### Итог перепроверки после правок (та же ВМ, seed 872173946)

| Что | Результат |
|---|---|
| `winreal` (с двумя тестами расписания и тестом смены пояса) | 12 из 12 |
| `tz`, родные тесты в поясах W. Europe / Pacific / Lord Howe | 2 из 2 в каждом |
| `tz`, настоящая служба и часы | весна W. Europe 8 из 8; весна Pacific: оба запуска ровно по разу в 03:00:01 (одна проверка упала на записях истории прошлого сценария: ошибка обвязки, исправлена); осень Lord Howe 8 из 8 |
| `signinst` | 30 из 30 |
| `ntfsav` / `eicar` | 20 из 20 / 8 из 8 |
| `net` ftp / ssh (второй прогон) | 31 из 33 (активный FTP; `ftp_fault_injection_upload` один раз по времени) / 59 из 63 (границы обвязки, см. выше) |
| Wine, полный `tools/run-tests.sh --seed 7773` (логи в `wine/`) | все 10 наборов ok: native, core 150 / 0 (197 пропусков: тесты только для настоящей Windows), core32 65 / 0, ftp 33 / 0, ssh 64 / 0, esxi 67 / 0, web, gui 4 / 0, package (37 проверок, в том числе «damaged cabinet»), package2 |

### Найдено и исправлено в продукте (регрессии - в тестах)

1. **Несуществующая локальная минута (весенний переход) превращалась в момент на час РАНЬШЕ** (Windows CRT `_mktime64`, изоляция проверена на W. Europe и Pacific; glibc ставит такую минуту на час ПОЗЖЕ): «ежедневно в 02:30» запускалось в 01:30 дня перехода, «один раз в 02:30» - на час раньше срока, а «ежедневно в 02:05» на настоящей службе в Pacific не запускалось в этот день вовсе (расписание получало момент из прошлого и уходило на следующие сутки). **Исправление (`src/schedule.cpp`, `LocalMinToUnix`)**: несуществующая минута -> первый момент после «дыры» (03:00), одинаково на всех CRT. Регрессии: `tests/native/t_schedule.cpp` (`NonexistentMinute`: Berlin, New York, Lord Howe), `winreal/schedule_across_dst_transitions...` (запуск не раньше 02:30), настоящие часы `tz/spring-*` (запуск в 03:00 ровно один раз).
2. **Минута, которая бывает дважды (осенний переход), превращалась во ВТОРОЕ вхождение**: планировщик идёт по первому проходу в реальном времени и никогда не встречал слот; «каждые 30 минут» терял 1 - 2 часа, «ежедневно в 01:30» в день перехода не запускалось вовсе. **Исправление:** первое вхождение (второй проход повторяющегося часа слот не повторяет: запуск ровно один раз). Регрессии: `t_schedule.cpp` (`AmbiguousMinute`: Berlin, New York, Lord Howe, London), `winreal/scheduler_runs_each_slot_once_across_dst_transitions` на настоящей Windows (до правки тест ждал недостающий запуск часами).
3. **Служба не замечала смену часового пояса** (msvcrt читает пояс один раз при первом вызове): после `tzutil /s` службе нужен был перезапуск, до него «следующий запуск» и планировщик шли по старому поясу. **Исправление:** `_tzset()` не реже раза в 2 секунды в `UnixToLocalMin` / `LocalMinToUnix`. Регрессия: `winreal/schedule_follows_a_time_zone_change_without_a_restart`.
4. **Тихая установка с повреждённым пакетом давала только rc 1 без объяснения.** **Исправление (`src/vm2blauncher.cpp`)**: причина записывается в `%TEMP%\VM2Backup-Setup.log`, окно (без `/silent`) называет повреждённый пакет вместо «не удалось распаковать»; регрессия `tools/test-package-wine.sh` («damaged cabinet»).

Из логики настоящего времени: отсутствие повтора в повторяющийся час **задумано** (слоты расписания - минуты настенных часов): «каждые 30 минут» осенью занимает паузу до 90 реальных минут (`gaps 30..90 min` в тесте), «ежедневно в 01:30» в день перехода запускается ровно один раз.

### Изменения обвязки и тестов раунда

* Фазы `signinst`, `leaks`, `ntfsav`, `eicar`, `ftprepeat`, `tz` и расширенная `signing` (для Windows 10 включает SmartScreen на время фазы); по умолчанию входят в списки `x86`, `w10`, `w11`.
* Скрипты `guest/signinst.ps1`, `leaks.ps1`, `ntfsav.ps1`, `defender-toggle.ps1`, `tzreal.ps1` (UTF-8 с BOM: иначе PowerShell 5 ломает строки с кириллицей); `tools/winvm-variants.sh`.
* `tzreal.ps1`: часы ставятся `SetSystemTime` (не `Set-Date`: .NET держит пояс, прочитанный при запуске процесса); перед сценарием служба и задачи синхронизации времени выключаются (иначе `svchost` возвращал время «с узла» через 1 - 3 минуты: в журнале `Kernel-General` событие 1, процесс `svchost.exe`).
* Знание про обвязку: **Defender на тестовых ВМ выключен политикой `DisableAntiSpyware=1` из `bootstrap.cmd`** (служба не запущена, `Get-MpComputerStatus` даёт `0x800106BA`); фаза `eicar` снимает политику на время проверки (перезапуск) и возвращает её (перезапуск); ничего в самом Defender не меняется.
* Если тест (или человек) оставил на гостевом экране окно, исполнитель после жёсткого `system_reset` ждёт: после сброса остаются сеансы SMB прежнего гостя, и qemu-smbd отвечает «NT_STATUS_IO_TIMEOUT» на открытие файлов `current.txt` / `*.out`; надёжный выход - корректный перезапуск гостя (`shutdown /r`) или `system_powerdown` + `docker start`.

### Не проверено

* (SmartScreen и «Open File - Security Warning» на русской Windows 10 проверены в раунде 9.) Репутация и допуск для настоящего сертификата (OV / EV); метка времени подписи (RFC 3161).
* Несуществующее / повторяющееся время для поясов без перехода в 2027 году и для зон с годовыми правилами, отличающимися от правил текущего года (UCRT / msvcrt читают правила один раз).

## Раунд 9: Windows 10 x64 ru (22H2, сборка 19045.3570, 64-битная нагрузка), цикл 3, 31.10.2026

Вторая ОС третьего цикла. Одна ВМ `vm2b-win` создана и прогнана **одной командой с нуля** (`tools/run-winvm.sh w10 --keep`: ISO `ru-ru_windows_10_business_editions_version_22h2_updated_oct_2023_x64_dvd_6072cf93.iso`, `dockurr/windows`, RAM 4 ГБ / 4 ядра, диск 32 ГБ; установка 1481 с, весь план 21389 с; ничего не пересоздавалось и не докручивалось руками). Пересборка ВМ не понадобилась; ключ `--keep` нужен был только для разбора и перепроверок после правок (`--no-create --phases ...`). ВМ, диск, сеть, `.winvm/shared/pki` и `hold` удалены в конце (`destroy.sh`). Гость: «Windows 10 Корпоративная», русская локаль, часовой пояс «Russian Standard Time», автовход `Docker`, у ВМ есть интернет.

Результаты: `.winvm-results/w10-20261031-cycle3/` (`run1/` - полный прогон, seed 865370330, до правок обвязки и продукта; `run2/` - `leaks`, `faultrepeat`, `tzru`, `tz` после правок обвязки; `run3/` - `leaks`, `ftpsoak` (первая выборка сбоя), `tzru`; `run4-ftpsoak-diagnosis/` - разбор сбоя FTP: журнал и снимок трафика; `final/` - перепроверка итоговой сборки). Большие журналы серверов обрезаны до 2 МБ.

### Базовые фазы (первый прогон, seed 865370330)

| Фаза | Результат |
|---|---|
| `autotest` core / service / gui | 108 из 108 (3 пропуска) / 28 из 28 / 4 из 4 |
| `winreal` | 10 из 10 (2 пропуска: у пояса «Russian Standard Time» нет перехода на летнее время в 2027 году; сами тесты переходов прошли в фазе `tz`) |
| `install` / `installer` (обновление, удаление, переустановка, ACL) / `unicode` | 32 / 0, 10 / 0, 2 / 0 (учётная запись с кириллическим именем: 12 родных тестов) |
| `panel` (`/selftest`, окна на 100 / 125 / 150 %, три перезапуска Windows) | rc 0 на всех трёх масштабах, `gui` 4 из 4 на каждом |
| `web` (TLS клиентов Windows, вход, DPAPI через перезапуск службы и Windows) | 38 / 0 (TLS 1.2 есть; TLS 1.0 / 1.1 / 1.3 у Windows 10 22H2 выключены или отсутствуют: как в раундах 1 - 3) |
| `net`: ftp / ssh / esxi (эмулятор внутри гостя) | 32 из 34 / 60 из 63 (1 пропуск) / 65 из 65 (2 пропуска); падения разобраны ниже |
| `smb` (`smbreal`, диск `Y:`, `smbinfo`) / `smbsign` | 11 + 1 + 5 / 6, без падений; подпись SMB обязательна на обеих сторонах: 10 из 10 |
| `svcacct` / `aclreal` / `speedlimit` / `restartjob` | 46 / 16 / 10 / 12, без падений |
| `big` / `winbig` / `verify` | 3 из 3 (самый долгий 594,7 с) / 6 ГиБ разреженный vmdk: копия 64 с, 25 томов, 4,1 МБ, пик рабочего набора 51 МиБ / 94 из 94 |
| `signing` (информационная) / `signinst` | снимки и журналы / **30 из 30** |
| `leaks` (36 минут) | 11 из 12, падение - ошибка обвязки на русской локали (см. ниже); после правок 15 из 15 |
| `ntfsav` / `eicar` | 20 из 20 / 8 из 8 |
| `ftprepeat` (`ftp_large_streamed` 10 раз, журнал сервера DEBUG) | **10 из 10** (0,7 - 3,6 с) |
| `tz` | 31 из 39: падения - ошибка обвязки (см. ниже); после правки **39 из 39** |

### Новые проверки раунда на x64 ru

**(1) Подписанный установщик (`signing`, `signinst`; тестовый корень `tools/make-test-cert.sh`).** 64-битная нагрузка, 32-битный загрузчик: `signinst` 30 из 30. `Get-AuthenticodeSignature`: неподписанный `NotSigned`; подписанный без доверия к корню `UnknownError`, после доверия `Valid`; изменённый после подписи `HashMismatch` (оба варианта: байт в загрузчике и байт внутри пакета). Загрузчик 0x14C (32-битный), установленная служба и панель - **0x8664**, подписи `VM2Backup.exe`, `VM2BackupService.exe` и копии `VM2Backup-Setup.exe` в Program Files `Valid`. Тихая установка с обновлением поверх старой версии (CreateProcess с Mark of the Web, 4875 мс), службу пересоздала (pid 6484 -> 3640). Изменён байт в загрузчике: rc 0 (подпись проверяет Windows, а не установщик); изменён байт внутри пакета: rc 1 за 1,5 с, без зависания, причина в `%TEMP%\VM2Backup-Setup.log` («The installation package is damaged or incomplete...»). `ShellExecute` файла с Mark of the Web ждёт нажатия в «Open File - Security Warning»: 196 с без ответа, как на x86 (при развёртывании запускать через CreateProcess).

Что видит пользователь на **русской** Windows 10 (снимки `run1/shots/signing/<метка>-N-*.png`, SmartScreen на время фазы включён политикой `Warn`, Mark of the Web ZoneId=3):

* Двойной щелчок по файлу: окно **«Система Windows защитила ваш компьютер»** («Фильтр SmartScreen в Microsoft Defender предотвратил запуск неопознанного приложения, которое может подвергнуть компьютер риску»), кнопка «Подробнее» открывает строки «Приложение» и «Издатель» и кнопку «Выполнить в любом случае». Для подписанного и доверенного файла: **«Издатель: Symo TEST Code Signing»**; у неподписанного, подписанного недоверенного и изменённого издателя нет.
* UAC после «Выполнить в любом случае»: подписанный и доверенный - синяя шапка «Разрешить этому приложению вносить изменения на вашем устройстве?», значок и имя «VM2Backup», **«Проверенный издатель: Symo TEST Code Signing»**, «Источник файла: Жёсткий диск компьютера»; неподписанный - жёлтая шапка «Разрешить этому приложению **от неизвестного издателя** вносить изменения...», «Издатель: Нет данных»; подписанный недоверенный и изменённый - как неподписанный.
* Подпись тестового центра окно SmartScreen не снимает (нужна репутация настоящего сертификата, `docs/SIGNING.md`); русский вид окон проверен, обрезанных строк и нечитаемых надписей нет.

**(2) Расписание на переходах летнего времени на реальных часах и CRT** (`tz`, `tzru`; исправления из 67ac720 держатся):

* Родные тесты `winreal/schedule_across_dst_transitions_matches_the_windows_clock` и `scheduler_runs_each_slot_once_across_dst_transitions` в поясах **W. Europe, Pacific, Lord Howe**: 2 из 2 в каждом (545 шагов у каждого перехода сверены с эталоном Windows; ежедневно 02:30, каждые 30 минут, «один раз», ежемесячно 31-го и в последний день: 12 запусков в году, не в 02:30 по местному - 0).
* Настоящая служба и настоящие часы (после правки обвязки, `run2`): **весна W. Europe** (2027-03-28 01:56:30) - ежедневная 02:05 (такой минуты нет) ровно один раз в **03:00:01** и «каждые 30 минут» один раз в 03:00:01; **весна Pacific** (2027-03-14) - оба по разу в 03:00:01; **осень Lord Howe** (сдвиг 30 минут, часы на первом проходе 01:43) - ежедневная 01:45:01 один раз, «каждые 30 минут» в 02:00:00. Все 39 проверок прошли, в том числе «часы никто не трогал», «ни один слот не повторился», «не больше 6 запусков».
* **Русские пояса** (`tzru`: Russian, Kaliningrad, Ekaterinburg, Russia Time Zone 11 (Камчатка); летнего времени в них нет с 2014 года): вся группа `winreal` в каждом поясе - **10 из 10** с 2 честными пропусками (тесты переходов; расписание на «несуществующем» времени для них не определено), в том числе `schedule_follows_a_time_zone_change_without_a_restart`: служба без перезапуска идёт за сменой пояса (Tokyo, UTC, Pacific, W. Europe подряд через `tzutil /s`); из этих поясов гость возвращён в исходный «Russian Standard Time».

**(3) Утечки (`leaks`, 36 минут)**: итоговый прогон на готовой сборке - 142 копии малой ВМ, **852 открытия и закрытия панели**, 2 перезапуска службы, смена порта сайта 8443 <-> 8444 на каждом витке, 204 входа / выхода через сайт. Служба (5 замеров, 30 - 36,5 мин): дескрипторы **230 -> 230 (0**; порог < 50), потоки -1, закрытая память 7,3 -> 7,7 МБ (+0,4; порог < 30), GDI / USER 0; долгоживущая панель (17 замеров, 4,2 - 36,5 мин): дескрипторы **193 -> 193 (0)**, потоки +1, закрытая память 2,4 -> 2,6 МБ, **GDI 0 / USER -2** (не растут). Колебания дескрипторов службы не монотонные (225 - 246 внутри отрезка, возвращаются). **Утечек нет** (15 проверок из 15, включая новую «в каждой строке таблицы 9 столбцов»). Предыдущий прогон после правки локали давал те же величины (234 -> 233, 193 -> 193).

**(4) NTFS и антивирус** (`ntfsav`, `eicar`, `guest/ntfsav.ps1`; служба работает под SYSTEM, задание ставит пользователь `Docker` из группы администраторов):

| Случай | Что сделал продукт |
|---|---|
| (a) файл под EFS (`cipher /e` от того же пользователя; `Archive, Encrypted`) | задание **Failed** за 0,016 с: «cannot read enc.vmdk: Access is denied (5)» (имя файла и причина названы; служба продолжает работать; копия ВМ целиком не создаётся: один нечитаемый файл = неудача копии ВМ) |
| (b) NTFS-сжатый файл (`compact /c`, 5,2 : 1) и разреженный файл (256 МБ логических, 2 МБ выделено, два диапазона) | **Success** (сырых 269 МБ, архив 2,15 МБ); восстановление побитно (SHA-256) совпало у обоих. Восстановленный разреженный файл **не разреженный** (на диске 256 МБ): содержимое верное, но место не экономится |
| (c) EICAR (Defender запущен: политика `DisableAntiSpyware` на время снята, сигнатуры 1.303.25.0 от 2019 года достаточны) | Defender обнаружил `Virus:DOS/EICAR_Test_File` (события 1116, `Get-MpThreatDetection`: действие выполнено) уже после первых секунд (через 12 с файл ещё лежал); задание **Failed** за 0,08 с: «cannot read eicar.com.txt: Operation did not complete successfully because the file contains a virus or potentially unwanted software (225)»; без зависания, готовой копии не осталось |
| (d) файл с именем из 255 символов и каталог с 5000 файлов | **Success**, все 5000 файлов и файл с длинным именем восстановлены с тем же содержимым (SHA-256 по всему набору) |
| (e) файл исчез посреди копии (большой файл с ограничением 12 Мбит/с, через 6 с удалён ещё не прочитанный `z-vanish.vmx`, создан EICAR) | задание **Failed** через 14 с: «cannot read z-vanish.vmx: The system cannot find the file specified (2)»; незавершённой копии, считающейся успешной, нет (0 готовых копий); Defender удалил и второй EICAR |

Журнал Defender записан в `results/23b-eicar.out` (события 1116, `Get-MpThreatDetection`); ничего в Defender не отключалось, политика `DisableAntiSpyware=1` возвращена после проверки.

**(5) Повтор `ftp_large_streamed` 10 раз** на госте с журналом сервера DEBUG (`VM2B_FTP_LOGLEVEL`, свои серверы на каждый прогон): **10 из 10 прошли** (6,9 - 18,8 с; журналы `results/srv-ftprepeat/run-N/`). Закономерность одна: тест сам выбирает обычный FTP или FTPS по зерну: 7 прогонов шли через FTPS (в `ftps.log` 6 - 15 тысяч строк DEBUG на записи TLS, 7,1 - 18,8 с), 3 через обычный FTP (`ftp.log` 75 строк, 6,9 / 10,1 / 13,9 с); времени и размеру журнала сбой не соответствует, обрывов в журналах нет, в двух обычных прогонах `net` тест тоже прошёл. Редкий сбой не воспроизвёлся.

### Итог перепроверки после правок (та же ВМ, seed 872173946)

| Что | Результат |
|---|---|
| `winreal` (с двумя тестами расписания и тестом смены пояса) | 12 из 12 |
| `tz`, родные тесты в поясах W. Europe / Pacific / Lord Howe | 2 из 2 в каждом |
| `tz`, настоящая служба и часы | весна W. Europe 8 из 8; весна Pacific: оба запуска ровно по разу в 03:00:01 (одна проверка упала на записях истории прошлого сценария: ошибка обвязки, исправлена); осень Lord Howe 8 из 8 |
| `signinst` | 30 из 30 |
| `ntfsav` / `eicar` | 20 из 20 / 8 из 8 |
| `net` ftp / ssh (второй прогон) | 31 из 33 (активный FTP; `ftp_fault_injection_upload` один раз по времени) / 59 из 63 (границы обвязки, см. выше) |
| Wine, полный `tools/run-tests.sh --seed 7773` (логи в `wine/`) | все 10 наборов ok: native, core 150 / 0 (197 пропусков: тесты только для настоящей Windows), core32 65 / 0, ftp 33 / 0, ssh 64 / 0, esxi 67 / 0, web, gui 4 / 0, package (37 проверок, в том числе «damaged cabinet»), package2 |

### Найдено и исправлено в продукте (регрессии - в тестах)

1. **Несуществующая локальная минута (весенний переход) превращалась в момент на час РАНЬШЕ** (Windows CRT `_mktime64`, изоляция проверена на W. Europe и Pacific; glibc ставит такую минуту на час ПОЗЖЕ): «ежедневно в 02:30» запускалось в 01:30 дня перехода, «один раз в 02:30» - на час раньше срока, а «ежедневно в 02:05» на настоящей службе в Pacific не запускалось в этот день вовсе (расписание получало момент из прошлого и уходило на следующие сутки). **Исправление (`src/schedule.cpp`, `LocalMinToUnix`)**: несуществующая минута -> первый момент после «дыры» (03:00), одинаково на всех CRT. Регрессии: `tests/native/t_schedule.cpp` (`NonexistentMinute`: Berlin, New York, Lord Howe), `winreal/schedule_across_dst_transitions...` (запуск не раньше 02:30), настоящие часы `tz/spring-*` (запуск в 03:00 ровно один раз).
2. **Минута, которая бывает дважды (осенний переход), превращалась во ВТОРОЕ вхождение**: планировщик идёт по первому проходу в реальном времени и никогда не встречал слот; «каждые 30 минут» терял 1 - 2 часа, «ежедневно в 01:30» в день перехода не запускалось вовсе. **Исправление:** первое вхождение (второй проход повторяющегося часа слот не повторяет: запуск ровно один раз). Регрессии: `t_schedule.cpp` (`AmbiguousMinute`: Berlin, New York, Lord Howe, London), `winreal/scheduler_runs_each_slot_once_across_dst_transitions` на настоящей Windows (до правки тест ждал недостающий запуск часами).
3. **Служба не замечала смену часового пояса** (msvcrt читает пояс один раз при первом вызове): после `tzutil /s` службе нужен был перезапуск, до него «следующий запуск» и планировщик шли по старому поясу. **Исправление:** `_tzset()` не реже раза в 2 секунды в `UnixToLocalMin` / `LocalMinToUnix`. Регрессия: `winreal/schedule_follows_a_time_zone_change_without_a_restart`.
4. **Тихая установка с повреждённым пакетом давала только rc 1 без объяснения.** **Исправление (`src/vm2blauncher.cpp`)**: причина записывается в `%TEMP%\VM2Backup-Setup.log`, окно (без `/silent`) называет повреждённый пакет вместо «не удалось распаковать»; регрессия `tools/test-package-wine.sh` («damaged cabinet»).

Из логики настоящего времени: отсутствие повтора в повторяющийся час **задумано** (слоты расписания - минуты настенных часов): «каждые 30 минут» осенью занимает паузу до 90 реальных минут (`gaps 30..90 min` в тесте), «ежедневно в 01:30» в день перехода запускается ровно один раз.

### Изменения обвязки и тестов раунда

* Фазы `signinst`, `leaks`, `ntfsav`, `eicar`, `ftprepeat`, `tz` и расширенная `signing` (для Windows 10 включает SmartScreen на время фазы); по умолчанию входят в списки `x86`, `w10`, `w11`.
* Скрипты `guest/signinst.ps1`, `leaks.ps1`, `ntfsav.ps1`, `defender-toggle.ps1`, `tzreal.ps1` (UTF-8 с BOM: иначе PowerShell 5 ломает строки с кириллицей); `tools/winvm-variants.sh`.
* `tzreal.ps1`: часы ставятся `SetSystemTime` (не `Set-Date`: .NET держит пояс, прочитанный при запуске процесса); перед сценарием служба и задачи синхронизации времени выключаются (иначе `svchost` возвращал время «с узла» через 1 - 3 минуты: в журнале `Kernel-General` событие 1, процесс `svchost.exe`).
* Знание про обвязку: **Defender на тестовых ВМ выключен политикой `DisableAntiSpyware=1` из `bootstrap.cmd`** (служба не запущена, `Get-MpComputerStatus` даёт `0x800106BA`); фаза `eicar` снимает политику на время проверки (перезапуск) и возвращает её (перезапуск); ничего в самом Defender не меняется.
* Если тест (или человек) оставил на гостевом экране окно, исполнитель после жёсткого `system_reset` ждёт: после сброса остаются сеансы SMB прежнего гостя, и qemu-smbd отвечает «NT_STATUS_IO_TIMEOUT» на открытие файлов `current.txt` / `*.out`; надёжный выход - корректный перезапуск гостя (`shutdown /r`) или `system_powerdown` + `docker start`.

### Не проверено

* (SmartScreen и «Open File - Security Warning» на русской Windows 10 проверены в раунде 9.) Репутация и допуск для настоящего сертификата (OV / EV); метка времени подписи (RFC 3161).
* Несуществующее / повторяющееся время для поясов без перехода в 2027 году и для зон с годовыми правилами, отличающимися от правил текущего года (UCRT / msvcrt читают правила один раз).

## Раунд 9: Windows 10 x64 ru (22H2, сборка 19045.3570, 64-битная нагрузка), цикл 3, 31.10.2026

Вторая ОС третьего цикла. Одна ВМ `vm2b-win` создана и прогнана **одной командой с нуля** (`tools/run-winvm.sh w10 --keep`: ISO `ru-ru_windows_10_business_editions_version_22h2_updated_oct_2023_x64_dvd_6072cf93.iso`, `dockurr/windows`, RAM 4 ГБ / 4 ядра, диск 32 ГБ; установка 1481 с, весь план 21389 с; ничего не пересоздавалось и не докручивалось руками). Пересборка ВМ не понадобилась; ключ `--keep` нужен был только для разбора и перепроверок после правок (`--no-create --phases ...`). ВМ, диск, сеть, `.winvm/shared/pki` и `hold` удалены в конце (`destroy.sh`). Гость: «Windows 10 Корпоративная», русская локаль, часовой пояс «Russian Standard Time», автовход `Docker`, у ВМ есть интернет.

Результаты: `.winvm-results/w10-20261031-cycle3/` (`run1/` - полный прогон, seed 865370330, до правок обвязки и продукта; `run2/` - `leaks`, `faultrepeat`, `tzru`, `tz` после правок обвязки; `run3/` - `leaks`, `ftpsoak` (первая выборка сбоя), `tzru`; `run4-ftpsoak-diagnosis/` - разбор сбоя FTP: журнал и снимок трафика; `final/` - перепроверка итоговой сборки). Большие журналы серверов обрезаны до 2 МБ.

### Базовые фазы (первый прогон, seed 865370330)

| Фаза | Результат |
|---|---|
| `autotest` core / service / gui | 108 из 108 (3 пропуска) / 28 из 28 / 4 из 4 |
| `winreal` | 10 из 10 (2 пропуска: у пояса «Russian Standard Time» нет перехода на летнее время в 2027 году; сами тесты переходов прошли в фазе `tz`) |
| `install` / `installer` (обновление, удаление, переустановка, ACL) / `unicode` | 32 / 0, 10 / 0, 2 / 0 (учётная запись с кириллическим именем: 12 родных тестов) |
| `panel` (`/selftest`, окна на 100 / 125 / 150 %, три перезапуска Windows) | rc 0 на всех трёх масштабах, `gui` 4 из 4 на каждом |
| `web` (TLS клиентов Windows, вход, DPAPI через перезапуск службы и Windows) | 38 / 0 (TLS 1.2 есть; TLS 1.0 / 1.1 / 1.3 у Windows 10 22H2 выключены или отсутствуют: как в раундах 1 - 3) |
| `net`: ftp / ssh / esxi (эмулятор внутри гостя) | 32 из 34 / 60 из 63 (1 пропуск) / 65 из 65 (2 пропуска); падения разобраны ниже |
| `smb` (`smbreal`, диск `Y:`, `smbinfo`) / `smbsign` | 11 + 1 + 5 / 6, без падений; подпись SMB обязательна на обеих сторонах: 10 из 10 |
| `svcacct` / `aclreal` / `speedlimit` / `restartjob` | 46 / 16 / 10 / 12, без падений |
| `big` / `winbig` / `verify` | 3 из 3 (самый долгий 594,7 с) / 6 ГиБ разреженный vmdk: копия 64 с, 25 томов, 4,1 МБ, пик рабочего набора 51 МиБ / 94 из 94 |
| `signing` (информационная) / `signinst` | снимки и журналы / **30 из 30** |
| `leaks` (36 минут) | 11 из 12, падение - ошибка обвязки на русской локали (см. ниже); после правки 14 из 14 |
| `ntfsav` / `eicar` | 20 из 20 / 8 из 8 |
| `ftprepeat` (`ftp_large_streamed` 10 раз, журнал сервера DEBUG) | **10 из 10** (0,7 - 3,6 с) |
| `tz` | 31 из 39: падения - ошибка обвязки (см. ниже); после правки **39 из 39** |

### Новые проверки раунда на x64 ru

**(1) Подписанный установщик (`signing`, `signinst`; тестовый корень `tools/make-test-cert.sh`).** 64-битная нагрузка, 32-битный загрузчик: `signinst` 30 из 30. `Get-AuthenticodeSignature`: неподписанный `NotSigned`; подписанный без доверия к корню `UnknownError`, после доверия `Valid`; изменённый после подписи `HashMismatch` (оба варианта: байт в загрузчике и байт внутри пакета). Загрузчик 0x14C (32-битный), установленная служба и панель - **0x8664**, подписи `VM2Backup.exe`, `VM2BackupService.exe` и копии `VM2Backup-Setup.exe` в Program Files `Valid`. Тихая установка с обновлением поверх старой версии (CreateProcess с Mark of the Web, 4875 мс), службу пересоздала (pid 6484 -> 3640). Изменён байт в загрузчике: rc 0 (подпись проверяет Windows, а не установщик); изменён байт внутри пакета: rc 1 за 1,5 с, без зависания, причина в `%TEMP%\VM2Backup-Setup.log` («The installation package is damaged or incomplete...»). `ShellExecute` файла с Mark of the Web ждёт нажатия в «Open File - Security Warning»: 196 с без ответа, как на x86 (при развёртывании запускать через CreateProcess).

Что видит пользователь на **русской** Windows 10 (снимки `run1/shots/signing/<метка>-N-*.png`, SmartScreen на время фазы включён политикой `Warn`, Mark of the Web ZoneId=3):

* Двойной щелчок по файлу: окно **«Система Windows защитила ваш компьютер»** («Фильтр SmartScreen в Microsoft Defender предотвратил запуск неопознанного приложения, которое может подвергнуть компьютер риску»), кнопка «Подробнее» открывает строки «Приложение» и «Издатель» и кнопку «Выполнить в любом случае». Для подписанного и доверенного файла: **«Издатель: Symo TEST Code Signing»**; у неподписанного, подписанного недоверенного и изменённого издателя нет.
* UAC после «Выполнить в любом случае»: подписанный и доверенный - синяя шапка «Разрешить этому приложению вносить изменения на вашем устройстве?», значок и имя «VM2Backup», **«Проверенный издатель: Symo TEST Code Signing»**, «Источник файла: Жёсткий диск компьютера»; неподписанный - жёлтая шапка «Разрешить этому приложению **от неизвестного издателя** вносить изменения...», «Издатель: Нет данных»; подписанный недоверенный и изменённый - как неподписанный.
* Подпись тестового центра окно SmartScreen не снимает (нужна репутация настоящего сертификата, `docs/SIGNING.md`); русский вид окон проверен, обрезанных строк и нечитаемых надписей нет.

**(2) Расписание на переходах летнего времени на реальных часах и CRT** (`tz`, `tzru`; исправления из 67ac720 держатся):

* Родные тесты `winreal/schedule_across_dst_transitions_matches_the_windows_clock` и `scheduler_runs_each_slot_once_across_dst_transitions` в поясах **W. Europe, Pacific, Lord Howe**: 2 из 2 в каждом (545 шагов у каждого перехода сверены с эталоном Windows; ежедневно 02:30, каждые 30 минут, «один раз», ежемесячно 31-го и в последний день: 12 запусков в году, не в 02:30 по местному - 0).
* Настоящая служба и настоящие часы (после правки обвязки, `run2`): **весна W. Europe** (2027-03-28 01:56:30) - ежедневная 02:05 (такой минуты нет) ровно один раз в **03:00:01** и «каждые 30 минут» один раз в 03:00:01; **весна Pacific** (2027-03-14) - оба по разу в 03:00:01; **осень Lord Howe** (сдвиг 30 минут, часы на первом проходе 01:43) - ежедневная 01:45:01 один раз, «каждые 30 минут» в 02:00:00. Все 39 проверок прошли, в том числе «часы никто не трогал», «ни один слот не повторился», «не больше 6 запусков».
* **Русские пояса** (`tzru`: Russian, Kaliningrad, Ekaterinburg, Russia Time Zone 11 (Камчатка); летнего времени в них нет с 2014 года): вся группа `winreal` в каждом поясе - **10 из 10** с 2 честными пропусками (тесты переходов; расписание на «несуществующем» времени для них не определено), в том числе `schedule_follows_a_time_zone_change_without_a_restart`: служба без перезапуска идёт за сменой пояса (Tokyo, UTC, Pacific, W. Europe подряд через `tzutil /s`); из этих поясов гость возвращён в исходный «Russian Standard Time».

**(3) Утечки (`leaks`, 36 минут)**: 132 копии малой ВМ (первая выборка 142), около 800 открытий и закрытий панели, 2 перезапуска службы, смена порта сайта 8443 <-> 8444 каждый виток. После правки обвязки службе: дескрипторы 234 -> 233 (-1; порог < 50), закрытая память 5,4 -> 6,4 МБ (+1,0; порог < 30), потоки 0; долгоживущая панель (16 замеров, 4 - 36 мин): дескрипторы 193 -> 193 (0), потоки +1, закрытая память 2,4 -> 2,5 МБ, **GDI 0 / USER 0**. Рост дескрипторов службы не монотонный (225 - 246 внутри отрезка). **Утечек нет** (14 проверок из 14).

**(4) NTFS и антивирус (`ntfsav`, `eicar`)**: 20 из 20 и 8 из 8 при **включённом** Defender (сигнатуры 1.463.41.0 от 30.10.2026; политика `DisableAntiSpyware` на время снимается и возвращается). EFS-файл: задание Failed с понятной причиной «Access is denied (5)», служба продолжает работу; NTFS-сжатый (5,2 : 1) и разреженный файлы: Success и побитное восстановление; имя 255 символов и 5000 файлов: Success, все файлы восстановлены; EICAR: Defender обнаружил файл (`MpThreatDetection`, события 1116 / 1117) и запретил чтение; задание **Failed** за 0,017 с с названием файла и сообщением Windows «Операция не была успешно завершена, так как файл содержит вирус или потенциально нежелательную программу (225)» (сам файл Defender за 12 с с диска не убрал); служба продолжает работу; файл исчез посреди копии: **Failed** через 13,9 с «cannot read z-vanish.vmx: Не удается найти указанный файл (2)», законченных копий в списке нет (никакого ложного «Success»). Журналы: `results/23b-eicar.out`, `23-ntfsav.out`.

### Падения первого прогона и что с ними сделано

**Ошибки продукта: нет.** Все падения - обвязка или среда; ни одно не потребовало правки службы, панели или установщика (кроме защитной правки клиента FTP, ниже).

1. **`ssh/auth_guard_scp_*` (3 теста) - были "объявленной границей обвязки"; теперь детерминированы (64 из 64 на госте).** Причина подтверждена: тесты считают строки журнала `sshd` (`LogLevel VERBOSE`), который гость читал через SMB; кеш клиента SMB отдавал старый файл (`sshd logged 0 failed password ... 0 connection(s)` при настоящих попытках на хосте). **Правка:** сервер журнала `tools/testservers/logread.py` (каждое подключение получает файл целиком как он есть на хосте сейчас), `tools/run-ssh-tests.sh` пишет в `servers.json` поле `x.authlog_tcp` (`хост:порт`), `src/tests_ssh_guard.cpp` читает журнал по TCP (при отсутствии поля - по пути, как под Wine). Теперь `sshd logged 1 failed password, 1 connection` и т.д. **Регрессия:** новый тест `ssh/authlog_shows_a_fresh_refused_login_at_once` (3 раза: отказ в входе виден в журнале через 0,7 с; падает, если между тестом и журналом есть кеш); повторы на госте: `faultrepeat` - 5 запусков четырёх тестов по 4 из 4.
2. **`ssh/scp_fault_drop_listing` ("0 of 6 listings were cut") - недетерминированный тест, не среда и не продукт.** Сервер отдаёт клиенту около 2 КБ до конца входа, листинг 40 имён ещё около 3 КБ; обрыв выбирался в 2500 - 11500 байт, то есть примерно в половине раундов приходился ПОСЛЕ конца листинга (вероятность "0 из 6" около 3 % на запуск, реально чаще из-за разброса). **Правка:** 120 имён (листинг около 8 КБ), обрыв в 2500 - 5500 байт: **всегда** внутри обмена; каждый раунд теперь печатает свой обрыв и результат. Проверено: 6 из 6 на всех сидах под Wine (5, 6, 7, 8) и 25 запусков на госте (`faultrepeat`: `6 of 6 listings were cut`).
3. **`ftp/ftp_fault_injection_upload` ("control connection: receive timed out", 72 - 82 с) - воспроизведён и разобран.** Короткие повторы (10 раз) его не поймали; **тест по кругу 8 - 10 минут на одном наборе серверов (`--minutes`, фаза `ftpsoak`) давал 4 - 6 сбоев на 93 - 113 запусков (около 5 %)**, всегда на TLS-ключах `ftpes` / `ftps`, всегда во время ПОВТОРНОЙ загрузки после обрыва. Журнал сервера (DEBUG) и снимок трафика: сервер pyftpdlib получает данные, затем `recv(): Unexpected EOF` (канал закрыт без TLS `close_notify`), после чего **сам уходит в бесконечный цикл** `_do_ssl_shutdown()` ("shutdown while in init" проглатывается библиотекой, канал не закрывается; 2,6 млн строк журнала за один прогон): однопоточный сервер больше никому не отвечает, клиент ждёт 226 положенные 60 с и сообщает "receive timed out". То есть **зависание создавал тестовый сервер**, а не продукт: на реальном сервере закрытие без `close_notify` не должно приводить к потере ответа. **Правки:** (а) сервер: после 8 безрезультатных оборотов канал закрывается (`tools/testservers/ftp_server.py`); (б) режим `SITE FAULT strict` - загрузка по TLS, закончившаяся без `close_notify` клиента, получает `451` и не переименовывается, плюс `SITE FAULT slowread N` (медленное чтение); (в) клиент (`src/tls.cpp`, `NetConn::shutdown`): сокет неблокирующий, и при полном буфере отправки `SSL_shutdown` отвечал WANT_WRITE, close_notify оставался внутри OpenSSL, а FIN уходил за уже поставленными в очередь данными; теперь клиент дожидается записи (до 10 с) и повторяет вызов. **Честно:** что именно закрывало канал без `close_notify` в 5 % случаев, достоверно не установлено; в строгом режиме на 299 запусках (около 900 загрузок по TLS) ни одной незавершённой загрузки, после правок сервера зависаний нет (241 + 268 + 299 запусков без сбоев). Правка клиента защитная, не доказанная как причина. **Регрессия:** `ftp/ftp_tls_upload_ends_with_close_notify_to_a_slow_reader` (строгий медленный сервер, 4 загрузки по 6 - 10 МБ на `ftpes` и `ftps`: принято, содержимое побитно совпадает; проходит и на старом клиенте, то есть закрепляет свойство, а не воспроизводит сбой). Попутно найден и исправлен **дефект самого теста**: `Uniq` брал имена из генератора, который при `--minutes` каждый раунд начинает с одного сида, и тест натыкался на файлы прошлого раунда (ложные падения "never under the final name"); к имени добавлен счётчик времени.
4. **`ftp_passive_active_and_listing_variants`**: активный FTP за NAT гостя - известный предел (`Known limits`), без изменений.

### Ошибки обвязки, найденные в этом раунде

* **`leaks.ps1`: «VM2BackupService measured :: no rows» на русской Windows.** `-f` и `N1` выдают десятичную ЗАПЯТУЮ, строка CSV `2,6,VM2BackupService,...` разваливалась на лишние столбцы. Правка: `InvariantCulture`, формат `F1`, проверка «в каждой строке таблицы ровно 9 столбцов» (локаль сразу видна как причина). Вторая ошибка той же фазы: при повторном запуске `tasks.save` отвечал «choose a server with virtual machines as the source», и нагрузка шла вхолостую (сам прогон показывал «132 копии», потому что проверялся только код ответа). Причина (нашёл печатью ответа `servers.save`): скрипт удалял сервер `leak src` ДО задачи `leak-task` прошлого прогона, а сервер, на который ссылается задача, не удаляется, и `servers.save` отвечал «a server named "leak src" already exists». Правка: сначала задача, потом сервер; скрипт ждёт появления ВМ у источника и при отказе `tasks.save` прекращает прогон (и печатает ответ `servers.save`).
* **`tzreal.ps1`: часы возвращались «с узла» через ~45 с.** В отличие от x86 (там у ВМ нет внешней сети), у dockur-ВМ есть интернет: примерно через 45 секунд после смены часов Windows САМА меняла тип запуска `w32time` «отключена -> Автоматически» (SCM 7040), служба стартовала и брала время с `time.windows.com` (`Time-Service` 266 / 261, процесс `svchost`); сценарий видел «часы тронул кто-то другой» и расписание не успевало (8 ложных падений `tz`). Правка: на время сценария закрыт исходящий UDP 123 (правило брандмауэра `vm2b-block-ntp`, снимается в `restore`) и тип запуска службы подтверждается заново в начале каждого сценария. После правки 39 из 39.
* **`winreal/schedule_across_dst_transitions...`**: строка `time zone: ;` без названия пояса (кириллическое имя через `%ls` в узком `printf` терялось); теперь через `FromWide`.
* **`run-winvm.sh`**: новые фазы `faultrepeat` (повторы прежде нестабильных тестов: `ftp_fault_injection_upload` 10 раз и четыре SCP-теста 5 раз), `ftpsoak` (один тест по кругу N минут: `FTPSOAK`, `FTPSOAK_TEST`, `FTPSOAK_LOG`, `FTPSOAK_STRICT`, `FTPSOAK_TAG`), `tzru` (русские пояса); все входят в список `w10` / `w11` / `x86` (кроме `ftpsoak`, он по запросу).

### Итог перепроверки после правок (та же ВМ)

| Что | Результат |
|---|---|
| `net` ftp (итоговая сборка, seed 513393780) | 34 из 35: только `ftp_passive_active_and_listing_variants` (активный FTP за NAT, известный предел); **`ftp_fault_injection_upload` прошёл** |
| `net` ssh (итоговая сборка) | **64 из 64** (1 пропуск), в том числе все три `auth_guard_scp_*` и `scp_fault_drop_listing`; до правки 60 из 63 |
| `faultrepeat` | `ftp_fault_injection_upload` 10 из 10; `scp_fault_drop_listing` + три `auth_guard_scp_*` 5 прогонов по 4 из 4 (`6 of 6 listings were cut` каждый раз) |
| `ftpsoak` (один набор серверов, тест по кругу) | до правки сервера: 93 из 97 и 107 из 113 (сбой около 5 %, всегда `ftpes` / `ftps`, повторная загрузка после обрыва); после: **241, 268, 299 (строгий сервер) и 197 (итоговая сборка, строгий) прогонов без сбоев** |
| `tz` (настоящая служба и часы) | **39 из 39** после правки брандмауэра для NTP (первый прогон 31 из 39) |
| `tzru` (4 русских пояса) | `winreal` 10 из 10 в каждом, 2 пропуска |
| `leaks` (итоговая сборка) | 15 из 15 |
| Wine, полный `tools/run-tests.sh` на итоговой сборке (логи `wine-logs*/`; ftp и ssh отдельными запусками: ВМ в это время использовала те же порты 21000 - 22999 на другом адресе) | все наборы ok: native, core 150 / 0 (199 пропусков: тесты для настоящей Windows), core32 65 / 0, ftp 34 / 0 (1 пропуск), ssh 65 / 0, esxi 67 / 0, web 1 / 0, gui 4 / 0, package ok, package2 ok |

### Изменения обвязки и тестов раунда

* `tools/testservers/logread.py` (журнал sshd по TCP), `tools/run-ssh-tests.sh` (`x.authlog_tcp`, порт 22399), тест `ssh/authlog_shows_a_fresh_refused_login_at_once`; `scp_fault_drop_listing`: 120 имён, обрыв в 2500 - 5500 байт, заметка по раунду.
* FTP: `ftp_server.py` (защита от бесконечного цикла `_do_ssl_shutdown`, `SITE FAULT strict` / `slowread`, `VM2B_FTP_STRICT=1`), `tls.cpp` (`close_notify` дожидается записи), тест `ftp/ftp_tls_upload_ends_with_close_notify_to_a_slow_reader`, уникальные имена `Uniq`.
* `guest/leaks.ps1` (локаль, ожидание ВМ, проверка столбцов), `guest/tzreal.ps1` (брандмауэр для NTP, повтор отключения службы), `winreal` (имя пояса).
* Фазы `faultrepeat`, `ftpsoak`, `tzru`.

### Не проверено

* SmartScreen с настоящим сертификатом (OV / EV): репутация и допуск (`docs/SIGNING.md`); метка времени подписи (RFC 3161).
* Windows 11: раунд цикла 3 выполнен (раунд 10).
* Что именно в 5 % случаев закрывало TLS-канал загрузки без `close_notify` (см. пункт 3): достоверно установлена только причина зависания (тестовый сервер).

## Раунд 10: Windows 11 25H2 ru (сборка 26200.9445, x64, 64-битная нагрузка), цикл 3, 31.10.2026

Третья (последняя) ОС третьего цикла. Одна ВМ `vm2b-win` создана и прогнана **одной командой с нуля** (`tools/run-winvm.sh w11 --keep`: ISO `Windows_11_5in1_25H2_26200_9445_2026_09_x64.iso`, `dockurr/windows`, RAM 4 ГБ / 4 ядра, диск 32 ГБ; установка 2544 с, весь план 22826 с = 6 ч 20 мин). **Ничего не пересоздавалось и не докручивалось руками**; одна перепроверка на той же ВМ (сценарий `spring-eu`, см. ниже) выполнена вручную через `job.sh` после полного прогона. Гость: «Windows 10 Enterprise» по строке реестра (так называет себя и Windows 11 25H2), русская локаль, часовой пояс «Russian Standard Time», автовход `Docker`, Defender включён (сигнатуры 1.459.120.0), `wmic.exe` отсутствует. ВМ, диск, сеть, `.winvm/shared/pki`, `hold` удалены в конце (`destroy.sh`).

Результаты: `.winvm-results/w11-20261031-cycle3/` (`run1/` - полный прогон, seed 240739642: `*.out`, `autotest-*.txt`, снимки `shots/`, журналы серверов `srv-faultrepeat/`, `srv-ftprepeat/`, `container.log`; `rerun-tz/` - повтор сценария `spring-eu` после единственного падения).

### Базовые фазы

| Фаза | Результат |
|---|---|
| `autotest` core / service / gui | 108 из 108 (3 пропуска) / 28 из 28 / 4 из 4 |
| `winreal` | 10 из 10 (2 пропуска: у пояса «Russian Standard Time» нет перехода на летнее время) |
| `install` / `installer` / `unicode` | 32 / 0, 10 / 0, 2 / 0 (учётная запись с кириллическим именем: 12 родных тестов) |
| `panel` (`/selftest`, окна на 100 / 125 / 150 %) | rc 0 на трёх масштабах, `gui` 4 из 4 на каждом |
| `web` (TLS клиентов Windows, вход, DPAPI через перезапуск службы и Windows) | 41 / 0; **TLS 1.2 и TLS 1.3 у клиентов Windows 11 работают** (`SslStream` ограничен TLS 1.3: `Tls13`; `curl --tlsv1.3` даёт 200), TLS 1.0 / 1.1 отключены системой, как на Windows 10 |
| `net`: ftp / ssh / esxi (эмулятор внутри гостя) | 33 из 34 / 64 из 64 / 65 из 65; единственное падение разобрано ниже |
| `smb` (`smbreal`, диск `Y:`, `smbinfo`) / `smbsign` | 11 + 1 + 5 / 6 (родные 10 из 10), без падений; SMB 3 с шифрованием (`vm2benc`) входит в `smbreal` |
| `svcacct` / `aclreal` / `speedlimit` / `restartjob` | 46 / 16 / 10 / 12, без падений |
| `big` / `winbig` / `verify` | 3 из 3 (самый долгий 494 с) / 6 ГиБ разреженный vmdk: копия 66 с, 25 томов, 4,1 МБ, пик рабочего набора 53 МиБ / 94 из 94 |
| `defender` | 5 из 5: защита в реальном времени включена, обнаружений нет, исключений нет |
| `smartscreen`, `signinst` | снимки двойного щелчка и «Подробнее» / **30 из 30** |
| `leaks` (36 минут) | **15 из 15** (см. ниже) |
| `ntfsav` / `eicar` | 20 из 20 / 8 из 8 |
| `ftprepeat` (`ftp_large_streamed` 10 раз) / `faultrepeat` | 10 из 10 / `ftp_fault_injection_upload` 10 из 10 и ssh (`scp_fault_drop_listing` + 3 × `auth_guard_scp`) 5 прогонов, 0 FAIL |
| `ftpsoak` (10 минут, один набор серверов, журнал DEBUG) | **317 запусков `ftp_fault_injection_upload`, 0 падений** |
| `tzru` (4 русских пояса) | 10 из 10 в каждом (2 честных пропуска), в том числе `schedule_follows_a_time_zone_change_without_a_restart` |
| `tz` | 38 из 39 с первого раза; единственное падение - дрейф часов гостя (см. ниже), повтор сценария 8 из 8 |
| `sac` | Smart App Control переведён в «On» через настоящий интерфейс «Безопасность Windows» (оценка -> `VerifiedAndReputablePolicyState=1`), после двух перезапусков значение действует |

### Проверки раундов 8 - 9 на Windows 11

**Подписанный установщик (`signinst`, 30 из 30).** `Get-AuthenticodeSignature`: неподписанный `NotSigned`; подписанный без доверия к корню `UnknownError`, после доверия `Valid`; изменённый после подписи `HashMismatch` (оба варианта: байт в загрузчике и байт внутри пакета). Загрузчик 0x14C (32-битный), установленные `VM2Backup.exe`, `VM2BackupService.exe` и копия `VM2Backup-Setup.exe` в Program Files - 0x8664 и `Valid`. Тихое обновление поверх старой версии с Mark of the Web (CreateProcess): rc 0, 9391 мс, служба пересоздана (pid 3920 -> 1632). Снимки: `shots/smartscreen-1.png` («Система Windows защитила ваш компьютер»), `smartscreen-2-more-info.png`, `signinst-securitywarning-doubleclick.png`.

**Smart App Control «On» (`sac`, последняя фаза; как в раунде 7).** При включённом SAC неподписанный `.exe` с Mark of the Web **блокируется** («Политика управления приложениями заблокировала этот файл», события CodeIntegrity 3077 / 3033 «не отвечает требованиям уровня подписи Enterprise»); неподписанный без Mark of the Web до доверия к тестовому корню запускается, после доверия **тоже блокируется**; подписанный нашим тестовым ключом запускается и с Mark of the Web, и без (до и после доверия к корню); изменённый после подписи блокируется в обоих случаях. Установленная служба и панель (подписаны, `Valid` после доверия) работают, тихая установка подписанного пакета: rc 0, служба Running; служба пережила два перезапуска Windows. Снимки: `shots/sac-*.png`. Вывод раунда 7 подтверждён: с SAC «On» продукт нужно подписывать (тестовый ключ годится технически; для пользователей нужен настоящий сертификат).

**Расписание на переходах летнего времени на реальных часах и CRT (`tz`, `tzru`).** Родные тесты `schedule_across_dst_transitions_matches_the_windows_clock` и `scheduler_runs_each_slot_once_across_dst_transitions` в W. Europe / Pacific / Lord Howe: 2 из 2 в каждом (545 шагов у каждого перехода: ежедневно 02:30 - 3 из 3, «каждые 30 минут» 103 - 104 из 103 - 104). Настоящая служба и настоящие часы: весна W. Europe - ежедневная 02:05 и «каждые 30 минут» по разу в **03:00:01**; весна Pacific - по разу; осень Lord Howe (сдвиг 30 минут) - ежедневная 01:45:01 один раз, без повторов, без зацикливания. Исправления 67ac720 держатся. Смена пояса без перезапуска службы - в `tzru` (Russian / Kaliningrad / Ekaterinburg / Russia Time Zone 11) и в `winreal`.

*Единственное падение `tz`: «часы гостя сдвинуты чем-то ещё на 115 с» в сценарии `spring-eu`* (через минуту после `SetSystemTime` часы ушли вперёд на 115 с, `w32time` остановлен, в этот раз по причине, которую выяснить не удалось: не продукт и не планировщик, обе задачи отработали ровно по разу в 03:00:01 / 03:00:02). Повтор на той же ВМ (`rerun-tz/`): **8 из 8**, дрейфа нет. Как и в раунде 9, это среда (часы гостя), а не продукт.

**Утечки (`leaks`, 36 минут):** 888 открытий и закрытий панели, 148 копий, 2 перезапуска службы. Служба (4 замера после прогрева, 29,7 - 36,3 мин): дескрипторы 240 -> 241 (+1; порог < 50), потоки -2, закрытая память 6,3 -> 7,4 МБ (+1,1; порог < 30), GDI / USER 0. Панель (15 замеров, 4,7 - 36,3 мин): дескрипторы **170 -> 170 (0)**, закрытая память +0,2 МБ, рабочий набор -2,2 МБ, GDI 0 / USER 0. Утечек нет.

**NTFS и антивирус (`ntfsav` 20 из 20, `eicar` 8 из 8):** те же результаты, что в раунде 9 на Windows 10 (EFS: задание Failed за долю секунды с именем файла; сжатый и разреженный файлы: побитно верно; EICAR при включённом Defender: Failed «contains a virus... (225)», без зависания; имя в 255 символов и 5000 файлов: Success; файл исчез по ходу: Failed с именем файла).

**FTP / SCP.** `ftprepeat` 10 из 10, `faultrepeat` 10 из 10 + ssh 5 прогонов без FAIL, `ftpsoak` 317 из 317.

### Падение `net` / ftp: `ftp_passive_active_and_listing_variants` (среда)

«active ftps: the server refused to store 'act.bin.part': 501 Rejected data connection to foreign address 172.30.0.2:59532». Активный режим FTP: сервер pyftpdlib сверяет адрес данных с адресом управляющего соединения, а гость стоит за NAT контейнера dockur (адрес гостя 172.30.0.2, сервер видит другой). Это граница стенда, а не продукта; те же тесты проходят в Wine (`tools/run-ftp-tests.sh`) и на гостевых ВМ с прямой маршрутизацией; на Windows 10 x64 (раунд 9) активный FTP тоже был ограничен NAT.

### Открытый вопрос раунда 9: TLS-канал закрывается без `close_notify` (~5 % загрузок после обрыва)

**Причина не установлена, на этом круге сбой не воспроизвёлся.** Что проверено:

1. **Windows 11 (TLS 1.3 везде: клиент OpenSSL всегда договаривается о TLS 1.3 с pyftpdlib).** `ftpsoak` 10 минут с журналом DEBUG: 317 запусков `ftp_fault_injection_upload` (около 1900 управляющих и 2500 каналов данных), **0 падений, 0 зависаний**. Плюс `faultrepeat` 10 из 10. Сбой раунда 9 был на Windows 10 (4 - 6 на 93 - 113 запусков); защитные правки раунда 9 (цикл в `_do_ssl_shutdown` сервера, ожидание записи `close_notify` в `NetConn::shutdown`) уже на месте, поэтому отсутствие сбоя не доказывает, какая из них помогла.
2. **Разбор журналов этого прогона** (`run1/srv-faultrepeat/ftp-soak/ftpes.log`, `ftps.log`). Закрытие без `close_notify` (`recv(): Unexpected EOF`) встречается ТОЛЬКО на **управляющем** соединении (634 + 635 случаев; примерно треть управляющих соединений - клиент просто закрывает сокет после последней команды или при `reconnect()` без `QUIT` и `close_notify`), и во всех них pyftpdlib один раз пишет «shutdown while in init» и закрывает канал (611 раз; дальше не крутится). **На каналах данных `Unexpected EOF` не было ни разу** (2536 из 2536 закрытий `shutdown completed`, то есть `close_notify` клиента доходит). Значит, в этом прогоне клиент нигде не обрывал загрузку без `close_notify`, а повторяющийся цикл сервера возникал бы только при закрытии канала данных без него.
3. **Локально под Wine** (loopback, `build/x64/vm2bautotest.exe`, `--minutes`, журнал DEBUG и INFO): 329 + 92 запусков, 0 падений; на каналах данных `Unexpected EOF` тоже нет, на управляющих - во всех (539 из 539 в выборке DEBUG).
4. **Снимок трафика раунда 9** (`w10-20261031-cycle3/run4-ftpsoak-diagnosis/ftp-s160.pcap`): в TLS 1.3 `alert` шифруется (внешний тип записи 23), поэтому по снимку нельзя отличить `close_notify` от данных; без ключей сеанса (SSLKEYLOGFILE) причину по снимку не установить. Различие TLS 1.3 / 1.2 именно в этом: ни `tshark`, ни сервер не показывают alert-запись, видно только FIN / RST и сообщение сервера.
5. **Вывод:** наиболее вероятный механизм - закрытие канала без `close_notify` на фоне обрыва (сервер сам рвёт оба соединения RST, `cut_everything`), когда сервер успевает прочесть EOF раньше RST; для настоящих серверов это безвредно (закрытие без `close_notify` даёт отказ только строгим серверам). Это предположение, не доказанное.

*Что добавлено для следующего подхода:* `tools/testservers/ftp_server.py` теперь пишет в журнал (уровень INFO, без DEBUG) каждый случай «канал данных загрузки закрыт БЕЗ close_notify клиента» (в обычном режиме тоже, не только в строгом): при следующем наблюдении сбоя будет видно сразу, какое соединение и какая команда. Если сбой повторится, нужен `SSLKEYLOGFILE` у клиента (в OpenSSL `SSL_CTX_set_keylog_callback`) для расшифровки снимка.

### Найдено и исправлено

Продуктовых ошибок на Windows 11 не найдено: **код продукта в этом раунде не менялся** (дрейф часов в `tz` и активный FTP за NAT - среда). Изменено:

* **Гонка в самом тесте** `svcesxi/esxi_task_survives_host_power_loss_and_service_restart` (`src/tests_svc_esxi.cpp`), найдена на полном прогоне под Wine (seed 7774, раунд 1: «power lost after 19 events: no (job finished first)», затем `fin` не Success): хост мог умереть МЕЖДУ проверкой «dead» и чтением строки задания (задание уже Failed из-за обрыва), и тест принимал настоящий обрыв питания за «задание закончилось раньше». Исправление: после цикла ожидания состояние хоста читается ещё раз. Регрессия - сам тест: на том же seed 7774 теперь «power lost ... yes» в обоих раундах (проходит), плюс seed 11 и 12.
* `tools/testservers/ftp_server.py`: журнал закрытия канала данных загрузки без `close_notify` (см. выше).

Wine, полный `tools/run-tests.sh --seed 7774`: native, core 150 / 0 (199 пропусков: только для настоящей Windows), core32 65 / 0, ftp 34 / 0, ssh 65 / 0, web (926 + 15), gui 4 / 0, package, package2 - зелёные с первого раза; esxi с первого раза 66 из 67 (гонка теста выше), после правки итоговый прогон на пересобранной сборке (`--only esxi,package,package2`, seed 7774): esxi **67 из 67**, package и package2 ok.

### Не проверено

* SmartScreen с настоящим сертификатом (OV / EV): репутация и допуск (`docs/SIGNING.md`); метка времени подписи (RFC 3161). На Windows 11 с Smart App Control проверен только тестовый корень.
* Причина закрытия TLS-канала без `close_notify` (см. выше): не установлена.
* Закрытие управляющего FTP-соединения без `QUIT` и `close_notify` (клиент закрывает сокет): у нормальных серверов безвредно; корректное завершение (`QUIT`, `close_notify`) - возможная доработка клиента, в этом раунде не делалась.

## Results

`.winvm-results/<os>-<time>/`: `summary.txt`, `results/*.out` (every job), `results/autotest-*.txt` (autotest logs), `results/shots/*.png`, `container.log`, `smbreal.log`.
