#include "config.h"
#include "codec.h"
#include "engine.h"
#include "crypto.h"
#include "schedule.h"
#include "winutil.h"
#include "smb.h"
#include <wincrypt.h>

namespace vm2b {

const char* const kLocalServerId = "local";
const char* const kMask = "********";

static const char* const kSourceTypes[] = { "directory", "esxi", "vcenter", nullptr };
static const char* const kTargetTypes[] = { "local", "ftp", "ftpes", "ftps", "scp", "smb", nullptr };
static const char kSftpGone[] = "SFTP is no longer supported, create an SCP server";   // old configs may still hold type "sftp"
static bool In(const char* const* list, const Str& v) { for (; *list; ++list) if (v == *list) return true; return false; }
static bool IsSource(const Str& t) { return In(kSourceTypes, t); }
static bool IsTarget(const Str& t) { return In(kTargetTypes, t); }

Config::Config() { InitializeCriticalSection(&m_cs); m_cfg = Json::object(); m_hist = Json::array(); m_state = Json::object(); }
Config::~Config() { DeleteCriticalSection(&m_cs); }

Str Config::Protect(const Str& plain)
{
    if (plain.empty()) return plain;
    DATA_BLOB in, out;
    in.pbData = (BYTE*)plain.data(); in.cbData = (DWORD)plain.size();
    if (!CryptProtectData(&in, L"VM2Backup", nullptr, nullptr, nullptr, CRYPTPROTECT_LOCAL_MACHINE | CRYPTPROTECT_UI_FORBIDDEN, &out)) return "";
    Str s = "dpapi:" + Base64Encode(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return s;
}

bool Config::Unprotect(const Str& stored, Str& plain)
{
    if (!stored.starts_with("dpapi:")) { plain = stored; return true; }
    Bytes raw;
    if (!Base64Decode(stored.c_str() + 6, stored.size() - 6, raw)) return false;
    DATA_BLOB in, out;
    in.pbData = raw.data(); in.cbData = (DWORD)raw.size();
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return false;
    plain = Str((const char*)out.pbData, out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return true;
}

Str Config::newId() const
{
    u8 b[6]; RandomBytes(b, 6);
    return HexEncode(b, 6);
}

static Json DefaultSettings(const Str& dir)
{
    Json s = Json::object();
    s["language"] = 0;                                  // 0 = the Windows language
    s["default_local_path"] = JoinPath(dir, "Backups");
    s["max_parallel"] = 2; s["max_history"] = 500; s["speed_limit_mbit"] = 0.0;
    s["retry_attempts"] = 8; s["retry_base_sec"] = 2; s["retry_max_sec"] = 60;
    s["level"] = 5; s["threads"] = 0; s["segment_mib"] = (int)kDefaultSegmentMiB;
    s["prevent_sleep"] = false; s["email"] = Json::object();
    s["kdf_iterations"] = (int)kEncIterationsDefault; s["misfire_grace_min"] = 15; s["retry_base_ms"] = 0;      // the last one: a test hook (milliseconds instead of retry_base_sec)
    return s;
}

// xz is the only compressor. A config written by an older version may name "gzip" or "none" (or nothing): the codec is replaced and the
// level mapped: gzip L -> xz clamp(L, 1, 9) (gzip's own default was 6), none -> xz 1, an unknown name -> xz 5, no name -> xz with the level kept.
// A task level 0 means "the level of the Settings" and stays 0.
static i64 MigratedLevel(const Json& comp, bool zeroIsDefault)
{
    Str cn = comp.get("codec").str(); i64 lv = comp.get("level").integer(0), out;
    if (cn.empty() || cn == "xz") out = lv < 0 ? 0 : lv > 9 ? 9 : lv;
    else if (cn == "gzip") out = lv <= 0 ? 6 : lv > 9 ? 9 : lv;
    else if (cn == "none") out = 1;
    else out = 5;
    return out == 0 && !zeroIsDefault ? 5 : out;
}

// Applied to the loaded config: the settings and every task element. Returns true when something was rewritten.
static bool MigrateConfig(Json& cfg)
{
    bool changed = false;
    if (cfg.get("settings").is_object()) {
        Json& st = cfg["settings"]; i64 lv = MigratedLevel(st, false);
        if (st.has("codec") || !st.get("level").is_number() || st.get("level").integer() != lv) { st.remove("codec"); st["level"] = lv; changed = true; }
        if (!st.get("threads").is_number() || st.get("threads").integer() < 0 || st.get("threads").integer() > 64) { st["threads"] = 0; changed = true; }
    }
    if (!cfg.get("tasks").is_array()) return changed;
    Json& ts = cfg["tasks"];
    for (size_t i = 0; i < ts.size(); ++i) {
        if (!ts.at(i).get("elements").is_array()) continue;
        Json& els = ts.item(i)["elements"];
        for (size_t k = 0; k < els.size(); ++k) {
            Json& el = els.item(k);
            if (el.is_object() && el.get("id").str().empty()) { u8 rb[6]; RandomBytes(rb, 6); el["id"] = HexEncode(rb, 6); changed = true; }      // every element has a stable id (its stored password is matched by it)
            if (!el.is_object() || !el.has("compress")) continue;
            const Json& comp = el.get("compress"); i64 lv = MigratedLevel(comp, true);
            if (!comp.is_object() || comp.get("codec").str() != "xz" || !comp.get("level").is_number() || comp.get("level").integer() != lv) {
                Json nc = Json::object(); nc["codec"] = "xz"; nc["level"] = lv; el["compress"] = nc; changed = true;
            }
        }
    }
    return changed;
}

bool Config::open(const Str& dataDir, Str& err)
{
    m_dir = dataDir;
    if (!MakeDirs(m_dir, err)) return false;
    Str p = JoinPath(m_dir, "config.json");
    Bytes b; Str e;
    // Файл есть, но не читается (нет прав: служба запущена под другой учётной записью; занят другим процессом): это ошибка запуска.
    // Раньше начиналась пустая конфигурация и saveCfg() ниже перезаписывала config.json - серверы и задачи пропадали молча.
    if (FileExists(p)) {
        if (!ReadWholeFile(p.c_str(), b, e, 64 << 20)) { err = "cannot read config.json (kept untouched): " + e + " - the account the service runs as needs full control of the data folder " + m_dir; return false; }
        if (!Json::parse((const char*)b.data(), b.size(), m_cfg, e) || !m_cfg.is_object()) { err = "config.json is damaged: " + e; return false; }
    }
    if (!m_cfg.has("servers")) m_cfg["servers"] = Json::array();
    if (!m_cfg.has("tasks")) m_cfg["tasks"] = Json::array();
    MigrateConfig(m_cfg);                                                     // old codecs -> xz; written back by saveCfg() below
    Json def = DefaultSettings(m_dir);
    for (size_t i = 0; i < def.size(); ++i) if (!m_cfg.get("settings").has(def.key_at(i).c_str())) m_cfg["settings"][def.key_at(i).c_str()] = def.value_at(i);
    if (!m_cfg.has("scheduler")) m_cfg["scheduler"] = false;                 // like the original: the scheduler is off until it is enabled
    Str hp = JoinPath(m_dir, "history.json");
    if (FileExists(hp)) { if (!ReadWholeFile(hp.c_str(), b, e, 256 << 20)) { err = "cannot read history.json (kept untouched): " + e; return false; } Json h; if (Json::parse((const char*)b.data(), b.size(), h, e) && h.is_array()) m_hist = h; }
    Str sp = JoinPath(m_dir, "state.json");
    if (FileExists(sp)) { if (!ReadWholeFile(sp.c_str(), b, e, 16 << 20)) { err = "cannot read state.json (kept untouched): " + e; return false; } Json st; if (Json::parse((const char*)b.data(), b.size(), st, e) && st.is_object()) m_state = st; }
    return saveCfg(err);
}

bool Config::saveCfg(Str& err)
{
    Str js = m_cfg.dump(true);
    return WriteWholeFile(JoinPath(m_dir, "config.json").c_str(), js.data(), js.size(), err);
}

bool Config::saveHist()
{
    int keep = (int)m_cfg.get("settings").get("max_history").integer(500); if (keep < 10) keep = 10;
    while ((int)m_hist.size() > keep) { Json n = Json::array(); for (size_t i = m_hist.size() - keep; i < m_hist.size(); ++i) n.push(m_hist.at(i)); m_hist = n; }
    Str js = m_hist.dump(), e;
    return WriteWholeFile(JoinPath(m_dir, "history.json").c_str(), js.data(), js.size(), e);
}

// ---- settings --------------------------------------------------------------------------------------------------
Json Config::settings() const { lock(); Json s = m_cfg.get("settings"); unlock(); return s; }

bool Config::setSettings(const Json& patch, Str& err)
{
    if (!patch.is_object()) { err = "settings must be an object"; return false; }
    lock();
    Json s = m_cfg.get("settings");
    for (size_t i = 0; i < patch.size(); ++i) {
        const Str& k = patch.key_at(i);
        if (k == "codec") { if (patch.value_at(i).str() != "xz") { unlock(); err = "only xz compression is supported"; return false; } continue; }   // tolerated, nothing to store
        if (!s.has(k.c_str())) { unlock(); err = "unknown setting: " + k; return false; }
        const Json& v = patch.value_at(i);
        auto cls = [](const Json& x) { return x.is_string() ? 1 : x.is_object() ? 2 : x.is_array() ? 3 : x.is_number() ? 4 : x.is_bool() ? 5 : 0; };
        bool defBool = s.get(k.c_str()).is_bool();                                                         // a switch may be sent as true / false or 1 / 0
        if (defBool ? !(v.is_bool() || v.is_number()) : cls(v) != cls(s.get(k.c_str()))) { unlock(); err = "bad value of " + k; return false; }   // a setting keeps the kind of its value (number / text / object)
        // upper limits: what the engine can really use (kdf: the decryptor refuses more than 100000000 iterations; the pauses are multiplied by 1000 as int; the volume size is shifted by 20)
        if ((k == "retry_base_sec" || k == "retry_max_sec") && v.integer() > 86400) { unlock(); err = "bad value of " + k; return false; }
        if ((k == "kdf_iterations" && v.integer() > 100000000) || (k == "segment_mib" && v.integer() > (1 << 30)) || (k == "misfire_grace_min" && v.integer() > 1000000) ||
            (k == "retry_base_ms" && v.integer() > 86400000) || (k == "speed_limit_mbit" && !(v.num() <= 1e7)) || (k == "max_history" && v.integer() > 10000000)) { unlock(); err = "bad value of " + k; return false; }
        if ((k == "max_parallel" && (v.integer() < 1 || v.integer() > 16)) || (k == "retry_attempts" && (v.integer() < 0 || v.integer() > 100)) ||
            (k == "retry_base_sec" && v.integer() < 1) || (k == "retry_max_sec" && v.integer() < 1) || (k == "speed_limit_mbit" && v.num() < 0) ||
            (k == "segment_mib" && v.integer() != 0 && v.integer() < 1) || (k == "kdf_iterations" && v.integer() < 1000) || (k == "misfire_grace_min" && v.integer() < 0) || (k == "retry_base_ms" && v.integer() < 0)) { unlock(); err = "bad value of " + k; return false; }
        if (k == "level" && (v.integer() < 1 || v.integer() > 9)) { unlock(); err = "the compression level is 1..9"; return false; }
        if (k == "threads" && (v.integer() < 0 || v.integer() > 64)) { unlock(); err = "bad number of compression threads"; return false; }
        if (defBool) s[k.c_str()] = Json(v.boolean()); else s[k.c_str()] = v;
    }
    m_cfg["settings"] = s;
    bool ok = saveCfg(err);
    unlock();
    return ok;
}

bool Config::schedulerEnabled() const { lock(); bool on = m_cfg.get("scheduler").boolean(); unlock(); return on; }
void Config::setSchedulerEnabled(bool on) { lock(); m_cfg["scheduler"] = on; Str e; saveCfg(e); unlock(); }

// ---- servers ---------------------------------------------------------------------------------------------------
static const char* const kSecretServerFields[] = { "password", "key_passphrase", "key_text", nullptr };

Json Config::publicServer(const Json& s) const
{
    Json o = Json::object();
    for (size_t i = 0; i < s.size(); ++i) {
        const Str& k = s.key_at(i); bool secret = false;
        for (const char* const* f = kSecretServerFields; *f; ++f) if (k == *f) secret = true;
        if (secret) o[("has_" + k).c_str()] = !s.value_at(i).str().empty(); else o[k.c_str()] = s.value_at(i);
    }
    return o;
}

Json Config::servers() const
{
    lock();
    Json out = Json::array();
    Json local = Json::object(); local["id"] = kLocalServerId; local["name"] = "Local Computer"; local["type"] = "local";
    local["directory"] = m_cfg.get("settings").get("default_local_path"); local["builtin"] = true;
    out.push(local);
    const Json& a = m_cfg.get("servers");
    for (size_t i = 0; i < a.size(); ++i) out.push(publicServerWithBlock(a.at(i)));
    unlock();
    return out;
}

// ---- the lock-out guard ----------------------------------------------------------------------------------------
static const char kBlocksKey[] = "_auth_blocks";

Str Config::fingerprintOf(const Json& r) const
{
    static const char* const f[] = { "type", "host", "port", "user", "path", "key_file", "password", "key_passphrase", "key_text", nullptr };
    Str all = "vm2b-creds-fp-1";
    for (const char* const* k = f; *k; ++k) { all += '\x1f'; all += *k; all += '='; Json v = r.get(*k); all += v.is_number() ? Str::fmt("%lld", (long long)v.integer()) : v.str(); }
    u8 d[32]; Sha256(all.data(), all.size(), d);
    return HexEncode(d, 16);
}

Str Config::credsFingerprint(const Str& id) const
{
    if (id == kLocalServerId) return Str();
    lock(); Str fp; const Json& a = m_cfg.get("servers");
    for (size_t i = 0; i < a.size(); ++i) if (a.at(i).get("id").str() == id) { fp = fingerprintOf(a.at(i)); break; }
    unlock();
    return fp;
}

bool Config::authBlockLocked(const Str& id, Json& out) const
{
    const Json& b = m_state.get(kBlocksKey).get(id.c_str());
    if (!b.is_object()) return false;
    const Json& a = m_cfg.get("servers");
    for (size_t i = 0; i < a.size(); ++i) if (a.at(i).get("id").str() == id) {
        if (b.get("creds_fp").str() != fingerprintOf(a.at(i))) return false;      // the record was edited since: the mark is void
        out = Json::object(); out["code"] = b.get("code"); out["text"] = b.get("text"); out["time"] = b.get("time"); return true;
    }
    return false;
}

bool Config::authBlock(const Str& id, Json& out) const { lock(); bool r = authBlockLocked(id, out); unlock(); return r; }

void Config::persistState() { Str js = m_state.dump(true), e; WriteWholeFile(JoinPath(m_dir, "state.json").c_str(), js.data(), js.size(), e); }

void Config::setAuthBlock(const Str& id, const Str& fp, int code, const Str& text, i64 when)
{
    if (fp.empty()) return;
    lock();
    const Json& a = m_cfg.get("servers"); bool same = false;
    for (size_t i = 0; i < a.size(); ++i) if (a.at(i).get("id").str() == id && fingerprintOf(a.at(i)) == fp) same = true;
    if (same) {
        Json b = Json::object(); b["code"] = (i64)code; b["text"] = text; b["time"] = when; b["creds_fp"] = fp;
        if (!m_state.get(kBlocksKey).is_object()) m_state[kBlocksKey] = Json::object();
        m_state[kBlocksKey][id.c_str()] = b;
        persistState();
    }
    unlock();
}

bool Config::clearAuthBlock(const Str& id)
{
    lock(); bool had = m_state.get(kBlocksKey).has(id.c_str());
    if (had) { m_state[kBlocksKey].remove(id.c_str()); persistState(); }
    unlock();
    return had;
}

Json Config::publicServerWithBlock(const Json& s) const
{
    Json o = publicServer(s); Json b;
    if (authBlockLocked(s.get("id").str(), b)) o["auth_blocked"] = b;
    return o;
}

bool Config::server(const Str& id, bool reveal, Json& out) const
{
    lock();
    bool found = false;
    if (id == kLocalServerId) {
        out = Json::object(); out["id"] = kLocalServerId; out["name"] = "Local Computer"; out["type"] = "local"; out["builtin"] = true;
        out["directory"] = m_cfg.get("settings").get("default_local_path"); found = true;
    } else {
        const Json& a = m_cfg.get("servers");
        for (size_t i = 0; i < a.size() && !found; ++i) if (a.at(i).get("id").str() == id) {
            found = true;
            if (!reveal) out = publicServer(a.at(i));
            else {
                out = a.at(i);
                for (const char* const* f = kSecretServerFields; *f; ++f) { Str v = out.get(*f).str(), plain; if (!v.empty() && Unprotect(v, plain)) out[*f] = plain; }
            }
        }
    }
    unlock();
    return found;
}

bool Config::saveServer(const Json& in, Json& saved, Str& err)
{
    if (!in.is_object()) { err = "bad server"; return false; }
    Str type = in.get("type").str(), name = in.get("name").str().trim();
    if (type == "local") { err = "the built-in Local Computer cannot be changed"; return false; }
    if (type == "sftp") { err = kSftpGone; return false; }
    if (!IsSource(type) && !IsTarget(type)) { err = "unknown server type: " + type; return false; }
    if (name.empty()) { err = "give the server a name"; return false; }
    if (type == "directory" && in.get("path").str().empty()) { err = "choose the folder that holds the virtual machines"; return false; }
    if (type == "smb") { Str unc, e; if (!SmbNormalizeUnc(in.get("path").str(), unc, e)) { err = "network folder: " + e; return false; } }
    else if (type == "directory" && SmbIsUncPath(in.get("path").str())) { Str unc, e; if (!SmbNormalizeUnc(in.get("path").str(), unc, e)) { err = "folder with virtual machines: " + e; return false; } }
    if (type != "directory" && type != "local" && type != "smb" && in.get("host").str().empty()) { err = "enter the address of the server"; return false; }
    if (in.has("port") && !in.get("port").is_null() && (in.get("port").integer() < 1 || in.get("port").integer() > 65535)) { err = "the port must be 1..65535"; return false; }
    lock();
    Str id = in.get("id").str();
    Json arr = m_cfg.get("servers"); int idx = -1;
    for (size_t i = 0; i < arr.size(); ++i) { if (arr.at(i).get("id").str() == id && !id.empty()) idx = (int)i; else if (arr.at(i).get("name").str() == name) { unlock(); err = "a server named \"" + name + "\" already exists"; return false; } }
    // an edit MERGES into the stored record: what the form did not send (key_file, tls_min, passive, datacenter, transport, ...) is kept;
    // the secrets are handled below (absent or the mask = keep the stored one)
    Json s = Json::object();
    if (idx >= 0 && arr.at((size_t)idx).get("type").str() == type) {
        s = arr.at((size_t)idx);
        for (const char* const* f = kSecretServerFields; *f; ++f) s.remove(*f);
    }
    for (size_t i = 0; i < in.size(); ++i) {
        const Str& k = in.key_at(i);
        if (k.starts_with("has_") || k == "builtin" || k == "auth_blocked") continue;      // computed fields of the public view
        s[k.c_str()] = in.value_at(i);
    }
    s["name"] = name;
    if (type == "smb" || (type == "directory" && SmbIsUncPath(in.get("path").str()))) { Str unc, e; if (SmbNormalizeUnc(in.get("path").str(), unc, e)) s["path"] = unc; s["user"] = in.get("user").str().trim(); }   // stored in the normal form
    if (id.empty() || idx < 0) { if (id.empty()) id = newId(); }
    s["id"] = id;
    for (const char* const* f = kSecretServerFields; *f; ++f) {
        Str v = s.get(*f).str();
        if (!s.has(*f) || v == kMask) { Str old = idx >= 0 ? arr.at((size_t)idx).get(*f).str() : Str(); if (old.empty()) s.remove(*f); else s[*f] = old; }
        else if (!v.empty()) { Str p = Protect(v); if (p.empty()) { unlock(); err = "cannot protect the password"; return false; } s[*f] = p; }
        else s.remove(*f);
    }
    if (idx >= 0) arr.item((size_t)idx) = s; else arr.push(s);
    Json prevServers = m_cfg.get("servers");
    m_cfg["servers"] = arr;
    bool ok = saveCfg(err);
    if (!ok) m_cfg["servers"] = prevServers;                  // a record that could not be written must not stay in memory (the answer says "failed", the list must not show it)
    if (ok) {
        if (m_state.get(kBlocksKey).has(id.c_str()) && m_state.get(kBlocksKey).get(id.c_str()).get("creds_fp").str() != fingerprintOf(s)) { m_state[kBlocksKey].remove(id.c_str()); persistState(); }
        saved = publicServerWithBlock(s);
    }
    unlock();
    return ok;
}

bool Config::deleteServer(const Str& id, Str& err)
{
    if (id == kLocalServerId) { err = "the built-in Local Computer cannot be deleted"; return false; }
    lock();
    Json arr = m_cfg.get("servers"), out = Json::array(); bool found = false;
    for (size_t i = 0; i < arr.size(); ++i) if (arr.at(i).get("id").str() == id) found = true; else out.push(arr.at(i));
    if (!found) { unlock(); err = "server not found"; return false; }
    const Json& ts = m_cfg.get("tasks"); Str users;
    for (size_t i = 0; i < ts.size(); ++i) for (size_t k = 0; k < ts.at(i).get("elements").size(); ++k) {
        const Json& e = ts.at(i).get("elements").at(k);
        if (e.get("source").get("server").str() == id || e.get("target").get("server").str() == id) { if (!users.empty()) users += ", "; users += ts.at(i).get("name").str(); break; }
    }
    if (!users.empty()) { unlock(); err = "the server is used by the tasks: " + users; return false; }
    Json prevServers = m_cfg.get("servers");
    m_cfg["servers"] = out;
    bool ok = saveCfg(err);
    if (!ok) m_cfg["servers"] = prevServers;
    if (ok && m_state.get(kBlocksKey).has(id.c_str())) { m_state[kBlocksKey].remove(id.c_str()); persistState(); }
    unlock();
    return ok;
}

// ---- tasks -----------------------------------------------------------------------------------------------------
bool Config::ValidateElement(Json& el, const Config& cfg, Str& err)
{
    if (!el.is_object()) { err = "bad task element"; return false; }
    if (el.get("type").str("backup") != "backup") { err = "unknown element type: " + el.get("type").str(); return false; }
    el["type"] = "backup";
    if (!el.has("enabled")) el["enabled"] = true;
    Json src, tgt;
    if (!cfg.server(el.get("source").get("server").str(), false, src) || !IsSource(src.get("type").str())) { err = "choose a server with virtual machines as the source"; return false; }
    if (!cfg.server(el.get("target").get("server").str(), false, tgt) || !IsTarget(tgt.get("type").str())) { err = "choose Local Computer, a network folder or an FTP / SCP server as the target"; return false; }
    const Json& vms = el.get("source").get("vms");
    if (!vms.size()) { err = "choose at least one virtual machine"; return false; }
    for (size_t i = 0; i < vms.size(); ++i) { Str v = vms.at(i).str(); if (v.empty() || v.find('/') >= 0 || v.find('\\') >= 0) { err = "bad virtual machine name"; return false; } }
    Str dir = el.get("target").get("directory").str();
    if (vms.size() > 1 && !dir.empty() && dir.find("{VM}") < 0) { err = "for several virtual machines the folder must contain the {VM} tag"; return false; }
    Json comp = el.get("compress"); Str cn = comp.get("codec").str("xz");
    if (!CodecNameOk(cn.c_str())) { err = "only xz compression is supported"; return false; }
    i64 lv = comp.get("level").integer(0); if (lv < 0 || lv > 9) { err = "the compression level is 1..9"; return false; }
    Json nc = Json::object(); nc["codec"] = "xz"; nc["level"] = lv; el["compress"] = nc;
    if (el.get("keep").integer(0) < 0) { err = "the number of backups to keep cannot be negative"; return false; }
    if (el.get("segment_mib").integer(0) < 0 || el.get("segment_mib").integer(0) > (1 << 30)) { err = "bad volume size"; return false; }
    if (el.get("speed_mbit").num(0) < 0) { err = "the speed limit cannot be negative"; return false; }
    if (!(el.get("speed_mbit").num(0) <= 1e7)) { err = "the speed limit is too large"; return false; }
    if (!el.has("verify")) el["verify"] = true;
    if (!el.has("on_error_next")) el["on_error_next"] = true;
    return true;
}

Json Config::publicTask(const Json& t) const
{
    Json o = t;
    Json els = Json::array();
    for (size_t i = 0; i < t.get("elements").size(); ++i) {
        Json e = t.get("elements").at(i);
        Json enc = e.get("encrypt"); Json ne = Json::object(); ne["enabled"] = enc.get("enabled").boolean(); ne["has_password"] = !enc.get("password").str().empty();
        e["encrypt"] = ne;
        els.push(e);
    }
    o["elements"] = els;
    return o;
}

Json Config::tasks() const
{
    lock(); Json out = Json::array();
    const Json& a = m_cfg.get("tasks");
    for (size_t i = 0; i < a.size(); ++i) {
        Json t = publicTask(a.at(i));
        Json st = m_state.get(a.at(i).get("id").str().c_str());
        if (st.is_object()) t["state"] = st;
        out.push(t);
    }
    unlock();
    return out;
}

bool Config::task(const Str& id, bool reveal, Json& out) const
{
    lock(); bool found = false;
    const Json& a = m_cfg.get("tasks");
    for (size_t i = 0; i < a.size() && !found; ++i) if (a.at(i).get("id").str() == id) {
        found = true;
        if (!reveal) out = publicTask(a.at(i));
        else {
            out = a.at(i);
            Json els = Json::array();
            for (size_t k = 0; k < out.get("elements").size(); ++k) {
                Json e = out.get("elements").at(k); Json enc = e.get("encrypt");
                Str pw = enc.get("password").str(), plain; if (!pw.empty() && Unprotect(pw, plain)) { enc["password"] = plain; e["encrypt"] = enc; }
                els.push(e);
            }
            out["elements"] = els;
        }
    }
    unlock();
    return found;
}

bool Config::findTask(const Str& ref, Str& id) const
{
    lock(); bool found = false;
    const Json& a = m_cfg.get("tasks");
    for (size_t i = 0; i < a.size() && !found; ++i) if (a.at(i).get("id").str() == ref || a.at(i).get("name").str() == ref) { id = a.at(i).get("id").str(); found = true; }
    unlock();
    return found;
}

bool Config::saveTask(const Json& in, Json& saved, Str& err)
{
    if (!in.is_object()) { err = "bad task"; return false; }
    Str name = in.get("name").str().trim();
    if (name.empty()) { err = "give the task a title"; return false; }
    Str id = in.get("id").str();
    Json sched = in.get("schedule"); if (!sched.is_object()) { sched = Json::object(); sched["enabled"] = false; }
    if (!ScheduleValidate(sched, err)) return false;
    Json windows = in.get("windows"); if (windows.is_null()) windows = Json::array();
    if (!WindowsValidate(windows, err)) return false;
    lock();
    Json arr = m_cfg.get("tasks"); int idx = -1;
    for (size_t i = 0; i < arr.size(); ++i) { if (!id.empty() && arr.at(i).get("id").str() == id) idx = (int)i; else if (arr.at(i).get("name").str() == name) { unlock(); err = "a task named \"" + name + "\" already exists"; return false; } }
    if (id.empty() || idx < 0) { if (id.empty()) id = newId(); }
    Json old = idx >= 0 ? arr.at((size_t)idx) : Json();
    Json t = Json::object(); t["id"] = id; t["name"] = name; t["enabled"] = in.get("enabled").boolean(true); t["schedule"] = sched; t["windows"] = windows;
    if (sched.get("enabled").boolean() && sched.get("mode").str() == "weekly" && !sched.has("anchor")) { /* the anchor of the weeks is the day it was saved: stable from now on */
        Json s2 = sched; i64 nowMin = UnixToLocalMin((i64)UnixNow()); s2["anchor"] = FormatLocalMin(nowMin - nowMin % 1440).substr(0, 10); t["schedule"] = s2; }
    Json els = Json::array();
    for (size_t i = 0; i < in.get("elements").size(); ++i) {
        Json e = in.get("elements").at(i);
        if (!ValidateElement(e, *this, err)) { unlock(); return false; }
        // a stable id per element: kept when sent (and unique in the task), made once otherwise; the stored password is matched by it, never by position
        Str eid = e.get("id").str();
        for (size_t q = 0; q < els.size() && !eid.empty(); ++q) if (els.at(q).get("id").str() == eid) eid.clear();
        if (eid.empty()) eid = newId();
        e["id"] = eid;
        Json enc = e.get("encrypt"); Json ne = Json::object(); bool on = enc.get("enabled").boolean(); ne["enabled"] = on;
        if (on) {
            Str pw = enc.get("password").str();
            if (!enc.has("password") || pw == kMask || pw.empty()) {                                  // keep the stored password of the element with the same id
                Str oldPw; const Json& oe = old.get("elements");
                if (idx >= 0 && !in.get("elements").at(i).get("id").str().empty()) for (size_t q = 0; q < oe.size(); ++q) if (oe.at(q).get("id").str() == eid) oldPw = oe.at(q).get("encrypt").get("password").str();
                if (oldPw.empty()) { unlock(); err = "enter the password for the encryption"; return false; }
                ne["password"] = oldPw;
            } else { Str p = Protect(pw); if (p.empty()) { unlock(); err = "cannot protect the password"; return false; } ne["password"] = p; }
        }
        e["encrypt"] = ne;
        els.push(e);
    }
    t["elements"] = els;
    if (idx >= 0) arr.item((size_t)idx) = t; else arr.push(t);
    Json prevTasks = m_cfg.get("tasks");
    m_cfg["tasks"] = arr;
    bool ok = saveCfg(err);
    if (!ok) m_cfg["tasks"] = prevTasks;
    if (ok) saved = publicTask(t);
    unlock();
    return ok;
}

bool Config::copyTask(const Str& ref, const Str& nameIn, Json& saved, Str& err)
{
    lock();
    Json arr = m_cfg.get("tasks"); int idx = -1;
    for (size_t i = 0; i < arr.size(); ++i) if (arr.at(i).get("id").str() == ref || arr.at(i).get("name").str() == ref) idx = (int)i;
    if (idx < 0) { unlock(); err = "task not found"; return false; }
    Json t = arr.at((size_t)idx); Str base = nameIn.trim(); if (base.empty()) base = t.get("name").str() + " (copy)";
    Str name = base; for (int k = 2; k < 1000; ++k) { bool taken = false; for (size_t i = 0; i < arr.size(); ++i) if (arr.at(i).get("name").str() == name) taken = true; if (!taken) break; name = base + Str::fmt(" %d", k); }
    t["id"] = newId(); t["name"] = name;
    Json els = Json::array(); for (size_t i = 0; i < t.get("elements").size(); ++i) { Json e = t.get("elements").at(i); e["id"] = newId(); els.push(e); }
    t["elements"] = els;
    arr.push(t); m_cfg["tasks"] = arr;
    bool ok = saveCfg(err);
    if (ok) saved = publicTask(t);
    unlock();
    return ok;
}

bool Config::deleteTask(const Str& id, Str& err)
{
    lock();
    Json arr = m_cfg.get("tasks"), out = Json::array(); bool found = false;
    for (size_t i = 0; i < arr.size(); ++i) if (arr.at(i).get("id").str() == id) found = true; else out.push(arr.at(i));
    if (!found) { unlock(); err = "task not found"; return false; }
    m_cfg["tasks"] = out; m_state.remove(id.c_str());
    Str js = m_state.dump(true), e; WriteWholeFile(JoinPath(m_dir, "state.json").c_str(), js.data(), js.size(), e);
    bool ok = saveCfg(err);
    unlock();
    return ok;
}

Json Config::taskState(const Str& id) const { lock(); Json s = m_state.get(id.c_str()); unlock(); return s.is_object() ? s : Json::object(); }

void Config::setTaskState(const Str& id, const Json& st, bool persist)
{
    lock(); m_state[id.c_str()] = st;
    if (persist) { Str js = m_state.dump(true), e; WriteWholeFile(JoinPath(m_dir, "state.json").c_str(), js.data(), js.size(), e); }
    unlock();
}

void Config::flushState()
{
    lock(); Str js = m_state.dump(true), e; WriteWholeFile(JoinPath(m_dir, "state.json").c_str(), js.data(), js.size(), e); unlock();
}

// ---- history ---------------------------------------------------------------------------------------------------
void Config::addHistory(const Json& rec) { lock(); m_hist.push(rec); saveHist(); unlock(); }

Json Config::history(int limit) const
{
    lock(); Json out = Json::array();
    for (size_t i = m_hist.size(); i-- > 0 && (limit <= 0 || (int)out.size() < limit);) {
        Json r = m_hist.at(i); r.remove("results");
        Json sum = Json::object(); int ok = 0, bad = 0, skipped = 0;
        const Json& rs = m_hist.at(i).get("results");
        for (size_t k = 0; k < rs.size(); ++k) { Str st = rs.at(k).get("status").str(); if (rs.at(k).get("skipped").boolean()) ++skipped; if (st == "Success" || st == "Warning") ++ok; else if (st != "Disabled") ++bad; }
        sum["ok"] = ok; sum["bad"] = bad; sum["count"] = rs.size(); if (skipped) sum["skipped"] = skipped; r["summary"] = sum;
        out.push(r);
    }
    unlock();
    return out;
}

bool Config::historyRecord(const Str& id, Json& out) const
{
    lock(); bool f = false;
    for (size_t i = m_hist.size(); i-- > 0 && !f;) if (m_hist.at(i).get("id").str() == id) { out = m_hist.at(i); f = true; }
    unlock();
    return f;
}

void Config::clearHistory() { lock(); m_hist = Json::array(); saveHist(); unlock(); }

} // namespace vm2b
