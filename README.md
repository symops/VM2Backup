# VM2Backup

Backup of virtual machines (VMware ESXi / vCenter, or a folder of VMs) to a folder, a network folder (SMB), FTP, FTP over TLS or SCP, with a
a Windows service for scheduled tasks, an HTTPS web interface that does all the work, and a small Windows control panel (`VM2Backup.exe`).
Native Win32 C++17; TLS and cryptography by OpenSSL, linked statically (libraries are fetched and built by `tools/deps.sh`, see `docs/DEV.md`); x64 and x86 builds; 16 interface languages besides English.
Author: Symo <symops@gmail.com>.

## The program is a control panel

**The work is done in the web interface at https://127.0.0.1:8443/ (default login `admin` / `admin`)**: servers, tasks, schedule, backups,
restore, history and settings. `VM2Backup.exe` is only a small fixed-size control panel: the state of the service (running / stopped /
not installed), its version and data folder, the state of the web site, the buttons Start / Stop / Restart service, Open web interface,
Open logs folder, the language and Exit. It talks to the service over the named pipe and needs administrator rights (service control).
Command line: `/pipe <name>`, `/selftest [ms]`, `/guitest <file>` (tests). The full former Windows GUI (views, wizards, dialogs, tests) is kept
for a possible restore in `/home/symo/Claude/.wip/gui-full-backup/` (sources, language files, tests, build and test scripts of the time before the panel).

## What it does

* **Sources:** VMware ESXi 6.x – 9.x and vCenter (vSphere API: snapshot, copy of the frozen disks, snapshot removed afterwards),
  or a folder where every sub-folder is a VM.
* **Targets:** local folder, **network folder (SMB / CIFS, UNC path with its own user and password)**, FTP, FTPS / FTPES (TLS), SCP
  (SSH by libssh2; SFTP is not supported). The service cannot see the drive letters of a user: use the UNC path. A folder with VMs may also lie on a network share.

* **Archive:** tar split into independent volumes, compressed with xz (LZMA2, the only compressor: levels 1-9, default 5; archives are `*.tar.xz[.enc]`; the number of compression threads is a setting, Auto by default), optionally encrypted
  (AES-256-GCM, PBKDF2), every volume with a SHA-256 in the manifest.
* **Resume:** a backup, a verification and a restore continue after a lost connection (volume by volume) and after a restart of
  the service. Restore to ESXi uploads whole files (HTTP PUT cannot append): a lost connection re-sends the current file.
* **Restore:** to a folder, or to an ESXi host / vCenter (files are uploaded and the VM is registered); both from the web site's Backup Explorer ("Restore to": Folder / ESXi host or vCenter, with the same choices as the old Windows dialog; admin role only, because the service writes as SYSTEM / connects with stored credentials).
* **Lock-out protection:** a domain account is locked after a few wrong passwords, and one call to a server with a wrong password counts twice (Kerberos, then NTLM; measured on a real
  domain controller). So when a server (SMB, FTP, SCP, ESXi / vCenter, a VM folder on a share) refuses the credentials (wrong / locked / disabled / expired password, logon not allowed;
  never a network error or a timeout), the service remembers it in `state.json` and does **not contact that server again** from a scheduled run or from another element of the same task:
  the element ends `Failed` with the mark "skipped" and the text "The credentials of server ... were rejected on ...; not tried again until the server record is changed or "Try again" is pressed".
  Changing the login data of the record (user, password, address, port, key) lifts the block by itself; so does "Try again" on the Datacenter page (`servers.unblock`, admin) and a successful
  connection. A manual run and "Test connection" make exactly one attempt and update the mark. Other servers are not affected. Details: `docs/WEB.md`.
* **Scheduler:** every N minutes/hours/days, weekly, monthly, once; time windows with a speed limit per window; bandwidth limit.
* **Web site (on by default, localhost only):** HTTPS, users with roles (admin / operator / viewer), TOTP 2FA, listening interfaces
  chosen by the administrator. A fresh installation has the user `admin` with the password `admin`; the site listens on localhost only
  by default; whoever can reach the site with that password controls the service (and the machine, as SYSTEM) - change it under Users,
  and keep the site on localhost or change the password before choosing a network address. The site administers itself (addresses, port, certificate, users, 2FA with a QR code, audit log), offers 17 languages, the network drive / network browse and folder pickers and the HTML report of a history entry; `VM2BackupService.exe /reset-web` is the way back in when nobody can sign in. See `docs/WEB.md`.

## Build and test (Linux, clang + MinGW-w64, Wine)

    tools/deps.sh build all     # once: OpenSSL (and the other libraries of deps/deps.lock) for x64 and x86
    ./build.sh                  # build/x64/ ; ARCH=x86 ./build.sh -> build/x86/ ; ONLY=autotest|service|gui
    ./package.sh                # dist/VM2Backup-Setup.exe (picks the 64- or 32-bit installer)
    tools/run-tests.sh [--seed N] [--skip a,b] [--only a,b]
                                # native (ASan/UBSan), core (x64 + x86), ftp, ssh, esxi, web, gui, package
    tools/run-winvm.sh x86      # the whole plan on a REAL Windows VM (x86 / w10 / w11), deleted afterwards: docs/WINDOWS-TESTS.md
    python3 tools/gen_lang.py --update   # after adding TR("...") strings; --check, --missing <code>

Details for developers: `docs/DEV.md`; tests on real Windows: `docs/WINDOWS-TESTS.md`; ESXi/vCenter: `docs/ESXI.md`; web site: `docs/WEB.md`; installation: `docs/INSTALL.en.txt`.

## What is verified, and what is not

* The bulk of the tests runs under **Wine** and against local test servers (OpenSSH `sshd`, paramiko, pyftpdlib, own mock servers).
* **Real Windows** (`tools/run-winvm.sh`, `docs/WINDOWS-TESTS.md`): the whole plan ran on real VMs of **Windows 10 x86 22H2**, **Windows 10 x64 22H2 (Russian)** and
  **Windows 11 25H2 (Russian)**, each deleted after its run: installer (silent install, update in place over a running service, removal, reinstall), the service under the
  real Service Control Manager (delayed auto start, STOP_PENDING, crash recovery), the named pipe ACL (Administrators + SYSTEM only), the panel at 100 / 125 / 150 % scaling,
  the web site with Windows' own TLS clients (TLS 1.2 everywhere, TLS 1.3 negotiated on Windows 11), DPAPI over a service and a Windows restart, the native autotests, FTP / SCP
  against servers on the Linux host, and **real SMB** (`WNetAddConnection2W` against a Samba share with a user and a password, also an SMB 3 encrypted share on Windows 11:
  backup / verify / restore, wrong password, unreachable server, a server that stops and returns in the middle of a backup, network browse, `net use` drives).
  The Windows 11 SmartScreen blocks the unsigned Setup.exe when it carries the Mark of the Web (the installer is not code-signed). A **real Active Directory domain** (two Windows Server 2022 VMs, `tools/run-winvm.sh domain`) covered: Kerberos / NTLM accounts, the service as LocalSystem / a domain user / a gMSA (real password rotation after a synchronized clock shift, no restart needed), expired / locked / disabled / must-change passwords with the lock-out guard, the network root listing the domain's servers (DsGetDcName + LDAP), unattended logon after every restart. Not covered: real ESXi / vCenter (the ESXi group ran on the x86 VM only, 5 tests limited by the Windows SMB client cache).
* **ESXi / vCenter** (backup and restore) was tested only against the mock servers in `tools/testservers/esxi_mock.py`
  (profiles 6.0 ... 9.0, host and vCenter). The API shapes follow the vSphere reference; behaviour of real hosts is assumed.
  ESXi 9.0 details are assumed.
* No incremental backups, no e-mail reports, no Hyper-V.
