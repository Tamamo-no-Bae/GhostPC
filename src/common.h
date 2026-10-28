// GhostPC - shared declarations
#pragma once
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <float.h>
#include <shellapi.h>

// ---------------------------------------------------------------------------
// Logging (ghostpc.log next to the exe, plus OutputDebugString)
void Log(const char *fmt, ...);
void Fatal(const char *fmt, ...);   // logs and exits the process (no dialogs)
extern int g_verbose;
#define LOGV(...) do { if (g_verbose) Log(__VA_ARGS__); } while (0)

// ---------------------------------------------------------------------------
// Hook registry. Every replacement for a function inside Ghost.exe registers
// itself under the exact decorated name used in Ghost.map. The loader writes a
// JMP at the original function to the replacement. scripts/checkhooks.py
// verifies that the stdcall/fastcall byte counts match the map.
struct HookReg {
    const char *name;
    void *fn;
    HookReg *next;
    HookReg(const char *n, void *f);
};
extern HookReg *g_hooks;

#define XB_CAT2(a, b) a##b
#define XB_CAT(a, b) XB_CAT2(a, b)

// stdcall:  _Name@N
#define XSTD(ret, name, nbytes, args)                                          \
    extern "C" ret __stdcall xb_##name args;                                   \
    static HookReg XB_CAT(hr_, name)("_" #name "@" #nbytes, (void *)xb_##name); \
    extern "C" ret __stdcall xb_##name args
// fastcall: @Name@N
#define XFAST(ret, name, nbytes, args)                                         \
    extern "C" ret __fastcall xb_##name args;                                  \
    static HookReg XB_CAT(hr_, name)("@" #name "@" #nbytes, (void *)xb_##name); \
    extern "C" ret __fastcall xb_##name args
// cdecl: _Name
#define XCDECL(ret, name, args)                                                \
    extern "C" ret __cdecl xb_##name args;                                     \
    static HookReg XB_CAT(hr_, name)("_" #name, (void *)xb_##name);            \
    extern "C" ret __cdecl xb_##name args
// arbitrary decorated name (C++ symbols etc.)
#define XRAW(decorated, fnptr) static HookReg XB_CAT(hr_raw_, __LINE__)(decorated, (void *)(fnptr))

// ---------------------------------------------------------------------------
// Ghost image / symbols
struct GhostSym { uint32_t addr; const char *name; const char *obj; bool func; };
uint32_t SymAddr(const char *decorated);          // 0 if missing
const GhostSym *SymAt(uint32_t addr);             // nearest symbol <= addr
const char *SymName(uint32_t addr, char *buf, size_t n);  // "name+off"
extern uint32_t g_imageBase, g_imageSize;
// crash reports: what GhostPC's per-frame code is doing (set before each step; logged by the crash filter/watchdog)
extern const char *volatile g_crumb;
template <class T> T *GVar(const char *decorated) { return (T *)SymAddr(decorated); }

// ---------------------------------------------------------------------------
// Paths
extern char g_gameDir[MAX_PATH];   // folder holding Ghost.exe + data (the "D:" drive)
extern char g_saveDir[MAX_PATH];   // T:/U: drives
bool XboxPathToHost(const char *xpath, char *out, size_t n);
extern char g_cmdline[1024];

// ---------------------------------------------------------------------------
// Per-thread init for threads that run Ghost code
void ThreadAttach();

// xboxkrnl import implementation: registered as "krnl:<decorated name from the map>"
#define KSTD(ret, name, nbytes, args)                                          \
    extern "C" ret __stdcall kx_##name args;                                   \
    static HookReg XB_CAT(kr_, name)("krnl:_" #name "@" #nbytes, (void *)kx_##name); \
    extern "C" ret __stdcall kx_##name args
#define KFAST(ret, name, nbytes, args)                                         \
    extern "C" ret __fastcall kx_##name args;                                  \
    static HookReg XB_CAT(kr_, name)("krnl:@" #name "@" #nbytes, (void *)kx_##name); \
    extern "C" ret __fastcall kx_##name args
#define KDATA(name, ptr) static HookReg XB_CAT(kd_, name)("krnl:_" #name, (void *)(ptr))
