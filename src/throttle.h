// VM2Backup — bandwidth limit. One RateLimiter can be shared by every transfer of a job (or of the whole service); the limit
// can be changed while they run (the schedule windows do that: "10 Mbit/s in working hours, unlimited at night").
#pragma once
#include "base.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace vm2b {

class RateLimiter {
public:
    RateLimiter() : m_limit(0), m_next(0), m_gen(0) { InitializeCriticalSection(&m_cs); }
    ~RateLimiter() { DeleteCriticalSection(&m_cs); }
    // 0 = unlimited. A real change drops the queue of the old limit (m_next) and wakes the waiters, which ask again at the new one: raising a
    // 1 kbit/s limit must not leave a transfer sleeping out a slot of the old limit (hours for one chunk).
    void setBytesPerSecond(u64 bps) { EnterCriticalSection(&m_cs); if (bps != m_limit) { m_limit = bps; m_next = 0; m_gen = m_gen + 1; } LeaveCriticalSection(&m_cs); }
    void setMbit(double mbit) { u64 b = mbit <= 0 ? 0 : mbit >= 1e12 ? (u64)1 << 60 : (u64)(mbit * 125000.0); setBytesPerSecond(mbit > 0 && b == 0 ? 1 : b); }   // a tiny positive limit is not "unlimited"
    u64 bytesPerSecond() const { return m_limit; }
    // Blocks until `n` more bytes may pass at the current limit. `aborted(ctx)` is polled while waiting; false = aborted.
    bool acquire(size_t n, bool (*aborted)(void*), void* ctx);
private:
    CRITICAL_SECTION m_cs; u64 m_limit; double m_next; volatile LONG m_gen;      // m_next: the time (seconds) from which the next byte may pass
};

} // namespace vm2b
