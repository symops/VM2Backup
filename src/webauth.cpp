// Authentication, sessions, rate limiting, roles, audit log of the administration web site, see webauth.h.
#include "webauth.h"
#include "crypto.h"
#include "config.h"
#include "winutil.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <time.h>

namespace vm2b {

namespace {

const int kMaxSessions = 2000;
const int kAbsoluteSeconds = 12 * 3600;
const int kLockSeconds = 900;                 // lockout after kLockAfter failures, and the memory of old failures
const int kLockAfter = 8;

struct Guard { CRITICAL_SECTION* c; explicit Guard(CRITICAL_SECTION* x) : c(x) { EnterCriticalSection(c); } ~Guard() { LeaveCriticalSection(c); } };
struct Unguard { CRITICAL_SECTION* c; explicit Unguard(CRITICAL_SECTION* x) : c(x) { LeaveCriticalSection(c); } ~Unguard() { EnterCriticalSection(c); } };

bool CtEq(const void* a, const void* b, size_t n)
{
    const u8* x = (const u8*)a; const u8* y = (const u8*)b; u8 d = 0;
    for (size_t i = 0; i < n; ++i) d |= (u8)(x[i] ^ y[i]);
    return d == 0;
}
bool CtEqStr(const Str& a, const Str& b) { return a.size() == b.size() && CtEq(a.data(), b.data(), a.size()); }   // lengths are fixed for tokens

Str Lower(const Str& s) { return s.lower(); }

bool NameOk(const Str& n)
{
    if (n.empty() || n.size() > 64) return false;
    for (size_t i = 0; i < n.size(); ++i) { char c = n[i]; if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '_' || c == '@' || c == '-')) return false; }
    return true;
}

Str Sanitize(const Str& s, size_t cap = 200)
{
    Str o;
    for (size_t i = 0; i < s.size() && i < cap; ++i) { unsigned char c = (unsigned char)s[i]; o += (c < 0x20 || c == 0x7f) ? ' ' : (char)c; }
    return o;
}

void HmacSha1(const void* key, size_t klen, const void* data, size_t n, u8 out[20])
{
    u8 k[64]; memset(k, 0, 64);
    if (klen > 64) Sha1(key, klen, k); else memcpy(k, key, klen);
    Bytes in; in.resize(64 + n);
    for (int i = 0; i < 64; ++i) in[i] = k[i] ^ 0x36;
    if (n) memcpy(in.data() + 64, data, n);
    u8 inner[20]; Sha1(in.data(), in.size(), inner);
    u8 o2[64 + 20];
    for (int i = 0; i < 64; ++i) o2[i] = k[i] ^ 0x5c;
    memcpy(o2 + 64, inner, 20);
    Sha1(o2, sizeof(o2), out);
}

Str RandomHex(size_t bytes) { Bytes b; b.resize(bytes); RandomBytes(b.data(), bytes); return HexEncode(b.data(), bytes); }

const char* const kAdminCmds[] = { "state", "scheduler.set", "settings.get", "settings.set", "servers.list", "servers.save", "servers.delete", "servers.unblock", "servers.test", "servers.vms",
    "tasks.list", "tasks.save", "tasks.copy", "tasks.delete", "tasks.run", "tasks.next", "backup.run", "jobs.list", "jobs.abort", "history.list", "history.get", "history.clear",
    "backups.list", "backups.verify", "backups.restore", "backups.restore_esxi", "esxi.restore_options", "net.drives", "net.browse", "fs.browse", "backups.delete", "resumable.discard",
    "web.status", "web.settings.get", "web.settings.set", "web.interfaces.list", "web.cert.generate", "web.cert.import", "web.users.list", "web.users.save", "web.users.delete",
    "web.users.totp_enroll", "web.users.totp_confirm", "web.users.totp_disable", "web.audit.tail", nullptr };
const char* const kViewerCmds[] = { "state", "servers.list", "tasks.list", "tasks.next", "jobs.list", "history.list", "history.get", "backups.list", nullptr };
const char* const kOperatorExtra[] = { "tasks.run", "jobs.abort", "backup.run", "backups.verify", nullptr };
const char* const kReadOnly[] = { "state", "settings.get", "servers.list", "tasks.list", "tasks.next", "jobs.list", "history.list", "history.get", "backups.list", "web.status", "web.settings.get", "web.interfaces.list", "web.users.list", "web.audit.tail", nullptr };
bool In(const char* const* l, const Str& c) { for (; *l; ++l) if (c == *l) return true; return false; }

} // namespace

// ---- helpers -----------------------------------------------------------------------------------------------------------------
bool WebAuth::RoleAllows(const Str& role, const Str& cmd)
{
    if (role == "admin") return In(kAdminCmds, cmd);             // service.shutdown is not in the list: never over the web
    if (role == "operator") return In(kViewerCmds, cmd) || In(kOperatorExtra, cmd);
    if (role == "viewer") return In(kViewerCmds, cmd);
    return false;
}
bool WebAuth::IsReadOnlyCmd(const Str& cmd) { return In(kReadOnly, cmd); }

bool WebAuth::PasswordOk(const Str& user, const Str& pw, Str& why)
{
    if (pw.size() < 10) { why = "The password must be at least 10 characters long."; return false; }
    if (pw.size() > 256) { why = "The password is too long (256 characters at most)."; return false; }
    if (!Utf8Valid(pw.data(), pw.size())) { why = "The password must be valid UTF-8 text."; return false; }
    Str lp = Lower(pw), lu = Lower(user);
    if (!lu.empty() && (lp == lu || (lu.size() >= 4 && lp.find(lu.c_str()) >= 0))) { why = "The password must not contain the user name."; return false; }
    bool same = true; for (size_t i = 1; i < pw.size(); ++i) if (pw[i] != pw[0]) same = false;
    if (same) { why = "The password is too simple."; return false; }
    return true;
}

Str WebAuth::Base32(const Bytes& b)
{
    static const char* A = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
    Str o; u32 buf = 0; int bits = 0;
    for (size_t i = 0; i < b.size(); ++i) { buf = (buf << 8) | b[i]; bits += 8; while (bits >= 5) { o += A[(buf >> (bits - 5)) & 31]; bits -= 5; } }
    if (bits) o += A[(buf << (5 - bits)) & 31];
    return o;
}
bool WebAuth::Base32Decode(const Str& s, Bytes& out)
{
    out.clear(); u32 buf = 0; int bits = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i]; int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A'; else if (c >= 'a' && c <= 'z') v = c - 'a'; else if (c >= '2' && c <= '7') v = c - '2' + 26;
        else if (c == '=' || c == ' ' || c == '-') continue; else return false;
        buf = (buf << 5) | (u32)v; bits += 5;
        if (bits >= 8) { out.push_back((u8)((buf >> (bits - 8)) & 0xff)); bits -= 8; }
    }
    return true;
}

Str WebAuth::Totp(const Bytes& secret, u64 t, int digits)
{
    u64 counter = t / 30;
    u8 msg[8]; for (int i = 7; i >= 0; --i) { msg[i] = (u8)(counter & 0xff); counter >>= 8; }
    u8 h[20]; HmacSha1(secret.data(), secret.size(), msg, 8, h);
    int off = h[19] & 0xf;
    u32 bin = ((u32)(h[off] & 0x7f) << 24) | ((u32)h[off + 1] << 16) | ((u32)h[off + 2] << 8) | h[off + 3];
    u32 mod = 1; for (int i = 0; i < digits; ++i) mod *= 10;
    return Str::fmt("%0*u", digits, (unsigned)(bin % mod));
}

// ---- construction / file ------------------------------------------------------------------------------------------------
WebAuth::WebAuth() : m_clock(nullptr), m_kdfIter(kEncIterationsDefault)
{
    InitializeCriticalSection(&m_cs);
    m_doc = Json::object();
}
WebAuth::~WebAuth() { DeleteCriticalSection(&m_cs); }

i64 WebAuth::now() const { return m_clock ? m_clock() : (i64)UnixNow(); }

// enabledDefault = the product's first-start defaults: the site is ON (127.0.0.1:8443), the default administrator admin/admin exists and
// the global 2FA requirement for admins is off (otherwise admin/admin could not sign in).  false (tests): off, no default user, 2FA required.
static Json DefaultWebSettings(bool enabledDefault)
{
    Json s = Json::object();
    s["enabled"] = enabledDefault;
    Json b = Json::array(); b.push("127.0.0.1"); s["bind"] = b;
    s["port"] = 8443; s["require_2fa_admins"] = !enabledDefault; s["session_minutes"] = 20; s["trust_proxy"] = false;
    return s;
}

bool WebAuth::IsLoopbackAddress(const Str& ipIn)
{
    Str ip = ipIn.trim(); int pc = ip.find('%'); if (pc >= 0) ip = ip.substr(0, (size_t)pc);
    if (ip.lower().starts_with("::ffff:") && ip.find('.') >= 0) ip = ip.substr(7);
    in_addr a4; in6_addr a6;
    if (inet_pton(AF_INET, ip.c_str(), &a4) == 1) return (ntohl(a4.s_addr) >> 24) == 127;
    if (inet_pton(AF_INET6, ip.c_str(), &a6) == 1) { for (int i = 0; i < 15; ++i) if (a6.s6_addr[i]) return false; return a6.s6_addr[15] == 1; }
    return false;
}

bool WebAuth::open(const Str& dataDir, Str& err, bool enabledDefault)
{
    Guard g(&m_cs);
    m_dir = dataDir; m_file = JoinPath(dataDir, "web.json"); m_auditFile = JoinPath(dataDir, "audit.log");
    if (!MakeDirs(dataDir, err)) return false;
    m_doc = Json::object();
    if (FileExists(m_file)) {
        Bytes b; if (!ReadWholeFile(m_file.c_str(), b, err, 16 << 20)) return false;
        Json j; Str e; if (!Json::parse((const char*)b.data(), b.size(), j, e) || !j.is_object()) { err = "web.json is damaged: " + e; return false; }
        m_doc = j;
    }
    Json def = DefaultWebSettings(enabledDefault), cur = m_doc.get("settings");
    for (size_t i = 0; i < def.size(); ++i) if (!cur.has(def.key_at(i).c_str())) cur[def.key_at(i).c_str()] = def.value_at(i);
    if (cur.get("bind").is_string()) { Json b = Json::array(); b.push(cur.get("bind").str()); cur["bind"] = b; }   // an older single address
    m_doc["settings"] = cur;
    if (!m_doc.get("users").is_array()) m_doc["users"] = Json::array();
    if (!m_doc.get("default_admin_created").boolean()) {
        if (m_doc.get("users").size() > 0) m_doc["default_admin_created"] = true;           // an existing installation: never gets the default administrator (kept in memory until the next save)
        else if (enabledDefault) {
            Json us = Json::array(); Json u = Json::object();
            u["name"] = "admin"; u["role"] = "admin"; u["twofa"] = "optional"; u["created"] = (i64)now(); u["pw"] = hashPassword("admin");
            us.push(u); m_doc["users"] = us; m_doc["default_admin_created"] = true;
            if (!save(err)) return false;
            audit("default_admin_created", "admin", "", "default administrator created (first start)");
        }
    }
    Bytes salt; salt.resize(16); RandomBytes(salt.data(), 16);
    Json d = Json::object(); d["salt"] = Base64Encode(salt.data(), 16); d["iter"] = (i64)m_kdfIter; d["hash"] = Base64Encode(salt.data(), 16); m_dummy = d;
    return true;
}

bool WebAuth::save(Str& err)
{
    Str t = m_doc.dump(true);
    return WriteWholeFile(m_file.c_str(), t.data(), t.size(), err);
}

Json WebAuth::settings() const { Guard g(&m_cs); return m_doc.get("settings"); }

bool WebAuth::BindEntryOk(const Str& e)
{
    if (e == "localhost" || e == "all") return true;
    if (e.starts_with("adapter:")) {
        Str g = e.substr(8); if (g.empty() || g.size() > 100) return false;
        for (size_t i = 0; i < g.size(); ++i) { char c = g[i]; if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '{' || c == '}' || c == '-' || c == '_' || c == '.')) return false; }
        return true;
    }
    if (e.empty() || e.size() > 80) return false;
    Str a = e; int pc = a.find('%');
    if (pc >= 0) { Str sc = a.substr((size_t)pc + 1); a = a.substr(0, (size_t)pc); if (sc.empty() || sc.size() > 10) return false; for (size_t i = 0; i < sc.size(); ++i) if (sc[i] < '0' || sc[i] > '9') return false; if (a.find(':') < 0) return false; }
    in_addr a4; in6_addr a6;
    return inet_pton(AF_INET, a.c_str(), &a4) == 1 || inet_pton(AF_INET6, a.c_str(), &a6) == 1;
}

bool WebAuth::setSettings(const Json& p, Str& err)
{
    Guard g(&m_cs);
    if (!p.is_object()) { err = "settings must be an object"; return false; }
    Json s = m_doc.get("settings");
    for (size_t i = 0; i < p.size(); ++i) {
        const Str& k = p.key_at(i); const Json& v = p.value_at(i);
        if (k == "enabled" || k == "require_2fa_admins" || k == "trust_proxy") { if (!v.is_bool()) { err = k + " must be true or false"; return false; } s[k.c_str()] = v.boolean(); }
        else if (k == "bind") {
            Json list = Json::array();
            if (v.is_string()) list.push(v.str().trim());
            else if (v.is_array()) { for (size_t j = 0; j < v.size(); ++j) { if (!v.at(j).is_string()) { err = "bind entries must be text"; return false; } list.push(v.at(j).str().trim()); } }
            else { err = "bind must be a list of addresses"; return false; }
            if (list.size() > 32) { err = "too many bind entries"; return false; }
            for (size_t j = 0; j < list.size(); ++j) if (!BindEntryOk(list.at(j).str())) { err = "bad bind entry \"" + Sanitize(list.at(j).str(), 80) + "\": use an IP address, adapter:<GUID>, localhost or all"; return false; }
            s["bind"] = list;
        }
        else if (k == "port") { i64 n = v.integer(-1); if (!v.is_number() || n < 1 || n > 65535) { err = "port must be 1..65535"; return false; } s["port"] = n; }
        else if (k == "session_minutes") { i64 n = v.integer(-1); if (!v.is_number() || n < 1 || n > 1440) { err = "session_minutes must be 1..1440"; return false; } s["session_minutes"] = n; }
        else { err = "unknown web setting: " + k; return false; }
    }
    if (s.get("enabled").boolean() && s.get("bind").size() == 0) { err = "at least one address or adapter must be chosen to listen on"; return false; }
    m_doc["settings"] = s;
    return save(err);
}

int WebAuth::sessionSeconds() const { Guard g(&m_cs); i64 m = m_doc.get("settings").get("session_minutes").integer(20); return (int)(m < 1 ? 20 : m) * 60; }

bool WebAuth::identity(WebTlsIdentity& id, bool* selfSigned) const
{
    Guard g(&m_cs);
    const Json& c = m_doc.get("cert");
    if (!c.is_object()) return false;
    Bytes der; Str d = c.get("der").str();
    if (d.empty() || !Base64Decode(d.data(), d.size(), der) || der.empty()) return false;
    id.certDer = der; id.keyProtected = c.get("key").str(); if (selfSigned) *selfSigned = c.get("self_signed").boolean();
    return !id.keyProtected.empty();
}

bool WebAuth::setIdentity(const WebTlsIdentity& id, bool selfSigned, Str& err)
{
    Guard g(&m_cs);
    Json c = Json::object(); c["der"] = Base64Encode(id.certDer.data(), id.certDer.size()); c["key"] = id.keyProtected; c["self_signed"] = selfSigned;
    m_doc["cert"] = c;
    return save(err);
}

// ---- users ---------------------------------------------------------------------------------------------------------------------
int WebAuth::findUser(const Str& lname) const
{
    const Json& us = m_doc.get("users");
    for (size_t i = 0; i < us.size(); ++i) if (Lower(us.at(i).get("name").str()) == lname) return (int)i;
    return -1;
}
int WebAuth::userCount() const { Guard g(&m_cs); return (int)m_doc.get("users").size(); }

Json WebAuth::hashPassword(const Str& pw)
{
    u8 salt[16]; RandomBytes(salt, 16); u8 h[32];
    Pbkdf2Sha256(pw.data(), pw.size(), salt, 16, m_kdfIter, h, 32);
    Json o = Json::object(); o["salt"] = Base64Encode(salt, 16); o["iter"] = (i64)m_kdfIter; o["hash"] = Base64Encode(h, 32);
    return o;
}

bool WebAuth::verifyPassword(const Json& ph, const Str& pw)
{
    Bytes salt, want; Str s = ph.get("salt").str(), h = ph.get("hash").str();
    i64 it = ph.get("iter").integer(0);
    if (!Base64Decode(s.data(), s.size(), salt) || !Base64Decode(h.data(), h.size(), want) || want.size() != 32 || it < 1 || it > 100000000) return false;
    u8 got[32]; if (!Pbkdf2Sha256(pw.data(), pw.size(), salt.data(), salt.size(), (u32)it, got, 32)) return false;
    return CtEq(got, want.data(), 32);
}

static void PurgeUser(Vec<WebSession>& ss, const Str& lname, const Str& keepId = Str())
{
    for (size_t i = ss.size(); i-- > 0;) if (Lower(ss[i].user) == lname && (keepId.empty() || ss[i].id != keepId)) ss.erase(i);
}

int WebAuth::adminCount() const
{
    Guard g(&m_cs); int n = 0; const Json& us = m_doc.get("users");
    for (size_t i = 0; i < us.size(); ++i) if (us.at(i).get("role").str() == "admin") ++n;
    return n;
}
bool WebAuth::userInfo(const Str& name, Str& role, bool& totpEnabled) const
{
    Guard g(&m_cs); int idx = findUser(Lower(name.trim())); if (idx < 0) return false;
    const Json& u = m_doc.get("users").at((size_t)idx); role = u.get("role").str(); totpEnabled = u.get("totp").get("confirmed").boolean(); return true;
}

bool WebAuth::resetDefaults(Str& report, Str& err)
{
    Guard g(&m_cs);
    Json s = DefaultWebSettings(true);
    Json old = m_doc.get("settings"); s["session_minutes"] = old.get("session_minutes").integer(20) > 0 ? old.get("session_minutes").integer(20) : (i64)20;
    m_doc["settings"] = s;
    Json us = m_doc.get("users"); if (!us.is_array()) us = Json::array();
    int idx = findUser("admin");
    Json ph = hashPassword("admin");
    if (idx < 0) {
        Json u = Json::object(); u["name"] = "admin"; u["role"] = "admin"; u["twofa"] = "optional"; u["created"] = (i64)now(); u["pw"] = ph; us.push(u);
        report = "settings reset to the defaults (enabled, 127.0.0.1:8443, no 2FA requirement); the user admin was created with the password admin";
    } else {
        Json u = us.at((size_t)idx); u["role"] = "admin"; u["pw"] = ph; u["twofa"] = "optional"; u.remove("totp"); u.remove("recovery"); us.item((size_t)idx) = u;
        report = "settings reset to the defaults (enabled, 127.0.0.1:8443, no 2FA requirement); the password of the user admin is now admin, its role admin, its two-factor authentication cleared";
    }
    m_doc["users"] = us; m_doc["default_admin_created"] = true; m_sessions.clear();
    if (!save(err)) return false;
    audit("web_reset", "local-admin", "service-cli", "settings reset to the defaults; admin / admin restored");
    return true;
}

Json WebAuth::usersList() const
{
    Guard g(&m_cs);
    Json out = Json::array(); const Json& us = m_doc.get("users");
    for (size_t i = 0; i < us.size(); ++i) {
        const Json& u = us.at(i); Json o = Json::object();
        o["name"] = u.get("name").str(); o["role"] = u.get("role").str(); o["twofa"] = u.get("twofa").str("optional");
        o["has_password"] = !u.get("pw").get("hash").str().empty();
        o["totp_enabled"] = u.get("totp").get("confirmed").boolean(); o["totp_pending"] = u.get("totp").is_object() && !u.get("totp").get("confirmed").boolean();
        o["recovery_left"] = (i64)u.get("recovery").size(); o["created"] = u.get("created").integer();
        out.push(o);
    }
    return out;
}

bool WebAuth::usersSave(const Json& a, Str& err)
{
    Str name = a.get("name").str().trim();
    if (!NameOk(name)) { err = "The user name must be 1-64 characters: letters, digits and . _ @ -"; return false; }
    Str role = a.get("role").str(); Str twofa = a.get("twofa").str("");
    Str pw = a.get("password").str();
    bool hasPw = a.has("password") && !a.get("password").is_null() && !pw.empty();
    if (hasPw) { Str why; if (!PasswordOk(name, pw, why)) { err = why; return false; } }
    Json ph; if (hasPw) ph = hashPassword(pw);                  // the slow hash outside the lock
    Guard g(&m_cs);
    int idx = findUser(Lower(name));
    if (idx < 0) {
        if (role != "admin" && role != "operator" && role != "viewer") { err = "role must be admin, operator or viewer"; return false; }
        if (!hasPw) { err = "a new user needs a password"; return false; }
        if (twofa.empty()) twofa = "optional";
    }
    if (!role.empty() && role != "admin" && role != "operator" && role != "viewer") { err = "role must be admin, operator or viewer"; return false; }
    if (!twofa.empty() && twofa != "off" && twofa != "optional" && twofa != "required") { err = "twofa must be off, optional or required"; return false; }
    Json us = m_doc.get("users");
    Json u = idx >= 0 ? us.at((size_t)idx) : Json::object();
    bool kill = false;
    if (idx < 0) { u["name"] = name; u["created"] = (i64)now(); }
    if (!role.empty()) { if (u.get("role").str() != role) kill = true; u["role"] = role; }
    if (!twofa.empty()) u["twofa"] = twofa;
    if (hasPw) { u["pw"] = ph; kill = true; }
    if (idx >= 0) us.item((size_t)idx) = u; else us.push(u);
    m_doc["users"] = us;
    if (kill) PurgeUser(m_sessions, Lower(name));
    if (!save(err)) return false;
    audit("user_saved", actorName(), actorIp(), Str::fmt("%s role=%s twofa=%s%s", name.c_str(), u.get("role").str().c_str(), u.get("twofa").str().c_str(), hasPw ? " password-set" : ""));
    return true;
}

bool WebAuth::usersDelete(const Str& nameIn, Str& err)
{
    Guard g(&m_cs);
    Str ln = Lower(nameIn.trim()); int idx = findUser(ln);
    if (idx < 0) { err = "no such user"; return false; }
    Json us = m_doc.get("users"); Str nm = us.at((size_t)idx).get("name").str();
    Json nu = Json::array(); for (size_t i = 0; i < us.size(); ++i) if ((int)i != idx) nu.push(us.at(i));
    m_doc["users"] = nu; PurgeUser(m_sessions, ln);
    if (!save(err)) return false;
    audit("user_deleted", actorName(), actorIp(), nm);
    return true;
}

bool WebAuth::totpEnroll(const Str& name, Json& out, Str& err)
{
    Guard g(&m_cs);
    Str ln = Lower(name.trim()); int idx = findUser(ln);
    if (idx < 0) { err = "no such user"; return false; }
    Json us = m_doc.get("users"); Json u = us.at((size_t)idx);
    if (u.get("totp").get("confirmed").boolean()) { err = "two-factor authentication is already set up for this user; disable it first"; return false; }
    Bytes sec; sec.resize(20); RandomBytes(sec.data(), 20);
    Str prot = Config::Protect(Base64Encode(sec.data(), sec.size()));
    if (prot.empty()) { err = "cannot protect the secret (DPAPI)"; return false; }
    Json t = Json::object(); t["secret"] = prot; t["confirmed"] = false; t["last_step"] = (i64)0; u["totp"] = t;
    us.item((size_t)idx) = u; m_doc["users"] = us;
    if (!save(err)) return false;
    Str b32 = Base32(sec); Str nm = u.get("name").str();
    out = Json::object(); out["secret"] = b32;
    out["uri"] = "otpauth://totp/VM2Backup:" + nm + "?secret=" + b32 + "&issuer=VM2Backup&algorithm=SHA1&digits=6&period=30";
    SecureZeroMemory(sec.data(), sec.size());
    audit("totp_enroll", actorName(), actorIp(), nm);
    return true;
}

static bool TotpSecret(const Json& u, Bytes& sec)
{
    Str prot = u.get("totp").get("secret").str(), plain;
    if (prot.empty() || !Config::Unprotect(prot, plain)) return false;
    return Base64Decode(plain.data(), plain.size(), sec) && !sec.empty();
}

// checks the code against the secret in a +-1 step window; a step is accepted once (replay protection)
bool WebAuth::checkTotp(Json& u, const Str& code)
{
    Bytes sec; if (!TotpSecret(u, sec)) return false;
    Str c = code.trim(); if (c.size() != 6) return false;
    i64 step = now() / 30, last = u.get("totp").get("last_step").integer(0);
    bool ok = false; i64 used = 0;
    for (i64 s = step - 1; s <= step + 1; ++s) {
        Str want = Totp(sec, (u64)(s * 30));
        bool eq = CtEqStr(want, c);
        if (eq && s > last && !ok) { ok = true; used = s; }
    }
    SecureZeroMemory(sec.data(), sec.size());
    if (ok) { Json t = u.get("totp"); t["last_step"] = used; u["totp"] = t; }
    return ok;
}

bool WebAuth::totpConfirm(const Str& name, const Str& code, Json& out, Str& err, const Str& keepSession)
{
    Guard g(&m_cs);
    Str ln = Lower(name.trim()); int idx = findUser(ln);
    if (idx < 0) { err = "no such user"; return false; }
    Json us = m_doc.get("users"); Json u = us.at((size_t)idx);
    if (!u.get("totp").is_object() || u.get("totp").get("confirmed").boolean()) { err = "there is no pending two-factor enrollment for this user"; return false; }
    if (!checkTotp(u, code)) { err = "the code is wrong"; return false; }
    Json t = u.get("totp"); t["confirmed"] = true; u["totp"] = t;
    Json codes = Json::array(), hashes = Json::array();
    for (int i = 0; i < 10; ++i) {
        Bytes r; r.resize(7); RandomBytes(r.data(), 7);
        Str b = Base32(r).substr(0, 10);                    // 50 bits
        Str shown = b.substr(0, 5) + "-" + b.substr(5);
        codes.push(shown);
        u8 h[32]; Sha256(b.data(), b.size(), h); hashes.push(HexEncode(h, 32));
    }
    u["recovery"] = hashes;
    us.item((size_t)idx) = u; m_doc["users"] = us;
    PurgeUser(m_sessions, ln, keepSession);
    if (!save(err)) return false;
    out = Json::object(); out["recovery_codes"] = codes;
    audit("totp_confirmed", actorName(), actorIp(), u.get("name").str());
    return true;
}

bool WebAuth::totpDisable(const Str& name, Str& err)
{
    Guard g(&m_cs);
    Str ln = Lower(name.trim()); int idx = findUser(ln);
    if (idx < 0) { err = "no such user"; return false; }
    Json us = m_doc.get("users"); Json u = us.at((size_t)idx);
    u.remove("totp"); u.remove("recovery");
    us.item((size_t)idx) = u; m_doc["users"] = us; PurgeUser(m_sessions, ln);
    if (!save(err)) return false;
    audit("totp_disabled", actorName(), actorIp(), u.get("name").str());
    return true;
}

bool WebAuth::useRecovery(Json& u, const Str& code)
{
    Str c; for (size_t i = 0; i < code.size(); ++i) { char ch = code[i]; if (ch == '-' || ch == ' ') continue; if (ch >= 'a' && ch <= 'z') ch -= 32; c += ch; }
    if (c.size() != 10) return false;
    u8 h[32]; Sha256(c.data(), c.size(), h); Str hx = HexEncode(h, 32);
    Json rec = u.get("recovery"); int found = -1;
    for (size_t i = 0; i < rec.size(); ++i) if (CtEqStr(rec.at(i).str(), hx) && found < 0) found = (int)i;
    if (found < 0) return false;
    Json nr = Json::array(); for (size_t i = 0; i < rec.size(); ++i) if ((int)i != found) nr.push(rec.at(i));
    u["recovery"] = nr;
    return true;
}

bool WebAuth::isTwofaRequired(const Json& u) const
{
    Str t = u.get("twofa").str("optional");
    if (t == "required") return true;
    return u.get("role").str() == "admin" && m_doc.get("settings").get("require_2fa_admins").boolean(true);     // the global switch overrides a personal "off"
}

// ---- throttling ---------------------------------------------------------------------------------------------------------------
int WebAuth::throttleLeft(const Str& key)
{
    i64 t = now();
    for (size_t i = 0; i < m_fails.size(); ++i) if (m_fails[i].key == key) {
        Fail& f = m_fails[i];
        if (t - f.last > kLockSeconds && f.lockedUntil <= t) { m_fails.erase(i); return 0; }
        return f.lockedUntil > t ? (int)(f.lockedUntil - t) : 0;
    }
    return 0;
}
void WebAuth::noteFail(const Str& key)
{
    i64 t = now(); Fail* f = nullptr;
    for (size_t i = 0; i < m_fails.size(); ++i) if (m_fails[i].key == key) f = &m_fails[i];
    if (!f) {
        if (m_fails.size() > 5000) for (size_t i = m_fails.size(); i-- > 0;) if (m_fails[i].lockedUntil <= t && t - m_fails[i].last > kLockSeconds) m_fails.erase(i);
        if (m_fails.size() > 20000) m_fails.erase(0);
        Fail n; n.count = 0; n.last = t; n.lockedUntil = 0; n.key = key; m_fails.push_back(n); f = &m_fails.back();
    }
    if (t - f->last > kLockSeconds) f->count = 0;
    f->count++; f->last = t;
    int wait = f->count >= kLockAfter ? kLockSeconds : (f->count > 6 ? 60 : (1 << (f->count - 1)));      // 1 2 4 8 16 32 60 s, then 15 minutes
    f->lockedUntil = t + wait;
}
void WebAuth::clearFail(const Str& key) { for (size_t i = 0; i < m_fails.size(); ++i) if (m_fails[i].key == key) { m_fails.erase(i); return; } }

// ---- login ---------------------------------------------------------------------------------------------------------------------
WebAuth::LoginResult WebAuth::login(const Str& ip, const Str& userIn, const Str& pw, const Str& code, const Str& recovery)
{
    LoginResult r; r.status = 401; r.retryAfter = 0;
    Str user = userIn.trim(); Str ln = Lower(user);
    if (user.size() > 64 || pw.size() > 1024 || code.size() > 64 || recovery.size() > 64) { r.message = "Invalid user name or password."; return r; }
    Str ipKey = "ip:" + ip, uKey = "user:" + ln;
    Guard g(&m_cs);
    int left = throttleLeft(ipKey), l2 = throttleLeft(uKey); if (l2 > left) left = l2;
    if (left > 0) {
        r.status = 429; r.retryAfter = left; r.message = Str::fmt("Too many failed attempts. Try again in %d seconds.", left);
        audit("login_blocked", ln, ip, "");
        return r;
    }
    if (m_doc.get("users").size() == 0) { r.message = "No users are configured. On the server, run VM2BackupService.exe /reset-web as an administrator."; audit("login_no_users", ln, ip, ""); return r; }
    int idx = findUser(ln);
    Json u = idx >= 0 ? m_doc.get("users").at((size_t)idx) : Json();
    Json ph = idx >= 0 ? u.get("pw") : m_dummy;
    bool ok;
    { Unguard ug(&m_cs); ok = verifyPassword(ph, pw); }
    // the lock was dropped for the slow hash: the user may have changed meanwhile
    idx = findUser(ln);
    if (idx < 0) ok = false; else u = m_doc.get("users").at((size_t)idx);
    if (!ok) {
        noteFail(ipKey); noteFail(uKey);
        audit("login_failed", ln, ip, "");
        r.message = "Invalid user name or password.";
        return r;
    }
    bool required = isTwofaRequired(u);
    bool enrolled = u.get("totp").get("confirmed").boolean() && (u.get("twofa").str("optional") != "off" || required);
    if (required && !enrolled) {
        audit("login_denied_no_2fa", u.get("name").str(), ip, "");
        r.message = "Two-factor authentication is required for this account, but it is not set up. Ask an administrator to set it up in the VM2Backup program.";
        return r;
    }
    if (enrolled) {
        if (code.empty() && recovery.empty()) { r.status = 202; r.message = "Enter the code from your authenticator app, or a recovery code."; return r; }
        bool good = false;
        if (!code.empty()) good = checkTotp(u, code);
        else good = useRecovery(u, recovery);
        if (good) { Json us = m_doc.get("users"); us.item((size_t)idx) = u; m_doc["users"] = us; Str e; save(e); if (code.empty()) audit("recovery_code_used", u.get("name").str(), ip, Str::fmt("%d left", (int)u.get("recovery").size())); }
        if (!good) {
            noteFail(ipKey); noteFail(uKey);
            audit("login_failed_2fa", u.get("name").str(), ip, "");
            r.message = "Invalid verification code.";
            return r;
        }
    }
    WebSession s; s.id = RandomHex(32); s.csrf = RandomHex(32); s.user = u.get("name").str(); s.role = u.get("role").str(); s.ip = ip; s.created = s.last = now();
    if ((int)m_sessions.size() >= kMaxSessions) { size_t o = 0; for (size_t i = 1; i < m_sessions.size(); ++i) if (m_sessions[i].last < m_sessions[o].last) o = i; m_sessions.erase(o); }
    m_sessions.push_back(s);
    clearFail(ipKey); clearFail(uKey);
    audit("login_ok", s.user, ip, "");
    r.status = 200; r.sessionId = s.id; r.csrf = s.csrf; r.user = s.user; r.role = s.role; r.message = "ok";
    return r;
}

bool WebAuth::session(const Str& id, WebSession& out)
{
    if (id.size() != 64) return false;
    Guard g(&m_cs);
    i64 t = now(); i64 idle = m_doc.get("settings").get("session_minutes").integer(20) * 60;
    for (size_t i = 0; i < m_sessions.size(); ++i) {
        if (!CtEqStr(m_sessions[i].id, id)) continue;
        WebSession& s = m_sessions[i];
        if (t - s.last > idle || t - s.created > kAbsoluteSeconds || t < s.created) { m_sessions.erase(i); return false; }
        int ui = findUser(Lower(s.user));
        if (ui < 0) { m_sessions.erase(i); return false; }
        s.role = m_doc.get("users").at((size_t)ui).get("role").str();
        s.last = t; out = s; return true;
    }
    return false;
}

void WebAuth::logout(const Str& id)
{
    Guard g(&m_cs);
    for (size_t i = 0; i < m_sessions.size(); ++i) if (CtEqStr(m_sessions[i].id, id)) { audit("logout", m_sessions[i].user, m_sessions[i].ip, ""); m_sessions.erase(i); return; }
}

int WebAuth::sessionCount()
{
    Guard g(&m_cs);
    i64 t = now(); i64 idle = m_doc.get("settings").get("session_minutes").integer(20) * 60;
    for (size_t i = m_sessions.size(); i-- > 0;) if (t - m_sessions[i].last > idle || t - m_sessions[i].created > kAbsoluteSeconds) m_sessions.erase(i);
    return (int)m_sessions.size();
}

bool WebAuth::changePassword(const Str& user, const Str& current, const Str& next, const Str& ip, Str& err)
{
    Str ln = Lower(user), key = "user:" + ln;
    Str why; if (!PasswordOk(user, next, why)) { err = why; return false; }
    Guard g(&m_cs);
    int left = throttleLeft(key); if (left > 0) { err = Str::fmt("Too many failed attempts. Try again in %d seconds.", left); return false; }
    int idx = findUser(ln); if (idx < 0) { err = "no such user"; return false; }
    Json ph = m_doc.get("users").at((size_t)idx).get("pw"); bool ok;
    { Unguard ug(&m_cs); ok = verifyPassword(ph, current); }
    if (!ok) { noteFail(key); audit("password_change_failed", user, ip, ""); err = "The current password is wrong."; return false; }
    Json nh; { Unguard ug(&m_cs); nh = hashPassword(next); }
    idx = findUser(ln); if (idx < 0) { err = "no such user"; return false; }
    Json us = m_doc.get("users"); Json u = us.at((size_t)idx); u["pw"] = nh; us.item((size_t)idx) = u; m_doc["users"] = us;
   
    PurgeUser(m_sessions, ln);
    if (!save(err)) return false;
    audit("password_changed", user, ip, "");
    return true;
}

// ---- audit -----------------------------------------------------------------------------------------------------------------------
void WebAuth::audit(const char* event, const Str& user, const Str& ip, const Str& detail)
{
    Guard g(&m_cs);                                          // recursive: callers may hold it
    if (m_auditFile.empty()) return;
    time_t tt = (time_t)now(); struct tm* tm = gmtime(&tt);
    char ts[40] = "";
    if (tm) snprintf(ts, sizeof(ts), "%04d-%02d-%02dT%02d:%02d:%02dZ", tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec);
    Str line = Str(ts) + "\t" + event + "\t" + Sanitize(user, 64) + "\t" + Sanitize(ip, 64) + "\t" + Sanitize(detail) + "\r\n";
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (GetFileAttributesExW(FilePath(m_auditFile), GetFileExInfoStandard, &fa) && fa.nFileSizeLow > (5u << 20) && !fa.nFileSizeHigh)
        MoveFileExW(FilePath(m_auditFile), FilePath(JoinPath(m_dir, "audit.1.log")), MOVEFILE_REPLACE_EXISTING);
    HANDLE h = CreateFileW(FilePath(m_auditFile), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w; WriteFile(h, line.data(), (DWORD)line.size(), &w, nullptr);
    CloseHandle(h);
}

Str WebAuth::auditTail(int lines)
{
    if (lines < 1) lines = 1;
    if (lines > 2000) lines = 2000;
    Guard g(&m_cs);
    Bytes b; Str e;
    if (!FileExists(m_auditFile) || !ReadWholeFile(m_auditFile.c_str(), b, e, 64 << 20)) return Str();
    size_t end = b.size(), pos = end; int n = 0;
    while (pos > 0) { if (b[pos - 1] == '\n') { ++n; if (n > lines) break; } --pos; }
    if (n <= lines) pos = 0;
    return Str((const char*)b.data() + pos, end - pos);
}

} // namespace vm2b
