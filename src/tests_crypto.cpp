// Autotests of the OpenSSL-backed crypto: the V2BENC3 format must stay byte-identical to what the original (Windows CNG) code wrote.
#include "autotest.h"
#include "crypto.h"

namespace vm2b {

// Fixed vector made by an independent implementation (Python: hashlib PBKDF2 + `cryptography` AES-GCM):
// password "pw-\xc3\xbc", salt 00 01 .. 0f, 2000 iterations, volume 3, two blocks (1 MiB flag 0, 10 bytes flag 2).
VM2B_TEST("core", crypto_v2benc3_fixed_vector)
{
    Bytes plain; plain.resize((1 << 20) + 10);
    for (size_t i = 0; i < plain.size(); ++i) plain[i] = (u8)(i * 7 + i / 251);
    u8 salt[16]; for (int i = 0; i < 16; ++i) salt[i] = (u8)i;
    Str pw = "pw-\xc3\xbc";
    MemSink ms;
    {
        EncryptSink es(&ms, pw, 2000, salt, 3);
        T_CHECK(es.write(plain.data(), 1000)); T_CHECK(es.write(plain.data() + 1000, plain.size() - 1000)); T_CHECK(es.finish());
    }
    Bytes enc = ms.data;
    u8 h[32]; Sha256(enc.data(), enc.size(), h);
    T_CHECK(enc.size() == 1048658);
    T_CHECK(HexEncode(h, 32) == "d99ef932512b6f0299fec6e75af82b76dd38baf7fe37c6279737e686b68447b7");
    Sha256(plain.data(), plain.size(), h);
    T_CHECK(HexEncode(h, 32) == "78cdad0dc0c6576184fb7075c98a8c5b1b5b4d3dcde135b27b2effbbdd56f6d7");
    // and the other way: decrypt what the independent implementation would have written (same bytes)
    MemSource src(enc.data(), enc.size());
    DecryptSource ds(&src, pw, 3);
    Bytes back; back.resize(plain.size() + 5);
    i64 got = ReadFull(ds, back.data(), back.size());
    T_CHECK(got == (i64)plain.size()); T_CHECK(memcmp(back.data(), plain.data(), plain.size()) == 0); T_CHECK(ds.finalVolume());
    // wrong password / flipped bit are detected
    MemSource s2(enc.data(), enc.size()); DecryptSource d2(&s2, "other", 3); u8 b1[16];
    T_CHECK(d2.read(b1, 16) < 0 && d2.wrongPassword());
    enc[100000] ^= 1; MemSource s3(enc.data(), enc.size()); DecryptSource d3(&s3, pw, 3);
    T_CHECK(ReadFull(d3, back.data(), back.size()) != (i64)plain.size());
    return true;
}

VM2B_TEST("core", crypto_primitives_misc)
{
    u8 a[20], b[32];
    Sha1("abc", 3, a); T_CHECK(HexEncode(a, 20) == "a9993e364706816aba3e25717850c26c9cd0d89d");
    HmacSha256("", 0, "", 0, b); T_CHECK(HexEncode(b, 32) == "b613679a0814d9ec772f95d778c35fc5ff1697c493715653c6c712144292c5ad");
    u8 r1[32], r2[32]; T_CHECK(RandomBytes(r1, 32)); T_CHECK(RandomBytes(r2, 32)); T_CHECK(memcmp(r1, r2, 32) != 0);
    return true;
}

} // namespace vm2b
