#include "throttle.h"

namespace vm2b {

static double NowSeconds()
{
    LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}

bool RateLimiter::acquire(size_t n, bool (*aborted)(void*), void* ctx)
{
    for (;;) {
        EnterCriticalSection(&m_cs);
        u64 limit = m_limit; LONG gen = m_gen;
        if (!limit) { m_next = 0; LeaveCriticalSection(&m_cs); return !(aborted && aborted(ctx)); }
        double now = NowSeconds();
        // idle time is not saved up: after a pause a burst of at most 0.25 s of data goes at once, never more
        double start = m_next < now - 0.25 ? now - 0.25 : m_next;
        m_next = start + (double)n / (double)limit;
        double due = start;
        LeaveCriticalSection(&m_cs);
        bool changed = false;
        for (;;) {
            double wait = due - NowSeconds();
            if (wait <= 0) break;
            if (m_gen != gen) { changed = true; break; }            // the limit was changed (lifted: a window ended; raised or lowered): do not sleep out the old slot
            if (aborted && aborted(ctx)) return false;
            Sleep(wait > 0.1 ? 100 : (DWORD)(wait * 1000) + 1);
        }
        if (!changed) return !(aborted && aborted(ctx));
    }
}

} // namespace vm2b
