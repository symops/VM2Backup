// VM2Backup — where backups are stored. A target is a flat folder of files: local / network folder, FTP / FTPS, SCP.
// A write is atomic for the reader: the data goes to "<name>.part" and takes its name when the sink is finished; an aborted
// write leaves nothing behind.
#pragma once
#include "stream.h"

namespace vm2b {

struct Target {
    Str err;
    // true when the last error is of the kind that may pass if tried again: the connection was lost / reset / timed out, the
    // server said "try later" (FTP 4xx), a data transfer broke off. Permanent errors (wrong password, no such folder, access
    // denied, disk full, a bad host key / certificate) leave it false. The engine retries and resumes only transient failures.
    bool transient;
    Target() : transient(false) {}
    virtual ~Target() {}
    virtual const char* kind() const = 0;
    virtual bool connect() = 0;                                  // opens the connection, creates the folder; may be called again after a failure
    virtual void close() {}
    virtual bool reconnect() { close(); return connect(); }      // after a transient failure: a fresh connection
    virtual Sink* open_write(const Str& name) = 0;               // owned by the caller; finish() commits, abort() discards
    virtual Source* open_read(const Str& name) = 0;              // owned by the caller
    virtual bool list(Vec<Str>& names) = 0;                      // the file names in the folder (no ".part" temporaries)
    virtual bool remove(const Str& name) = 0;                    // a missing file is not an error
    virtual bool exists(const Str& name, bool& yes);
    virtual bool free_space(u64& bytes) { (void)bytes; return false; }   // free bytes for the account, when the target can tell (local / network folder)
    bool put_bytes(const Str& name, const void* p, size_t n);
    bool get_bytes(const Str& name, Bytes& out, size_t maxSize = (size_t)64 << 20);
protected:
    bool fail(const Str& e, bool temp = false) { err = e; transient = temp; return false; }
};

// Builds a target from its configuration:
//   {"type":"local","path":"D:\\Backups"}
//   {"type":"ftp"|"ftpes"|"ftps","host":..,"port":..,"user":..,"password":..,"directory":"/x","verify":true,"fingerprint":"sha256:..","passive":true}
//   {"type":"smb","path":"\\\\host\\share\\dir","user":"DOMAIN\\u","password":..,"directory":"sub\\folder"}   (network folder, see smb.h)
//   {"type":"scp","host":..,"port":22,"user":..,"password":..,"key_file":..,"key_passphrase":..,"directory":"x","host_key":"SHA256:.."}
// Returns nullptr and sets err when the type is unknown or the configuration is incomplete.
Target* MakeTarget(const Json& cfg, Str& err);

Target* NewLocalTarget(const Json& cfg, Str& err);
Target* NewFtpTarget(const Json& cfg, Str& err);
Target* NewSshTarget(const Json& cfg, Str& err);         // scp (type "sftp" of old configs: MakeTarget refuses it with a clear message)

} // namespace vm2b
