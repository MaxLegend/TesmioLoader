// tesmioloader - mod gate for Workers & Resources: Soviet Republic
//
// Phase A (done): code execution + a virtual file system over the game's data,
//                 built entirely out of import-table swaps.
// Phase B (here): an inline hook on the game's own resource-name resolver,
//                 first to read out the real resource enum, then to hand back a
//                 reserved slot index for names the base game has never heard of.
//
// The resolver was located by taking the one and only code reference to the
// string "ResourceGet - not found %s" and reading the function bounds out of the
// executable's exception table.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <intrin.h>

// ---------------------------------------------------------------- exported names

#define SYM_READ_FILE   "?C3DHelp_ReadFileIntoBuffer@@YAHPEBDPEAPEADPEAI_N@Z"
#define SYM_FILE_EXISTS "?C3DHelp_CheckIfFileExist@@YA_NPEBD_N1@Z"
#define SYM_LOG_INFO    "?C3DLog_PrintInfo@@YAXPEADZZ"
#define SYM_LOG_WARN    "?C3DLog_PrintWarning@@YAXPEADZZ"
#define SYM_LOG_ERROR   "?C3DLog_PrintError@@YAXPEADZZ"

#define DLL_ENGINE "C3DDLL64.dll"
#define DLL_STDIO  "api-ms-win-crt-stdio-l1-1-0.dll"

// ---------------------------------------------------------------- state

static HMODULE          g_self;
static HMODULE          g_exe;
static BYTE*            g_exeBase;
static char             g_baseDir[MAX_PATH];
static char             g_vfsRoot[MAX_PATH];
static CRITICAL_SECTION g_lock;
static HANDLE           g_hLog   = INVALID_HANDLE_VALUE;
static HANDLE           g_hReads = INVALID_HANDLE_VALUE;

static int  g_traceReads  = 0;
static int  g_logGame     = 1;
static int  g_vfsEnabled  = 1;
static int  g_probeMap    = 0;    // guard-page probe for the deposit map
static LONG g_mapSeen     = 0;    // raised the moment a deposit map is opened
static DWORD WINAPI ProbeThread(LPVOID);
static void  AddMapCopy(BYTE* start, SIZE_T len, int stride, const char* how);
// The map is opened more than once - the engine takes it as a texture and the
// game reads it for itself, back to back. Tracking a single stream meant the
// second open overwrote the first and one of the buffers was never captured.
#define MAX_MAP_STREAMS 6
static FILE* g_mapFiles[MAX_MAP_STREAMS];
static char g_traceFilter[128] = "buildings_types";

static LONG g_nRedirects = 0;

// ---------------------------------------------------------------- logging

static void WriteTo(HANDLE h, const char* s, int len)
{
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    WriteFile(h, s, (DWORD)len, &w, NULL);
}

static void Logf(const char* fmt, ...)
{
    char body[4096];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(body, sizeof(body), _TRUNCATE, fmt, ap);
    va_end(ap);

    SYSTEMTIME t;
    GetLocalTime(&t);

    char line[4200];
    int n = _snprintf_s(line, sizeof(line), _TRUNCATE, "[%02d:%02d:%02d.%03d] %s\r\n",
                        t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, body);
    if (n <= 0) return;

    EnterCriticalSection(&g_lock);
    WriteTo(g_hLog, line, n);
    LeaveCriticalSection(&g_lock);
}

static void TraceRead(const char* kind, const char* path, const char* note)
{
    if (!g_traceReads || g_hReads == INVALID_HANDLE_VALUE || !path) return;
    if (g_traceReads == 1 && g_traceFilter[0] && !strstr(path, g_traceFilter)) return;

    char line[1200];
    int n = _snprintf_s(line, sizeof(line), _TRUNCATE, "%-10s %s%s\r\n",
                        kind, path, note ? note : "");
    if (n <= 0) return;

    EnterCriticalSection(&g_lock);
    WriteTo(g_hReads, line, n);
    LeaveCriticalSection(&g_lock);
}

// ---------------------------------------------------------------- virtual file system

static bool VfsResolve(const char* path, char* out, size_t n)
{
    if (!g_vfsEnabled || !g_vfsRoot[0] || !path || !path[0]) return false;
    if (path[1] == ':' || path[0] == '\\' || path[0] == '/') return false;
    if (path[0] == '.' && (path[1] == '/' || path[1] == '\\')) path += 2;

    char rel[MAX_PATH * 2];
    size_t i = 0;
    for (; path[i] && i < sizeof(rel) - 1; i++)
        rel[i] = (path[i] == '/') ? '\\' : path[i];
    rel[i] = 0;

    if (_snprintf_s(out, n, _TRUNCATE, "%s\\%s", g_vfsRoot, rel) < 0) return false;
    DWORD a = GetFileAttributesA(out);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static bool VfsResolveW(const wchar_t* path, wchar_t* out, size_t n)
{
    if (!g_vfsEnabled || !g_vfsRoot[0] || !path || !path[0]) return false;
    if (path[1] == L':' || path[0] == L'\\' || path[0] == L'/') return false;
    if (path[0] == L'.' && (path[1] == L'/' || path[1] == L'\\')) path += 2;

    wchar_t rel[MAX_PATH * 2];
    size_t i = 0;
    for (; path[i] && i < (sizeof(rel) / sizeof(rel[0])) - 1; i++)
        rel[i] = (path[i] == L'/') ? L'\\' : path[i];
    rel[i] = 0;

    wchar_t rootW[MAX_PATH];
    MultiByteToWideChar(CP_UTF8, 0, g_vfsRoot, -1, rootW, MAX_PATH);
    if (_snwprintf_s(out, n, _TRUNCATE, L"%s\\%s", rootW, rel) < 0) return false;

    DWORD a = GetFileAttributesW(out);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// ---------------------------------------------------------------- IAT hooking

static void** FindIatSlot(HMODULE mod, const char* dll, const char* fn)
{
    BYTE* base = (BYTE*)mod;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return NULL;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return NULL;

    IMAGE_DATA_DIRECTORY* dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir->VirtualAddress) return NULL;

    IMAGE_IMPORT_DESCRIPTOR* imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir->VirtualAddress);
    for (; imp->Name; imp++)
    {
        if (_stricmp((const char*)(base + imp->Name), dll) != 0) continue;
        DWORD nameRva = imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk;
        IMAGE_THUNK_DATA* oft = (IMAGE_THUNK_DATA*)(base + nameRva);
        IMAGE_THUNK_DATA* ft  = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        for (; oft->u1.AddressOfData; oft++, ft++)
        {
            if (IMAGE_SNAP_BY_ORDINAL(oft->u1.Ordinal)) continue;
            IMAGE_IMPORT_BY_NAME* ibn = (IMAGE_IMPORT_BY_NAME*)(base + oft->u1.AddressOfData);
            if (strcmp((const char*)ibn->Name, fn) == 0) return (void**)&ft->u1.Function;
        }
    }
    return NULL;
}

static bool PatchIat(HMODULE mod, const char* dll, const char* fn,
                     void* repl, void** origOut, const char* label)
{
    void** slot = FindIatSlot(mod, dll, fn);
    if (!slot) { Logf("hook FAILED  %-22s (no import slot)", label); return false; }

    DWORD prot = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &prot))
    { Logf("hook FAILED  %-22s (VirtualProtect %lu)", label, GetLastError()); return false; }

    *origOut = *slot;
    *slot = repl;
    VirtualProtect(slot, sizeof(void*), prot, &prot);
    Logf("hook ok      %-22s orig=%p", label, *origOut);
    return true;
}

// ---------------------------------------------------------------- inline hooking

// Places a 14-byte absolute "jmp [rip+0]" over the function entry and rebuilds
// the displaced instructions in a trampoline. Only valid because the stolen
// prologue is position independent - it is byte-compared before we touch memory.
static bool InstallInlineHook(void* target, void* detour, void** trampolineOut,
                              const BYTE* expect, size_t stolen, const char* label)
{
    if (memcmp(target, expect, stolen) != 0)
    {
        Logf("hook FAILED  %-22s prologue mismatch - wrong game build, refusing to patch", label);
        BYTE* t = (BYTE*)target;
        char hex[128]; int o = 0;
        for (size_t i = 0; i < stolen && o < 120; i++) o += _snprintf_s(hex + o, sizeof(hex) - o, _TRUNCATE, "%02X ", t[i]);
        Logf("             found: %s", hex);
        return false;
    }

    BYTE* tramp = (BYTE*)VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) { Logf("hook FAILED  %-22s (VirtualAlloc %lu)", label, GetLastError()); return false; }

    memcpy(tramp, target, stolen);
    BYTE* back = tramp + stolen;
    back[0] = 0xFF; back[1] = 0x25;                       // jmp qword ptr [rip+0]
    *(DWORD*)(back + 2) = 0;
    *(void**)(back + 6) = (BYTE*)target + stolen;

    DWORD prot = 0;
    if (!VirtualProtect(target, stolen, PAGE_EXECUTE_READWRITE, &prot))
    {
        Logf("hook FAILED  %-22s (VirtualProtect %lu)", label, GetLastError());
        VirtualFree(tramp, 0, MEM_RELEASE);
        return false;
    }

    BYTE* p = (BYTE*)target;
    p[0] = 0xFF; p[1] = 0x25;
    *(DWORD*)(p + 2) = 0;
    *(void**)(p + 6) = detour;
    for (size_t i = 14; i < stolen; i++) p[i] = 0x90;     // pad, keeps disassembly sane

    VirtualProtect(target, stolen, prot, &prot);
    FlushInstructionCache(GetCurrentProcess(), target, stolen);

    *trampolineOut = tramp;
    Logf("hook ok      %-22s target=%p tramp=%p (%zu bytes stolen)", label, target, tramp, stolen);
    return true;
}

// ---------------------------------------------------------------- small helpers

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

// Executable memory within +-2GB of an anchor, which is as far as a `call
// rel32` or a `jmp rel32` reaches. Walk allocation granularity outward from the
// anchor until VirtualAlloc takes a hinted address. Never freed - a cave holds
// live code for as long as the process runs.
static BYTE* AllocNear(BYTE* anchor, SIZE_T size)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    SIZE_T gran = si.dwAllocationGranularity;

    for (SIZE_T delta = gran; delta < 0x30000000; delta += gran)
    {
        for (int dir = 0; dir < 2; dir++)
        {
            BYTE* want = dir ? anchor + delta : anchor - delta;
            void* got = VirtualAlloc(want, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
            if (got) return (BYTE*)got;
        }
    }
    return NULL;
}

// ---------------------------------------------------------------- safe pointer reads

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
        if ((unsigned char)c < 32 || (unsigned char)c > 126) return false;  // not a plain name
        out[i] = c;
    }
    out[i] = 0;
    return i > 0;
}

// ---------------------------------------------------------------- hooked engine calls

typedef int  (__cdecl* t_ReadFileIntoBuffer)(const char*, char**, unsigned int*, bool);
typedef bool (__cdecl* t_CheckIfFileExist)(const char*, bool, bool);
typedef void (__cdecl* t_LogPrint)(char*, ...);

static t_ReadFileIntoBuffer o_ReadFile;
static t_CheckIfFileExist   o_FileExists;
static t_LogPrint           o_LogInfo, o_LogWarn, o_LogError;

// The guard-page probe needs a thread, and DllMain runs under the loader lock,
// where CreateThread deadlocks. The first asset read is the earliest point that
// is safely outside it - and early enough, because the deposit maps are not
// opened until a world loads. This used to hang off the resource hook, which is
// no longer in the loader.
static void StartMapProbeOnce()
{
    if (!g_probeMap) return;
    static LONG started = 0;
    if (InterlockedCompareExchange(&started, 1, 0) != 0) return;
    HANDLE h = CreateThread(NULL, 0, ProbeThread, NULL, 0, NULL);
    if (h) CloseHandle(h);
}

static int __cdecl h_ReadFileIntoBuffer(const char* path, char** buf, unsigned int* size, bool flag)
{
    StartMapProbeOnce();
    char over[MAX_PATH * 2];
    if (VfsResolve(path, over, sizeof(over)))
    {
        InterlockedIncrement(&g_nRedirects);
        Logf("vfs  read   %s", path);
        return o_ReadFile(over, buf, size, flag);
    }
    TraceRead("read", path, NULL);
    return o_ReadFile(path, buf, size, flag);
}

static bool __cdecl h_CheckIfFileExist(const char* path, bool a, bool b)
{
    char over[MAX_PATH * 2];
    if (VfsResolve(path, over, sizeof(over))) return true;
    TraceRead("exists", path, NULL);
    return o_FileExists(path, a, b);
}

#define LOG_FORWARD(HOOK, ORIG, TAG)                                  \
    static void __cdecl HOOK(char* fmt, ...)                          \
    {                                                                 \
        char body[4096];                                              \
        va_list ap;                                                   \
        va_start(ap, fmt);                                            \
        _vsnprintf_s(body, sizeof(body), _TRUNCATE, fmt, ap);         \
        va_end(ap);                                                   \
        if (g_logGame) Logf("%s %s", TAG, body);                      \
        if (ORIG) ORIG((char*)"%s", body);                            \
    }

LOG_FORWARD(h_LogInfo,  o_LogInfo,  "game.info ")
LOG_FORWARD(h_LogWarn,  o_LogWarn,  "game.WARN ")
LOG_FORWARD(h_LogError, o_LogError, "game.ERROR")


// ---------------------------------------------------------------- hooked CRT opens

typedef FILE*   (__cdecl* t_fopen)(const char*, const char*);
typedef errno_t (__cdecl* t_fopen_s)(FILE**, const char*, const char*);
typedef FILE*   (__cdecl* t_wfopen)(const wchar_t*, const wchar_t*);
typedef errno_t (__cdecl* t_wfopen_s)(FILE**, const wchar_t*, const wchar_t*);

static t_fopen    o_fopen;
static t_fopen_s  o_fopen_s;
static t_wfopen   o_wfopen;
static t_wfopen_s o_wfopen_s;

static FILE* __cdecl h_fopen(const char* path, const char* mode)
{
    bool isMap = (path && strstr(path, "resourcemap") != NULL);
    if (isMap) InterlockedExchange(&g_mapSeen, 1);

    char over[MAX_PATH * 2];
    FILE* f;
    if (mode && mode[0] == 'r' && VfsResolve(path, over, sizeof(over)))
    {
        InterlockedIncrement(&g_nRedirects);
        Logf("vfs  fopen  %s", path);
        f = o_fopen(over, mode);
    }
    else
    {
        TraceRead("fopen", path, NULL);
        f = o_fopen(path, mode);
    }

    // Remembering the stream lets the fread hook hand us the destination buffer
    // outright - far better than hunting for it afterwards, which took long
    // enough that the game had already finished with it.
    if (isMap && f)
    {
        EnterCriticalSection(&g_lock);
        for (int i = 0; i < MAX_MAP_STREAMS; i++)
            if (!g_mapFiles[i]) { g_mapFiles[i] = f; break; }
        LeaveCriticalSection(&g_lock);
    }
    return f;
}

typedef size_t (__cdecl* t_fread)(void*, size_t, size_t, FILE*);
static t_fread o_fread;

static size_t __cdecl h_fread(void* buf, size_t sz, size_t cnt, FILE* f)
{
    size_t r = o_fread(buf, sz, cnt, f);

    // The stream alone is not enough to go on: the CRT recycles FILE structures,
    // so a later file can be handed the very same pointer. Demand a payload the
    // size of a 1024x1024 BGRA image, and let go of the stream once it arrives.
    size_t bytes = sz * cnt;
    if (f && buf && bytes >= 0x400000 && bytes <= 0x410000)
    {
        bool mine = false;
        EnterCriticalSection(&g_lock);
        for (int i = 0; i < MAX_MAP_STREAMS; i++)
            if (g_mapFiles[i] == f) { g_mapFiles[i] = NULL; mine = true; break; }
        LeaveCriticalSection(&g_lock);

        if (mine) AddMapCopy((BYTE*)buf, bytes, 4, "read straight from the file");
    }
    return r;
}

static errno_t __cdecl h_fopen_s(FILE** f, const char* path, const char* mode)
{
    char over[MAX_PATH * 2];
    if (mode && mode[0] == 'r' && VfsResolve(path, over, sizeof(over)))
    {
        InterlockedIncrement(&g_nRedirects);
        Logf("vfs  fopen_s %s", path);
        return o_fopen_s(f, over, mode);
    }
    TraceRead("fopen_s", path, NULL);
    return o_fopen_s(f, path, mode);
}

static FILE* __cdecl h_wfopen(const wchar_t* path, const wchar_t* mode)
{
    wchar_t over[MAX_PATH * 2];
    if (mode && mode[0] == L'r' && VfsResolveW(path, over, MAX_PATH * 2))
    {
        InterlockedIncrement(&g_nRedirects);
        return o_wfopen(over, mode);
    }
    return o_wfopen(path, mode);
}

static errno_t __cdecl h_wfopen_s(FILE** f, const wchar_t* path, const wchar_t* mode)
{
    wchar_t over[MAX_PATH * 2];
    if (mode && mode[0] == L'r' && VfsResolveW(path, over, MAX_PATH * 2))
    {
        InterlockedIncrement(&g_nRedirects);
        return o_wfopen_s(f, over, mode);
    }
    return o_wfopen_s(f, path, mode);
}

// The engine opens plenty of files itself, and some of it goes through the Win32
// API rather than the CRT - textures in particular.
typedef HANDLE (WINAPI* t_CreateFileA)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
static t_CreateFileA o_CreateFileA;

static HANDLE WINAPI h_CreateFileA(LPCSTR path, DWORD access, DWORD share,
                                   LPSECURITY_ATTRIBUTES sa, DWORD disp,
                                   DWORD flags, HANDLE tmpl)
{
    char over[MAX_PATH * 2];
    if (!(access & GENERIC_WRITE) && VfsResolve(path, over, sizeof(over)))
    {
        InterlockedIncrement(&g_nRedirects);
        Logf("vfs  CreateFileA %s", path);
        return o_CreateFileA(over, access, share, sa, disp, flags, tmpl);
    }
    TraceRead("CreateFileA", path, NULL);
    return o_CreateFileA(path, access, share, sa, disp, flags, tmpl);
}

static void TraceReadW(const char* kind, const wchar_t* path, const char* note)
{
    if (!g_traceReads || !path) return;
    char utf8[MAX_PATH * 2];
    if (!WideCharToMultiByte(CP_UTF8, 0, path, -1, utf8, sizeof(utf8), NULL, NULL)) return;
    TraceRead(kind, utf8, note);
}

// The wide-character openers. Textures come through these, which is why the
// deposit map slipped past every hook we had.
typedef HANDLE (WINAPI* t_CreateFileW)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef HANDLE (WINAPI* t_CreateFile2)(LPCWSTR, DWORD, DWORD, DWORD, LPVOID);

static t_CreateFileW o_CreateFileW;
static t_CreateFile2 o_CreateFile2;

static HANDLE WINAPI h_CreateFileW(LPCWSTR path, DWORD access, DWORD share,
                                   LPSECURITY_ATTRIBUTES sa, DWORD disp,
                                   DWORD flags, HANDLE tmpl)
{
    wchar_t over[MAX_PATH * 2];
    if (!(access & GENERIC_WRITE) && VfsResolveW(path, over, MAX_PATH * 2))
    {
        InterlockedIncrement(&g_nRedirects);
        TraceReadW("CreateFileW", path, "   -> VFS");
        return o_CreateFileW(over, access, share, sa, disp, flags, tmpl);
    }
    TraceReadW("CreateFileW", path, NULL);
    return o_CreateFileW(path, access, share, sa, disp, flags, tmpl);
}

static HANDLE WINAPI h_CreateFile2(LPCWSTR path, DWORD access, DWORD share,
                                   DWORD disp, LPVOID ext)
{
    wchar_t over[MAX_PATH * 2];
    if (!(access & GENERIC_WRITE) && VfsResolveW(path, over, MAX_PATH * 2))
    {
        InterlockedIncrement(&g_nRedirects);
        TraceReadW("CreateFile2", path, "   -> VFS");
        return o_CreateFile2(over, access, share, disp, ext);
    }
    TraceReadW("CreateFile2", path, NULL);
    return o_CreateFile2(path, access, share, disp, ext);
}

// ---------------------------------------------------------------- texture texels
//
// The deposit maps are plain textures. Nothing parses their pixels: the loader
// calls CreateManagedTexture, then TextureAccessInitTempResource to get a copy
// the CPU can reach, and the game reads individual texels through the texture's
// vtable. Virtual calls never appear in an import table, which is exactly why
// every hook we had - and the guard page - saw nothing.
//
// C3DAPI_D3D11_TEXTURE vtable, C3DDLL64.dll RVA 0x187BF0:
//   [ 2] +0x10  Load2DFromFile
//   [19] +0x98  TextureAccessInitTempResource
//   [20] +0xA0  TextureAccesGetTexel(x, y) -> colour
//   [23] +0xB8  TextureAccesSetTexel(x, y, colour)
//   [36] +0x120 SaveToDDS          - how depletion survives a save
#define TEX_VTABLE_RVA   0x187BF0
#define TEX_SLOT_LOAD    2
#define TEX_SLOT_GETTEXEL 20

static int   g_probeTexel;

typedef int   (*t_Load2DFromFile)(void*, const char*, int, int, unsigned int, int);
typedef DWORD (*t_GetTexel)(void*, int, int);

static t_Load2DFromFile o_Load2DFromFile;
static t_GetTexel       o_GetTexel;

// Texture objects whose file name marked them as deposit maps.
static void* g_depositTex[4];
static char  g_depositName[4][64];
static int   g_depositCount;

static LONG  g_texelLogged;
static BYTE* g_texelCallers[16];
static int   g_texelCallerCount;

static int h_Load2DFromFile(void* self, const char* file, int a, int b, unsigned int c, int fmt)
{
    int r = o_Load2DFromFile(self, file, a, b, c, fmt);

    if (file && strstr(file, "resourcemap"))
    {
        EnterCriticalSection(&g_lock);
        if (g_depositCount < 4)
        {
            g_depositTex[g_depositCount] = self;
            const char* slash = strrchr(file, '/');
            strncpy_s(g_depositName[g_depositCount], 64, slash ? slash + 1 : file, _TRUNCATE);
            Logf("texel  deposit map texture %d = %p (%s)", g_depositCount, self, file);
            g_depositCount++;
        }
        LeaveCriticalSection(&g_lock);
    }
    return r;
}

static DWORD h_GetTexel(void* self, int x, int y)
{
    DWORD r = o_GetTexel(self, x, y);

    int which = -1;
    for (int i = 0; i < g_depositCount; i++)
        if (g_depositTex[i] == self) { which = i; break; }
    if (which < 0) return r;

    BYTE* ret = (BYTE*)_ReturnAddress();
    bool  fresh = true;

    EnterCriticalSection(&g_lock);
    for (int i = 0; i < g_texelCallerCount; i++)
        if (g_texelCallers[i] == ret) { fresh = false; break; }
    if (fresh && g_texelCallerCount < 16) g_texelCallers[g_texelCallerCount++] = ret;
    LONG n = ++g_texelLogged;
    LeaveCriticalSection(&g_lock);

    // Every distinct call site, plus the first handful of samples for context.
    if (fresh || n <= 24)
    {
        size_t rva = (size_t)(ret - g_exeBase);
        Logf("texel  %s (%d,%d) -> %08lX   %sfrom SOVIET64.exe + 0x%zX",
             g_depositName[which], x, y, r, fresh ? "NEW SITE " : "", rva);
    }
    return r;
}

static void HookTextureVtable(HMODULE engine)
{
    if (!g_probeTexel || !engine) return;

    void** vt = (void**)((BYTE*)engine + TEX_VTABLE_RVA);
    DWORD  prot = 0;
    if (!VirtualProtect(vt, 64 * sizeof(void*), PAGE_READWRITE, &prot))
    {
        Logf("texel  cannot write the texture vtable (%lu)", GetLastError());
        return;
    }

    o_Load2DFromFile = (t_Load2DFromFile)vt[TEX_SLOT_LOAD];
    o_GetTexel       = (t_GetTexel)vt[TEX_SLOT_GETTEXEL];
    vt[TEX_SLOT_LOAD]     = (void*)h_Load2DFromFile;
    vt[TEX_SLOT_GETTEXEL] = (void*)h_GetTexel;

    VirtualProtect(vt, 64 * sizeof(void*), prot, &prot);
    FlushInstructionCache(GetCurrentProcess(), vt, 64 * sizeof(void*));

    Logf("texel  vtable hooked: Load2DFromFile=%p GetTexel=%p", o_Load2DFromFile, o_GetTexel);
}

// ---------------------------------------------------------------- deposit map probe
//
// The function that samples the deposit map touches no string literal, so the
// xref trick that found everything else cannot find it. Instead we let the game
// point at it: locate the loaded map in memory by a marker baked into its unused
// alpha channel, turn those pages into guard pages, and note who faults on them.
// Each hit hands us the address of an instruction that reads the map.

#ifndef STATUS_GUARD_PAGE_VIOLATION
#define STATUS_GUARD_PAGE_VIOLATION ((DWORD)0x80000001L)
#endif

// Written into the alpha channel of resourcemap2.dds at a known pixel. Values
// are arbitrary but unlikely to occur together in real deposit data.
static const BYTE kMapMarker[16] = {
    0xA5, 0x5A, 0xC3, 0x3C, 0x99, 0x66, 0xF0, 0x0F,
    0x11, 0xEE, 0x77, 0x88, 0xB4, 0x4B, 0xD2, 0x2D
};
#define MAP_MARKER_PIXEL (1024 * 3 + 100)   // row 3, column 100

// The file is read into a staging buffer and then converted into whatever the
// game actually samples, so a single buffer is not enough: guard every copy of
// the marker we can find and let the hits say which one is live.
#define MAX_MAP_COPIES 8

struct MapCopy
{
    BYTE* from;      // page-aligned, strictly inside the buffer - guarding
    BYTE* to;        // outside it yields faults we cannot attribute, and an
    DWORD prot;      // unattributed guard violation kills the process
    int   stride;
    LONG  armed;
};
static MapCopy g_maps[MAX_MAP_COPIES];
static int     g_mapCount;

static BYTE*  g_probeSeen[32];
static int    g_probeSeenCount;

static SIZE_T g_exeSize;

static const char* ExceptionName(DWORD c)
{
    switch (c)
    {
        case EXCEPTION_ACCESS_VIOLATION:      return "ACCESS_VIOLATION";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "ARRAY_BOUNDS_EXCEEDED";
        case EXCEPTION_DATATYPE_MISALIGNMENT: return "DATATYPE_MISALIGNMENT";
        case EXCEPTION_ILLEGAL_INSTRUCTION:   return "ILLEGAL_INSTRUCTION";
        case EXCEPTION_INT_DIVIDE_BY_ZERO:    return "INT_DIVIDE_BY_ZERO";
        case EXCEPTION_PRIV_INSTRUCTION:      return "PRIV_INSTRUCTION";
        case EXCEPTION_STACK_OVERFLOW:        return "STACK_OVERFLOW";
        default:                              return NULL;
    }
}

// Turns "it crashed" into an address. Passes everything through untouched -
// this only observes, it never swallows an exception.
static LONG volatile g_inCrashHandler;
static LONG          g_crashesReported;

static void ArmMapGuard();

// A guard page fires once and disarms itself, so the instruction that tripped it
// is exactly what we want to record. Returning CONTINUE_EXECUTION re-runs it,
// this time against an ordinary page, and the game carries on unaware.
static LONG OnGuardPage(PEXCEPTION_POINTERS ep)
{
    BYTE* at = (BYTE*)ep->ExceptionRecord->ExceptionInformation[1];

    int hit = -1;
    for (int i = 0; i < g_mapCount; i++)
        if (at >= g_maps[i].from && at < g_maps[i].to) { hit = i; break; }
    if (hit < 0) return EXCEPTION_CONTINUE_SEARCH;

    BYTE* rip = (BYTE*)ep->ContextRecord->Rip;

    // Our own scanner walking these pages is not a finding. Worse, every such
    // hit disarms the guard, leaving a window in which a real read goes unseen -
    // so put it straight back.
    {
        HMODULE m = NULL;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)rip, &m) && m == g_self)
        {
            InterlockedExchange(&g_maps[hit].armed, 0);
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }

    bool fresh = true;
    for (int i = 0; i < g_probeSeenCount; i++)
        if (g_probeSeen[i] == rip) { fresh = false; break; }

    if (fresh && g_probeSeenCount < 32)
    {
        g_probeSeen[g_probeSeenCount++] = rip;

        int    st  = g_maps[hit].stride;
        size_t off = (size_t)(at - g_maps[hit].from);
        const char* where = "?";
        size_t rva = 0;
        if (rip >= g_exeBase && rip < g_exeBase + g_exeSize) { where = "SOVIET64.exe"; rva = (size_t)(rip - g_exeBase); }
        else
        {
            HMODULE m = NULL;
            char nm[MAX_PATH] = "?";
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   (LPCSTR)rip, &m) && m)
            {
                GetModuleFileNameA(m, nm, MAX_PATH);
                const char* slash = strrchr(nm, '\\');
                where = slash ? slash + 1 : nm;
                rva = (size_t)(rip - (BYTE*)m);
            }
        }

        Logf("probe  copy%d read at +0x%zX (pixel %zu, channel %d) from %s + 0x%zX",
             hit, off, off / (st ? st : 1), st == 4 ? (int)(off % 4) : 0, where, rva);
    }

    InterlockedExchange(&g_maps[hit].armed, 0);
    return EXCEPTION_CONTINUE_EXECUTION;
}

static LONG CALLBACK CrashHandler(PEXCEPTION_POINTERS ep)
{
    if (ep->ExceptionRecord->ExceptionCode == STATUS_GUARD_PAGE_VIOLATION &&
        ep->ExceptionRecord->NumberParameters >= 2)
        return OnGuardPage(ep);

    const char* what = ExceptionName(ep->ExceptionRecord->ExceptionCode);
    if (!what) return EXCEPTION_CONTINUE_SEARCH;

    // Faults inside our own module are the memory scanner walking off the end of
    // a region; they are caught by its __except and are not worth reporting.
    {
        HMODULE m = NULL;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)ep->ExceptionRecord->ExceptionAddress, &m) && m == g_self)
            return EXCEPTION_CONTINUE_SEARCH;
    }

    // Never report from inside a report: a fault raised by this handler would
    // otherwise re-enter it and bury the original crash under its own noise.
    if (InterlockedCompareExchange(&g_inCrashHandler, 1, 0) != 0)
        return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedIncrement(&g_crashesReported) > 8)
    {
        InterlockedExchange(&g_inCrashHandler, 0);
        return EXCEPTION_CONTINUE_SEARCH;
    }

    BYTE* addr = (BYTE*)ep->ExceptionRecord->ExceptionAddress;
    Logf("=== CRASH: %s at %p ===", what, addr);

    if (addr >= g_exeBase && addr < g_exeBase + g_exeSize)
        Logf("    SOVIET64.exe + 0x%zX", (size_t)(addr - g_exeBase));
    else
    {
        HMODULE m = NULL;
        char name[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)addr, &m) && m)
        {
            GetModuleFileNameA(m, name, MAX_PATH);
            Logf("    %s + 0x%zX", name, (size_t)(addr - (BYTE*)m));
        }
    }

    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
        ep->ExceptionRecord->NumberParameters >= 2)
    {
        void* bad = (void*)ep->ExceptionRecord->ExceptionInformation[1];
        Logf("    %s address %p",
             ep->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading", bad);

    }

    CONTEXT* c = ep->ContextRecord;
    Logf("    rax=%016llX rbx=%016llX rcx=%016llX rdx=%016llX",
         c->Rax, c->Rbx, c->Rcx, c->Rdx);
    Logf("    rsi=%016llX rdi=%016llX r8 =%016llX r9 =%016llX",
         c->Rsi, c->Rdi, c->R8, c->R9);
    Logf("    rsp=%016llX rbp=%016llX rip=%016llX", c->Rsp, c->Rbp, c->Rip);

    // Cheap stack walk: any qword on the stack pointing into the executable is
    // very likely a return address.
    Logf("    --- return addresses on the stack ---");
    {
        // Bound the walk by the stack's own allocation. Reading past it faults,
        // which is exactly what turned the last report into a cascade.
        MEMORY_BASIC_INFORMATION smbi;
        BYTE* limit = NULL;
        if (VirtualQuery((void*)c->Rsp, &smbi, sizeof(smbi)))
            limit = (BYTE*)smbi.BaseAddress + smbi.RegionSize;

        BYTE** sp = (BYTE**)c->Rsp;
        int shown = 0;
        for (int i = 0; i < 768 && shown < 16; i++)
        {
            if (limit && (BYTE*)(sp + i + 1) > limit) break;
            BYTE* v = sp[i];
            if (v >= g_exeBase && v < g_exeBase + g_exeSize)
            {
                Logf("      SOVIET64.exe + 0x%zX", (size_t)(v - g_exeBase));
                shown++;
            }
        }
    }

    Logf("=== end crash report ===");
    FlushFileBuffers(g_hLog);
    InterlockedExchange(&g_inCrashHandler, 0);
    return EXCEPTION_CONTINUE_SEARCH;
}

static void ArmMapGuards()
{
    for (int i = 0; i < g_mapCount; i++)
    {
        MapCopy& m = g_maps[i];
        if (m.to <= m.from) continue;
        if (InterlockedCompareExchange(&m.armed, 1, 0) != 0) continue;

        DWORD old = 0;
        if (!VirtualProtect(m.from, (SIZE_T)(m.to - m.from), m.prot | PAGE_GUARD, &old))
            InterlockedExchange(&m.armed, 0);
    }
}

static bool AlreadyKnown(BYTE* start)
{
    for (int i = 0; i < g_mapCount; i++)
        if (start + 0x1000 >= g_maps[i].from && start <= g_maps[i].to) return true;
    return false;
}

static void AddMapCopy(BYTE* start, SIZE_T len, int stride, const char* how)
{
    if (!g_probeMap || g_mapCount >= MAX_MAP_COPIES) return;

    EnterCriticalSection(&g_lock);
    if (!AlreadyKnown(start))
    {
        const uintptr_t PG = 0xFFF;
        MapCopy& m = g_maps[g_mapCount];
        m.from   = (BYTE*)(((uintptr_t)start + PG) & ~PG);
        m.to     = (BYTE*)(((uintptr_t)(start + len)) & ~PG);
        m.stride = stride;
        m.armed  = 0;

        MEMORY_BASIC_INFORMATION b2;
        m.prot = VirtualQuery(m.from, &b2, sizeof(b2)) ? b2.Protect : PAGE_READWRITE;

        if (m.to > m.from)
        {
            Logf("probe  copy%d at %p (%zu bytes, stride %d, %s), guarding %p..%p",
                 g_mapCount, start, len, stride, how, m.from, m.to);
            g_mapCount++;

            // Armed here and not on the probe thread's next tick: the code that
            // converts the pixels runs immediately after the read returns, well
            // inside the 50 ms we would otherwise wait.
            DWORD old = 0;
            if (VirtualProtect(m.from, (SIZE_T)(m.to - m.from), m.prot | PAGE_GUARD, &old))
                InterlockedExchange(&m.armed, 1);
        }
    }
    LeaveCriticalSection(&g_lock);
}

// Records every copy of the map it can find. The marker lives in the alpha
// channel, so it shows up at stride 4 while the data is still interleaved as
// loaded, and at stride 1 once the channel is pulled into a plane of its own.
static int FindMapCopies()
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    BYTE* p   = (BYTE*)si.lpMinimumApplicationAddress;
    BYTE* top = (BYTE*)si.lpMaximumApplicationAddress;
    int added = 0;

    while (p < top && g_mapCount < MAX_MAP_COPIES)
    {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(p, &mbi, sizeof(mbi))) break;

        bool usable = mbi.State == MEM_COMMIT &&
                      mbi.RegionSize >= 0x100000 &&
                      !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
                      (mbi.Protect & (PAGE_READWRITE | PAGE_READONLY | PAGE_WRITECOPY));

        // Never read a region we are already guarding: that trips our own trap.
        if (usable)
            for (int i = 0; i < g_mapCount; i++)
                if ((BYTE*)mbi.BaseAddress < g_maps[i].to &&
                    (BYTE*)mbi.BaseAddress + mbi.RegionSize > g_maps[i].from)
                { usable = false; break; }

        if (usable)
        {
            BYTE*  q = (BYTE*)mbi.BaseAddress;
            SIZE_T n = mbi.RegionSize;
            // memchr rather than a byte loop, and both strides checked from one
            // position. The previous version needed 39 seconds over this
            // process's few gigabytes, by which time the map had been converted
            // and the staging buffer abandoned.
            __try
            {
                BYTE* cur = q;
                BYTE* end = q + n - 64;
                while (cur < end && g_mapCount < MAX_MAP_COPIES)
                {
                    BYTE* f = (BYTE*)memchr(cur, kMapMarker[0], (size_t)(end - cur));
                    if (!f) break;

                    for (int t = 0; t < 2; t++)
                    {
                        int st = t == 0 ? 4 : 1;
                        int k  = 1;
                        for (; k < 16; k++)
                            if (f[(SIZE_T)k * st] != kMapMarker[k]) break;
                        if (k != 16) continue;

                        // The marker lives in the alpha byte, three past the
                        // start of its pixel while the data is interleaved.
                        SIZE_T back = (SIZE_T)MAP_MARKER_PIXEL * st + (st == 4 ? 3 : 0);
                        if ((SIZE_T)(f - q) < back) continue;

                        BYTE*  start = f - back;
                        SIZE_T want  = (SIZE_T)1024 * 1024 * st;
                        SIZE_T avail = (SIZE_T)(q + n - start);
                        AddMapCopy(start, want < avail ? want : avail, st, "marker scan");
                        added++;
                    }
                    cur = f + 1;
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER) { }
        }
        p = (BYTE*)mbi.BaseAddress + mbi.RegionSize;
    }
    return added;
}

static DWORD WINAPI ProbeThread(LPVOID)
{
    // Wait on the file rather than on the clock. Scanning has to begin the
    // moment the map exists: last time it started half a minute late and by
    // then the pass that converts the raw pixels was long finished.
    for (int i = 0; i < 3000 && !g_mapSeen; i++) Sleep(100);
    if (!g_mapSeen)
    {
        Logf("probe  no deposit map was ever opened");
        return 0;
    }
    Logf("probe  deposit map opened - scanning for it in memory");

    for (int round = 0; round < 700 && g_probeSeenCount < 32; round++)
    {
        if (round < 30) FindMapCopies();   // early rounds only: the scan is slow
        ArmMapGuards();
        Sleep(round < 30 ? 50 : 400);
    }
    Logf("probe  finished: %d copies watched, %d distinct readers", g_mapCount, g_probeSeenCount);
    return 0;
}

// ---------------------------------------------------------------- guards for injected UI
//
// Everything below this point runs inside the game's own draw and input paths,
// reading structures this project mapped by inference. A wrong offset there is
// not a wrong pixel, it is a dead process - and the vectored crash handler
// deliberately ignores faults raised inside this module, so such a crash would
// leave nothing in the log at all.
//
// So every injected entry point runs under __try, and a fault logs where it
// happened and switches that feature off for the rest of the session. The game
// keeps running without the mod's addition instead of dying with it.

static LONG FaultFilter(const char* what, PEXCEPTION_POINTERS ep)
{
    BYTE* addr = (BYTE*)ep->ExceptionRecord->ExceptionAddress;
    Logf("FAULT    %s: code %08lX at %p", what,
         (unsigned long)ep->ExceptionRecord->ExceptionCode, addr);
    if (addr >= g_exeBase && addr < g_exeBase + g_exeSize)
        Logf("         SOVIET64.exe + 0x%zX", (size_t)(addr - g_exeBase));
    else
        Logf("         inside tesmioloader.dll (base %p)", g_self);
    if (ep->ExceptionRecord->NumberParameters >= 2)
        Logf("         %s of %p",
             ep->ExceptionRecord->ExceptionInformation[0] ? "write to" : "read from",
             (void*)ep->ExceptionRecord->ExceptionInformation[1]);
    return EXCEPTION_EXECUTE_HANDLER;
}

// True when the whole range is committed and readable. Used before following a
// pointer that came out of a game structure rather than out of a call we made:
// a stale resource record hands back a plausible-looking icon pointer, and
// dereferencing it is the difference between a missing icon and a crash.
//
// It has to **walk** the regions rather than trust one VirtualQuery, because a
// region is a run of pages sharing state and protection - not an allocation.
// The engine's big globals live in .data, which the image loader maps
// PAGE_WRITECOPY; the first write to a page turns it into a private
// PAGE_READWRITE one and splits the region there. So the reported RegionSize
// around any long-lived object shrinks as the game runs, and a single-query
// check on a 54 KB structure starts failing partway through a session even
// though every byte of it is perfectly readable.
//
// That is exactly what silently disabled the terrain-editor brushes: the panel
// hook kept drawing the buttons because it checks nothing, while the cursor and
// dispatch hooks - which asked whether all 0xD430 bytes of the editor object
// were readable before reading one pointer out of it - began answering no. The
// symptom was a tool that selected but had no brush.
static bool ReadablePtr(const void* p, size_t n)
{
    if (!p) return false;

    const BYTE* at   = (const BYTE*)p;
    const BYTE* want = at + n;
    while (at < want)
    {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(at, &mbi, sizeof(mbi))) return false;
        if (mbi.State != MEM_COMMIT) return false;
        if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;

        const BYTE* end = (const BYTE*)mbi.BaseAddress + mbi.RegionSize;
        if (end <= at) return false;    // no progress: refuse rather than spin
        at = end;
    }
    return true;
}

// ---------------------------------------------------------------- plugins
//
// Everything above this line is compiled into the loader because it is
// infrastructure: the VFS, the resource registry, the deposit registry and the
// three subsystems that registry drives. A *feature* does not have to be.
//
// A plugin is an ordinary DLL in `plugins\`. The loader finds it, checks it was
// built against a compatible `tesmio_api.h`, and hands it a table of the things
// the loader already knows how to do - where the executable is, how to swap an
// import, how to splice a hook, how to log, and what deposits.ini declared. The
// plugin patches the game itself from there.
//
// The point is not isolation; a plugin is in the same address space and can
// corrupt the process exactly as easily as the loader can. The point is that a
// feature can be written, rebuilt and removed without touching this file, and
// that shipping one is copying a DLL.

#include "tesmio_api.h"

#define MAX_PLUGINS 32

struct LoadedPlugin
{
    HMODULE          module;
    char             file[64];
    const char*      name;
    const char*      version;
    TsmPluginStartFn start;      // optional second phase; null when absent
};
static LoadedPlugin g_plugin[MAX_PLUGINS];
static int          g_pluginCount;
static int          g_plugins = 1;
static char         g_pluginDir[MAX_PATH];

// --- the host table -------------------------------------------------------
//
// Thin adapters rather than the internals directly: the internals use C++
// types and their signatures are free to change, while everything here is
// frozen by TSM_API_VERSION.

static void**  api_FindIatSlot(void* mod, const char* dll, const char* fn)
{
    return FindIatSlot((HMODULE)mod, dll, fn);
}

static int api_PatchIat(void* mod, const char* dll, const char* fn,
                        void* detour, void** original, const char* label)
{
    return PatchIat((HMODULE)mod, dll, fn, detour, original, label) ? 1 : 0;
}

static int api_InstallInlineHook(void* target, void* detour, void** tramp,
                                 const unsigned char* expect, size_t stolen,
                                 const char* label)
{
    return InstallInlineHook(target, detour, tramp, expect, stolen, label) ? 1 : 0;
}

static unsigned char* api_AllocNear(unsigned char* anchor, size_t size)
{
    return AllocNear(anchor, size);
}

static int api_ReadablePtr(const void* p, size_t n)
{
    return ReadablePtr(p, n) ? 1 : 0;
}

static long api_FaultFilter(const char* what, void* ep)
{
    return FaultFilter(what, (PEXCEPTION_POINTERS)ep);
}

static void api_IniPath(const char* iniName, char* out, size_t n)
{
    _snprintf_s(out, n, _TRUNCATE, "%s\\%s", g_baseDir, iniName ? iniName : "tesmioloader.ini");
}

static int api_ConfigInt(const char* iniName, const char* section,
                         const char* key, int fallback)
{
    char ini[MAX_PATH];
    api_IniPath(iniName, ini, sizeof(ini));
    return GetPrivateProfileIntA(section, key, fallback, ini);
}

static int api_ConfigString(const char* iniName, const char* section,
                            const char* key, char* out, int outSize,
                            const char* fallback)
{
    char ini[MAX_PATH];
    api_IniPath(iniName, ini, sizeof(ini));
    DWORD n = GetPrivateProfileStringA(section, key, fallback ? fallback : "",
                                       out, (DWORD)outSize, ini);
    Trim(out);
    return (int)n;
}

// --- the service noticeboard ----------------------------------------------
//
// The host never looks inside an interface and has no opinion on what any of
// them mean. A name, a version and a pointer - that is the whole mechanism, and
// it is what lets `depletion` use `deposits` without either knowing the other
// exists at build time.
//
// Publishing is done from Init and looking up from Start, so by the time anyone
// asks, everyone has published.

#define MAX_SERVICES 32

struct Service
{
    char        name[32];
    unsigned    version;
    const void* iface;
    const char* provider;
};
static Service g_service[MAX_SERVICES];
static int     g_serviceCount;
static const char* g_currentPlugin = "?";

static int api_Provide(const char* name, unsigned version, const void* iface)
{
    if (!name || !iface) return 0;
    if (g_serviceCount >= MAX_SERVICES)
    {
        Logf("plugin   service \"%s\" refused - only %d fit", name, MAX_SERVICES);
        return 0;
    }
    for (int i = 0; i < g_serviceCount; i++)
        if (_stricmp(g_service[i].name, name) == 0 && g_service[i].version == version)
        {
            Logf("plugin   service \"%s\" v%u already provided by %s",
                 name, version, g_service[i].provider);
            return 0;
        }

    Service* s = &g_service[g_serviceCount++];
    strncpy_s(s->name, sizeof(s->name), name, _TRUNCATE);
    s->version  = version;
    s->iface    = iface;
    s->provider = g_currentPlugin;
    Logf("plugin   service \"%s\" v%u from %s", name, version, s->provider);
    return 1;
}

static const void* api_Consume(const char* name, unsigned version)
{
    if (!name) return NULL;
    for (int i = 0; i < g_serviceCount; i++)
        if (_stricmp(g_service[i].name, name) == 0 && g_service[i].version == version)
            return g_service[i].iface;
    return NULL;
}

static TsmHost g_host;

static void BuildHostTable()
{
    memset(&g_host, 0, sizeof(g_host));
    g_host.apiVersion   = TSM_API_VERSION;
    g_host.structSize   = (unsigned)sizeof(TsmHost);

    g_host.exeModule    = (void*)g_exe;
    g_host.exeBase      = g_exeBase;
    g_host.exeSize      = g_exeSize;
    g_host.engineModule = (void*)GetModuleHandleA(DLL_ENGINE);
    g_host.baseDir      = g_baseDir;
    g_host.pluginDir    = g_pluginDir;

    g_host.log               = Logf;
    g_host.findIatSlot       = api_FindIatSlot;
    g_host.patchIat          = api_PatchIat;
    g_host.installInlineHook = api_InstallInlineHook;
    g_host.allocNear         = api_AllocNear;
    g_host.readablePtr       = api_ReadablePtr;
    g_host.faultFilter       = api_FaultFilter;
    g_host.configInt         = api_ConfigInt;
    g_host.configString      = api_ConfigString;
    g_host.provide           = api_Provide;
    g_host.consume           = api_Consume;
}

// --- loading --------------------------------------------------------------

static void LoadOnePlugin(const char* file)
{
    if (g_pluginCount >= MAX_PLUGINS)
    {
        Logf("plugin   \"%s\" skipped - only %d fit", file, MAX_PLUGINS);
        return;
    }

    char path[MAX_PATH];
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\%s", g_pluginDir, file);

    // The full path, and LOAD_WITH_ALTERED_SEARCH_PATH so a plugin's own
    // dependencies resolve next to it rather than out of the game folder.
    HMODULE mod = LoadLibraryExA(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!mod)
    {
        Logf("plugin   \"%s\" failed to load (%lu)", file, GetLastError());
        return;
    }

    TsmPluginApiVersionFn ver =
        (TsmPluginApiVersionFn)GetProcAddress(mod, TSM_EXPORT_APIVERSION);
    TsmPluginInitFn init =
        (TsmPluginInitFn)GetProcAddress(mod, TSM_EXPORT_INIT);

    if (!ver || !init)
    {
        Logf("plugin   \"%s\" is not a tesmioloader plugin - no %s/%s export",
             file, TSM_EXPORT_APIVERSION, TSM_EXPORT_INIT);
        FreeLibrary(mod);
        return;
    }

    // Asked before Init, and Init is not called on a mismatch: a plugin built
    // against a different header would be reading moved fields out of the host
    // table, which corrupts the process rather than misbehaving.
    unsigned got = 0;
    __try { got = ver(); }
    __except (FaultFilter("plugin api version", GetExceptionInformation())) { got = 0; }

    if (got != TSM_API_VERSION)
    {
        Logf("plugin   \"%s\" wants API %u, this loader is %u - not initialised",
             file, got, TSM_API_VERSION);
        FreeLibrary(mod);
        return;
    }

    // Reserved before Init runs, so anything the plugin publishes is credited
    // to it in the log.
    LoadedPlugin* p = &g_plugin[g_pluginCount];
    memset(p, 0, sizeof(*p));
    p->module = mod;
    strncpy_s(p->file, sizeof(p->file), file, _TRUNCATE);
    p->name    = p->file;
    p->version = "";
    g_currentPlugin = p->file;

    TsmPluginInfo info;
    memset(&info, 0, sizeof(info));
    int rc = 1;
    __try { rc = init(&g_host, &info); }
    __except (FaultFilter("plugin init", GetExceptionInformation())) { rc = 1; }
    g_currentPlugin = "?";

    if (info.name)    p->name    = info.name;
    if (info.version) p->version = info.version;

    if (rc != 0)
    {
        // Only legitimate when nothing was hooked - a plugin that installed an
        // inline hook can never be unloaded, because the game would jump into
        // freed memory. Any service it published points into the DLL, so those
        // go too, which is why FreeLibrary only happens on this path.
        Logf("plugin   \"%s\" declined to install (%d)", file, rc);
        FreeLibrary(mod);
        return;
    }

    p->start = (TsmPluginStartFn)GetProcAddress(mod, TSM_EXPORT_START);
    g_pluginCount++;

    Logf("plugin   %-16s %-8s from %s", p->name, p->version, file);
}

// Second phase. Every plugin's Init has run, so every service that will ever
// exist is on the noticeboard and consume() can see all of it.
static void StartPlugins()
{
    for (int i = 0; i < g_pluginCount; i++)
    {
        LoadedPlugin* p = &g_plugin[i];
        if (!p->start) continue;

        int rc = 0;
        g_currentPlugin = p->file;
        __try { rc = p->start(); }
        __except (FaultFilter("plugin start", GetExceptionInformation())) { rc = -1; }
        g_currentPlugin = "?";

        if (rc != 0) Logf("plugin   %s started with %d - inactive", p->name, rc);
    }
}

static void LoadPlugins()
{
    _snprintf_s(g_pluginDir, sizeof(g_pluginDir), _TRUNCATE, "%s\\plugins", g_baseDir);

    DWORD attr = GetFileAttributesA(g_pluginDir);
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY))
    {
        Logf("plugin   no plugins folder at %s", g_pluginDir);
        return;
    }

    BuildHostTable();

    char pattern[MAX_PATH];
    _snprintf_s(pattern, sizeof(pattern), _TRUNCATE, "%s\\*.dll", g_pluginDir);

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
    {
        Logf("plugin   no plugins in %s", g_pluginDir);
        return;
    }

    // Alphabetical, which is what FindFirstFile gives on NTFS - so a plugin
    // that has to run before another can be named for it.
    do
    {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        LoadOnePlugin(fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);

    StartPlugins();
    Logf("plugin   %d loaded", g_pluginCount);
}

// ---------------------------------------------------------------- main menu version line
//
// The line along the bottom of the main menu is one call at the very end of the
// menu builder at rva 0x28AEF0:
//
//   lea  rax,[rip+0x60A26D]              ; L"v%d.%d.%d.%d (64 bit DX11.1 - GPU: %ls)"
//   mov  [rsp+0x48],7 / [rsp+0x40],1 / [rsp+0x38],1 / [rsp+0x30],1
//   mov  [rsp+0x28],rax                  ; the format string argument
//   mov  [rsp+0x20],0xFFAA0000           ; colour
//   call C3D_FONTMANAGER::PrintLeftUnicode
//
// so the four version numbers are immediates on the stack and the GPU name is
// the wide string the C3D_MIDDLEPOINT call above returned. The whole line is
// decided by which string that one `lea` computes.
//
// Which is why this is a **displacement rewrite, not a hook**. Hooking
// PrintLeftUnicode through the import table would be the usual first choice,
// but it is a variadic every label in the game goes through, and a va_list
// cannot be forwarded to a variadic callee - the hook would have to re-format
// every string in the UI through its own CRT to pass anything on. Four bytes of
// operand at a single call site cost nothing at runtime and touch nothing else.
//
// The suffix is appended to the format string rather than printed separately,
// so the game does the drawing and the line stays one string with one layout.
#define P_MENU_VERSION_SITE 0x28B55C     // LEA RAX,[rip+disp32] - the format argument
#define MENU_LEA_LEN        7

static const BYTE kMenuLeaOrig[3] = { 0x48, 0x8D, 0x05 };   // lea rax,[rip+disp32]

// What the displacement must resolve to. Compared before anything is written:
// on any other build this is a different string and the patch refuses, which
// beats redirecting an argument whose callee expects something else.
static const wchar_t kMenuVersionFmt[] = L"v%d.%d.%d.%d (64 bit DX11.1 - GPU: %ls)";

static int     g_menuPatch = 1;
static wchar_t g_menuTag[96] = L"tsmloader v.a0.1.";

static void PatchMenuVersion()
{
    if (!g_menuTag[0]) { Logf("menu     tag is empty - version line left alone"); return; }

    BYTE* site = g_exeBase + P_MENU_VERSION_SITE;
    if (memcmp(site, kMenuLeaOrig, sizeof(kMenuLeaOrig)) != 0)
    {
        Logf("menu     FAILED  no LEA RAX at +0x%X - wrong game build, refusing to patch",
             P_MENU_VERSION_SITE);
        return;
    }

    int      disp = *(int*)(site + 3);
    wchar_t* fmt  = (wchar_t*)(site + MENU_LEA_LEN + disp);
    if (!ReadablePtr(fmt, sizeof(kMenuVersionFmt)) ||
        wcscmp(fmt, kMenuVersionFmt) != 0)
    {
        Logf("menu     FAILED  the LEA at +0x%X does not compute the version format string",
             P_MENU_VERSION_SITE);
        return;
    }

    // The replacement has to be reachable by a 32-bit displacement from the call
    // site, which the loader's own image is not guaranteed to be.
    size_t   chars = wcslen(kMenuVersionFmt) + 3 + wcslen(g_menuTag) + 1;
    wchar_t* mine  = (wchar_t*)AllocNear(site, chars * sizeof(wchar_t));
    if (!mine) { Logf("menu     FAILED  no allocation within reach of the call site"); return; }

    _snwprintf_s(mine, chars, _TRUNCATE, L"%s | %s", kMenuVersionFmt, g_menuTag);

    __int64 rel = (BYTE*)mine - (site + MENU_LEA_LEN);
    if (rel != (int)rel) { Logf("menu     FAILED  displacement out of range"); return; }

    DWORD prot = 0;
    if (!VirtualProtect(site, MENU_LEA_LEN, PAGE_EXECUTE_READWRITE, &prot))
    { Logf("menu     FAILED  VirtualProtect %lu", GetLastError()); return; }

    int newDisp = (int)rel;
    memcpy(site + 3, &newDisp, 4);
    VirtualProtect(site, MENU_LEA_LEN, prot, &prot);
    FlushInstructionCache(GetCurrentProcess(), site, MENU_LEA_LEN);

    Logf("menu     version line -> \"%ls\"", mine);
}

static void ReadConfig()
{
    char ini[MAX_PATH];
    _snprintf_s(ini, sizeof(ini), _TRUNCATE, "%s\\tesmioloader.ini", g_baseDir);
    if (GetFileAttributesA(ini) == INVALID_FILE_ATTRIBUTES) return;

    g_traceReads = GetPrivateProfileIntA("tesmioloader", "trace_reads",  g_traceReads, ini);
    g_logGame    = GetPrivateProfileIntA("tesmioloader", "log_game",     g_logGame,    ini);
    g_vfsEnabled = GetPrivateProfileIntA("tesmioloader", "vfs",          g_vfsEnabled, ini);
    GetPrivateProfileStringA("tesmioloader", "trace_filter", g_traceFilter,
                             g_traceFilter, sizeof(g_traceFilter), ini);

    g_probeMap   = GetPrivateProfileIntA("tesmioloader", "probe_map", g_probeMap, ini);
    g_probeTexel = GetPrivateProfileIntA("tesmioloader", "probe_texel", g_probeTexel, ini);

    g_plugins   = GetPrivateProfileIntA("tesmioloader", "plugins", g_plugins, ini);
    g_menuPatch = GetPrivateProfileIntA("tesmioloader", "menu_patch", g_menuPatch, ini);
    {
        // Read as bytes and widened here rather than through the W profile API,
        // which would decode the file as ANSI. Keep the tag to ASCII: this is a
        // UTF-8 file and anything past 0x7F would arrive as its raw bytes.
        char tag[96] = {0};
        GetPrivateProfileStringA("tesmioloader", "menu_tag", "", tag, sizeof(tag), ini);
        Trim(tag);
        if (tag[0])
            MultiByteToWideChar(CP_UTF8, 0, tag, -1, g_menuTag,
                                sizeof(g_menuTag) / sizeof(g_menuTag[0]));
    }
}

static HANDLE OpenLog(const char* name)
{
    char p[MAX_PATH];
    _snprintf_s(p, sizeof(p), _TRUNCATE, "%s\\%s", g_baseDir, name);
    return CreateFileA(p, GENERIC_WRITE,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
}

static void Init()
{
    InitializeCriticalSection(&g_lock);

    GetModuleFileNameA(g_self, g_baseDir, MAX_PATH);
    if (char* s = strrchr(g_baseDir, '\\')) *s = 0;
    _snprintf_s(g_vfsRoot, sizeof(g_vfsRoot), _TRUNCATE, "%s\\vfs", g_baseDir);

    ReadConfig();

    g_hLog = OpenLog("tesmioloader.log");
    if (g_traceReads) g_hReads = OpenLog("tesmioloader.reads.log");

    g_exe     = GetModuleHandleW(NULL);
    g_exeBase = (BYTE*)g_exe;
    {
        IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)g_exeBase;
        IMAGE_NT_HEADERS* nt  = (IMAGE_NT_HEADERS*)(g_exeBase + dos->e_lfanew);
        g_exeSize = nt->OptionalHeader.SizeOfImage;
    }
    AddVectoredExceptionHandler(1, CrashHandler);
    HMODULE engine = GetModuleHandleA(DLL_ENGINE);

    Logf("tesmioloader phase B");
    Logf("exe base  %p    engine base %p", (void*)g_exe, (void*)engine);
    Logf("vfs root  %s (%s)", g_vfsRoot, g_vfsEnabled ? "on" : "off");
    Logf("---");

    PatchIat(g_exe, DLL_ENGINE, SYM_READ_FILE,   (void*)h_ReadFileIntoBuffer, (void**)&o_ReadFile,   "ReadFileIntoBuffer");
    PatchIat(g_exe, DLL_ENGINE, SYM_FILE_EXISTS, (void*)h_CheckIfFileExist,   (void**)&o_FileExists, "CheckIfFileExist");
    PatchIat(g_exe, DLL_ENGINE, SYM_LOG_INFO,    (void*)h_LogInfo,            (void**)&o_LogInfo,    "C3DLog_PrintInfo");
    PatchIat(g_exe, DLL_ENGINE, SYM_LOG_WARN,    (void*)h_LogWarn,            (void**)&o_LogWarn,    "C3DLog_PrintWarning");
    PatchIat(g_exe, DLL_ENGINE, SYM_LOG_ERROR,   (void*)h_LogError,           (void**)&o_LogError,   "C3DLog_PrintError");

    PatchIat(g_exe, DLL_STDIO,  "fopen",         (void*)h_fopen,              (void**)&o_fopen,      "fopen");
    PatchIat(g_exe, DLL_STDIO,  "fopen_s",       (void*)h_fopen_s,            (void**)&o_fopen_s,    "fopen_s");
    PatchIat(g_exe, DLL_STDIO,  "_wfopen",       (void*)h_wfopen,             (void**)&o_wfopen,     "_wfopen");
    PatchIat(g_exe, DLL_STDIO,  "_wfopen_s",     (void*)h_wfopen_s,           (void**)&o_wfopen_s,   "_wfopen_s");
    PatchIat(g_exe, DLL_STDIO,  "fread",         (void*)h_fread,              (void**)&o_fread,      "fread");

    // C3DDLL64.dll carries its own import table, so anything the engine opens
    // for itself - textures above all - never touches the executable's copy of
    // fopen. Patching only the exe left the whole engine side invisible.
    if (engine)
    {
        void* discard = NULL;
        PatchIat(engine, DLL_STDIO,   "fopen",       (void*)h_fopen,      &discard, "engine fopen");
        PatchIat(engine, DLL_STDIO,   "fopen_s",     (void*)h_fopen_s,    &discard, "engine fopen_s");
        PatchIat(engine, DLL_STDIO,   "_wfopen",     (void*)h_wfopen,     &discard, "engine _wfopen");
        PatchIat(engine, DLL_STDIO,   "fread",       (void*)h_fread,      (void**)&o_fread, "engine fread");
        PatchIat(engine, "KERNEL32.dll", "CreateFileA", (void*)h_CreateFileA,
                 (void**)&o_CreateFileA, "engine CreateFileA");
        PatchIat(engine, "KERNEL32.dll", "CreateFile2", (void*)h_CreateFile2,
                 (void**)&o_CreateFile2, "engine CreateFile2");
    }

    // The executable opens files through the wide API as well.
    PatchIat(g_exe, "KERNEL32.dll", "CreateFileW", (void*)h_CreateFileW,
             (void**)&o_CreateFileW, "CreateFileW");

    HookTextureVtable(engine);

    if (g_menuPatch) PatchMenuVersion();

    // Everything that is a feature rather than infrastructure lives out here
    // now - resources, deposits, depletion. They load last so each sees a fully
    // built loader: every import already swapped, the VFS live, the crash
    // handler armed.
    if (g_plugins) LoadPlugins();

    Logf("--- hooks installed ---");
}

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_self = mod;
        DisableThreadLibraryCalls(mod);
        Init();
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        Logf("--- shutdown: %ld VFS redirects, %d plugin(s) ---",
             g_nRedirects, g_pluginCount);
        if (g_hLog   != INVALID_HANDLE_VALUE) CloseHandle(g_hLog);
        if (g_hReads != INVALID_HANDLE_VALUE) CloseHandle(g_hReads);
    }
    return TRUE;
}
