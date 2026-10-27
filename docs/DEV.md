# VM2Backup — developer notes (conventions every module follows)

VM2Backup is a native Win32 program in C++17, built on Linux with clang + MinGW-w64 and tested under Wine, like its sister
projects Click2Screen / Speak2Mic. Read `src/base.h`, `src/stream.h`, `src/target.h`, `src/engine.h`, `src/crypto.h` first.

## Rules

* **No C++ library.** There is no libstdc++ for MinGW here: no `std::string`, `std::vector`, `<new>`, exceptions, RTTI,
  `<thread>`, `<functional>`. Use `Str`, `Vec<T>`, `Json`, `Bytes` from `base.h`; plain C library; Win32 API (threads with
  `CreateThread`, locks with `CRITICAL_SECTION`/`InterlockedXxx`). Build flags: `-fno-exceptions -fno-rtti`. Function-local
  statics are fine (`cxxrt.cpp` provides the guards). Placement new is `new (ptr, vm2b_place_tag()) T(...)`.
* **Pure modules also build natively** on Linux (`g++ -fsanitize=address,undefined`), which is the fast way to test them:
  `base tar` use only the C library (`xz` needs liblzma: natively `-llzma` of the system, in the program the static one of `deps/`) and `#ifdef _WIN32` for threads/files. Windows-only modules (crypto and TLS via OpenSSL, sockets via Winsock, files via Win32) are tested under Wine.
* Errors are returned (`bool` / `-1` / `nullptr`) with a message in `err`; messages are English, one line, no secrets in them.
  Never `abort()` on bad input from a file / the network: damaged data must give an error, not a crash (fuzz it).
* Everything long-running must poll for an abort (`Progress::aborted()`, or a flag the caller sets) and stop promptly;
  every network call has a timeout.
* Secrets (passwords, passphrases, keys) are never logged and never put into error messages.
* Style: like the existing sources — 4 spaces, `m_` for members, short comments that say *why*.

## Interfaces you build against

* `Sink` (`write` / `finish` / `abort`) and `Source` (`read` returns bytes, 0 = end, -1 = error) — `stream.h`.
* `Target` — `target.h`: `connect/open_write/open_read/list/remove`; a write goes to `<name>.part` and takes its name when
  `finish()` succeeds; `abort()` removes it; `list()` hides `.part` files. `MakeTarget()` (in `target_local.cpp`) builds one from a
  JSON config; the FTP and SSH modules provide `NewFtpTarget` / `NewSshTarget` (see the config keys in `target.h`).
* `Codec` — `codec.h`: xz is the ONLY compressor (levels 1-9, default 5, threads 0 = Auto): `CodecMakeSink / CodecMakeSource` wrap `MakeXzSink / MakeXzSource`. gzip and "none" were removed; a manifest whose `compression` is not `"xz"` is listed but refused with "only xz is supported", a journal of another codec is ignored, and `Config::open` migrates old configs/tasks (gzip L -> xz L, none -> xz 1, unknown -> xz 5) and writes them back.
* Crypto primitives (OpenSSL) — `crypto.h`: SHA-256/SHA-1, HMAC, PBKDF2, AES-GCM, random. Add what you need there (additive only).
  TLS client (`tls.h`, FTPS / HTTPS / ESXi) and TLS server + certificates (`webtls.h`) are OpenSSL as well; the sockets stay ours (`select` loops with
  timeouts and an abort flag, OpenSSL only does the records). The format V2BENC3 is fixed by `tests_crypto.cpp` (vector from an independent implementation).

## Отказ в учётных данных (`Target::auth_failed`)

Цель, которой сервер отказал в ЛОГИНЕ (неверный / заблокированный / истёкший пароль, вход запрещён), вызывает `failAuth(текст, код)` вместо `fail(...)`: ошибка постоянная, `auth_failed` = true (до удаления объекта), `auth_code` и `auth_err` хранят код и текст.
Сеть, таймаут, неизвестный ключ хоста и сертификат - только `fail(...)`. Служба (`svccore.cpp`, `authGate / authFailedNow / authOk`) по этому признаку ставит отметку в `state.json`; новая цель с логином обязана ставить признак, иначе
защита от блокировки (docs/WEB.md) для неё не работает. Для источников ВМ признак определяется по тексту ошибки (`IsAuthTextOfSource`).

## Resumable backups (what the targets must support)

A backup is written as a series of independent volume files (`<archive>.00001`, `.00002`, ...), each one an atomic `.part` -> rename
write, plus a small journal file `<archive>.resume` that is rewritten after every volume. After a lost connection the engine calls
`reconnect()`, lists the folder, and continues with the first volume that is not complete. So a target needs nothing special for
that — but it **must** report `transient = true` for errors a retry can cure (reset, timeout, EOF on the socket, FTP 4xx, SSH
disconnect) and `false` for permanent ones; `connect()` must work again after any failure; `list()` / `exists()` / `remove()` must be
reliable (the engine trusts them to decide which volumes exist); an interrupted `open_write` must never leave a file under the
final name (the `.part` rule).

## Third-party libraries (`deps/`)

Ready-made libraries are used where correctness matters (TLS, crypto, SSH, xz): OpenSSL, libssh2, liblzma. Sources are **not** in the
repository. `deps/deps.lock` lists `name version url sha256 license [track]` per library; `tools/deps.sh`:

```
tools/deps.sh fetch                 # download into deps/src/ (cache) and verify the SHA-256; a mismatch is refused
tools/deps.sh build [x64|x86|all]   # static MinGW-w64 libs -> deps/out/<arch>/{include,lib,<name>.flags}; incremental (stamp = version + script)
```

The libraries are updated by changing the line in `deps/deps.lock` (version, URL, sha256) and rebuilding the whole project (`tools/deps.sh build all`, `./build.sh`, then `tools/run-tests.sh`); there is no separate update tool. Each library has
`tools/deps/<name>.sh build <arch> <srcdir> <prefix>` which must write `<prefix>/<name>.flags` (`CFLAGS=` and `LIBS=` lines; LIBS lists the
library, then what it needs: `-lssh2 -lssl -lcrypto -lws2_32 ...`). `tools/deps-env.sh <arch> [cflags|libs]` merges the flags of all libraries
in `deps.lock` order and `build.sh` uses it generically (so a new library is: a line in deps.lock + a script; nothing in build.sh).
Link is static; `-DVM2B_HAVE_OPENSSL` etc. come from the `.flags` files. First build of OpenSSL takes ~2 minutes per architecture.
Licences: `THIRD-PARTY-NOTICES.md` (OpenSSL Apache-2.0, libssh2 BSD-3-Clause, liblzma 0BSD / public domain); it ships with the package.

## Building and testing

```
ONLY=autotest OUT=/home/symo/Claude/.wip/<you>-out/x64 ./build.sh          # builds vm2bautotest.exe only, into your own folder
WINEPREFIX=/home/symo/Claude/.wip/wine-<you> WINEDEBUG=-all wine64 /home/symo/Claude/.wip/<you>-out/x64/vm2bautotest.exe --seed 1 --only <group-or-test>
ARCH=x86 ONLY=autotest OUT=/tmp/<you>/x86 ./build.sh # the 32-bit build must compile too
```

`build.sh` compiles every `src/*.cpp` that exists (the optional ones by name: `tls http target_ftp target_ssh ssh esxi xml`)
and every `src/tests_*.cpp`. A test is written like this (see `src/tests_core.cpp`):

```cpp
VM2B_TEST("group", test_name) { ...; T_CHECK(cond); ...; return true; }   // c_ is the Ctx: c_.rnd(n), c_.coin(), c_.fill(), c_.note(...)
```

Tests are **randomized** from a seed (printed first, `--seed N` repeats a run); one test draws its numbers from its own stream.
Network tests read the servers' addresses from `--servers file.json` (c_.servers) and call `c_.skip("...")` when absent.
Test servers are Python scripts in `tools/testservers/` (pyftpdlib, paramiko, and the system `sshd`/`ssh-keygen` are installed;
python is `python3`, run it with `-I` for files you did not write). Each module has `tools/run-<module>-tests.sh` that starts its
servers on its own port range, writes the servers json, runs the wine test, and stops the servers **by PID only** (never `pkill`).

`src/tests_deep.cpp` holds the "what the other tests do not catch" suite: every byte of a small archive flipped / cut with the manifest hashes refreshed,
a target that fails at random (transient / permanent / "committed but reported as failed") against a model of the expected state, abort and pause at random
points, the service under random cancels and restarts, hostile RPC arguments (a fuzzer over the commands), tar volume boundaries, retention with a clock set back,
scheduler gaps, settings limits. Its `big` tests (`VM2B_BIG=1`, set `TEMP=Z:\\tmp\\x` to keep the multi-GiB files off the system disk) cover 130 MiB of mixed data at levels 9 / 6 and
a 4.3 GiB disk restored in two runs. Native coverage-guided fuzzers (clang `-fsanitize=fuzzer,address,undefined`) for the tar reader, the xz decoder and Json were built
from three one-function harnesses; they are not part of `run-tests.sh`.

## Network folders (SMB)

`smb.h` / `target_smb.cpp`: target type `smb` (UNC path + user + password, `WNetAddConnection2W` without a drive letter, then the local-folder
target on the UNC path, errors classed transient / permanent by their Win32 number). The WNet calls go through `SmbSetApi()`; the tests
(`src/tests_smb.cpp`, group `smb`, no ports) install a fake that maps `\\fileserver\backup` to a local folder, so everything except real
SMB runs under Wine. Never put a password into an error text (the fake checks this).
(The screenshot script of the old SMB dialogs, `tools/run-smb-shots.sh`, was removed with the GUI; it is in the backup.)

### net.drives / net.browse (service RPC for the web site)

`netbrowse.h` / `netbrowse.inc` (included by `target_smb.cpp`; `build.sh` compiles only listed modules) back two ADMIN-ONLY commands of `ServiceCore::handle` (contract in the header of `svccore.h`).
`net.drives` reads `HKEY_USERS\<SID>\Network\<letter>` of the loaded profiles (+ the service's own remote drives); `net.browse` walks the neighbourhood (WNetOpenEnumW), a server's shares
(WNet over a temporary `\\server\IPC$` connection, NetShareEnum as fallback) and a share's sub-folders. Each browse runs in a worker thread with a 15 s deadline that owns copies of the credentials, wipes them and
cancels its connection by itself (the caller returns a temporary error `(1460)` on timeout; at most 8 workers live). If one of our connections to the server is open, the credentials are ignored (Windows allows one set per server; a running backup is never disturbed).
Windows is reached through `NetApi` (`NetBrowseSetApi`) + `SmbSetApi`; tests: `src/tests_netbrowse.cpp` (group `smb`). The Windows dialogs are not changed.

### Real SMB (group `smbreal`, `src/tests_smb_real.cpp`)

Runs only with `--servers` that has the key `smb` (a real Samba share with a user and a password, plus a control port that stops / starts it); the Windows VM harness
(`docs/WINDOWS-TESTS.md`) provides it, everywhere else the tests SKIP. They use the real `WNetAddConnection2W` path of `target_smb.cpp`: round trip with xz (plain / encrypted),
wrong password -> permanent 1326 without the password in any text, unreachable server -> transient, missing share -> permanent 67 (Windows reports 53 for it with Samba; `SmbApi::probe`
tells it from a dead server), the server stopped in the middle of a backup and started again, `net.browse` / `net.drives` (a drive mapped with `net use`), concurrency, the service commands.

## Port ranges (parallel work)

xz — none; FTP/FTPS/TLS/HTTP — 21000-21999; SSH/SCP — 22000-22999; ESXi emulators — 23000-23999.

## Working copy rule for parallel agents

Work in your own copy of the tree (`cp -a /home/symo/Claude/VM2Backup /home/symo/Claude/.wip/vm2b-<you>`; NOT /tmp: it is wiped by a reboot), not in the shared one. When your
tests pass, copy back **only your new files** (and, if unavoidable, small additive edits to shared headers, which you must list
in your report). Never edit `engine.*`, `tests_core.cpp`, `tests_engine.cpp`, `build.sh` in the shared tree.

## Window tests (group "gui", `tools/run-gui-tests.sh`, `tools/run-gui-layout.sh`)

* The program is a control panel (`src/main.cpp`; the pipe client `src/client.*`, the toolkit `src/ui.*`, the pure URL helper `src/panelutil.*`, which also builds into the
  autotest). `VM2Backup.exe` links only `base cxxrt applog winutil ipc panelutil` (no engine / web / targets): about 0.3 MB.
* `program_selftest` — `VM2Backup.exe /selftest` shows the panel in every state (running, stopped, not installed, web on / off / not listening, long texts) and exits 0.
  `gui_interaction` — `VM2Backup.exe /guitest <result file>` (`src/guitest.cpp`, started by `src/tests_gui.cpp`): a script thread plays the user through window messages and
  reads the state back from the service over a second pipe: state display, Open web interface (URL for 127.0.0.1 / ::1, message when the site is off), Open logs folder,
  all 17 languages, Enter / Esc, fixed size. `gui_service_stops_and_restarts` — the Start / Stop / Restart buttons against the test service (the panel's seams
  `g_panelHooks` in `src/panel.h` replace the SCM and ShellExecute; the autotest stops / starts the in-process service on `stop-N.req` / `start-N.req` files).
  `panel_without_service` — no service at all. `panel_web_url` (group `panel`, no display) — the URL built from web.status (IPv6, no listener, port 8443, wildcard, link-local).
  `VM2B_GUITEST_ONLY="state,open web"` runs some scenarios only; a new scenario is one function plus one table entry.
* `Form::checkLayout` (ui.cpp) lists controls whose text does not fit; `Form::fitAll` draws such texts 1-2 pt smaller instead. `tools/run-gui-layout.sh` runs the
  `/selftest` under every language (`VM2B_LAYOUT_OUT=<file>`) and fails on any finding. Wine measures with Noto Sans, which is wider than Segoe UI: findings are conservative.
  `GUI_BIN=<dir>` (run-gui-tests.sh) / `--bin <dir>` (run-gui-layout.sh) use another build folder.
* The former full GUI (Datacenter, Backup Explorer, tasks, history, settings, wizards, Web access dialog and their scenarios) is in `/home/symo/Claude/.wip/gui-full-backup/`.
* A default push button must be created with `button(..., def = true)`: the form answers `DM_GETDEFID` with it, otherwise Enter sends IDOK and closes a wizard.

## More tests (second round)

* Web (`tools/testservers/web_client_tests.py`, run by `tools/run-web-tests.sh`): `t_tls_matrix` (every suite openssl knows, TLS 1.3 suites, groups, ALPN, SNI,
  resumption, renegotiation, mutated ClientHellos), `t_tls_churn` (1000 mixed connections, RSS / threads of the Wine process), `t_tls_exhaustion` (stalled sockets from
  many 127.0.0.x addresses), `t_pfx_variants`, `t_rpc_fuzz` (`VM2B_FUZZ_SEED`, `VM2B_FUZZ_VERBOSE=1` prints the pipe-side calls), `t_concurrent_admin`, `t_totp_races`, `t_clock_jumps`.
  The web server keeps a connection that has not sent a byte parked at the end of the queue (a worker never waits for it); a full queue drops the oldest entry.
* Window test scenarios (`guitest.cpp`): `network drives` (the picker with fake drives: `SmbSetFakeDrives`, test hook in target_smb.cpp), `compression` (all 9 levels and the
  thread counts saved and read back), `web 2fa` (certificate dialogs, 2FA enrolment with a TOTP computed by the script), `dialog churn` (private bytes after 30 rounds of dialogs;
  handles / GDI are not measurable under Wine), and the restore dialog in ESXi mode (server down). `run-gui-layout.sh` cycles all 9 compression levels on the settings page.
* Installer: `tools/test-package-extra.sh` (own package from `BUILD_X64` / `BUILD_X86`, flags, path with spaces / Cyrillic, bad `/dir`, update in place, moving the folder, removal with
  the service running, damaged Setup.exe). With `/silent` the launcher never shows an error box (exit code 1 instead).

## Real Windows (VM)

`tools/run-winvm.sh <x86|w10|w11>` builds a Windows test VM from the user's ISOs and runs the whole plan on it; see `docs/WINDOWS-TESTS.md`. The test servers of the network groups take
`VM2B_BIND_ADDR` / `VM2B_SERVER_HOST` / `VM2B_SERVER_NAME` / `VM2B_GUEST_MAP` (defaults unchanged: 127.0.0.1, `Z:` paths); tests must not assume a loopback address: take the host from
`servers.json` (the fault control port of the paramiko servers was hard-wired to 127.0.0.1 and failed on the VM). File helpers of `base.cpp` and `winutil.cpp` must accept paths
over 260 characters (long-path prefix): the Wine runs never notice, Windows does (`core/whole_file_helpers_accept_long_paths`).

## Test VMs are deleted after every test
The ESXi and svcesxi tests create VMs on the mock servers. After EVERY test of these groups (also after a failure or an abort) the
harness calls `EsxiTeardown` (src/tests_esxi.cpp, registered with `RegTeardown`, run by src/vm2bautotest.cpp): it resets every mock
server, i.e. deletes all VMs, sessions and injected faults. A test still deletes its own VMs where it can (`DeleteVm`), but nothing
depends on it. A new group with external resources registers its own teardown the same way: `static RegTeardown r("group", fn);`.

