// VM2Backup — cryptography on OpenSSL (libcrypto, static): hashes, HMAC, PBKDF2, AES-GCM, random numbers, and the stream
// encryption of backups.
//
// Backup encryption format "V2BENC3" — one per volume file of a backup (see engine.h):
//   "V2BENC3\n" | salt(16) | iterations(4, big endian) | volume index(4, big endian) | blocks...
//   block = length(4, big endian) | AES-256-GCM(ciphertext || 16-byte tag) of up to 1 MiB of data
//   key = PBKDF2-HMAC-SHA256(password, salt, iterations) (the same for all volumes of a backup);
//   nonce = volume index(4) || block index(8, big endian); additional data = 0x00 (more blocks follow in this volume),
//   0x01 (the last block of the volume) or 0x02 (the last block of the last volume of the backup). The indexes and the
//   last-block flags are authenticated, so a removed, reordered, swapped or cut-off block / volume is detected.
#pragma once
#include "stream.h"

namespace vm2b {

bool RandomBytes(void* p, size_t n);

void Sha256(const void* p, size_t n, u8 out[32]);
void Sha1(const void* p, size_t n, u8 out[20]);
void HmacSha256(const void* key, size_t klen, const void* data, size_t n, u8 out[32]);
bool Pbkdf2Sha256(const void* pw, size_t pwlen, const void* salt, size_t slen, u32 iterations, u8* out, size_t outlen);

class Sha256Stream {                       // incremental SHA-256
public:
    Sha256Stream();
    ~Sha256Stream();
    void update(const void* p, size_t n);
    void final(u8 out[32]);
private:
    void* m_h;
};

class AesGcm {
public:
    AesGcm();
    ~AesGcm();
    bool init(const u8 key[32]);
    // out has n bytes; tag 16 bytes. in/out may be the same buffer.
    bool encrypt(const u8 nonce[12], const void* aad, size_t aadLen, const void* in, size_t n, void* out, u8 tag[16]);
    bool decrypt(const u8 nonce[12], const void* aad, size_t aadLen, const void* in, size_t n, void* out, const u8 tag[16]);   // false: wrong tag
private:
    void* m_key;                 // EVP_CIPHER_CTX*
    void* m_obj;                 // the 32-byte key
};

enum { kEncIterationsDefault = 600000 };

// PBKDF2 key of a backup; the last few results are cached (a backup of 1000 volumes derives the key once).
bool DeriveBackupKey(const Str& password, const u8 salt[16], u32 iterations, u8 key[32]);

class EncryptSink : public Sink {
public:
    // salt: 16 bytes shared by all volumes of one backup (null = new random ones); volume: this volume's index.
    EncryptSink(Sink* out, const Str& password, u32 iterations = kEncIterationsDefault, const u8* salt = nullptr, u32 volume = 0);   // `out` is not owned
    ~EncryptSink();
    void setLastVolume(bool last) { m_lastVolume = last; }        // default true (a single-volume archive); call before finish()
    const u8* salt() const { return m_salt; }
    bool write(const void* p, size_t n) override;
    bool finish() override;
    void abort() override { m_out->abort(); }
private:
    bool emit(bool last);
    Sink* m_out; AesGcm m_aes; Bytes m_buf; u32 m_volume; u64 m_index; bool m_ok, m_done, m_header, m_lastVolume;
    Str m_pw; u32 m_iter; u8 m_salt[16];
};

class DecryptSource : public Source {
public:
    DecryptSource(Source* in, const Str& password, u32 volume = 0);                         // `in` is not owned; volume = the index it must carry
    i64 read(void* p, size_t n) override;
    bool wrongPassword() const { return m_wrongPw; }
    bool finalVolume() const { return m_final; }                 // valid at the end of data: the volume was marked as the last of the backup
private:
    bool nextBlock();
    Source* m_in; Str m_pw; AesGcm m_aes; Bytes m_plain; size_t m_pos; u32 m_volume; u64 m_index; bool m_hdr, m_eof, m_wrongPw, m_failed, m_final;
};

// Feeds everything through unchanged and keeps the SHA-256 and the size of what passed (the manifest of a backup).
class HashSink : public Sink {
public:
    explicit HashSink(Sink* out) : m_out(out), m_size(0), m_done(false) { memset(m_digest, 0, 32); }
    bool write(const void* p, size_t n) override;
    bool finish() override;
    void abort() override { m_out->abort(); }
    u64 size() const { return m_size; }
    Str hex() const { return HexEncode(m_digest, 32); }          // valid after finish()
private:
    Sink* m_out; Sha256Stream m_sha; u64 m_size; bool m_done; u8 m_digest[32];
};

class HashSource : public Source {
public:
    explicit HashSource(Source* in) : m_in(in), m_size(0), m_done(false), m_eof(false) { memset(m_digest, 0, 32); }
    i64 read(void* p, size_t n) override;
    bool sawEof() const { return m_eof; }                        // the data ended (as opposed to a read error)
    void drain();                                                // reads to the end (the hash covers the whole file)
    u64 size() const { return m_size; }
    Str hex();                                                   // finalizes
private:
    Source* m_in; Sha256Stream m_sha; u64 m_size; bool m_done, m_eof; u8 m_digest[32];
};

} // namespace vm2b
