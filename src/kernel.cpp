// xboxkrnl imports still reachable from kept code, and LIBCMT per-thread data.
#include "common.h"
#include <algorithm>

// ---------------------------------------------------------------------------
// Critical sections. Xbox RTL_CRITICAL_SECTION is 28 bytes (KEVENT + counts),
// Win32's is 24, so keep a Win32 CS pointer in the first dword.
// keyed by address: Xbox code may also initialize them statically, so their contents can't be trusted
#include <unordered_map>
static SRWLOCK g_csMapLock = SRWLOCK_INIT;
static std::unordered_map<void *, CRITICAL_SECTION *> *g_csMap;
static CRITICAL_SECTION *XcsGet(void *p) {
    AcquireSRWLockShared(&g_csMapLock);
    CRITICAL_SECTION *cs = nullptr;
    if (g_csMap) { auto it = g_csMap->find(p); if (it != g_csMap->end()) cs = it->second; }
    ReleaseSRWLockShared(&g_csMapLock);
    if (cs) return cs;
    AcquireSRWLockExclusive(&g_csMapLock);
    if (!g_csMap) g_csMap = new std::unordered_map<void *, CRITICAL_SECTION *>();
    auto &slot = (*g_csMap)[p];
    if (!slot) { slot = new CRITICAL_SECTION; InitializeCriticalSection(slot); }
    cs = slot;
    ReleaseSRWLockExclusive(&g_csMapLock);
    return cs;
}
KSTD(void, RtlInitializeCriticalSection, 4, (void *p)) { XcsGet(p); }
KSTD(void, RtlEnterCriticalSection, 4, (void *p)) { EnterCriticalSection(XcsGet(p)); }
KSTD(void, RtlLeaveCriticalSection, 4, (void *p)) { LeaveCriticalSection(XcsGet(p)); }

KSTD(void, KeBugCheck, 4, (ULONG code)) { Fatal("KeBugCheck(%x)", code); }
// ---------------------------------------------------------------------------
// IRQL. The Xbox is a single CPU: code at DISPATCH_LEVEL excludes DPCs. We keep a per-thread IRQL in
// TEB.ArbitraryUserPointer (fs:[0x14], see PatchIrqlReads) and hold one global lock while >= DISPATCH;
// DPCs (timer.cpp-style thread below) run while holding the same lock.
static CRITICAL_SECTION g_dpcLock;
static INIT_ONCE g_dpcInit = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK DpcInit(PINIT_ONCE, void *, void **) { InitializeCriticalSection(&g_dpcLock); return TRUE; }
static UCHAR GetIrql() { return (UCHAR)(uintptr_t)((NT_TIB *)NtCurrentTeb())->ArbitraryUserPointer; }
static void SetIrql(UCHAR v) { ((NT_TIB *)NtCurrentTeb())->ArbitraryUserPointer = (void *)(uintptr_t)v; }
static UCHAR RaiseTo(UCHAR n) {
    InitOnceExecuteOnce(&g_dpcInit, DpcInit, nullptr, nullptr);
    UCHAR old = GetIrql();
    if (n >= 2 && old < 2) EnterCriticalSection(&g_dpcLock);
    SetIrql(n);
    return old;
}
static void LowerTo(UCHAR n) {
    UCHAR old = GetIrql();
    SetIrql(n);
    if (old >= 2 && n < 2) LeaveCriticalSection(&g_dpcLock);
}
KFAST(UCHAR, KfRaiseIrql, 4, (UCHAR n)) { return RaiseTo(n); }
KFAST(void, KfLowerIrql, 4, (UCHAR n)) { LowerTo(n); }
KSTD(UCHAR, KeRaiseIrqlToDpcLevel, 0, ()) { return RaiseTo(2); }

// ---------------------------------------------------------------------------
// Kernel timers + DPCs (XACT's engine tick). KDPC: DeferredRoutine at +0x0C, context at +0x10.
typedef void(__stdcall *DpcRoutine)(void *dpc, void *ctx, void *a1, void *a2);
struct XTimer { LONGLONG due; void *dpc; bool armed; };
#include <map>
#include <mutex>
static std::mutex g_timerMu;
static std::map<void *, XTimer> g_timers;
static HANDLE g_timerEvent, g_timerThread;
static LONGLONG Now100ns() { FILETIME f; GetSystemTimeAsFileTime(&f); return ((LONGLONG)f.dwHighDateTime << 32) | f.dwLowDateTime; }
static DWORD WINAPI TimerThread(void *) {
    ThreadAttach();
    for (;;) {
        LONGLONG next = MAXLONGLONG, now = Now100ns();
        void *fireTimer = nullptr, *fireDpc = nullptr;
        {
            std::lock_guard<std::mutex> l(g_timerMu);
            for (auto &kv : g_timers) {
                if (!kv.second.armed) continue;
                if (kv.second.due <= now) { fireTimer = kv.first; fireDpc = kv.second.dpc; kv.second.armed = false; break; }
                next = std::min(next, kv.second.due);
            }
        }
        if (fireTimer) {
            if (fireDpc) {
                auto routine = *(DpcRoutine *)((uint8_t *)fireDpc + 0x0C);
                void *ctx = *(void **)((uint8_t *)fireDpc + 0x10);
                UCHAR old = RaiseTo(2);
                routine(fireDpc, ctx, nullptr, nullptr);
                LowerTo(old);
            }
            continue;
        }
        DWORD ms = next == MAXLONGLONG ? INFINITE : (DWORD)std::max<LONGLONG>(1, (next - now) / 10000);
        WaitForSingleObject(g_timerEvent, ms);
    }
}
KSTD(void, KeInitializeDpc, 12, (void *dpc, DpcRoutine routine, void *ctx)) {
    memset(dpc, 0, 0x1C);
    *(DpcRoutine *)((uint8_t *)dpc + 0x0C) = routine;
    *(void **)((uint8_t *)dpc + 0x10) = ctx;
}
KSTD(void, KeInitializeTimerEx, 8, (void *timer, DWORD type)) {
    memset(timer, 0, 0x28);
    std::lock_guard<std::mutex> l(g_timerMu);
    g_timers[timer] = {0, nullptr, false};
}
KSTD(BOOLEAN, KeSetTimer, 16, (void *timer, LARGE_INTEGER due, void *dpc)) {
    if (!g_timerThread) {
        g_timerEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        g_timerThread = CreateThread(nullptr, 1 << 20, TimerThread, nullptr, 0, nullptr);
        SetThreadPriority(g_timerThread, THREAD_PRIORITY_ABOVE_NORMAL);
    }
    LONGLONG t = due.QuadPart < 0 ? Now100ns() - due.QuadPart : due.QuadPart;
    bool was;
    {
        std::lock_guard<std::mutex> l(g_timerMu);
        auto &x = g_timers[timer];
        was = x.armed;
        x = {t, dpc, true};
    }
    SetEvent(g_timerEvent);
    return was;
}
KSTD(BOOLEAN, KeCancelTimer, 4, (void *timer)) {
    std::lock_guard<std::mutex> l(g_timerMu);
    auto it = g_timers.find(timer);
    if (it == g_timers.end()) return FALSE;
    bool was = it->second.armed;
    it->second.armed = false;
    return was;
}
KSTD(void, KeQuerySystemTime, 4, (LARGE_INTEGER *t)) { t->QuadPart = Now100ns(); }
KSTD(LONG, KeSaveFloatingPointState, 4, (void *)) { return 0; }
KSTD(LONG, KeRestoreFloatingPointState, 4, (void *)) { return 0; }

// DirectSound's CFpState::Save/Restore read the KPCR (fs:[0x58]); the FPU state needs no saving here
static void __fastcall FpNoop(void *) {}
XRAW("?Save@CFpState@DirectSound@@QAEXXZ", FpNoop);
XRAW("?Restore@CFpState@DirectSound@@QAEXXZ", FpNoop);

// xapilib RtlRandom (Park-Miller, as in NT)
XSTD(ULONG, RtlRandom, 4, (ULONG *seed)) {
    *seed = (ULONG)(((unsigned long long)*seed * 16807ull) % 0x7fffffffull);
    return *seed;
}
KSTD(void *, ExAllocatePoolWithTag, 8, (SIZE_T n, ULONG)) { return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n); }
KSTD(void, ExFreePool, 4, (void *p)) { HeapFree(GetProcessHeap(), 0, p); }

// xboxkrnl.lib import thunk called directly by the CRT's SEH code
XSTD(void, RtlUnwind, 16, (void *frame, void *ip, EXCEPTION_RECORD *rec, void *ret)) { RtlUnwind(frame, ip, rec, ret); }

// ---------------------------------------------------------------------------
// LIBCMT per-thread data (_tiddata, 0x88 bytes). The Xbox CRT keeps it in XAPI
// TLS reached through fs:[0x28]; use a Win32 TLS slot instead.
static DWORD g_ptdTls = TLS_OUT_OF_INDEXES;
typedef void *(__cdecl *calloc_t)(size_t, size_t);

static void *NewPtd() {
    static calloc_t gcalloc = (calloc_t)SymAddr("_calloc");
    uint8_t *p = (uint8_t *)gcalloc(1, 0x88);
    if (!p) Fatal("_getptd: out of memory");
    *(uint32_t *)(p + 0x54) = SymAddr("__XcptActTab");
    *(uint32_t *)(p + 0x14) = 1;
    *(uint32_t *)(p + 0x00) = GetCurrentThreadId();
    *(uint32_t *)(p + 0x04) = 0xffffffff;
    return p;
}
XCDECL(void *, _getptd, ()) {
    if (g_ptdTls == TLS_OUT_OF_INDEXES) g_ptdTls = TlsAlloc();
    void *p = TlsGetValue(g_ptdTls);
    if (!p) { p = NewPtd(); TlsSetValue(g_ptdTls, p); }
    return p;
}
XCDECL(void, _freeptd, (void *ptd)) {
    if (g_ptdTls == TLS_OUT_OF_INDEXES) return;
    if (!ptd) ptd = TlsGetValue(g_ptdTls);
    if (ptd) ((void(__stdcall *)(void *))SymAddr("__freefls@4"))(ptd);
    TlsSetValue(g_ptdTls, nullptr);
}
XCDECL(int, _mtinit, ()) {
    if (!((int(__cdecl *)())SymAddr("__mtinitlocks"))()) Fatal("_mtinitlocks failed");
    if (g_ptdTls == TLS_OUT_OF_INDEXES) g_ptdTls = TlsAlloc();
    xb__getptd();
    return 0;
}

void ThreadAttach() {
    // Xbox runs x87 at its default (64-bit precision, all exceptions masked)
    unsigned int cw;
    _controlfp_s(&cw, _PC_64 | _MCW_EM, _MCW_PC | _MCW_EM);
}
