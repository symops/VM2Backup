// Автотесты сетевой папки (SMB) против НАСТОЯЩЕГО домена Active Directory (группы "domainreal" и "domainlock"): Kerberos и NTLM доменных учётных записей,
// три формы имени пользователя, три формы адреса сервера, ошибки входа (1326 / 1909 / 1907 / 1330 / 1331 / 5), число попыток входа при неверном пароле
// (в домене повтор = блокировка пользователя), обзор сети.  Нужен ключ "domain" в --servers (иначе каждый тест пропускается): стенд .winvm/dom
// (docs/WINDOWS-TESTS.md, "Домен AD") - контроллер vm2b-dc и рядовой сервер vm2b-member, тесты идут на member.
//   "domain": { "unc_fqdn": "\\\\dc.vm2b.test\\Backups", "unc_short": "\\\\dc\\Backups", "unc_ip": "\\\\10.77.0.1\\Backups",
//               "restricted_unc": "\\\\dc.vm2b.test\\Restricted",                 (шара, где у bkuser прав нет)
//               "user_netbios": "VM2B\\bkuser", "user_upn": "bkuser@vm2b.test", "user_short": "bkuser", "password": "...", "wrong_password": "...",
//               "nouser_name": "VM2B\\nouser", "nouser_password": "...",         (верный пароль, но прав на шару нет -> 5)
//               "locked_user": "VM2B\\lockuser", "locked_password": "...",       (политика: блокировка после 5 неверных попыток; группа domainlock)
//               "mustchange_user", "mustchange_password", "expired_user", "expired_password", "disabled_user", "disabled_password",
//               "badpwd_script": "C:\\vm2b\\guest\\domain-badpwd.ps1" }          (печатает badPwdCount пользователя с контроллера домена; необязателен)
// Под Wine SMB-клиента нет: тесты пропускаются (в servers.json нет "domain").
#include "autotest.h"
#include "engine.h"
#include "config.h"
#include "smb.h"
#include "netbrowse.h"
#include "tests_svc.h"
#include "schedule.h"
#include <winsock2.h>
#include <stdio.h>

namespace vm2b {
namespace {

struct DomCfg {
    Str fqdn, shortUnc, ip, restricted, uNet, uUpn, uShort, pw, wrongPw, noUser, noPw, lockUser, lockPw, chgUser, chgPw, expUser, expPw, disUser, disPw, badpwd;
    bool ok;
    explicit DomCfg(Ctx& c) : ok(false) {
        const Json& s = c.servers.get("domain");
        if (!s.is_object()) return;
        fqdn = s.get("unc_fqdn").str(); shortUnc = s.get("unc_short").str(); ip = s.get("unc_ip").str(); restricted = s.get("restricted_unc").str();
        uNet = s.get("user_netbios").str(); uUpn = s.get("user_upn").str(); uShort = s.get("user_short").str(); pw = s.get("password").str(); wrongPw = s.get("wrong_password").str("not-the-password-1");
        noUser = s.get("nouser_name").str(); noPw = s.get("nouser_password").str(); lockUser = s.get("locked_user").str(); lockPw = s.get("locked_password").str();
        chgUser = s.get("mustchange_user").str(); chgPw = s.get("mustchange_password").str(); expUser = s.get("expired_user").str(); expPw = s.get("expired_password").str();
        disUser = s.get("disabled_user").str(); disPw = s.get("disabled_password").str(); badpwd = s.get("badpwd_script").str();
        ok = !fqdn.empty() && !uNet.empty() && !pw.empty();
    }
    bool leaks(const Str& t) const {
        const Str* all[] = { &pw, &wrongPw, &noPw, &lockPw, &chgPw, &expPw, &disPw };
        for (const Str* p : all) if (p->size() >= 4 && t.find(p->c_str()) >= 0) return true;
        return false;
    }
};

Json SmbJson(const Str& unc, const Str& user, const Str& pw, const Str& dir)
{
    Json j = Json::object(); j["type"] = "smb"; j["path"] = unc; j["user"] = user; j["password"] = pw; if (!dir.empty()) j["directory"] = dir; return j;
}

Str Unique(Ctx& c, const char* tag) { return Str::fmt("%s-%lu-%llu", tag, (unsigned long)GetCurrentProcessId(), (unsigned long long)(c.next() % 1000000)); }

Str Stamp() { SYSTEMTIME t; GetLocalTime(&t); return Str::fmt("%02d:%02d:%02d.%03d", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds); }

// Считает настоящие обращения к WNetAddConnection2W (каждое = одна попытка входа в домене; при неверном пароле каждая увеличивает badPwdCount).
volatile LONG g_adds = 0; volatile LONG g_lastRc = 0;
unsigned long CountingAdd(const wchar_t* remote, const wchar_t* user, const wchar_t* pass)
{
    InterlockedIncrement(&g_adds);
    HMODULE h = LoadLibraryW(L"mpr.dll"); if (!h) return ERROR_NO_NETWORK;
    typedef DWORD (WINAPI *AddFn)(LPNETRESOURCEW, LPCWSTR, LPCWSTR, DWORD);
    AddFn f = (AddFn)(void*)GetProcAddress(h, "WNetAddConnection2W"); if (!f) return ERROR_NO_NETWORK;
    NETRESOURCEW nr; memset(&nr, 0, sizeof(nr)); nr.dwType = RESOURCETYPE_DISK; nr.lpRemoteName = (LPWSTR)remote;
    DWORD rc = f(&nr, pass, user, CONNECT_TEMPORARY); g_lastRc = (LONG)rc; return rc;
}
unsigned long CountingCancel(const wchar_t* remote, int force)
{
    HMODULE h = LoadLibraryW(L"mpr.dll"); if (!h) return ERROR_NO_NETWORK;
    typedef DWORD (WINAPI *CancelFn)(LPCWSTR, DWORD, BOOL);
    CancelFn f = (CancelFn)(void*)GetProcAddress(h, "WNetCancelConnection2W"); if (!f) return ERROR_NO_NETWORK;
    return f(remote, 0, force ? TRUE : FALSE);
}
const SmbApi kCountApi = { CountingAdd, CountingCancel, nullptr, nullptr };
struct CountScope { CountScope() { g_adds = 0; SmbSetApi(&kCountApi); } ~CountScope() { SmbSetApi(nullptr); } int count() const { return (int)g_adds; } };

// badPwdCount de l'utilisateur с контроллера (скрипт гостя); -1 = не удалось
int BadPwd(const DomCfg& d, const Str& user)
{
    if (d.badpwd.empty()) return -1;
    Str cmd = Str("powershell.exe -NoProfile -ExecutionPolicy Bypass -File \"") + d.badpwd + "\" \"" + user + "\" 2>&1";
    FILE* f = _popen(cmd.c_str(), "r"); if (!f) return -1;
    char buf[512]; Str out; size_t n; while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    _pclose(f);
    int p = out.find("BADPWD="); return p >= 0 ? atoi(out.c_str() + p + 7) : -1;
}

// Копия, папка, чтение, список, удаление через цель; возвращает причину сбоя или пустую строку.
Str Roundtrip(Ctx& c, const DomCfg& d, const Str& unc, const Str& user, const Str& pw, const Str& dir)
{
    Str e; Target* t = MakeTarget(SmbJson(unc, user, pw, dir), e); if (!t) return "MakeTarget: " + e;
    Str why;
    Bytes data = Random(c, (size_t)c.range(2000, 90000));
    if (!t->connect()) why = "connect: " + t->err;
    else if (!t->put_bytes("probe.bin", data.data(), data.size())) why = "put: " + t->err;
    else {
        Bytes got; Vec<Str> names;
        if (!t->get_bytes("probe.bin", got) || got.size() != data.size() || memcmp(got.data(), data.data(), got.size()) != 0) why = "get: " + t->err;
        else if (!t->list(names) || names.empty()) why = "list: " + t->err;
        else if (!t->remove("probe.bin")) why = "remove: " + t->err;
    }
    delete t;
    return why;
}

// Резервная копия + проверка + восстановление ВМ через цель (как в smbreal).
Str BackupRestore(Ctx& c, const DomCfg& d, const Str& unc, const Str& user, const Str& pw, const Str& dir, Tmp& tmp, const char* tag)
{
    Str work = tmp.sub(tag), vms = JoinPath(work, "vms"), vm = "vm-dom";
    Bytes b = Random(c, (size_t)c.range(300000, 900000)); WriteBytes(JoinPath(JoinPath(vms, vm), "disk.vmdk"), b);
    Str e; Target* t = MakeTarget(SmbJson(unc, user, pw, dir), e); if (!t) return "MakeTarget: " + e;
    BackupOptions o; o.kdfIterations = 3000; o.codec.level = 1; o.segmentBytes = 512 * 120; o.verify = true; o.retry.attempts = 6; o.retry.baseDelayMs = 200; o.retry.maxDelayMs = 1000;
    VmSource* src = NewDirectorySource(vms); Manifest m; Str warn, err;
    bool ok = BackupVm(*src, vm, *t, o, nullptr, m, warn, err); delete src;
    if (!ok) { delete t; return "backup: " + err; }
    Str ve; if (!VerifyBackup(*t, m.archive, o.password, nullptr, nullptr, ve)) { delete t; return "verify: " + ve; }
    Str dest = JoinPath(work, "restored"); Manifest rm; Str re;
    if (!RestoreBackup(*t, m.archive, dest, o.password, nullptr, &rm, re)) { delete t; return "restore: " + re; }
    Bytes got; Str ge; if (!ReadWholeFile(JoinPath(JoinPath(dest, vm), "disk.vmdk").c_str(), got, ge) || got.size() != b.size() || memcmp(got.data(), b.data(), b.size()) != 0) { delete t; return "restored data differs"; }
    delete t; return Str();
}

} // namespace

#define NEED_DOMAIN() DomCfg dc(c_); if (!dc.ok) { c_.skip("no \"domain\" in --servers (a real Active Directory domain is needed)"); return true; }

VM2B_TEST("domainreal", domain_user_forms_and_server_names)
{
    // три формы имени пользователя x три формы адреса сервера: вход, запись, чтение, список, удаление; потом проверка, что соединений не осталось
    NEED_DOMAIN();
    const Str uncs[3] = { dc.fqdn, dc.shortUnc, dc.ip }; const char* un[3] = { "FQDN", "short", "IP" };
    const Str users[3] = { dc.uNet, dc.uUpn, dc.uShort }; const char* fn[3] = { "VM2B\\user", "user@vm2b.test", "user" };
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) {
        if (uncs[i].empty() || users[j].empty()) continue;
        Str t0 = Stamp(); Str why = Roundtrip(c_, dc, uncs[i], users[j], dc.pw, Unique(c_, "forms"));
        c_.note("[%s] server %-5s user %-15s -> %s", t0.c_str(), un[i], fn[j], why.empty() ? "OK" : why.c_str());
        T_CHECK(why.empty()); T_CHECK(!dc.leaks(why));
        T_CHECK(SmbOpenConnections() == 0);
    }
    return true;
}

VM2B_TEST("domainreal", domain_backup_verify_restore_by_name_form)
{
    NEED_DOMAIN();
    Tmp tmp(c_, "domrt");
    struct C { const Str* unc; const Str* user; const char* tag; } cs[3] = { { &dc.fqdn, &dc.uUpn, "fqdn-upn" }, { &dc.shortUnc, &dc.uNet, "short-netbios" }, { &dc.ip, &dc.uShort, "ip-short" } };
    for (const C& x : cs) {
        if (x.unc->empty() || x.user->empty()) continue;
        Str why = BackupRestore(c_, dc, *x.unc, *x.user, dc.pw, Unique(c_, "rt"), tmp, x.tag);
        c_.note("backup + verify + restore %s: %s", x.tag, why.empty() ? "OK" : why.c_str());
        T_CHECK(why.empty()); T_CHECK(SmbOpenConnections() == 0);
    }
    return true;
}

VM2B_TEST("domainreal", domain_no_rights_user_is_access_denied_5)
{
    NEED_DOMAIN();
    if (dc.noUser.empty()) { c_.skip("no nouser_name"); return true; }
    // пользователь есть, пароль верный, прав на шару нет -> отказ 5 (постоянный), текст без пароля; у bkuser прав на Restricted тоже нет
    struct K { Str unc, user, pw; const char* what; } ks[2] = { { dc.fqdn, dc.noUser, dc.noPw, "nouser on Backups" }, { dc.restricted, dc.uNet, dc.pw, "bkuser on Restricted" } };
    for (const K& k : ks) {
        if (k.unc.empty()) continue;
        Str e; Target* t = MakeTarget(SmbJson(k.unc, k.user, k.pw, ""), e); T_CHECK(t != nullptr); if (!t) return false;
        bool ok = t->connect();
        c_.note("%s: ok=%d transient=%d code=%lu text=[%s]", k.what, ok ? 1 : 0, t->transient ? 1 : 0, SmbErrorCodeOf(t->err), t->err.c_str());
        T_CHECK(!ok); T_CHECK(!t->transient); T_CHECK(SmbErrorCodeOf(t->err) == 5); T_CHECK(!dc.leaks(t->err));
        delete t; T_CHECK(SmbOpenConnections() == 0);
    }
    return true;
}

VM2B_TEST("domainreal", domain_account_states_give_clear_permanent_errors)
{
    NEED_DOMAIN();
    struct K { Str user, pw; const char* what; unsigned long code; const char* words; } ks[3] = {
        { dc.chgUser, dc.chgPw, "password must be changed (1907)", 1907, "change" },
        { dc.expUser, dc.expPw, "password expired (1330)", 1330, "expire" },
        { dc.disUser, dc.disPw, "account disabled (1331)", 1331, "disabled" } };
    for (const K& k : ks) {
        if (k.user.empty()) continue;
        Str e; Target* t = MakeTarget(SmbJson(dc.fqdn, k.user, k.pw, ""), e); T_CHECK(t != nullptr); if (!t) return false;
        bool ok = t->connect(); unsigned long code = SmbErrorCodeOf(t->err);
        c_.note("%s: ok=%d transient=%d code=%lu text=[%s]", k.what, ok ? 1 : 0, t->transient ? 1 : 0, code, t->err.c_str());
        T_CHECK(!ok); T_CHECK(!t->transient); T_CHECK(!dc.leaks(t->err));
        // Windows может ответить и общим 1326 («неверное имя или пароль»): тогда текст верен, но подсказки нет - это тоже записываем, но НЕ считаем ошибкой продукта
        if (code == 1907 || code == 1330) T_CHECK(t->err.lower().find("password") >= 0 && t->err.lower().find("changed") + t->err.lower().find("expired") > -2 && t->err.find("\xd0") < 0 && t->err.find("\xd1") < 0);   // понятный английский текст, не системный русский
        else c_.note("   Windows returned %lu instead of %lu", code, k.code);
        T_CHECK(code != 0);
        T_CHECK(t->err.find("Win32") < 0 || code != k.code);      // не «голый» код без объяснения
        delete t; T_CHECK(SmbOpenConnections() == 0);
    }
    return true;
}

VM2B_TEST("domainlock", domain_wrong_password_one_attempt_only)
{
    // Неверный пароль в домене: ОДНО обращение к серверу на операцию (повтор = лишний badPwdCount и блокировка пользователя по политике).
    NEED_DOMAIN();
    struct K { Str unc, user; const char* what; } ks[3] = { { dc.fqdn, dc.uUpn, "FQDN + UPN" }, { dc.shortUnc, dc.uNet, "short + NETBIOS" }, { dc.ip, dc.uShort, "IP + short" } };
    for (const K& k : ks) {
        if (k.unc.empty() || k.user.empty()) continue;
        { Str e; Target* t = MakeTarget(SmbJson(k.unc, k.user, dc.pw, ""), e); T_CHECK(t && t->connect()); delete t; }      // верный вход сбрасывает badPwdCount
        int bad0 = BadPwd(dc, dc.uShort);
        int adds;
        { CountScope cs; Str e; Target* t = MakeTarget(SmbJson(k.unc, k.user, dc.wrongPw, ""), e); T_CHECK(t != nullptr); if (!t) return false;
          bool ok = t->connect(); unsigned long code = SmbErrorCodeOf(t->err);
          c_.note("%s, connect() with the wrong password: ok=%d transient=%d code=%lu text=[%s]", k.what, ok ? 1 : 0, t->transient ? 1 : 0, code, t->err.c_str());
          T_CHECK(!ok); T_CHECK(!t->transient); T_CHECK(code == 1326); T_CHECK(!dc.leaks(t->err));
          T_CHECK(t->open_write("x") == nullptr); T_CHECK(!t->transient);          // следующая операция той же цели
          delete t; adds = cs.count(); }
        int bad1 = BadPwd(dc, dc.uShort);
        c_.note("%s: WNetAddConnection2 calls = %d (connect + open_write), badPwdCount on the DC %d -> %d", k.what, adds, bad0, bad1);
        T_CHECK(adds <= 2);
        // Windows сам делает ДВЕ попытки на одно обращение к WNetAddConnection2 (Kerberos, потом NTLM): badPwdCount растёт на 2 за обращение - это записываем
        if (bad0 >= 0 && bad1 >= 0) T_CHECK(bad1 - bad0 <= 2 * adds);
        T_CHECK(SmbOpenConnections() == 0);
    }
    return true;
}

VM2B_TEST("domainlock", domain_backup_with_wrong_password_does_not_hammer_the_server)
{
    // Целая резервная копия с неверным паролем и «политикой повторов» движка (6 попыток): число обращений к серверу входа - не больше трёх.
    NEED_DOMAIN();
    Tmp tmp(c_, "domhammer");
    { Str e; Target* t = MakeTarget(SmbJson(dc.fqdn, dc.uUpn, dc.pw, ""), e); T_CHECK(t && t->connect()); delete t; }
    int bad0 = BadPwd(dc, dc.uShort), adds; Str why;
    { CountScope cs; why = BackupRestore(c_, dc, dc.fqdn, dc.uUpn, dc.wrongPw, Unique(c_, "hm"), tmp, "w"); adds = cs.count(); }
    int bad1 = BadPwd(dc, dc.uShort);
    c_.note("backup with the wrong password: [%s]; logon calls %d; badPwdCount %d -> %d", why.c_str(), adds, bad0, bad1);
    T_CHECK(!why.empty()); T_CHECK(!dc.leaks(why)); T_CHECK(why.lower().find("password") >= 0);
    T_CHECK(adds <= 3);
    T_CHECK(SmbOpenConnections() == 0);
    return true;
}

VM2B_TEST("domainlock", domain_scheduled_task_with_wrong_password_is_tried_once)
{
    // Защита от блокировки (служба в этом же процессе, настоящий вход в домен): запись сервера с НЕВЕРНЫМ паролем lockuser (порог блокировки 5) + задача по расписанию
    // с быстрыми тиками (поддельные часы).  После N запусков планировщика badPwdCount вырос только на то, что стоит ОДНО обращение (Kerberos + NTLM = 2),
    // учётная запись не заблокирована; после смены пароля в записи сервера задача снова работает.
    NEED_DOMAIN();
    if (dc.lockUser.empty() || dc.lockPw.empty()) { c_.skip("no locked_user"); return true; }
    const Str lockShort = dc.lockUser.substr(dc.lockUser.rfind('\\') + 1);
    { Str e; Target* t = MakeTarget(SmbJson(dc.fqdn, dc.lockUser, dc.lockPw, ""), e); bool ok = t && t->connect(); if (!ok && t) c_.note("lockuser first logon: %s", t->err.c_str()); T_CHECK(ok); delete t; }   // сбрасывает badPwdCount
    CountScope cs;
    Svc s(c_, "domag", true); if (!s.up) return false; s.record = true;
    Str vms = s.tmp.sub("vms"); WriteBytes(JoinPath(vms, "vm1\\disk.vmdk"), Random(c_, 80000)); Str src = s.addDirServer(vms);
    Json sv = Json::object(); sv["type"] = "smb"; sv["name"] = "Domain share"; sv["path"] = dc.fqdn; sv["user"] = dc.lockUser; sv["password"] = dc.wrongPw;
    Json sa = Json::object(); sa["server"] = sv; Str id = s.ok("servers.save", sa).get("id").str(); T_CHECK(!id.empty());
    g_fakeNow = (i64)UnixNow() + 3600; i64 t0 = g_fakeNow;
    Json sch = Json::object(); sch["enabled"] = true; sch["mode"] = "every"; Json ev = Json::object(); ev["value"] = 10; ev["unit"] = "minutes"; sch["every"] = ev;
    sch["first_time"] = FormatLocalMin(UnixToLocalMin(t0)).replace_all(" ", "T");
    Json el = Svc::Element(src, "vm1", "{VM}"); Json tg = Json::object(); tg["server"] = id; tg["directory"] = Unique(c_, "dlk") + "\\{VM}"; el["target"] = tg; el["verify"] = false;
    Str task = s.saveTask("Domain wrong password", el, sch); T_CHECK(!task.empty());
    int bad0 = BadPwd(dc, lockShort);          // до включения планировщика: первый запуск может случиться сразу
    s.ok("scheduler.set", [] { Json a = Json::object(); a["enabled"] = true; return a; }()); Sleep(300);
    const int N = 8;
    for (int i = 0; i < N; ++i) { int h = s.historyCount(); g_fakeNow += 10 * 60; T_CHECK(s.waitHistory(h + 1, 30000)); }
    int bad1 = BadPwd(dc, lockShort);
    c_.note("%d scheduler runs with the wrong password: logon calls %d, badPwdCount of %s on the DC %d -> %d", N, cs.count(), lockShort.c_str(), bad0, bad1);
    T_CHECK(cs.count() == 1);
    if (bad0 >= 0 && bad1 >= 0) { T_CHECK(bad1 - bad0 >= 1 && bad1 - bad0 <= 2); T_CHECK(bad1 < 5); }   // одно обращение = Kerberos (+ NTLM) = +1..+2 (на lockuser измерено +1, на bkuser +2; раньше N запусков дали бы блокировку)
    Json list = s.ok("servers.list", Json::object()); Json mark;
    for (size_t i = 0; i < list.size(); ++i) if (list.at(i).get("id").str() == id) mark = list.at(i).get("auth_blocked");
    T_CHECK(mark.is_object()); T_CHECK(mark.get("code").integer() == 1326);
    Json hl = s.ok("history.list", [] { Json a = Json::object(); a["limit"] = 1; return a; }());
    if (hl.size()) { Json ha = Json::object(); ha["id"] = hl.at(0).get("id").str(); Json rec = s.ok("history.get", ha); T_CHECK(rec.get("results").at(0).get("skipped").boolean()); c_.note("last run: %s", rec.get("results").at(0).get("message").str().c_str()); }
    // учётная запись цела: верный пароль входит (и сбрасывает счётчик)
    { Str e; Target* t = MakeTarget(SmbJson(dc.fqdn, dc.lockUser, dc.lockPw, ""), e); bool ok = t && t->connect(); if (!ok && t) c_.note("lockuser after the series: %s", t->err.c_str()); T_CHECK(ok); delete t; }
    // новый пароль в записи сервера снимает блокировку: следующий запуск доходит до сервера и копирует
    sv["id"] = id; sv["password"] = dc.lockPw; Json sb = Json::object(); sb["server"] = sv; s.ok("servers.save", sb);
    int callsBefore = cs.count(); { int h = s.historyCount(); g_fakeNow += 10 * 60; T_CHECK(s.waitHistory(h + 1, 120000)); }
    Json hl2 = s.ok("history.list", [] { Json a = Json::object(); a["limit"] = 1; return a; }());
    c_.note("after the new password: logon calls +%d, last result %s", cs.count() - callsBefore, hl2.size() ? hl2.at(0).get("status").str().c_str() : "?");
    T_CHECK(cs.count() > callsBefore); T_CHECK(hl2.size() && hl2.at(0).get("status").str() == "Success");
    T_CHECK(!dc.leaks(s.transcript)); T_CHECK(s.transcript.find("auth_blocked") >= 0);
    Json off = Json::object(); off["enabled"] = false; s.ok("scheduler.set", off);
    return true;
}

VM2B_TEST("domainlock", domain_lockout_policy_gives_1909_without_retries)
{
    // lockuser: политика блокирует после 5 неверных попыток.  Неверные пароли - вручную (по одной попытке), потом с верным паролем: 1909 и ни одного повтора.
    // Разблокировку делает скрипт гостя после прогона (Unlock-ADAccount): тест оставляет учётную запись заблокированной.
    NEED_DOMAIN();
    if (dc.lockUser.empty()) { c_.skip("no locked_user"); return true; }
    { Str e; Target* t = MakeTarget(SmbJson(dc.fqdn, dc.lockUser, dc.lockPw, ""), e); bool ok = t && t->connect(); if (!ok && t) c_.note("lockuser first logon: %s", t->err.c_str()); T_CHECK(ok); delete t; }
    // одна неудачная операция «резервной копии» с неверным паролем не блокирует пользователя (меньше порога): после неё верный пароль работает
    { Tmp tmp(c_, "domlock"); CountScope cs; Str why = BackupRestore(c_, dc, dc.fqdn, dc.lockUser, dc.wrongPw, Unique(c_, "lk"), tmp, "w");
      c_.note("one failed backup of lockuser: [%s], logon calls %d", why.c_str(), cs.count()); T_CHECK(!why.empty()); T_CHECK(cs.count() <= 3); }
    { Str e; Target* t = MakeTarget(SmbJson(dc.fqdn, dc.lockUser, dc.lockPw, ""), e); bool ok = t && t->connect(); if (!ok && t) c_.note("after one failed backup the right password: %s", t->err.c_str()); T_CHECK(ok); delete t; }
    int attempts = 0; bool locked = false; unsigned long lastCode = 0; Str lastText;
    for (int i = 0; i < 12 && !locked; ++i) {
        Str e; Target* t = MakeTarget(SmbJson(dc.fqdn, dc.lockUser, dc.wrongPw, ""), e); T_CHECK(t != nullptr); if (!t) return false;
        bool ok = t->connect(); ++attempts; lastCode = SmbErrorCodeOf(t->err); lastText = t->err; T_CHECK(!ok); T_CHECK(!t->transient);
        if (lastCode == 1909) locked = true; delete t;
    }
    c_.note("manual wrong attempts until the lockout: %d, last code %lu [%s]", attempts, lastCode, lastText.c_str());
    T_CHECK(locked);
    // заблокирована: верный пароль даёт 1909, постоянная ошибка, текст понятный, число обращений - одно на операцию
    CountScope cs; Str e; Target* t = MakeTarget(SmbJson(dc.fqdn, dc.lockUser, dc.lockPw, ""), e); T_CHECK(t != nullptr); if (!t) return false;
    bool ok = t->connect(); unsigned long code = SmbErrorCodeOf(t->err);
    c_.note("locked-out account with the RIGHT password: ok=%d transient=%d code=%lu text=[%s] calls=%d", ok ? 1 : 0, t->transient ? 1 : 0, code, t->err.c_str(), cs.count());
    T_CHECK(!ok); T_CHECK(!t->transient); T_CHECK(code == 1909); T_CHECK(t->err.find("locked") >= 0); T_CHECK(!dc.leaks(t->err)); T_CHECK(cs.count() <= 1);
    delete t;
    Tmp tmp2(c_, "domlock2"); int before = cs.count();
    Str why = BackupRestore(c_, dc, dc.fqdn, dc.lockUser, dc.lockPw, Unique(c_, "lk2"), tmp2, "x");
    c_.note("backup to a locked-out account: [%s], logon calls %d", why.c_str(), cs.count() - before);
    T_CHECK(!why.empty()); T_CHECK(cs.count() - before <= 3);
    return true;
}

VM2B_TEST("domainreal", domain_empty_user_uses_the_process_account)
{
    // пустой пользователь: права учётной записи, под которой работает ПРОЦЕСС (тест = локальный администратор member: домен его не знает -> отказ,
    // а не вход под чужой записью). То же под LocalSystem / службой делает скрипт domain-svc.ps1.
    NEED_DOMAIN();
    Str e; Target* t = MakeTarget(SmbJson(dc.fqdn, "", "", ""), e); T_CHECK(t != nullptr); if (!t) return false;
    bool ok = t->connect(); c_.note("empty user as the test process: ok=%d transient=%d code=%lu text=[%s]", ok ? 1 : 0, t->transient ? 1 : 0, SmbErrorCodeOf(t->err), t->err.c_str());
    if (!ok) { T_CHECK(!t->transient); T_CHECK(t->err.size() > 0); T_CHECK(!dc.leaks(t->err)); }
    delete t; T_CHECK(SmbOpenConnections() == 0);
    return true;
}

VM2B_TEST("domainreal", domain_net_browse_server_share_and_network)
{
    NEED_DOMAIN();
    int cut = dc.fqdn.rfind('\\'); Str server = dc.fqdn.substr(0, (size_t)cut), share = dc.fqdn.substr((size_t)cut + 1);
    const Str servers[3] = { server, "\\\\" + dc.shortUnc.substr(2, dc.shortUnc.rfind('\\') - 2), dc.ip.substr(0, (size_t)dc.ip.rfind('\\')) };
    const char* nm[3] = { "FQDN", "short", "IP" };
    for (int i = 0; i < 3; ++i) {
        if (servers[i].size() < 4) continue;
        Json r; Str e; bool ok = NetBrowse(servers[i], dc.uUpn, dc.pw, r, e);
        c_.note("net.browse %s (%s): %s, %d entries", servers[i].c_str(), nm[i], ok ? "ok" : e.c_str(), ok ? (int)r.get("entries").size() : 0);
        T_CHECK(ok); if (!ok) continue;
        bool sh = false; for (size_t k = 0; k < r.get("entries").size(); ++k) if (r.get("entries").at(k).get("name").str().lower() == share.lower()) sh = true;
        T_CHECK(sh);
        Json r2; Str e2; bool ok2 = NetBrowse(servers[i] + "\\" + share, dc.uUpn, dc.pw, r2, e2); c_.note("net.browse share of %s: %s", nm[i], ok2 ? "ok" : e2.c_str()); T_CHECK(ok2);
    }
    Json rw; Str ew; bool okw = NetBrowse(server, dc.uUpn, dc.wrongPw, rw, ew);
    c_.note("net.browse with the wrong password: ok=%d [%s]", okw ? 1 : 0, ew.c_str()); T_CHECK(!okw); T_CHECK(ew.find("password") >= 0); T_CHECK(!dc.leaks(ew));
    // корень сетевого окружения: находит ли он контроллер и его шары (на домене без NetBIOS-обзора может быть пусто) - только запись
    Json r4; Str e4; bool ok4 = NetBrowse("", "", "", r4, e4);
    Str names; for (size_t k = 0; k < r4.get("entries").size(); ++k) { names += r4.get("entries").at(k).get("name").str(); names += " "; }
    c_.note("network root: ok=%d entries=%d [%s] %s", ok4 ? 1 : 0, (int)r4.get("entries").size(), names.c_str(), e4.c_str());
    Json dr; Str de; bool okd = NetListDrives(dr, de); c_.note("net.drives: ok=%d drives=%d %s", okd ? 1 : 0, okd ? (int)dr.get("drives").size() : 0, de.c_str()); T_CHECK(okd);
    return true;
}

} // namespace vm2b
