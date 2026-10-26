# Tests on a real Windows VM

Everything else in this project runs under Wine or against fakes. This harness builds a **real Windows** test VM from the user's ISOs, installs the package
(`dist/VM2Backup-Setup.exe`) on it and runs the whole plan: the service under the real Service Control Manager, the named pipe ACL, the panel on a real desktop,
the web site with Windows' own TLS clients, DPAPI, the native autotests, the network groups against servers on the Linux host, a **real SMB share**
(`WNetAddConnection2W`) and the installer's update / removal.

    tools/run-winvm.sh x86            # Windows 10 x86 (WIN10X86.iso), plain qemu inside the dockurr/windows image
    tools/run-winvm.sh w10            # Windows 10 x64 ru business 22H2 (dockur)      [run 2026-10-26: see "Windows 10 x64 ru"]
    tools/run-winvm.sh w11            # Windows 11 5in1 25H2 (dockur)                 [run 2026-10-26: see "Windows 11"]
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

## Results

`.winvm-results/<os>-<time>/`: `summary.txt`, `results/*.out` (every job), `results/autotest-*.txt` (autotest logs), `results/shots/*.png`, `container.log`, `smbreal.log`.
