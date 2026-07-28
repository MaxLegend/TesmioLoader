// tesmio_plugin.h - the boilerplate every plugin would otherwise repeat.
//
// Include this instead of tesmio_api.h. It binds the host table to file-scope
// names with the same shapes the loader itself uses - Logf, FindIatSlot,
// InstallInlineHook, ReadablePtr, FaultFilter - so code moved out of
// tesmioloader.cpp compiles unchanged, and so a new plugin reads like the
// loader rather than like a foreign thing bolted to it.
//
//   #include "../../src/tesmio_plugin.h"
//
//   extern "C" __declspec(dllexport) unsigned TsmPluginApiVersion(void) { return TSM_API_VERSION; }
//   extern "C" __declspec(dllexport) int TsmPluginInit(const TsmHost* host, TsmPluginInfo* info)
//   {
//       TsmBind(host);
//       ...
//   }
//
// Header-only and every function static, so each plugin gets its own copy. That
// is the point: nothing is shared across the DLL boundary but PODs.

#ifndef TESMIO_PLUGIN_H
#define TESMIO_PLUGIN_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <intrin.h>

#include "tesmio_api.h"

#define DLL_ENGINE "C3DDLL64.dll"
#define DLL_STDIO  "api-ms-win-crt-stdio-l1-1-0.dll"

static const TsmHost*   H;
static HMODULE          g_exe;
static BYTE*            g_exeBase;
static SIZE_T           g_exeSize;
static HMODULE          g_engine;
static const char*      g_baseDir;
static CRITICAL_SECTION g_lock;

// Logf(...) rather than H->log(...), because that is what several hundred lines
// of moved code already say.
#define Logf (H->log)

static void TsmBind(const TsmHost* host)
{
    H         = host;
    g_exe     = (HMODULE)host->exeModule;
    g_exeBase = host->exeBase;
    g_exeSize = host->exeSize;
    g_engine  = (HMODULE)host->engineModule;
    g_baseDir = host->baseDir;
    InitializeCriticalSection(&g_lock);
}

static void** FindIatSlot(HMODULE mod, const char* dll, const char* fn)
{
    return H->findIatSlot((void*)mod, dll, fn);
}

static bool PatchIat(HMODULE mod, const char* dll, const char* fn,
                     void* detour, void** original, const char* label)
{
    return H->patchIat((void*)mod, dll, fn, detour, original, label) != 0;
}

static bool InstallInlineHook(void* target, void* detour, void** trampoline,
                              const BYTE* expect, size_t stolen, const char* label)
{
    return H->installInlineHook(target, detour, trampoline, expect, stolen, label) != 0;
}

static BYTE* AllocNear(BYTE* anchor, SIZE_T size)
{
    return H->allocNear(anchor, size);
}

static bool ReadablePtr(const void* p, size_t n)
{
    return H->readablePtr(p, n) != 0;
}

static LONG FaultFilter(const char* what, PEXCEPTION_POINTERS ep)
{
    return H->faultFilter(what, (void*)ep);
}

static void Trim(char* s)
{
    char* p = s;
    while (*p == ' ' || *p == '\t' || *p == '\r') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    for (char* e = s + strlen(s); e > s; e--)
    {
        char c = e[-1];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
        e[-1] = 0;
    }
}

// Reads a NUL-terminated plain-ASCII name out of a pointer that came from a
// game structure rather than from a call we made. Refuses anything with a byte
// outside 32..126, which is what tells a real name from a stale pointer.
static bool SafeReadStr(const void* p, char* out, size_t n)
{
    out[0] = 0;
    if (!p) return false;

    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;

    const char* s = (const char*)p;
    size_t room = (size_t)(((BYTE*)mbi.BaseAddress + mbi.RegionSize) - (BYTE*)p);
    size_t lim  = (room < n - 1) ? room : n - 1;

    size_t i = 0;
    for (; i < lim; i++)
    {
        char c = s[i];
        if (c == 0) break;
        if ((unsigned char)c < 32 || (unsigned char)c > 126) return false;
        out[i] = c;
    }
    out[i] = 0;
    return i > 0;
}

// A log file of the plugin's own, next to the loader's. The game holds these
// open, so reading one while it runs needs FileShare.ReadWrite.
static HANDLE TsmOpenLog(const char* name)
{
    char p[MAX_PATH];
    _snprintf_s(p, sizeof(p), _TRUNCATE, "%s\\%s", g_baseDir, name);
    return CreateFileA(p, GENERIC_WRITE,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
}

static void TsmWrite(HANDLE h, const char* s, int len)
{
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD wrote = 0;
    WriteFile(h, s, (DWORD)len, &wrote, NULL);
}

#endif // TESMIO_PLUGIN_H
