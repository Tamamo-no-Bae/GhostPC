// GhostPC loader: maps the Xbox Ghost.exe (PE, Xbox subsystem) into this Win32
// process, resolves xboxkrnl imports, patches XDK library functions with PC
// replacements (by symbol name from Ghost.map) and runs the game.
#include "common.h"
#include "settings.h"
#include <dbghelp.h>
#include <vector>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>

HookReg *g_hooks;
HookReg::HookReg(const char *n, void *f) : name(n), fn(f), next(g_hooks) { g_hooks = this; }

int g_verbose;
static FILE *g_log;
static CRITICAL_SECTION g_logLock;
char g_gameDir[MAX_PATH], g_saveDir[MAX_PATH], g_cmdline[1024];
uint32_t g_imageBase, g_imageSize;
const char *volatile g_crumb = "";
static DWORD g_gameThreadId;

std::string *g_logCapture;   // debug: while set, Log() calls from g_logCaptureThread are appended here
DWORD g_logCaptureThread;
void Log(const char *fmt, ...) {
    char buf[2048];
    va_list ap; va_start(ap, fmt);
    int n = _vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
    va_end(ap);
    if (n < 0) n = (int)strlen(buf);
    if (n == 0 || buf[n - 1] != '\n') { buf[n++] = '\n'; buf[n] = 0; }
    if (g_logCapture && GetCurrentThreadId() == g_logCaptureThread) { g_logCapture->append(buf); return; }
    EnterCriticalSection(&g_logLock);
    if (g_log) {
        static DWORD t0 = GetTickCount();
        fprintf(g_log, "[%7.2f] ", (GetTickCount() - t0) / 1000.0);
        fputs(buf, g_log);
        fflush(g_log);
    }
    LeaveCriticalSection(&g_logLock);
    OutputDebugStringA(buf);
}

void Fatal(const char *fmt, ...) {
    char buf[2048];
    va_list ap; va_start(ap, fmt);
    _vsnprintf(buf, sizeof(buf) - 1, fmt, ap); buf[sizeof(buf) - 1] = 0;
    va_end(ap);
    Log("FATAL: %s", buf);
    TerminateProcess(GetCurrentProcess(), 3);
}

// ---------------------------------------------------------------------------
// Symbols (parsed from Ghost.map)
static std::vector<GhostSym> g_syms;               // sorted by addr
static std::unordered_map<std::string, uint32_t> g_symByName;
static std::unordered_map<std::string, uint32_t> g_replSymByName;  // symbols inside replaced XDK libs
static bool IsReplacedLib(const char *obj);
static std::vector<std::string> g_strings;         // backing store

uint32_t SymAddr(const char *n) {
    auto it = g_symByName.find(n);
    return it == g_symByName.end() ? 0 : it->second;
}
const GhostSym *SymAt(uint32_t a) {
    auto it = std::upper_bound(g_syms.begin(), g_syms.end(), a,
                               [](uint32_t v, const GhostSym &s) { return v < s.addr; });
    if (it == g_syms.begin()) return nullptr;
    return &*(it - 1);
}
const char *SymName(uint32_t a, char *buf, size_t n) {
    const GhostSym *s = SymAt(a);
    if (!s || a >= g_imageBase + g_imageSize || a < g_imageBase) _snprintf(buf, n, "%08x", a);
    else if (s->addr == a) _snprintf(buf, n, "%s", s->name);
    else _snprintf(buf, n, "%s+%x", s->name, a - s->addr);
    buf[n - 1] = 0;
    return buf;
}

static void LoadMap(const char *path, int32_t delta) {
    FILE *f = fopen(path, "r");
    if (!f) Fatal("cannot open %s", path);
    char line[4096];
    g_strings.reserve(40000 * 2);
    std::vector<std::pair<std::string, std::string>> tmpNames;
    while (fgets(line, sizeof line, f)) {
        unsigned seg, off, addr;
        char name[2048], rest[2048];
        rest[0] = 0;
        if (sscanf(line, " %x:%x %2047s %x %2047[^\n]", &seg, &off, name, &addr, rest) < 4) continue;
        if (seg == 0 || addr < 0x10000) continue;
        // rest: "f   obj" or "f i obj" or "obj"
        bool func = false;
        char *p = rest;
        while (*p == ' ') p++;
        if (p[0] == 'f' && p[1] == ' ') { func = true; p += 2; while (*p == ' ') p++; }
        if (p[0] == 'i' && p[1] == ' ') { p += 2; while (*p == ' ') p++; }
        char *e = p + strlen(p);
        while (e > p && (e[-1] == ' ' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
        g_strings.emplace_back(name);
        const char *nm = g_strings.back().c_str();
        g_strings.emplace_back(p);
        const char *ob = g_strings.back().c_str();
        g_syms.push_back({addr + delta, nm, ob, func});
    }
    fclose(f);
    std::stable_sort(g_syms.begin(), g_syms.end(), [](const GhostSym &a, const GhostSym &b) { return a.addr < b.addr; });
    for (auto &s : g_syms) {
        g_symByName.emplace(s.name, s.addr);
        if (IsReplacedLib(s.obj) && s.func) g_replSymByName.emplace(s.name, s.addr);
    }
    Log("map: %u symbols", (unsigned)g_syms.size());
}

// ---------------------------------------------------------------------------
// Image mapping
static std::vector<uint8_t> ReadFileAll(const char *path) {
    std::vector<uint8_t> v;
    FILE *f = fopen(path, "rb");
    if (!f) return v;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    v.resize(n);
    fread(v.data(), 1, n, f);
    fclose(f);
    return v;
}

static IMAGE_NT_HEADERS32 *g_nt;
static uint8_t *g_img;

static void MapImage(const char *path, int32_t *deltaOut) {
    std::vector<uint8_t> file = ReadFileAll(path);
    if (file.empty()) Fatal("cannot read %s", path);
    auto dos = (IMAGE_DOS_HEADER *)file.data();
    auto nt = (IMAGE_NT_HEADERS32 *)(file.data() + dos->e_lfanew);
    uint32_t want = nt->OptionalHeader.ImageBase, size = nt->OptionalHeader.SizeOfImage;
    uint8_t *img = (uint8_t *)VirtualAlloc((void *)want, size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!img) {
        img = (uint8_t *)VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        if (!img) Fatal("cannot allocate image (%u bytes)", size);
        Log("image: preferred base %08x busy, relocating to %p", want, img);
    }
    memcpy(img, file.data(), nt->OptionalHeader.SizeOfHeaders);
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        uint32_t raw = std::min(sec[i].SizeOfRawData, sec[i].Misc.VirtualSize ? sec[i].Misc.VirtualSize : sec[i].SizeOfRawData);
        if (raw) memcpy(img + sec[i].VirtualAddress, file.data() + sec[i].PointerToRawData, raw);
    }
    int32_t delta = (int32_t)((uint32_t)img - want);
    g_nt = (IMAGE_NT_HEADERS32 *)(img + dos->e_lfanew);
    if (delta) {
        auto &dir = g_nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
        uint8_t *p = img + dir.VirtualAddress, *end = p + dir.Size;
        while (p < end) {
            auto blk = (IMAGE_BASE_RELOCATION *)p;
            if (!blk->SizeOfBlock) break;
            uint16_t *e = (uint16_t *)(blk + 1);
            int cnt = (blk->SizeOfBlock - sizeof(*blk)) / 2;
            for (int i = 0; i < cnt; i++)
                if ((e[i] >> 12) == IMAGE_REL_BASED_HIGHLOW)
                    *(uint32_t *)(img + blk->VirtualAddress + (e[i] & 0xfff)) += delta;
            p += blk->SizeOfBlock;
        }
    }
    g_img = img;
    g_imageBase = (uint32_t)img;
    g_imageSize = size;
    *deltaOut = delta;
    Log("image: %s mapped at %p size %x entry %08x", path, img, size,
        (uint32_t)img + g_nt->OptionalHeader.AddressOfEntryPoint);
}

// ---------------------------------------------------------------------------
// Trap stubs for XDK functions we have not implemented
static const char *g_trapNames[65536];
static int g_trapCount;
static std::unordered_map<uint32_t, uint32_t> g_int3Redirect;  // addr -> target (for <5 byte funcs)

extern "C" void __stdcall TrapHit(uint32_t idx, uint32_t retaddr) {
    char b[256];
    Fatal("unimplemented XDK function %s called from %s", g_trapNames[idx], SymName(retaddr, b, sizeof b));
}

static uint8_t *g_thunkMem, *g_thunkPtr;
static uint8_t *AllocThunk(size_t n) {
    if (!g_thunkMem) g_thunkPtr = g_thunkMem = (uint8_t *)VirtualAlloc(nullptr, 1 << 20, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    uint8_t *p = g_thunkPtr; g_thunkPtr += (n + 15) & ~15; return p;
}

// thunk: push idx; push [esp+4] (retaddr); call TrapHit
static void *MakeTrapThunk(const char *name) {
    int idx = g_trapCount++;
    g_trapNames[idx] = name;
    uint8_t *t = AllocThunk(16);
    t[0] = 0xFF; t[1] = 0x34; t[2] = 0x24;           // push [esp]   (return address)
    t[3] = 0x68; *(uint32_t *)(t + 4) = idx;         // push idx
    t[8] = 0xE8; *(int32_t *)(t + 9) = (int32_t)((uint8_t *)TrapHit - (t + 13));  // call TrapHit
    t[13] = 0xCC;
    return t;
}

static uint32_t FuncSize(uint32_t addr) {
    auto it = std::upper_bound(g_syms.begin(), g_syms.end(), addr,
                               [](uint32_t v, const GhostSym &s) { return v < s.addr; });
    if (it == g_syms.end()) return 16;
    return it->addr - addr;
}

static void WriteJmp(uint32_t at, void *to) {
    if (FuncSize(at) < 5) {
        *(uint8_t *)at = 0xCC;
        g_int3Redirect[at] = (uint32_t)to;
        return;
    }
    *(uint8_t *)at = 0xE9;
    *(int32_t *)(at + 1) = (int32_t)((uint32_t)to - (at + 5));
}

static LONG CALLBACK VehHandler(EXCEPTION_POINTERS *ep) {
    auto rec = ep->ExceptionRecord;
    // debug (GHOSTPC_LOGFIRSTCHANCE): log access violations when raised, before any handler (game SEH included)
    static bool firstChance = getenv("GHOSTPC_LOGFIRSTCHANCE") != nullptr;
    static volatile LONG fcLogged;
    if (firstChance && (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION || rec->ExceptionCode == EXCEPTION_STACK_OVERFLOW) &&
        InterlockedIncrement(&fcLogged) <= 3) {
        if (rec->ExceptionCode == EXCEPTION_STACK_OVERFLOW) Log("first-chance STACK OVERFLOW");
        char b[256];
        auto c = ep->ContextRecord;
        Log("first-chance AV at %s (addr %08x) %s %08x  eax=%08x ebx=%08x ecx=%08x edx=%08x esi=%08x edi=%08x esp=%08x",
            SymName(c->Eip, b, sizeof b), c->Eip, rec->ExceptionInformation[0] ? "write" : "read",
            (uint32_t)rec->ExceptionInformation[1], c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Esp);
        uint32_t *sp = (uint32_t *)c->Esp;
        for (int i = 0, shown = 0; i < 1024 && shown < 14; i++) {
            uint32_t v;
            __try { v = sp[i]; } __except (EXCEPTION_EXECUTE_HANDLER) { break; }
            if (v >= g_imageBase && v < g_imageBase + g_imageSize) {
                const GhostSym *s = SymAt(v);
                if (s && s->func) { Log("    [esp+%03x] %s", i * 4, SymName(v, b, sizeof b)); shown++; }
            }
        }
    }
    if (rec->ExceptionCode == EXCEPTION_PRIV_INSTRUCTION) {
        // Xbox titles run in ring 0: skip cache flushes and interrupt masking
        uint8_t *ip = (uint8_t *)ep->ContextRecord->Eip;
        static volatile LONG logged;
        if (!InterlockedExchange(&logged, 1)) Log("skipping privileged instruction %02x %02x at %p", ip[0], ip[1], ip);
        if (ip[0] == 0x0F && (ip[1] == 0x09 || ip[1] == 0x08)) { ep->ContextRecord->Eip += 2; return EXCEPTION_CONTINUE_EXECUTION; }
        if (ip[0] == 0xFA || ip[0] == 0xFB) { ep->ContextRecord->Eip += 1; return EXCEPTION_CONTINUE_EXECUTION; }
    }
    if (rec->ExceptionCode == EXCEPTION_BREAKPOINT) {
        uint32_t a = (uint32_t)rec->ExceptionAddress;
        auto it = g_int3Redirect.find(a);
        if (it != g_int3Redirect.end()) { ep->ContextRecord->Eip = it->second; return EXCEPTION_CONTINUE_EXECUTION; }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static bool IsReplacedLib(const char *obj) {
    // xacteng (XACT) runs unmodified on top of our DirectSound
    static const char *libs[] = {"d3d8:", "dsound:", "xapilib:", "xnet:", "xboxkrnl:", nullptr};
    // the Xbox WMA decoder (used by XACT for music) is pure software inside dsound.lib: it runs as is
    static const char *keep[] = {"dsound:wmaxmo.obj", "dsound:wmaudio.obj", "dsound:wmaudio_parse.obj", "dsound:msaudio.obj",
                                 "dsound:msaudiodec.obj", "dsound:strmdec_wma.obj", "dsound:wmadecs_api.obj",
                                 "dsound:lowrate_common.obj", "dsound:huffdec.obj", "dsound:entropydec.obj", "dsound:lpc.obj",
                                 "dsound:fft.obj", "dsound:float.obj", "dsound:x86.obj", nullptr};
    for (int i = 0; keep[i]; i++) if (!strcmp(obj, keep[i])) return false;
    for (int i = 0; libs[i]; i++) if (!strncmp(obj, libs[i], strlen(libs[i]))) return true;
    return false;
}

// functions inside replaced libs that are pure code and may run unmodified
static const char *g_passthrough[] = {
    "_lstrcatA@8", "_lstrcatW@8", "_lstrcmpW@8", "_lstrcpyA@8", "_lstrcpyW@8", "_lstrcpynA@12",
    "_lstrcpynW@12", "_lstrlenA@4", "_lstrlenW@4", "_wsprintfW", "_wsprintfA", "_wvsprintfW@12", "_wvsprintfA@12",
#include "passthrough_gen.h"
    nullptr};

// kernel import implementations register as "krnl:<decorated>"
static void ResolveImports() {
    auto &dir = g_nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.Size) return;
    auto imp = (IMAGE_IMPORT_DESCRIPTOR *)(g_img + dir.VirtualAddress);
    std::unordered_map<std::string, void *> krnl;
    for (HookReg *h = g_hooks; h; h = h->next)
        if (!strncmp(h->name, "krnl:", 5)) krnl[h->name + 5] = h->fn;
    int missing = 0;
    for (; imp->Name; imp++) {
        auto iat = (uint32_t *)(g_img + imp->FirstThunk);
        for (; *iat; iat++) {
            // name via the __imp_ symbol in the map at this IAT slot
            const GhostSym *s = SymAt((uint32_t)iat);
            const char *nm = (s && s->addr == (uint32_t)iat && !strncmp(s->name, "__imp_", 6)) ? s->name + 6 : nullptr;
            uint32_t ord = *iat & 0x7fffffff;
            auto it = nm ? krnl.find(nm) : krnl.end();
            if (it != krnl.end()) *iat = (uint32_t)it->second;
            else {
                char *buf = (char *)malloc(80);
                _snprintf(buf, 80, "xboxkrnl#%u %s", ord, nm ? nm : "?");
                *iat = (uint32_t)MakeTrapThunk(buf);
                missing++;
            }
        }
    }
    Log("imports: %d kernel imports trapped", missing);
}

void CameraInit();
static void ApplyHooks() {
    std::unordered_set<uint32_t> hooked;
    int n = 0, bad = 0;
    for (HookReg *h = g_hooks; h; h = h->next) {
        if (!strncmp(h->name, "krnl:", 5)) continue;
        auto rit = g_replSymByName.find(h->name);
        uint32_t a = rit != g_replSymByName.end() ? rit->second : SymAddr(h->name);
        if (!a) { Log("hook: symbol %s not in map", h->name); bad++; continue; }
        WriteJmp(a, h->fn);
        hooked.insert(a);
        n++;
    }
    for (int i = 0; g_passthrough[i]; i++) if (uint32_t a = SymAddr(g_passthrough[i])) hooked.insert(a);
    int traps = 0;
    for (auto &s : g_syms) {
        if (!s.func || !IsReplacedLib(s.obj) || hooked.count(s.addr)) continue;
        WriteJmp(s.addr, MakeTrapThunk(s.name));
        hooked.insert(s.addr);
        traps++;
    }
    Log("hooks: %d replaced, %d trapped, %d unknown names", n, traps, bad);
    if (bad) Fatal("hook table references symbols missing from Ghost.map");
}

// XACT/DirectSound read the Xbox IRQL with "movzx eax, byte ptr fs:[0x24]" (KPCR.Irql). On Win32 that is
// TEB.ClientId; point those reads at TEB.ArbitraryUserPointer (fs:[0x14]), where kernel.cpp keeps our IRQL.
static void PatchIrqlReads() {
    static const uint8_t pat[] = {0x64, 0x0F, 0xB6, 0x05, 0x24, 0x00, 0x00, 0x00};
    int n = 0;
    for (auto &s : g_syms) {
        if (!s.func || IsReplacedLib(s.obj)) continue;
        uint32_t sz = FuncSize(s.addr);
        if (sz > 0x10000) sz = 0x10000;
        for (uint8_t *p = (uint8_t *)s.addr, *e = p + (sz >= 8 ? sz - 7 : 0); p < e; p++)
            if (!memcmp(p, pat, sizeof pat)) { p[4] = 0x14; n++; }
    }
    Log("patched %d IRQL reads", n);
}

// XACT reads the MCPX APU sample counter (48 kHz) at 0xFE80200C: "mov eax, [0xFE80200C]" (5 bytes) becomes
// "call ApuCounterStub" (5 bytes), which returns a 48 kHz counter derived from the performance counter.
extern "C" DWORD ApuSampleCounter() {
    static LARGE_INTEGER f, t0;
    if (!f.QuadPart) { QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0); }
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (DWORD)((unsigned long long)(t.QuadPart - t0.QuadPart) * 48000ull / (unsigned long long)f.QuadPart);
}
static __declspec(naked) void ApuCounterStub() {
    __asm {
        push ecx
        push edx
        call ApuSampleCounter
        pop edx
        pop ecx
        ret
    }
}
// M4Decoder::decode (cutscene video) ends each frame with "wbinvd" (Xbox code runs in ring 0). VehHandler would skip
// it, but a debugger stops on that exception first, so it is replaced by two NOPs.
static void PatchPrivileged() {
    uint8_t *p = (uint8_t *)SymAddr("?decode@M4Decoder@@QAE_NPBEKPAPBUVIDImage@@@Z");
    if (p && p[0xe8] == 0x0F && p[0xe9] == 0x09) { p[0xe8] = 0x90; p[0xe9] = 0x90; }
    else Log("M4Decoder::decode: wbinvd not found");
}

static void PatchApuReads() {
    static const uint8_t pat[] = {0xA1, 0x0C, 0x20, 0x80, 0xFE};
    int n = 0;
    for (auto &s : g_syms) {
        if (!s.func || strncmp(s.obj, "xacteng:", 8)) continue;
        uint32_t sz = std::min<uint32_t>(FuncSize(s.addr), 0x10000);
        for (uint8_t *p = (uint8_t *)s.addr, *e = p + (sz >= 5 ? sz - 4 : 0); p < e; p++)
            if (!memcmp(p, pat, sizeof pat)) {
                p[0] = 0xE8;
                *(int32_t *)(p + 1) = (int32_t)((uint8_t *)ApuCounterStub - (p + 5));
                n++;
            }
    }
    Log("patched %d APU counter reads", n);
}

// ---------------------------------------------------------------------------
static void RunInitTable(const char *a, const char *z) {
    uint32_t *p = (uint32_t *)SymAddr(a), *e = (uint32_t *)SymAddr(z);
    for (; p < e; p++) {
        if (!*p || *p == 0xffffffff) continue;
        const GhostSym *s = SymAt(*p);
        if (s && IsReplacedLib(s->obj)) { LOGV("init: skip %s (%s)", s->name, s->obj); continue; }
        ((void(__cdecl *)())*p)();
    }
}

static DWORD WINAPI GameThread(void *) {
    ThreadAttach();
    if (getenv("GHOSTPC_LOGFIRSTCHANCE")) {   // leave room for the exception logger if the stack overflows
        ULONG guarantee = 128 * 1024;
        SetThreadStackGuarantee(&guarantee);
    }
    Log("startup: running C initializers");
    RunInitTable("___xri_a", "___xri_z");
    if (uint32_t fp = *(uint32_t *)SymAddr("__FPinit")) ((void(__cdecl *)())fp)();
    RunInitTable("___xi_a", "___xi_z");
    RunInitTable("___xc_a", "___xc_z");
    Log("startup: calling main");
    int r = ((int(__cdecl *)(int, char **, char **))SymAddr("_main"))(0, nullptr, nullptr);
    Log("main returned %d", r);
    ExitProcess(0);
    return 0;
}

// GhostPC.exe's own code range (fixed base 0x10000000; build\GhostPC.map resolves these addresses)
static bool InGhostPC(uint32_t v) {
    static uint32_t base, size;
    if (!base) {
        base = (uint32_t)GetModuleHandleA(nullptr);
        auto nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
        size = nt->OptionalHeader.SizeOfImage;
    }
    return v >= base + 0x1000 && v < base + size;
}
// crude stack scan for return addresses into Ghost.exe (by symbol) and GhostPC.exe (raw)
static void LogStack(uint32_t esp, int maxShown) {
    char b[256];
    uint32_t *sp = (uint32_t *)esp;
    int shown = 0;
    for (int i = 0; i < 4096 && shown < maxShown; i++) {
        uint32_t v;
        __try { v = sp[i]; } __except (EXCEPTION_EXECUTE_HANDLER) { break; }
        if (v >= g_imageBase && v < g_imageBase + g_imageSize) {
            const GhostSym *s = SymAt(v);
            if (s && s->func) { Log("  [esp+%03x] %s", i * 4, SymName(v, b, sizeof b)); shown++; }
        } else if (InGhostPC(v)) {
            Log("  [esp+%03x] GhostPC %08x", i * 4, v);
            shown++;
        }
    }
}
// a minidump next to the log (ghostpc_crash.dmp, or ghostpc_hang.dmp), for a closer look at a reported crash
static void WriteDump(const char *name, EXCEPTION_POINTERS *ep) {
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    strcpy(strrchr(path, '\\') + 1, name);
    HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    MINIDUMP_EXCEPTION_INFORMATION info = {GetCurrentThreadId(), ep, FALSE};
    BOOL ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f,
                                (MINIDUMP_TYPE)(MiniDumpWithDataSegs | MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo),
                                ep ? &info : nullptr, nullptr, nullptr);
    CloseHandle(f);
    Log("  minidump %s: %s", name, ok ? "written" : "failed");
}
static LONG WINAPI CrashFilter(EXCEPTION_POINTERS *ep) {
    char b[256];
    auto c = ep->ContextRecord;
    Log("CRASH: exception %08x at %s (addr %08x)%s", ep->ExceptionRecord->ExceptionCode,
        InGhostPC(c->Eip) ? "GhostPC" : SymName(c->Eip, b, sizeof b), c->Eip, InGhostPC(c->Eip) ? " (GhostPC code)" : "");
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION)
        Log("  %s address %08x", ep->ExceptionRecord->ExceptionInformation[0] ? "write" : "read",
            (uint32_t)ep->ExceptionRecord->ExceptionInformation[1]);
    extern UINT g_frame;
    Log("  thread %u (%s), frame %u, GhostPC step: %s", GetCurrentThreadId(),
        GetCurrentThreadId() == g_gameThreadId ? "game" : "other", g_frame, g_crumb);
    Log("  eax=%08x ebx=%08x ecx=%08x edx=%08x esi=%08x edi=%08x ebp=%08x esp=%08x", c->Eax, c->Ebx, c->Ecx,
        c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp);
    LogStack(c->Esp, 32);
    WriteDump("ghostpc_crash.dmp", ep);
    TerminateProcess(GetCurrentProcess(), 2);
    return EXCEPTION_EXECUTE_HANDLER;
}
// hang watchdog: no frame for 15 s while the game window has focus -> log it with the GhostPC step (once per stall)
static DWORD WINAPI Watchdog(void *) {
    extern UINT g_frame;
    extern HWND g_hwnd;
    UINT last = 0;
    int still = 0;
    bool reported = false;
    for (;;) {
        Sleep(1000);
        UINT f = g_frame;
        bool focused = g_hwnd && GetForegroundWindow() == g_hwnd;
        if (f != last || !f || !focused) { last = f, still = 0, reported = false; continue; }
        if (++still < 15 || reported) continue;
        reported = true;
        Log("HANG: no frame for 15 s at frame %u, GhostPC step: %s", f, g_crumb);
    }
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR cmd, int) {
    InitializeCriticalSection(&g_logLock);
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    *strrchr(exe, '\\') = 0;
    strcpy(g_gameDir, exe);
    // options: -game <dir>  -v  ; everything else is passed to the game as its command line
    std::string pass;
    bool forceSetup = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    {
        int argc; LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        for (int i = 1; i < argc; i++) {
            char a[1024]; WideCharToMultiByte(CP_ACP, 0, argv[i], -1, a, sizeof a, 0, 0);
            if (!strcmp(a, "-game") && i + 1 < argc) { WideCharToMultiByte(CP_ACP, 0, argv[++i], -1, g_gameDir, MAX_PATH, 0, 0); continue; }
            if (!strcmp(a, "-v")) { g_verbose = 1; continue; }
            if (!strcmp(a, "-setup")) { forceSetup = true; continue; }
            if (!pass.empty()) pass += ' ';
            pass += a;
        }
    }
    strncpy(g_cmdline, pass.c_str(), sizeof g_cmdline - 1);
    char path[MAX_PATH];
    _snprintf(path, MAX_PATH, "%s\\ghostpc.log", exe);
    {
        // keep the previous session's log (a crash report would otherwise be lost on the next start)
        char prev[MAX_PATH];
        _snprintf(prev, MAX_PATH, "%s\\ghostpc.prev.log", exe);
        MoveFileExA(path, prev, MOVEFILE_REPLACE_EXISTING);
    }
    g_log = fopen(path, "w");
    _snprintf(g_saveDir, MAX_PATH, "%s\\save", g_gameDir);
    CreateDirectoryA(g_saveDir, nullptr);
    Log("GhostPC starting; game dir %s; cmdline '%s'", g_gameDir, g_cmdline);
    SettingsLoad();
    if ((g_settings.showAtStartup || forceSetup) && !getenv("GHOSTPC_NOSETUP")) {
        if (!SettingsDialog()) { Log("settings window: quit"); return 0; }
    }
    Log("settings: %dx%d mode %d input %d sensitivity %d%% vsync %d", g_settings.width, g_settings.height, g_settings.mode,
        g_settings.input, g_settings.mouseSensitivity, g_settings.vsync);

    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    AddVectoredExceptionHandler(1, VehHandler);
    SetUnhandledExceptionFilter(CrashFilter);

    int32_t delta;
    _snprintf(path, MAX_PATH, "%s\\Ghost.exe", g_gameDir);
    MapImage(path, &delta);
    _snprintf(path, MAX_PATH, "%s\\Ghost.map", g_gameDir);
    LoadMap(path, delta);
    ResolveImports();
    CameraInit();   // reads a prologue the hooks overwrite
    ApplyHooks();
    PatchIrqlReads();
    PatchApuReads();
    PatchPrivileged();
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);

    // Xbox titles get a large main-thread stack; run the game on its own thread.
    HANDLE t = CreateThread(nullptr, 8 << 20, GameThread, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, &g_gameThreadId);
    CreateThread(nullptr, 0, Watchdog, nullptr, 0, nullptr);
    WaitForSingleObject(t, INFINITE);
    return 0;
}
