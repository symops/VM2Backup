// The little of the C++ runtime the programs use (new / delete), so they link without libstdc++
// (built with -fno-exceptions -fno-rtti, linked by gcc).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>

void* operator new(size_t n)
{
    void* p = HeapAlloc(GetProcessHeap(), 0, n ? n : 1);
    if (!p) abort();
    return p;
}
void* operator new[](size_t n) { return operator new(n); }
void operator delete(void* p) noexcept { if (p) HeapFree(GetProcessHeap(), 0, p); }
void operator delete[](void* p) noexcept { operator delete(p); }
void operator delete(void* p, size_t) noexcept { operator delete(p); }
void operator delete[](void* p, size_t) noexcept { operator delete(p); }
extern "C" void __cxa_pure_virtual() { abort(); }

// Thread-safe initialisation of function-local statics (the compiler calls these; there is no libstdc++ here).
// One recursive spin lock: a static's initializer may itself touch another static.
static volatile LONG g_guardOwner = 0;
static volatile LONG g_guardDepth = 0;
extern "C" int __cxa_guard_acquire(long long* g)
{
    if (*(volatile char*)g) return 0;
    LONG me = (LONG)GetCurrentThreadId();
    for (;;) {
        LONG prev = InterlockedCompareExchange(&g_guardOwner, me, 0);
        if (prev == 0 || prev == me) break;
        Sleep(0);
    }
    ++g_guardDepth;
    if (*(volatile char*)g) { if (--g_guardDepth == 0) InterlockedExchange(&g_guardOwner, 0); return 0; }
    return 1;
}
extern "C" void __cxa_guard_release(long long* g)
{
    *(volatile char*)g = 1;
    if (--g_guardDepth == 0) InterlockedExchange(&g_guardOwner, 0);
}
extern "C" void __cxa_guard_abort(long long*)
{
    if (--g_guardDepth == 0) InterlockedExchange(&g_guardOwner, 0);
}
