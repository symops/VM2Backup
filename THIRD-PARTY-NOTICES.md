# Third-party software in VM2Backup

The libraries are linked statically; versions are pinned in `deps/deps.lock`.

| Library | Use | License |
|---|---|---|
| OpenSSL | TLS client/server, X.509, PKCS#12, SHA-1/256, HMAC, PBKDF2, AES-256-GCM, random numbers | Apache License 2.0 (https://www.openssl.org/source/license.html) |
| libssh2 | SSH-2 client for SCP (four small memory-safety fixes for damaged key files, see tools/deps/libssh2.sh) | BSD 3-Clause (https://libssh2.org/license.html) |
| liblzma (xz) | xz / LZMA2 compression | 0BSD / public domain (https://tukaani.org/xz/) |

OpenSSL: "This product includes software developed by the OpenSSL Project" is no longer required (Apache-2.0 since 3.0); the
license text and NOTICE are in the OpenSSL source tarball named in `deps/deps.lock`.
