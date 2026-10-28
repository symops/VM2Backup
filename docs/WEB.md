# VM2Backup web administration

An optional HTTPS site, served by the **service** (`VM2BackupService.exe`), with which an administrator manages VM2Backup from a
browser: Datacenter (servers), Backup Explorer, Scheduled Tasks, Task History, Settings, the "Virtual Machine Backup" /
"Add Server" / "Add Scheduled Task" / "Scheduled Task Details" / restore dialogs and a live "Running jobs" panel (progress, speed,
abort).  The Windows program is only a small control panel (service state, start / stop, open the site): **the site is the whole user interface, and it administers itself** (the "Web access" page: addresses, port, security settings, certificate, users, 2FA, audit log).  **It is on by default, on `https://127.0.0.1:8443/` only.**

## Defaults (secure by default)

| Setting | Default | Meaning |
|---|---|---|
| `enabled` | `true` | listens on `bind`; `false` = nothing listens, no port is opened |
| `bind` | `["127.0.0.1"]` | a list; only these addresses / adapters listen |
| `port` | `8443` | |
| `require_2fa_admins` | `false` on a fresh `web.json` | `true` = an `admin` can sign in only with TOTP set up (values already stored are kept) |
| `session_minutes` | `20` | idle timeout; absolute lifetime is 12 h |
| `trust_proxy` | `false` | `X-Forwarded-For` is ignored |

A fresh `web.json` (no users, no `default_admin_created` flag) gets one user, **`admin` with the password `admin`** (role `admin`, PBKDF2 like
every password; audit event `default_admin_created`).  It is created once: deleting every user later does not bring it back, and an
installation whose `web.json` already has users is not touched.  There is no registration on the web; users are created by an administrator of the
site (page "Web access", Users) or with the local administration channel (the named pipe, ACL Administrators + SYSTEM).  If all users are deleted
the login page says that no user is configured and how to get back in (`/reset-web`, below).  The consequences are in the threat model below ("What is NOT protected").  `service.shutdown` is never reachable over the web; the `web.*` commands are reachable by the **admin** role only.

## Opening it, and managing the site from the site

On a fresh installation the site already runs: open `https://127.0.0.1:8443/` (the control panel of the Windows program has a button for it), sign in as `admin` / `admin` and change that password under **Web access, Users**.  Everything else is on the same page:

* **Listening interfaces:** a checklist (localhost - always on, a disabled checkbox -, "All interfaces", every adapter from `web.interfaces.list`, extra addresses you add), the port, and for every entry whether it listens or why not.
* **Security:** require 2FA for administrators, the idle timeout, trust of `X-Forwarded-For` (with a warning: only behind your own reverse proxy).
* **Certificate:** subject / validity / fingerprint, "Create new self-signed" with extra names, "Upload PFX" (the file travels base64-encoded in the JSON body, 1 MB at most; the site never reads a certificate by a path on the server).
* **Users:** add / edit / delete, role, password (strength hint), 2FA enrolment with a QR code drawn in the browser (plus the secret and the `otpauth://` link as text), recovery codes shown once, disable.
* **Audit log:** the tail of `audit.log`.

The same commands exist for the local channel (the named pipe) and, for admins, over HTTPS (`POST /api/<command>`, CSRF header required):

```
web.users.list / web.users.save {name, role: admin|operator|viewer, password?, twofa: off|optional|required} / web.users.delete {name}
web.users.totp_enroll {name}        -> {secret (base32, shown once), uri (otpauth://...)}
web.users.totp_confirm {name, code} -> {recovery_codes[10]}   (shown once; 6-digit code from the authenticator)
web.users.totp_disable {name}
web.interfaces.list                 -> every adapter with its IPv4 / IPv6 addresses (for a checklist)
web.settings.get / web.settings.set {enabled, bind: [...], port, require_2fa_admins, session_minutes, trust_proxy}   -> {settings, warnings[]}
web.cert.generate {names[]} / web.cert.import {pfx_base64 | pfx_path (local channel only), password}
web.status                          -> listening per entry / address, certificate fingerprint, users, sessions
web.audit.tail {lines}              -> {text}
```

**Lock-out protections (over HTTPS only; the local channel is not restricted):** the site cannot be switched off from itself (`enabled:false` is refused); the `bind` list must keep a loopback entry (`localhost` / `127.0.0.1`), additional addresses, adapters or `all` are allowed; a new port or bind is tried first (every entry must be able to listen on at least one of its addresses) and refused with the operating system's error if impossible - the old settings stay; the listener restart happens *after* the answer has been sent, and only when `bind`, `port` or `enabled` changed (sessions survive, other connections are dropped); you cannot delete your own account, change your own role or demote / delete the last administrator; you cannot switch off your own 2FA while it is required for administrators, nor require it (or require it of yourself) before your own 2FA is set up; a change of your own password or 2FA ends your sessions; every change is in the audit log with the acting user (never a password, secret or code).

**Recovery (nobody can sign in, a bind hides the site, a lost 2FA device):** stop the service, then in an elevated command prompt
`VM2BackupService.exe /reset-web [/data <folder>]`.  It refuses while the service runs and without administrator rights; it resets the site's settings to the defaults (on, `127.0.0.1:8443`, no 2FA requirement, no proxy trust), sets the user `admin` to the password `admin` with the admin role and no 2FA (creating it when missing), keeps all other users and the certificate (a damaged `web.json` is kept as `web.json.damaged`), writes the audit event `web_reset` and prints what it did.  Then start the service and sign in again.

Open `https://127.0.0.1:8443/`.  The first start makes a self-signed certificate (RSA-2048, SHA-256, 3 years, subjectAltName =
machine name + localhost + 127.0.0.1 + ::1 + the concrete addresses chosen at that time).  Its SHA-256 fingerprint is in `web.status`
(`cert.fingerprint`) - compare it with the one the browser shows before you accept it.  The certificate and the
DPAPI-protected private key live in `web.json` in the data folder; no key container is left in the Windows key store.

### Choosing where it listens

`bind` is a **list**.  Entries:

* an address: `192.168.1.10`, `::1`, `fe80::1%12` (link-local with the interface index);
* `adapter:<AdapterName GUID>` (from `web.interfaces.list`): *all* addresses of that adapter, so a changed DHCP address keeps working;
* `localhost` = `127.0.0.1` + `::1`;  `all` = `0.0.0.0` + `::` (every interface - think twice).

One listening socket per resolved address (IPv6 sockets are `IPV6_V6ONLY`).  An entry that cannot be bound (address gone, port taken)
is reported **per entry** in `web.status` (`entries[].addresses[].error`) and is **not** a global failure; the other entries keep
serving.  Unbound addresses are retried every 30 s and when Windows reports a network change (`NotifyIpInterfaceChange` /
`NotifyUnicastIpAddressChange`).  Changing `bind`, `port` or the certificate restarts the listener only; sessions survive.
`web.settings.set` refuses an empty list while enabled and returns `warnings[]` when a non-loopback address is chosen while the
certificate is the self-signed default or while no user has 2FA enabled.

**Windows firewall.**  The installer does not open any port.  To let the LAN in (only after you chose a non-loopback address):

```
netsh advfirewall firewall add rule name="VM2Backup web" dir=in action=allow protocol=TCP localport=8443 program="%ProgramFiles%\VM2Backup\VM2BackupService.exe" remoteip=LocalSubnet profile=private,domain
netsh advfirewall firewall delete rule name="VM2Backup web"
```

(use your port; `remoteip=` narrows the sources - an administrator's address is better than `LocalSubnet`).

## Users, roles, sessions

* Password: >= 10 characters, not containing the user name; stored as PBKDF2-HMAC-SHA256, >= 300000 iterations (600000 by default),
  16-byte random salt, constant-time comparison.  Unknown users cost the same time as known ones (no enumeration; one message).
* TOTP (RFC 6238, SHA-1, 6 digits, 30 s, window +-1 step, a time step is accepted once - no replay); the secret is DPAPI-protected.
  10 one-time recovery codes (stored hashed).  `twofa`: `off` | `optional` (asked when enrolled) | `required`; the global
  `require_2fa_admins` overrides a personal `off` for admins.  A user who must use 2FA but has not enrolled is refused (after the
  password was right) with an explanation; an administrator enrols users on the Web access page (or through the local channel).
* Roles: `viewer` = state, servers.list, tasks.list, tasks.next, jobs.list, history.list, history.get, backups.list;
  `operator` = viewer + tasks.run, jobs.abort, backup.run, backups.verify; `admin` = everything the service offers (including restore to ESXi / vCenter, `net.drives` / `net.browse` and the site's own `web.*` commands) except
  `service.shutdown`.  Everything else is 403.
* Sessions: 256-bit random ids in memory, idle 20 min, absolute 12 h, cookie `__Host-vm2b; Path=/; Secure; HttpOnly; SameSite=Strict`
  (host-only), a per-session CSRF token sent in `X-CSRF-Token`.  A password / role change, 2FA change or user deletion ends the
  user's sessions at once.  `POST /api/password` changes the own password.
* Rate limiting: per IP and per user name (existing or not): after each failure a growing wait (1, 2, 4 ... 60 s), after 8 failures
  15 minutes; while waiting the attempt is answered `429` + `Retry-After` **without being evaluated**.
* Audit log `audit.log` (rotated at 5 MB) in the data folder: logins (ok / failed / blocked), logout, password and user changes,
  settings / certificate changes, every state-changing API call with user + IP (command and an id / name only - never arguments with
  secrets), refused calls.  `web.audit.tail {lines}` reads it.

## What the server does against abuse

TLS 1.2 minimum and TLS 1.3 (OpenSSL), ECDHE + AEAD suites only (RC4 / 3DES / DES / CBC refused), no
client certificates, renegotiation refused.  HTTP/1.1 parser: keep-alive and pipelining; request line + headers <= 16 KiB, <= 64
headers; bodies only with `Content-Length` <= 8 MiB (chunked / `Transfer-Encoding` -> 501; both CL and TE, duplicate CL / Host,
obsolete line folding, bare LF, NUL, `X : y` -> 400); 15 s to finish the header, 60 s for a body, 10 s idle keep-alive (an idle
connection yields to a waiting one), 10 s for the TLS handshake; 24 worker threads, <= 32 connections per client address.
API: `POST` only, `Content-Type: application/json`, a JSON object body, a same-origin `Origin` / `Sec-Fetch-Site`, the CSRF header.
Static files come from memory (no file system access, so no traversal).  Headers on everything: `Content-Security-Policy:
default-src 'self'; img-src 'self' data:; frame-ancestors 'none'; base-uri 'none'; form-action 'self'`, `X-Content-Type-Options:
nosniff`, `X-Frame-Options: DENY`, `Referrer-Policy: no-referrer`, `Strict-Transport-Security`, COOP / CORP, `Cache-Control`.
The UI is plain JS: no libraries, no inline scripts, no `innerHTML`, nothing fetched from the internet.

## Behind a reverse proxy

The site only speaks HTTPS, so the proxy re-encrypts.  Keep it on `127.0.0.1`, set `trust_proxy: true` (the proxy's address must be
loopback; the **last** `X-Forwarded-For` entry is used as the client address for rate limiting and the audit log), forward `Host`
unchanged (the `Origin` check compares it) and do not strip `Cookie`.  nginx:

```
location / {
    proxy_pass https://127.0.0.1:8443;
    proxy_ssl_verify off;                 # or: proxy_ssl_trusted_certificate with the certificate; its SAN has 127.0.0.1
    proxy_set_header Host $host;
    proxy_set_header X-Forwarded-For $remote_addr;      # overwrite, never append a client-supplied value
    proxy_http_version 1.1; proxy_set_header Connection "";
    client_max_body_size 8m;
}
```

IIS ARR / Caddy / Traefik are the same idea.  The cookie is host-only (`__Host-`), so the proxy must serve the site on its own
host name over HTTPS.

## What is NOT protected / known limits

* **Default administrator:** a fresh installation has the user `admin` with the password `admin`; the site listens on localhost only by default; whoever can reach the site with that password controls the service (and the machine, as SYSTEM) - on a multi-user computer any local process can do it.  Change the password under Web access, Users (or `web.users.save {name:"admin", password}`) and keep the site on localhost, or change the password before choosing a network address.
* **The admin of the site controls the site itself:** its users, certificate, addresses and port (with the lock-out protections above), and with that everything below.
* An **administrator of the web site is SYSTEM-equivalent on the machine**: the service runs as SYSTEM and `backups.restore dest`,
  `servers.save` (directory / local paths), `settings.set default_local_path` etc. accept arbitrary paths.  Give the `admin` role
  only to people who may administer the server; use `operator` / `viewer` otherwise, plus 2FA.
* Someone who can read the data folder as Administrator / SYSTEM can read `web.json` (hashes, the DPAPI-protected secrets can be
  unprotected with the machine key).  DPAPI is machine scope, like the other secrets of VM2Backup: ANY account of the computer can
  decrypt them, so what keeps ordinary users out is the folder's ACL - since build 1.0.299 the data folder is `SYSTEM` + Administrators only
  (protected DACL, set by the service at start, also on an upgrade); only `logs\` stays writable for Users (the panel writes its log there).
  Changing the account the service runs as loses nothing; a standard (non-admin) service account needs full control of the folder (`icacls`).
* Denial of service by a network attacker is only limited (timeouts, caps, 429), not prevented; a per-user lockout lets anybody who
  knows a user name lock that user for 15 minutes (accepted trade-off; the local administrator is never locked out).
* The self-signed certificate is ECDSA P-256 / SHA-256, 825 days; an imported PFX may hold RSA or EC (the key is kept as DER under DPAPI; the old RSA blob format is still read).
* Sessions are in memory: a service restart signs everybody out.  There is one global audit log, no per-user history view.
* Imported PFX: RSA keys of the classic provider only (no ECDSA); only the leaf certificate is served (no intermediates).
* Self-signed certificates teach users to click through warnings: prefer your own CA or a proxy with a trusted certificate.

## Tests

```
ONLY=autotest ./build.sh                       # needs the optional core modules webtls webauth webserver (see below)
tools/run-web-tests.sh [--exe build/x64/vm2bautotest.exe] [--quick]
```

runs (1) the UI logic under node with a fake DOM (`tools/testservers/web_ui_tests.js`: syntax, forbidden constructs, translations,
role table equals the service's, dialogs, role-aware UI, XSS, job panel, the QR generator against libqrencode, the Web access page, the network / folder pickers, the languages), (2) the in-process `web` autotests (RFC 6238 vectors,
policy, login throttling and sessions with a settable clock, certificate, the HTTPS server through our own TLS client, interface
selection incl. an adapter that changes / disappears), (3) the Python clients (`tools/testservers/web_client_tests.py`, python3
stdlib + openssl + curl) over real HTTPS against the service hosted by Wine: TLS floor / ciphers, fingerprint, headers, cookies,
CSRF, expiry, lockout, TOTP, recovery codes, a role x command table, malformed / smuggling / slow-loris requests, concurrency,
secrets never in answers or the audit log, a full login -> tasks.save -> tasks.run -> history -> restore flow, PFX import, curl,
choosing addresses live (127.0.0.1 / 127.0.0.2, IPv6 where available, a busy port, adapters).  Port range 24000-24999.

Not verifiable under Wine: `NotifyIpInterfaceChange` delivery and real
adapters / IPv6 link-local binding, and `SO_EXCLUSIVEADDRUSE` (Wine uses `SO_REUSEADDR` instead).  Those paths follow the Microsoft
documentation but were only compiled, not run, on real Windows.

## Network drives and network browse (service commands for the site's server wizard)

A browser cannot list the network or the drive letters of a user; the service can. Both commands are **admin only** (the role table of the web server must say so).

* `net.drives {}` -> `{drives:[{letter:"Z:", unc:"\\\\server\\share", user:"DOMAIN\\name or empty", owner:"Windows account whose drive it is", source:"profile|service"}]}`.
  Read from `HKEY_USERS\<SID>\Network\<letter>` (RemotePath, UserName) of every loaded profile, plus the service's own remote drives; entries without a UNC path are skipped; no passwords.
* `net.browse {path, user?, password?}` -> `{path, kind:"root|server|share|folder", parent, entries:[{name, path, kind:"server|share|folder", comment?}], truncated, hint?}`.
  `path` `""` = network neighbourhood (can be empty on modern networks: `hint` explains that a server name can be typed; on a member of an Active Directory domain the list also holds the domain controller and the domain's computers with a server operating system, found with `DsGetDcName` + an LDAP search under the service's own account, and the answer carries `domain`; without a domain nothing changes); `\\\\server` = disk shares; `\\\\server\\share[\\sub]` = sub-folders (hidden/system hidden, max 5000, `truncated`).
  `user`/`password` serve only a temporary connection; never stored, logged or returned. Errors are one English line ending with the Windows code: `(1326)` wrong user/password (permanent), `(53)` server not found, `(1460)` no answer within ~15 s or lookups pending (temporary).

### Network picker in the UI

"Add / Edit Server" for a network folder (and a VM folder given as a UNC path) has **Network drive...** (lists `net.drives`: choosing one stores its UNC path, the service cannot see drive letters, and puts the cursor on the user / password fields) and **Browse network...** (computers -> shares -> folders with a breadcrumb; the user and password typed in the form travel in the request body only and are never kept or shown; a slow call can be stopped).  A typed `Z:\folder` is converted through `net.drives` when Next is pressed.

## Folders of the service computer (`fs.browse`)

`fs.browse {path, files?}` (admin only) -> `{path, parent, entries:[{name, path, kind:"drive|dir|file", drive_type?}], truncated}`.  `path` `""` lists the drives; otherwise a local absolute path (`C:\Dir`); UNC paths, `\\?\` / `\\.\` forms, `..`, wildcards, device names and control characters are refused; hidden and system entries are not listed; at most 5000 entries.  The web UI uses it for the "Browse..." buttons of the default folder, the target folder of a backup, the restore destination, the VM folder / local folder of a server and the SCP key file (`files:true`).

## Other service commands used by the site

`tasks.copy {id|name, name?}` (admin) copies a task with new ids and the stored encryption passwords; `tasks.next {id}` or `{schedule}` (a schedule that is being edited) gives the next start; `backups.restore_esxi` / `esxi.restore_options` (admin) restore to ESXi / vCenter - see `docs/ESXI.md`.  The history page builds the HTML report of an entry in the browser (every value escaped) and saves it as a file.

## Защита от блокировки учётной записи (`servers.unblock`, пометка на сервере)

Причина: в домене один вызов `WNetAddConnection2W` с неверным паролем увеличивает `badPwdCount` пользователя на 2 (Kerberos, затем NTLM; измерено на настоящем контроллере домена), а планировщик
перезапускает задачи. Поэтому сервер, который **отказал в учётных данных**, больше не опрашивается без явного действия пользователя.

* Что считается отказом (постоянная ошибка, `Target::auth_failed`): SMB 86, 1326, 1909, 1331, 1330, 1907, 1385, 1327, 1328; FTP 530; SCP: ошибка аутентификации libssh2 (неверный пароль / ключ, истёкший пароль); ESXi / vCenter: `InvalidLogin`;
  источник «папка с ВМ» на сетевой шаре: те же коды SMB. Сеть, таймаут, 1219 (конфликт соединений), 5 (нет прав у верной записи), неизвестный ключ хоста / сертификат отказом **не** считаются и ничего не блокируют.
* Состояние хранится в `state.json` (раздел `_auth_blocks`: `{server_id: {code, text, time, creds_fp}}`), не в `config.json`; переживает перезапуск службы. `creds_fp` = SHA-256 (первые 16 байт) от типа, адреса, порта, пользователя,
  пути, файла ключа и защищённых DPAPI значений секретов; пароль из него получить нельзя, в журнал он не попадает. Любое изменение этих полей (`servers.save`) делает отметку недействительной; переименование сервера - нет.
* Запуск по расписанию (`trigger` `schedule`, в том числе после пропуска) и каждый следующий элемент той же задачи, дошедший до такого сервера, **не обращаются к серверу вообще**: результат `Failed` с `skipped: true`,
  `skip_reason: "credentials"` и текстом «The credentials of server "X" were rejected on <время> (<текст с кодом>); not tried again until the server record is changed or "Try again" is pressed.».
  Внутри одной задачи (в том числе ручной) тот же сервер после первого отказа второй раз не пробуется.
* Явные действия делают ровно одну попытку: ручной запуск (`tasks.run`, `backup.run`), `backups.verify` / `backups.restore`, `servers.test` по `id` («Проверить соединение»). Новый отказ снова ставит отметку, успех снимает.
  `servers.test` с неизвестной записью мастера (без `id`) отметок не трогает. `backups.list` (Проводник копий) с отметкой отвечает ошибкой тем же текстом, не обращаясь к серверу.
* `servers.unblock {id}` (admin, CSRF как у остальных изменяющих команд) снимает отметку без изменения записи («Try again»); ответ `{was_blocked}`; неизвестный сервер - ошибка.
* Видимость: `servers.list` отдаёт `auth_blocked: {code, text, time}` (без пароля); страница «Datacenter» показывает красную пометку «Credentials rejected since <время>: <текст>» и кнопку «Try again» (admin); в списке истории -
  «N skipped: credentials rejected»; в деталях записи и в HTML-отчёте у пропущенного элемента - «Skipped: credentials rejected». Тексты вставляются как текст (экранирование), во всех 16 языковых файлах есть перевод.

## Languages

The page offers 17 languages (login page and header).  `src/web/lang/<code>.txt` holds `English<TAB>translation` per line (`tools/webui_strings.py --update` syncs the keys with `src/web/app.js`, `--check` verifies placeholders `{0}`..., `--missing <code>` lists the untranslated ones); `tools/gen_webui.py` embeds one small script per language (`/lang/<code>.js`), loaded when the language is chosen.  The choice is remembered in the browser (`localStorage`); the first visit follows the browser language; an untranslated text is shown in English.
