// XAPI (xapilib) replacements mapped onto Win32.
#include "common.h"
#include <malloc.h>
#include <string>
#include <algorithm>

// ---------------------------------------------------------------------------
// Paths. D: is the game folder; T:, U:, Z: live under <game>\save\<letter>.
bool XboxPathToHost(const char *x, char *out, size_t n) {
    std::string p = x;
    for (auto &c : p) if (c == '/') c = '\\';
    std::string r;
    if (p.size() >= 2 && p[1] == ':') {
        char d = (char)toupper(p[0]);
        std::string rest = p.substr(2);
        if (!rest.empty() && rest[0] == '\\') rest = rest.substr(1);
        if (d == 'D') r = std::string(g_gameDir) + "\\" + rest;
        else {
            std::string base = std::string(g_saveDir) + "\\" + d;
            CreateDirectoryA(base.c_str(), nullptr);
            r = base + "\\" + rest;
        }
    } else if (!_strnicmp(p.c_str(), "\\Device\\", 8)) {
        r = std::string(g_saveDir) + "\\dev\\" + p.substr(8);
    } else if (!p.empty() && p[0] == '\\') {
        r = std::string(g_gameDir) + p;
    } else {
        r = std::string(g_gameDir) + "\\" + p;
    }
    while (r.size() > 3 && r.back() == '\\' && r[r.size() - 2] != ':') {
        // keep "dir\" for directory opens out of FindFirstFile patterns only
        break;
    }
    if (r.size() + 1 > n) return false;
    strcpy(out, r.c_str());
    return true;
}

// ---------------------------------------------------------------------------
// Files
XSTD(HANDLE, CreateFileA, 28, (LPCSTR name, DWORD acc, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl)) {
    char p[MAX_PATH * 2];
    XboxPathToHost(name, p, sizeof p);
    HANDLE h = CreateFileA(p, acc, share | FILE_SHARE_READ, sa, disp, flags & ~FILE_FLAG_NO_BUFFERING, tmpl);
    if (h == INVALID_HANDLE_VALUE) LOGV("CreateFile %s -> FAIL %u", name, GetLastError());
    else LOGV("CreateFile %s -> %p", name, h);
    return h;
}
XSTD(BOOL, ReadFile, 20, (HANDLE h, void *b, DWORD n, DWORD *rd, OVERLAPPED *ov)) { return ReadFile(h, b, n, rd, ov); }
XSTD(BOOL, WriteFile, 20, (HANDLE h, const void *b, DWORD n, DWORD *wr, OVERLAPPED *ov)) { return WriteFile(h, b, n, wr, ov); }
XSTD(DWORD, SetFilePointer, 16, (HANDLE h, LONG d, LONG *hi, DWORD m)) { return SetFilePointer(h, d, hi, m); }
XSTD(DWORD, GetFileSize, 8, (HANDLE h, DWORD *hi)) { return GetFileSize(h, hi); }
XSTD(BOOL, SetEndOfFile, 4, (HANDLE h)) { return SetEndOfFile(h); }
XSTD(BOOL, FlushFileBuffers, 4, (HANDLE h)) { return FlushFileBuffers(h); }
// Xbox closes FindFirstFile handles with CloseHandle; Win32 needs FindClose
#include <unordered_set>
#include <mutex>
static std::unordered_set<HANDLE> g_findHandles;
static std::mutex g_findMu;
XSTD(BOOL, CloseHandle, 4, (HANDLE h)) {
    {
        std::lock_guard<std::mutex> l(g_findMu);
        if (g_findHandles.erase(h)) return FindClose(h);
    }
    return CloseHandle(h);
}
XSTD(BOOL, CancelIo, 4, (HANDLE h)) { return CancelIo(h); }
XSTD(BOOL, GetOverlappedResult, 16, (HANDLE h, OVERLAPPED *ov, DWORD *n, BOOL w)) { return GetOverlappedResult(h, ov, n, w); }
XSTD(HANDLE, FindFirstFileA, 8, (LPCSTR pat, WIN32_FIND_DATAA *fd)) {
    char p[MAX_PATH * 2];
    XboxPathToHost(pat, p, sizeof p);
    HANDLE h = FindFirstFileA(p, fd);
    LOGV("FindFirstFile %s -> %p", pat, h);
    if (h != INVALID_HANDLE_VALUE) { std::lock_guard<std::mutex> l(g_findMu); g_findHandles.insert(h); }
    return h;
}
XSTD(BOOL, FindNextFileA, 8, (HANDLE h, WIN32_FIND_DATAA *fd)) { return FindNextFileA(h, fd); }
XSTD(BOOL, GetDiskFreeSpaceExA, 16, (LPCSTR d, ULARGE_INTEGER *a, ULARGE_INTEGER *t, ULARGE_INTEGER *f)) {
    if (a) a->QuadPart = 4ull << 30;
    if (t) t->QuadPart = 8ull << 30;
    if (f) f->QuadPart = 4ull << 30;
    return TRUE;
}
XSTD(DWORD, XGetDiskSectorSizeA, 4, (LPCSTR)) { return 512; }
XSTD(BOOL, XSetFileCacheSize, 4, (SIZE_T)) { return TRUE; }

// ---------------------------------------------------------------------------
// Errors / debug / misc
XSTD(DWORD, GetLastError, 0, ()) { return GetLastError(); }
XSTD(void, SetLastError, 4, (DWORD e)) { SetLastError(e); }
XSTD(void, OutputDebugStringA, 4, (LPCSTR s)) {
    if (!s) return;
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r')) n--;
    if (n) Log("dbg: %.*s", (int)n, s);
}
XSTD(void, DebugBreak, 0, ()) { Log("DebugBreak() ignored"); }
XSTD(void, RaiseException, 16, (DWORD c, DWORD f, DWORD n, const ULONG_PTR *a)) { RaiseException(c, f, n, a); }
static LPTOP_LEVEL_EXCEPTION_FILTER g_gameFilter;
XSTD(LPTOP_LEVEL_EXCEPTION_FILTER, SetUnhandledExceptionFilter, 4, (LPTOP_LEVEL_EXCEPTION_FILTER f)) {
    // keep the loader's crash logger installed
    auto old = g_gameFilter; g_gameFilter = f; return old;
}
XSTD(BOOL, IsBadReadPtr, 8, (const void *p, UINT_PTR n)) { return IsBadReadPtr(p, n); }
XSTD(BOOL, IsBadWritePtr, 8, (void *p, UINT_PTR n)) { return IsBadWritePtr(p, n); }
XSTD(BOOL, IsBadCodePtr, 4, (FARPROC p)) { return IsBadCodePtr(p); }
XSTD(int, WideCharToMultiByte, 32, (UINT cp, DWORD fl, LPCWSTR w, int wn, LPSTR m, int mn, LPCSTR d, LPBOOL u)) {
    return WideCharToMultiByte(cp, fl, w, wn, m, mn, d, u);
}

// ---------------------------------------------------------------------------
// Time
XSTD(DWORD, GetTickCount, 0, ()) { return GetTickCount(); }
XSTD(BOOL, QueryPerformanceCounter, 4, (LARGE_INTEGER *c)) { return QueryPerformanceCounter(c); }
XSTD(BOOL, QueryPerformanceFrequency, 4, (LARGE_INTEGER *f)) { return QueryPerformanceFrequency(f); }
XSTD(void, GetLocalTime, 4, (SYSTEMTIME *t)) { GetLocalTime(t); }
XSTD(void, GetSystemTimeAsFileTime, 4, (FILETIME *t)) { GetSystemTimeAsFileTime(t); }
XSTD(void, Sleep, 4, (DWORD ms)) { Sleep(ms); }

// ---------------------------------------------------------------------------
// Threads and sync
struct ThreadStart { LPTHREAD_START_ROUTINE fn; void *arg; };
static DWORD WINAPI ThreadTramp(void *p) {
    ThreadStart ts = *(ThreadStart *)p;
    delete (ThreadStart *)p;
    ThreadAttach();
    return ts.fn(ts.arg);
}
XSTD(HANDLE, CreateThread, 24, (LPSECURITY_ATTRIBUTES sa, SIZE_T stack, LPTHREAD_START_ROUTINE fn, void *arg, DWORD fl, DWORD *id)) {
    char b[256];
    LOGV("CreateThread %s stack %x flags %x", SymName((uint32_t)fn, b, sizeof b), stack, fl);
    auto ts = new ThreadStart{fn, arg};
    if (stack < (256 << 10)) stack = 256 << 10;
    return CreateThread(sa, stack, ThreadTramp, ts, fl | STACK_SIZE_PARAM_IS_A_RESERVATION, id);
}
XSTD(DWORD, ResumeThread, 4, (HANDLE h)) { return ResumeThread(h); }
XSTD(DWORD, GetCurrentThreadId, 0, ()) { return GetCurrentThreadId(); }
XSTD(HANDLE, CreateEventA, 16, (LPSECURITY_ATTRIBUTES sa, BOOL m, BOOL i, LPCSTR n)) { return CreateEventA(sa, m, i, n); }
XSTD(BOOL, SetEvent, 4, (HANDLE h)) { return SetEvent(h); }
XSTD(HANDLE, CreateMutexA, 12, (LPSECURITY_ATTRIBUTES sa, BOOL o, LPCSTR n)) { return CreateMutexA(sa, o, n); }
XSTD(BOOL, ReleaseMutex, 4, (HANDLE h)) { return ReleaseMutex(h); }
XSTD(DWORD, WaitForSingleObject, 8, (HANDLE h, DWORD ms)) { return WaitForSingleObject(h, ms); }
XSTD(DWORD, WaitForMultipleObjects, 16, (DWORD n, const HANDLE *h, BOOL all, DWORD ms)) { return WaitForMultipleObjects(n, h, all, ms); }
XSTD(BOOL, XRegisterThreadNotifyRoutine, 8, (void *, BOOL)) { return TRUE; }

// ---------------------------------------------------------------------------
// Memory
XSTD(HANDLE, GetProcessHeap, 0, ()) { return GetProcessHeap(); }
// The Xbox heap hands out 16-byte aligned blocks (game code uses movaps on heap objects).
// Layout: [pad][hdr: base ptr, size][user data]
struct HeapHdr { void *base; SIZE_T size; };
static void *HAlloc(SIZE_T n, bool zero) {
    uint8_t *b = (uint8_t *)HeapAlloc(GetProcessHeap(), zero ? HEAP_ZERO_MEMORY : 0, n + 16 + sizeof(HeapHdr));
    if (!b) return nullptr;
    uint8_t *u = (uint8_t *)(((uintptr_t)b + sizeof(HeapHdr) + 15) & ~(uintptr_t)15);
    ((HeapHdr *)u)[-1] = {b, n};
    return u;
}
static HeapHdr *HHdr(void *p) { return &((HeapHdr *)p)[-1]; }
XSTD(void *, RtlAllocateHeap, 12, (HANDLE h, DWORD f, SIZE_T n)) { return HAlloc(n, (f & HEAP_ZERO_MEMORY) != 0); }
XSTD(void *, RtlReAllocateHeap, 16, (HANDLE h, DWORD f, void *p, SIZE_T n)) {
    if (!p) return HAlloc(n, (f & HEAP_ZERO_MEMORY) != 0);
    if (f & HEAP_REALLOC_IN_PLACE_ONLY) return n <= HHdr(p)->size ? p : nullptr;
    void *q = HAlloc(n, (f & HEAP_ZERO_MEMORY) != 0);
    if (!q) return nullptr;
    memcpy(q, p, std::min(n, HHdr(p)->size));
    HeapFree(GetProcessHeap(), 0, HHdr(p)->base);
    return q;
}
XSTD(SIZE_T, RtlSizeHeap, 12, (HANDLE h, DWORD f, void *p)) { return p ? HHdr(p)->size : (SIZE_T)-1; }
XSTD(BOOL, HeapFree, 12, (HANDLE h, DWORD f, void *p)) { return p ? HeapFree(GetProcessHeap(), 0, HHdr(p)->base) : TRUE; }

// XMemAlloc attributes: bits 16-19 alignment code (0=default 4/16), bit 31 physical
static size_t XAlign(DWORD attr) {
    static const size_t tab[16] = {16, 16, 16, 16, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 4096, 4096, 4096};
    return tab[(attr >> 16) & 0xF];
}
XSTD(void *, XMemAlloc, 8, (SIZE_T n, DWORD attr)) {
    void *p = _aligned_malloc(n ? n : 1, XAlign(attr));
    if (p && (attr & (1 << 30))) memset(p, 0, n);   // XALLOC_ZEROINITIALIZE
    return p;
}
XSTD(void, XMemFree, 8, (void *p, DWORD)) { if (p) _aligned_free(p); }
XSTD(void *, XPhysicalAlloc, 16, (SIZE_T n, ULONG_PTR maxaddr, ULONG_PTR align, DWORD prot)) {
    return VirtualAlloc(nullptr, n, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
}
XSTD(void, XPhysicalFree, 4, (void *p)) { if (p) VirtualFree(p, 0, MEM_RELEASE); }
XSTD(void, GlobalMemoryStatus, 4, (MEMORYSTATUS *m)) {
    // report an Xbox development kit (128 MB) so memory budgets behave
    memset(m, 0, sizeof *m);
    m->dwLength = sizeof *m;
    m->dwTotalPhys = 128u << 20;
    m->dwAvailPhys = 96u << 20;
    m->dwTotalVirtual = 512u << 20;
    m->dwAvailVirtual = 400u << 20;
    m->dwTotalPageFile = 0;
}

// ---------------------------------------------------------------------------
// Title / system information
#define LDT_FROM_DEBUGGER_CMDLINE 2
XSTD(DWORD, XGetLaunchInfo, 8, (DWORD *type, char *data)) {
    // the game treats both type 0 (title launch) and 3 as a command line string
    *type = 3;
    memset(data, 0, 3072);
    strncpy(data, g_cmdline, 3071);
    return 0;
}
XSTD(DWORD, XLaunchNewImageA, 8, (LPCSTR img, void *)) {
    Log("XLaunchNewImage(%s): exiting", img ? img : "dashboard");
    ExitProcess(0);
}
XSTD(DWORD, XGetLanguage, 0, ()) { return 1; }        // English
XSTD(DWORD, XGetVideoFlags, 0, ()) { return 0; }      // 4:3, no HDTV modes
XSTD(DWORD, XGetAVPack, 0, ()) { return 6; }          // XC_AV_PACK_STANDARD
XSTD(DWORD, XAutoPowerDownResetTimer, 0, ()) { return 0; }

// Save-game signatures: any stable 20-byte digest will do on PC
struct SigCtx { uint32_t h[5]; };
XSTD(HANDLE, XCalculateSignatureBegin, 4, (DWORD)) { auto c = new SigCtx; for (int i = 0; i < 5; i++) c->h[i] = 0x811c9dc5u + i; return c; }
XSTD(DWORD, XCalculateSignatureUpdate, 12, (HANDLE h, const BYTE *d, DWORD n)) {
    auto c = (SigCtx *)h;
    for (DWORD i = 0; i < n; i++) { c->h[i % 5] = (c->h[i % 5] ^ d[i]) * 16777619u; }
    return 0;
}
XSTD(DWORD, XCalculateSignatureEnd, 8, (HANDLE h, BYTE *out)) {
    auto c = (SigCtx *)h;
    memcpy(out, c->h, 20);
    delete c;
    return 0;
}

// ---------------------------------------------------------------------------
// Saved games: U:\<name> folders. XGAME_FIND_DATA = WIN32_FIND_DATAA + szSaveGameDirectory[MAX_PATH] + szSaveGameName[MAX_NAMELEN=64] (wide)
#pragma pack(push, 4)
struct XGAME_FIND_DATA {
    WIN32_FIND_DATAA fd;
    CHAR szSaveGameDirectory[MAX_PATH];
    WCHAR szSaveGameName[64];
};
#pragma pack(pop)
static void SaveRoot(const char *root, std::string &out) {
    char p[MAX_PATH * 2];
    XboxPathToHost(root, p, sizeof p);
    out = p;
    if (!out.empty() && out.back() != '\\') out += '\\';
    CreateDirectoryA(out.c_str(), nullptr);
}
static std::string SaveDirName(const wchar_t *name) {
    // directory name: hex of the wide name (keeps it filesystem safe and reversible)
    std::string s;
    char b[8];
    for (const wchar_t *p = name; *p; p++) { sprintf(b, "%04X", (unsigned)*p); s += b; }
    return s;
}
static std::wstring SaveNameFromDir(const char *d) {
    std::wstring w;
    for (size_t i = 0; d[i] && d[i + 1] && d[i + 2] && d[i + 3]; i += 4) {
        unsigned v; char t[5] = {d[i], d[i + 1], d[i + 2], d[i + 3], 0};
        if (sscanf(t, "%4X", &v) != 1) break;
        w += (wchar_t)v;
    }
    return w;
}
XSTD(DWORD, XCreateSaveGame, 24, (LPCSTR root, LPCWSTR name, DWORD disp, DWORD flags, LPSTR path, UINT n)) {
    std::string r; SaveRoot(root, r);
    std::string host = r + SaveDirName(name);
    bool existed = GetFileAttributesA(host.c_str()) != INVALID_FILE_ATTRIBUTES;
    if (disp == CREATE_NEW && existed) return ERROR_ALREADY_EXISTS;
    if (disp == OPEN_EXISTING && !existed) return ERROR_FILE_NOT_FOUND;
    CreateDirectoryA(host.c_str(), nullptr);
    // hand back an Xbox-style path that maps to the same folder
    std::string xp = std::string(root);
    if (!xp.empty() && xp.back() != '\\') xp += '\\';
    xp += SaveDirName(name) + "\\";
    if (path) { strncpy(path, xp.c_str(), n); path[n - 1] = 0; }
    Log("XCreateSaveGame %s -> %s", xp.c_str(), host.c_str());
    return 0;
}
XSTD(DWORD, XDeleteSaveGame, 8, (LPCSTR root, LPCWSTR name)) {
    std::string r; SaveRoot(root, r);
    std::string host = r + SaveDirName(name);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((host + "\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) DeleteFileA((host + "\\" + fd.cFileName).c_str()); } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    return RemoveDirectoryA(host.c_str()) ? 0 : GetLastError();
}
struct SaveFind { HANDLE h; std::string root, xroot; };
static bool FillSave(SaveFind *s, WIN32_FIND_DATAA &fd, XGAME_FIND_DATA *out) {
    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.') return false;
    memset(out, 0, sizeof *out);
    out->fd = fd;
    _snprintf(out->szSaveGameDirectory, MAX_PATH, "%s%s\\", s->xroot.c_str(), fd.cFileName);
    std::wstring w = SaveNameFromDir(fd.cFileName);
    wcsncpy(out->szSaveGameName, w.c_str(), 63);
    return true;
}
XSTD(HANDLE, XFindFirstSaveGame, 8, (LPCSTR root, XGAME_FIND_DATA *out)) {
    auto s = new SaveFind;
    SaveRoot(root, s->root);
    s->xroot = root;
    if (!s->xroot.empty() && s->xroot.back() != '\\') s->xroot += '\\';
    WIN32_FIND_DATAA fd;
    s->h = FindFirstFileA((s->root + "*").c_str(), &fd);
    if (s->h == INVALID_HANDLE_VALUE) { delete s; SetLastError(ERROR_NO_MORE_FILES); return INVALID_HANDLE_VALUE; }
    do { if (FillSave(s, fd, out)) return s; } while (FindNextFileA(s->h, &fd));
    FindClose(s->h); delete s;
    SetLastError(ERROR_NO_MORE_FILES);
    return INVALID_HANDLE_VALUE;
}
XSTD(BOOL, XFindNextSaveGame, 8, (HANDLE h, XGAME_FIND_DATA *out)) {
    auto s = (SaveFind *)h;
    WIN32_FIND_DATAA fd;
    while (FindNextFileA(s->h, &fd)) if (FillSave(s, fd, out)) return TRUE;
    SetLastError(ERROR_NO_MORE_FILES);
    return FALSE;
}
XSTD(BOOL, XFindClose, 4, (HANDLE h)) {
    auto s = (SaveFind *)h;
    if (s && s != INVALID_HANDLE_VALUE) { FindClose(s->h); delete s; }
    return TRUE;
}

// Memory units: none attached
XSTD(DWORD, XMountMUA, 12, (DWORD, DWORD, PCHAR)) { return ERROR_DEVICE_NOT_CONNECTED; }
XSTD(DWORD, XUnmountMU, 8, (DWORD, DWORD)) { return 0; }
XSTD(DWORD, XMUNameFromDriveLetter, 12, (CHAR, LPWSTR n, UINT c)) { if (n && c) n[0] = 0; return ERROR_DEVICE_NOT_CONNECTED; }
