// Server-side TLS (OpenSSL) and the certificate of the administration web site, see webtls.h.
#include "webtls.h"
#include "crypto.h"
#include "config.h"
#include "winutil.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/pkcs12.h>
#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/param_build.h>
#include <openssl/provider.h>

namespace vm2b {

namespace {

// The identity on disk: {certificate DER, private key protected with DPAPI}.  The protected text is "pkey:" + base64(DER of the key,
// as written by i2d_PrivateKey); older versions stored the base64 of a CryptoAPI RSA PRIVATEKEYBLOB, which is still read.
const char kKeyTag[] = "pkey:";

Str ProtectKey(EVP_PKEY* k)
{
    int n = i2d_PrivateKey(k, nullptr); if (n <= 0) return Str();
    Bytes der; der.resize((size_t)n); unsigned char* p = der.data();
    if (i2d_PrivateKey(k, &p) != n) return Str();
    Str s = Config::Protect(Str(kKeyTag) + Base64Encode(der.data(), der.size()));
    OPENSSL_cleanse(der.data(), der.size());
    return s;
}

// RSA PRIVATEKEYBLOB (little endian: BLOBHEADER 8, RSAPUBKEY 12, modulus, prime1, prime2, exp1, exp2, coefficient, private exponent).
EVP_PKEY* KeyFromLegacyBlob(const Bytes& b)
{
    if (b.size() < 20 || b[0] != 0x07 || memcmp(b.data() + 8, "RSA2", 4) != 0) return nullptr;
    u32 bits = (u32)b[12] | (u32)b[13] << 8 | (u32)b[14] << 16 | (u32)b[15] << 24, e = (u32)b[16] | (u32)b[17] << 8 | (u32)b[18] << 16 | (u32)b[19] << 24;
    size_t m = bits / 8, h = bits / 16;
    if (bits < 512 || bits > 16384 || bits % 16 || b.size() < 20 + 2 * m + 5 * h) return nullptr;
    const u8* p = b.data() + 20;
    BIGNUM* bn[8]; const char* names[8] = { OSSL_PKEY_PARAM_RSA_N, OSSL_PKEY_PARAM_RSA_FACTOR1, OSSL_PKEY_PARAM_RSA_FACTOR2, OSSL_PKEY_PARAM_RSA_EXPONENT1,
                                            OSSL_PKEY_PARAM_RSA_EXPONENT2, OSSL_PKEY_PARAM_RSA_COEFFICIENT1, OSSL_PKEY_PARAM_RSA_D, OSSL_PKEY_PARAM_RSA_E };
    size_t lens[8] = { m, h, h, h, h, h, m, 0 };
    OSSL_PARAM_BLD* bld = OSSL_PARAM_BLD_new(); bool ok = bld != nullptr; EVP_PKEY* pk = nullptr;
    for (int i = 0; i < 8; ++i) bn[i] = nullptr;
    for (int i = 0; ok && i < 7; ++i) { bn[i] = BN_lebin2bn(p, (int)lens[i], nullptr); p += lens[i]; ok = bn[i] && OSSL_PARAM_BLD_push_BN(bld, names[i], bn[i]); }
    bn[7] = BN_new(); ok = ok && bn[7] && BN_set_word(bn[7], e) && OSSL_PARAM_BLD_push_BN(bld, names[7], bn[7]);
    OSSL_PARAM* par = ok ? OSSL_PARAM_BLD_to_param(bld) : nullptr;
    EVP_PKEY_CTX* ctx = par ? EVP_PKEY_CTX_new_from_name(nullptr, "RSA", nullptr) : nullptr;
    if (ctx && EVP_PKEY_fromdata_init(ctx) == 1) EVP_PKEY_fromdata(ctx, &pk, EVP_PKEY_KEYPAIR, par);
    EVP_PKEY_CTX_free(ctx); OSSL_PARAM_free(par); OSSL_PARAM_BLD_free(bld);
    for (int i = 0; i < 8; ++i) BN_clear_free(bn[i]);
    return pk;
}

EVP_PKEY* UnprotectKey(const Str& s)
{
    Str plain; if (!Config::Unprotect(s, plain)) return nullptr;
    EVP_PKEY* k = nullptr;
    if (plain.starts_with(kKeyTag)) {
        Bytes der; Str b64 = plain.substr(sizeof(kKeyTag) - 1);
        if (Base64Decode(b64.data(), b64.size(), der)) { const unsigned char* p = der.data(); k = d2i_AutoPrivateKey(nullptr, &p, (long)der.size()); OPENSSL_cleanse(der.data(), der.size()); }
    } else {
        Bytes blob; if (Base64Decode(plain.data(), plain.size(), blob)) k = KeyFromLegacyBlob(blob);
    }
    if (!plain.empty()) OPENSSL_cleanse((void*)plain.data(), plain.size());
    return k;
}

Str MachineName()
{
    wchar_t b[256]; DWORD n = 256;
    if (GetComputerNameExW(ComputerNameDnsFullyQualified, b, &n) && n) return FromWide(b);
    n = 256;
    if (GetComputerNameW(b, &n)) return FromWide(b);
    return Str("localhost");
}

bool IsIp(const Str& s) { in_addr a4; in6_addr a6; return inet_pton(AF_INET, s.c_str(), &a4) == 1 || inet_pton(AF_INET6, s.c_str(), &a6) == 1; }

X509* ParseCert(const Bytes& der)
{
    const unsigned char* p = der.data();
    return d2i_X509(nullptr, &p, (long)der.size());
}

} // namespace

Str WebTlsFingerprint(const Bytes& der)
{
    u8 h[32]; Sha256(der.data(), der.size(), h);
    return "sha256:" + HexEncode(h, 32);
}

bool WebTlsGenerate(const Vec<Str>& namesIn, WebTlsIdentity& id, Str& err)
{
    // always: the machine name + localhost + loopback addresses, then the chosen names
    Vec<Str> names; names.push_back(MachineName()); names.push_back("localhost"); names.push_back("127.0.0.1"); names.push_back("::1");
    for (const Str& n : namesIn) { bool d = false; for (const Str& x : names) if (x == n) d = true; if (!d) names.push_back(n); }
    // names must be plausible host names or IP literals (they end up in a certificate)
    for (const Str& n : names) {
        bool ok = !n.empty() && n.size() <= 253;
        for (size_t i = 0; ok && i < n.size(); ++i) { char c = n[i]; ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '-' || c == '_' || c == ':' || c == '*'; }
        if (ok && n.find(":") >= 0 && !IsIp(n)) ok = false;
        if (!ok) { err = "a certificate name is not a valid host name or address"; return false; }
    }
    Str san;
    for (const Str& n : names) { if (!san.empty()) san += ","; san += (IsIp(n) ? "IP:" : "DNS:") + n; }
    bool ok = false;
    EVP_PKEY* key = nullptr; X509* x = nullptr;
    do {
        key = EVP_EC_gen("P-256");                      // ECDSA P-256: fast, small, accepted by every browser
        if (!key) { err = "cannot generate the key"; break; }
        x = X509_new();
        if (!x || !X509_set_version(x, 2)) { err = "cannot create the certificate"; break; }
        u8 sn[16]; if (!RandomBytes(sn, 16)) { err = "no random numbers"; break; }
        sn[0] &= 0x7f; sn[0] |= 0x01;
        BIGNUM* bn = BN_bin2bn(sn, 16, nullptr); ASN1_INTEGER* ai = bn ? BN_to_ASN1_INTEGER(bn, nullptr) : nullptr;
        bool s1 = ai && X509_set_serialNumber(x, ai); ASN1_INTEGER_free(ai); BN_free(bn);
        if (!s1) { err = "cannot set the serial number"; break; }
        X509_NAME* nm = X509_get_subject_name(x);
        Str cn = "VM2Backup " + names[0];
        if (!X509_NAME_add_entry_by_txt(nm, "CN", MBSTRING_UTF8, (const unsigned char*)cn.c_str(), -1, -1, 0) || !X509_set_issuer_name(x, nm)) { err = "bad certificate subject"; break; }
        // a day back (clients with a slightly slow clock), 825 days (the longest a public CA may issue; some clients refuse more)
        if (!X509_gmtime_adj(X509_getm_notBefore(x), -24 * 3600) || !X509_gmtime_adj(X509_getm_notAfter(x), (long)825 * 24 * 3600)) { err = "bad validity"; break; }
        if (!X509_set_pubkey(x, key)) { err = "cannot set the public key"; break; }
        X509V3_CTX c; X509V3_set_ctx_nodb(&c); X509V3_set_ctx(&c, x, x, nullptr, nullptr, 0);
        struct E { int nid; const char* v; } ex[] = { { NID_subject_alt_name, san.c_str() }, { NID_basic_constraints, "critical,CA:FALSE" },
            { NID_key_usage, "critical,digitalSignature" }, { NID_ext_key_usage, "serverAuth" }, { NID_subject_key_identifier, "hash" } };
        bool ext = true;
        for (const E& e : ex) {
            X509_EXTENSION* xe = X509V3_EXT_conf_nid(nullptr, &c, e.nid, e.v);
            if (!xe || !X509_add_ext(x, xe, -1)) ext = false;
            X509_EXTENSION_free(xe);
        }
        if (!ext) { err = "cannot encode the certificate extensions"; break; }
        if (!X509_sign(x, key, EVP_sha256())) { err = "cannot sign the certificate"; break; }
        int n = i2d_X509(x, nullptr); if (n <= 0) { err = "cannot encode the certificate"; break; }
        id.certDer.resize((size_t)n); unsigned char* p = id.certDer.data(); i2d_X509(x, &p);
        id.keyProtected = ProtectKey(key);
        ok = !id.keyProtected.empty();
        if (!ok) err = "cannot protect the private key (DPAPI)";
    } while (false);
    X509_free(x); EVP_PKEY_free(key);
    ERR_clear_error();
    return ok;
}

bool WebTlsImportPfx(const Str& path, const Str& password, WebTlsIdentity& id, Str& err)
{
    Bytes data; if (!ReadWholeFile(path.c_str(), data, err, 4 << 20)) return false;
    // PFX files of old tools use RC2 / 3DES: the legacy provider (built in, static) next to the default one
    static bool provs = (OSSL_PROVIDER_load(nullptr, "default"), OSSL_PROVIDER_load(nullptr, "legacy"), ERR_clear_error(), true); (void)provs;
    const unsigned char* dp = data.data();
    PKCS12* p12 = d2i_PKCS12(nullptr, &dp, (long)data.size());
    if (!p12) { ERR_clear_error(); err = "the file is not a PFX / PKCS#12 file"; return false; }
    EVP_PKEY* key = nullptr; X509* cert = nullptr; STACK_OF(X509)* ca = nullptr;
    bool ok = false;
    if (!PKCS12_parse(p12, password.c_str(), &key, &cert, &ca)) err = "cannot open the PFX (wrong password or unsupported format)";
    else if (!cert || !key) err = "the PFX holds no certificate with a private key";
    else {
        int n = i2d_X509(cert, nullptr);
        if (n <= 0) err = "the certificate cannot be read";
        else {
            id.certDer.resize((size_t)n); unsigned char* p = id.certDer.data(); i2d_X509(cert, &p);
            id.keyProtected = ProtectKey(key);
            ok = !id.keyProtected.empty();
            if (!ok) err = "cannot protect the private key (DPAPI)";
        }
    }
    EVP_PKEY_free(key); X509_free(cert); sk_X509_pop_free(ca, X509_free); PKCS12_free(p12);
    ERR_clear_error();
    return ok;
}

Json WebTlsDescribe(const Bytes& der)
{
    Json o = Json::object();
    X509* x = ParseCert(der);
    if (!x) return o;
    char cn[256] = "";
    if (X509_NAME_get_text_by_NID(X509_get_subject_name(x), NID_commonName, cn, sizeof(cn)) > 0) o["subject"] = Str(cn);
    ASN1_TIME* epoch = ASN1_TIME_set(nullptr, 0);
    auto unixOf = [&](const ASN1_TIME* t) { int d = 0, s = 0; if (!ASN1_TIME_diff(&d, &s, epoch, t)) return (i64)0; return (i64)d * 86400 + s; };
    o["not_before"] = unixOf(X509_get0_notBefore(x)); o["not_after"] = unixOf(X509_get0_notAfter(x));
    ASN1_STRING_free((ASN1_STRING*)epoch);
    Json names = Json::array();
    GENERAL_NAMES* gn = (GENERAL_NAMES*)X509_get_ext_d2i(x, NID_subject_alt_name, nullptr, nullptr);
    for (int i = 0; gn && i < sk_GENERAL_NAME_num(gn); ++i) {
        const GENERAL_NAME* g = sk_GENERAL_NAME_value(gn, i);
        if (g->type == GEN_DNS) names.push(Json(Str((const char*)ASN1_STRING_get0_data(g->d.dNSName), (size_t)ASN1_STRING_length(g->d.dNSName))));
        else if (g->type == GEN_IPADD) {
            char t[64] = ""; int l = ASN1_STRING_length(g->d.iPAddress); const unsigned char* a = ASN1_STRING_get0_data(g->d.iPAddress);
            if (l == 4) inet_ntop(AF_INET, (void*)a, t, sizeof(t)); else if (l == 16) inet_ntop(AF_INET6, (void*)a, t, sizeof(t));
            names.push(Json(Str(t)));
        }
    }
    GENERAL_NAMES_free(gn);
    o["names"] = names;
    X509_free(x);
    return o;
}

// ---- server context -------------------------------------------------------------------------------------------------------
WebTlsServer::WebTlsServer() : m_ctx(nullptr) {}
WebTlsServer::~WebTlsServer() { if (m_ctx) SSL_CTX_free((SSL_CTX*)m_ctx); }

bool WebTlsServer::init(const WebTlsIdentity& id, Str& err)
{
    EVP_PKEY* key = UnprotectKey(id.keyProtected);
    if (!key) { err = "cannot unprotect the private key of the certificate (DPAPI)"; return false; }
    X509* x = ParseCert(id.certDer);
    if (!x) { EVP_PKEY_free(key); err = "the stored certificate is damaged"; return false; }
    SSL_CTX* c = SSL_CTX_new(TLS_server_method());
    bool ok = c != nullptr;
    if (ok) {
        SSL_CTX_set_min_proto_version(c, TLS1_2_VERSION); SSL_CTX_set_max_proto_version(c, TLS1_3_VERSION);
        // TLS 1.2: forward secrecy + AEAD only; TLS 1.3 suites are all strong
        SSL_CTX_set_cipher_list(c, "ECDHE+AESGCM:ECDHE+CHACHA20");
        SSL_CTX_set_options(c, SSL_OP_NO_RENEGOTIATION | SSL_OP_CIPHER_SERVER_PREFERENCE | SSL_OP_NO_TICKET | SSL_OP_NO_COMPRESSION);
        SSL_CTX_set_mode(c, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER | SSL_MODE_AUTO_RETRY);
        SSL_CTX_set_verify(c, SSL_VERIFY_NONE, nullptr);                       // no client certificates
        SSL_CTX_set_session_cache_mode(c, SSL_SESS_CACHE_OFF);
        ok = SSL_CTX_use_certificate(c, x) == 1 && SSL_CTX_use_PrivateKey(c, key) == 1 && SSL_CTX_check_private_key(c) == 1;
        if (!ok) err = "the certificate and the private key do not belong together";
    } else err = "cannot set up TLS (SSL_CTX_new)";
    X509_free(x); EVP_PKEY_free(key);
    ERR_clear_error();
    if (!ok) { if (c) SSL_CTX_free(c); return false; }
    if (m_ctx) SSL_CTX_free((SSL_CTX*)m_ctx);
    m_ctx = c;
    m_fp = WebTlsFingerprint(id.certDer);
    return true;
}

// ---- a connection ------------------------------------------------------------------------------------------------------------
WebTlsConn::WebTlsConn() : m_sock((size_t)INVALID_SOCKET), m_ssl(nullptr), m_open(false), m_eof(false) {}
WebTlsConn::~WebTlsConn() { if (m_ssl) SSL_free((SSL*)m_ssl); }

int WebTlsConn::waitSock(bool wr, int ms)
{
    fd_set fs; FD_ZERO(&fs); FD_SET((SOCKET)m_sock, &fs);
    timeval tv; tv.tv_sec = ms / 1000; tv.tv_usec = (ms % 1000) * 1000;
    int r = select(0, wr ? nullptr : &fs, wr ? &fs : nullptr, nullptr, &tv);
    return r > 0 ? 1 : r == 0 ? 0 : -1;
}

bool WebTlsConn::accept(size_t sock, WebTlsServer& srv, int deadlineMs, const volatile long* stop, Str& err)
{
    m_sock = sock;
    SSL* ssl = SSL_new((SSL_CTX*)srv.handle());
    if (!ssl) { err = "cannot set up TLS (SSL_new)"; return false; }
    m_ssl = ssl;
    u_long nb = 1; ioctlsocket((SOCKET)sock, FIONBIO, &nb);              // the select() timeouts below do the waiting
    SSL_set_fd(ssl, (int)sock);
    DWORD t0 = GetTickCount();
    for (;;) {
        int r = SSL_accept(ssl);
        if (r == 1) break;
        int e = SSL_get_error(ssl, r);
        if (e != SSL_ERROR_WANT_READ && e != SSL_ERROR_WANT_WRITE) {
            unsigned long en = ERR_get_error(); ERR_clear_error();
            const char* t = en ? ERR_reason_error_string(en) : nullptr;
            err = Str("TLS handshake failed") + (t ? Str(": ") + t : Str());
            if (e == SSL_ERROR_SYSCALL || (en && ERR_GET_REASON(en) == SSL_R_UNEXPECTED_EOF_WHILE_READING)) err = "the client closed the connection during the TLS handshake";
            return false;
        }
        int left = deadlineMs - (int)(GetTickCount() - t0);
        if (left <= 0 || (stop && *stop)) { err = "TLS handshake timed out"; return false; }
        int w = waitSock(e == SSL_ERROR_WANT_WRITE, left < 500 ? left : 500);
        if (w < 0) { err = "TLS handshake: socket error"; return false; }
    }
    m_ver = SSL_version(ssl) == TLS1_3_VERSION ? "TLS 1.3" : "TLS 1.2";
    const SSL_CIPHER* ci = SSL_get_current_cipher(ssl);
    m_cipher = ci ? SSL_CIPHER_get_name(ci) : "";
    m_open = true;
    return true;
}

i64 WebTlsConn::recv(void* p, size_t n, int timeoutMs)
{
    if (!m_open || !n) return -1;
    SSL* ssl = (SSL*)m_ssl;
    if (m_eof) return 0;
    DWORD t0 = GetTickCount();
    for (;;) {
        size_t got = 0;
        if (SSL_read_ex(ssl, p, n > (1u << 20) ? (1u << 20) : n, &got) == 1) return (i64)got;
        int e = SSL_get_error(ssl, 0);
        if (e == SSL_ERROR_ZERO_RETURN) { m_eof = true; return 0; }
        if (e != SSL_ERROR_WANT_READ && e != SSL_ERROR_WANT_WRITE) {
            unsigned long en = ERR_get_error(); ERR_clear_error();
            if (e == SSL_ERROR_SYSCALL || (en && ERR_GET_REASON(en) == SSL_R_UNEXPECTED_EOF_WHILE_READING)) { m_eof = true; return 0; }   // FIN without close_notify
            return -1;
        }
        int left = timeoutMs - (int)(GetTickCount() - t0); if (left < 0) left = 0;
        int w = waitSock(e == SSL_ERROR_WANT_WRITE, left);
        if (w < 0) return -1;
        if (w == 0) return kTlsTimeout;
    }
}

bool WebTlsConn::send(const void* p, size_t n, int timeoutMs)
{
    if (!m_open) return false;
    SSL* ssl = (SSL*)m_ssl;
    const u8* d = (const u8*)p;
    while (n) {
        size_t wr = 0;
        if (SSL_write_ex(ssl, d, n > (1u << 20) ? (1u << 20) : n, &wr) == 1) { d += wr; n -= wr; continue; }
        int e = SSL_get_error(ssl, 0);
        if (e != SSL_ERROR_WANT_READ && e != SSL_ERROR_WANT_WRITE) { ERR_clear_error(); return false; }
        if (waitSock(e == SSL_ERROR_WANT_WRITE, timeoutMs) <= 0) return false;
    }
    return true;
}

void WebTlsConn::shutdown()
{
    if (!m_open || !m_ssl) return;
    m_open = false;
    SSL_shutdown((SSL*)m_ssl);            // close_notify; the socket stays the caller's
    ERR_clear_error();
}

} // namespace vm2b
