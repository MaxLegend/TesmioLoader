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
#define SYM_GET_STRING  "?GetString@C3D_LANGUAGE@@QEAAPEA_WH@Z"

// The three calls the engine's own resource table makes to give a record its
// cargo geometry. Used to give a mod resource meshes of its own instead of the
// template's - see AttachResourceMeshes.
#define SYM_CREATE_MESH "?CreateManagedMesh@C3D_MIDDLEPOINT@@QEAAPEAVC3D_MESH@@PEBD@Z"
#define SYM_MESH_LOAD   "?LoadFromFile@C3D_MESH@@QEAAHPEBDPEAVC3D_MIDDLEPOINT@@_N@Z"
#define SYM_MESH_MTL    "?LoadMaterial@C3D_MESH@@QEAAHPEBDH@Z"

// C3D_MIDDLEPOINT, the object every managed asset is created through. Both the
// resource table at 0x2A1D60 and the editor's button drawer pass this same
// address, so it is shared rather than defined twice.
#define P_MIDDLEPOINT   0x9EACD0

#define DLL_ENGINE "C3DDLL64.dll"
#define DLL_STDIO  "api-ms-win-crt-stdio-l1-1-0.dll"

// SOVIET64.exe v1.1.1.7. Verified against the prologue below before hooking, so
// a game update makes the hook refuse to install rather than corrupt the process.
#define DEFAULT_RESOURCEGET_RVA 0x2AA7C0

static const BYTE kResourceGetPrologue[] = {
    0x40, 0x55,                                     // push rbp
    0x57,                                           // push rdi
    0x41, 0x56,                                     // push r14
    0x48, 0x8B, 0xEC,                               // mov  rbp, rsp
    0x48, 0x83, 0xEC, 0x40,                         // sub  rsp, 40h
    0x48, 0xC7, 0x45, 0xE0, 0xFE, 0xFF, 0xFF, 0xFF  // mov  qword ptr [rbp-20h], -2
};
#define STOLEN_BYTES (sizeof(kResourceGetPrologue))

// ---------------------------------------------------------------- state

static HMODULE          g_self;
static HMODULE          g_exe;
static BYTE*            g_exeBase;
static char             g_baseDir[MAX_PATH];
static char             g_vfsRoot[MAX_PATH];
static CRITICAL_SECTION g_lock;
static HANDLE           g_hLog   = INVALID_HANDLE_VALUE;
static HANDLE           g_hReads = INVALID_HANDLE_VALUE;
static HANDLE           g_hRes   = INVALID_HANDLE_VALUE;

static int  g_traceReads  = 0;
static int  g_logGame     = 1;
static int  g_vfsEnabled  = 1;
static int  g_resHook     = 1;    // 0 off, 1 observe only, 2 observe + inject
static int  g_probeMap    = 0;    // guard-page probe for the deposit map
static LONG g_mapSeen     = 0;    // raised the moment a deposit map is opened
static DWORD WINAPI ProbeThread(LPVOID);
static void  AddMapCopy(BYTE* start, SIZE_T len, int stride, const char* how);
// The map is opened more than once - the engine takes it as a texture and the
// game reads it for itself, back to back. Tracking a single stream meant the
// second open overwrote the first and one of the buffers was never captured.
#define MAX_MAP_STREAMS 6
static FILE* g_mapFiles[MAX_MAP_STREAMS];
static DWORD g_resRva     = DEFAULT_RESOURCEGET_RVA;
static char g_traceFilter[128] = "buildings_types";

static LONG g_nRedirects = 0;
static LONG g_nInjected  = 0;

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

// ---------------------------------------------------------------- resource registry

// Descriptor array geometry, established empirically in the observation pass:
// ResourceGet returns a pointer into a contiguous array of fixed-size records,
// and the record index is exactly the field position in the Resources struct
// documented in media_soviet/scripts/SOVIETInstructions.txt.
#define RES_STRIDE 832
#define RES_COUNT  63
// Indices 0..56 are the records the engine actually stores in the vector.
// waste_mixed (57) and service_material (58) appear in the script-facing struct
// but are kept as standalone objects, so they must never be used to work out
// where the array starts - doing so yields a bogus base.
#define RES_KNOWN  57

static const char* kResourceOrder[RES_KNOWN] = {
    "workers", "eletric", "vehicles", "trains", "heat", "gravel", "rawgravel",
    "plants", "steel", "aluminium", "prefabpanels", "bricks", "wood", "oil",
    "chemicals", "coal", "rawcoal", "iron", "rawiron", "bauxite", "rawbauxite",
    "bitumen", "boards", "uranium", "yellowcake", "uf6", "nuclearfuel",
    "nuclearfuelburned", "fuel", "fabric", "alcohol", "cement", "alumina",
    "food", "clothes", "meat", "livestock", "asphalt", "concrete",
    "ecomponents", "mcomponents", "plastics", "eletronics", "explosives",
    "water", "usagewater", "fertiliser_liquid", "waste_gravel", "waste_steel",
    "waste_aluminium", "waste_plastic", "waste_bio", "fertiliser",
    "waste_burnable", "waste_toxic", "waste_other", "waste_ash"
};

static int CanonicalIndex(const char* name)
{
    for (int i = 0; i < RES_KNOWN; i++)
        if (strcmp(kResourceOrder[i], name) == 0) return i;
    return -1;
}

static BYTE* g_resBase;        // descriptor of index 0, once corroborated
static int   g_baseVotes;      // independent names that agreed on it
static BYTE* g_baseCandidate;
static int   g_nameOff = -1;   // offset of the inline name buffer inside a record
static bool  g_layoutDone;

// The global std::vector holding the resource records, found at SOVIET64+0x9E11C0
// by scanning for a begin/end pair pointing at the array.
struct ResVector { BYTE* begin; BYTE* end; BYTE* cap; };
static DWORD g_vecRva = 0x9E11C0;

// Records the array should have room for. The engine ships 63; anything larger
// makes tesmioloader move the buffer. 0 leaves the engine's allocation alone.
static int g_wantCapacity = 0;

// Offset of the localisation id inside a record, found by diffing the records
// of workers/coal/rawiron/alcohol - the only field that differed and stayed
// stable across runs (518 / 508 / 524 / 512).
#define RES_TEXTID_OFF 0x40

// The record's last five qwords are its cargo meshes, and they are exactly the
// tail of the 832-byte record - 0x338 + 8 == 0x340.
//
// Found in the resource table at rva 0x2A1D60, which builds each record in one
// stack buffer at rsp+0x40 and pushes it. Its prologue is
// `lea rbp,[rsp-0x290]` before `sub rsp,0x390`, so **rbp == record + 0xC0**,
// and every block in it writes its meshes to rbp+0x258..0x278. Per resource:
//
//     mesh = middlepoint->CreateManagedMesh("resources/steel.nmf");
//     mesh->LoadFromFile("resources/steel.nmf", middlepoint, true);
//     mesh->LoadMaterial("resources/steel.mtl", 0);
//     record[+0x318] = mesh;
//
// **The paths are literals in .rdata, not built from the name.** Only the icon
// is looked up by name, through "resources/%s.png" at 0x899C48. So a record
// cloned from a template inherits the *template's* mesh objects and a mod
// resource is drawn as steel or aluminium however its own files are named -
// which is what AttachResourceMeshes exists to fix.
#define RES_MESH_VEHICLE 0x318   // load carried on a vehicle; the only mesh an
                                 // open-transport resource has
#define RES_MESH_STAGE1  0x320   // pile stages 1..4, bulk resources only
#define RES_MESH_STAGES  4

// Ids we hand out for mod resources. Anything at or above this is answered
// locally and never reaches the real string table, so the base has to clear
// every id the game itself uses - otherwise a mod caption silently replaces a
// real string somewhere else in the interface.
//
// **The game's highest id is 580231.** Measured, not assumed: a `.btf` is a
// big-endian header of {count, file size, ...} followed by `count` records of
// {id:u32, offset:u32, length:u16}, so the id range reads out in ten lines of
// Python over media_soviet/soviet*.btf. Twenty of the twenty-one language files
// top out at exactly 580231 and 793 entries sit at or above 60000, which is
// where this base used to be - the settings panel was showing mod resource
// names in place of four of its own labels.
//
// Re-measure after a game update before assuming this is still clear.
#define TML_TEXT_ID_BASE 1000000

struct ResEntry
{
    char    name[64];
    int     index;       // -1 = auto, resolved against the live count at arm time
    int     resolved;    // the index actually claimed
    int     tmplIndex;
    wchar_t display[64];
    int     textId;
    bool    armed;
};
static ResEntry g_reg[32];
static int      g_regCount;

static void Trim(char* s)
{
    char* p = s;
    while (*p == ' ' || *p == '\t') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r')) s[--n] = 0;
}

// Parsed by hand rather than through the profile API: display names are UTF-8
// and GetPrivateProfileString would mangle anything outside the ANSI codepage.
static void LoadResourceRegistry()
{
    char path[MAX_PATH];
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\resources.ini", g_baseDir);

    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
    {
        Logf("registry  no resources.ini - nothing to inject");
        return;
    }

    char   buf[8192];
    DWORD  got = 0;
    ReadFile(h, buf, sizeof(buf) - 1, &got, NULL);
    CloseHandle(h);
    buf[got] = 0;

    bool inSection = false;
    char* ctx = NULL;
    for (char* line = strtok_s(buf, "\n", &ctx); line; line = strtok_s(NULL, "\n", &ctx))
    {
        Trim(line);
        if (!line[0] || line[0] == ';' || line[0] == '#') continue;
        if (line[0] == '[') { inSection = _strnicmp(line, "[resources]", 11) == 0; continue; }
        if (!inSection || g_regCount >= 32) continue;

        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;

        ResEntry& e = g_reg[g_regCount];
        memset(&e, 0, sizeof(e));
        e.tmplIndex = -1;
        e.textId    = 0;
        e.resolved  = -1;

        Trim(line);
        strncpy_s(e.name, sizeof(e.name), line, _TRUNCATE);

        // [<slot>,]<template>[,<display name>]
        //
        // The slot is optional, and leaving it out is the better default:
        // hard-coding 57 and 58 only holds as long as nothing else claims them
        // first, and how many resources the base game ships is not something a
        // mod should have to know. A leading field that is not a number means
        // the value starts at the template, which is what makes the short form
        // unambiguous without a second syntax.
        char* value = eq + 1;
        Trim(value);
        e.index = -1;                                   // auto until told otherwise

        if (value[0] >= '0' && value[0] <= '9')
        {
            char* rest = strchr(value, ',');
            if (rest) *rest++ = 0;
            e.index = atoi(value);
            value = rest;
        }
        else if (_strnicmp(value, "auto", 4) == 0 && (value[4] == 0 || value[4] == ','))
        {
            char* rest = strchr(value, ',');
            value = rest ? rest + 1 : NULL;
        }

        if (value)
        {
            char* caption = strchr(value, ',');
            if (caption) *caption++ = 0;
            Trim(value);
            if (value[0])
            {
                e.tmplIndex = CanonicalIndex(value);
                if (e.tmplIndex < 0) Logf("registry  \"%s\": unknown template \"%s\"", e.name, value);
            }
            if (caption)
            {
                Trim(caption);
                if (caption[0])
                {
                    MultiByteToWideChar(CP_UTF8, 0, caption, -1, e.display,
                                        sizeof(e.display) / sizeof(e.display[0]));
                    e.textId = TML_TEXT_ID_BASE + g_regCount;
                }
            }
        }

        if (e.index < 0)
            Logf("registry  \"%s\" -> next free slot, template %d, text id %d",
                 e.name, e.tmplIndex, e.textId);
        else
            Logf("registry  \"%s\" -> slot %d, template %d, text id %d",
                 e.name, e.index, e.tmplIndex, e.textId);
        g_regCount++;
    }
}

// ---------------------------------------------------------------- seen-name table

static char g_seen[512][64];
static int  g_seenCount;

static bool MarkSeen(const char* name)
{
    for (int i = 0; i < g_seenCount; i++)
        if (strcmp(g_seen[i], name) == 0) return false;
    if (g_seenCount >= 512) return false;
    strncpy_s(g_seen[g_seenCount], sizeof(g_seen[0]), name, _TRUNCATE);
    g_seenCount++;
    return true;
}

// ---------------------------------------------------------------- ResourceGet hook

typedef unsigned __int64 (*t_ResourceGet)(void*, void*, void*, void*);
static t_ResourceGet o_ResourceGet;

static void HexDump(HANDLE h, const char* label, const BYTE* p, size_t n)
{
    char head[128];
    int k = _snprintf_s(head, sizeof(head), _TRUNCATE, "\r\n%s  @ %p\r\n", label, p);
    WriteTo(h, head, k);

    for (size_t off = 0; off < n; off += 16)
    {
        char line[160];
        int o = _snprintf_s(line, sizeof(line), _TRUNCATE, "  +%03zX  ", off);
        for (size_t i = 0; i < 16; i++)
            o += _snprintf_s(line + o, sizeof(line) - o, _TRUNCATE, "%02X ", p[off + i]);
        o += _snprintf_s(line + o, sizeof(line) - o, _TRUNCATE, " ");
        for (size_t i = 0; i < 16; i++)
        {
            BYTE c = p[off + i];
            o += _snprintf_s(line + o, sizeof(line) - o, _TRUNCATE, "%c",
                             (c >= 32 && c < 127) ? (char)c : '.');
        }
        o += _snprintf_s(line + o, sizeof(line) - o, _TRUNCATE, "\r\n");
        WriteTo(h, line, o);
    }
}

// Walks regions rather than trusting one query, for the reason spelled out on
// ReadablePtr below: a region ends wherever protection changes, so a single
// query understates how much is readable and gets worse as the process runs.
static bool Readable(const void* p, size_t n)
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
        if (end <= at) return false;
        at = end;
    }
    return true;
}

static void DumpRecords(BYTE* base);

static bool g_warnedSlot;

// Allocation must come from the process CRT, not ours. tesmioloader links the
// static runtime, so its malloc owns a private heap; the engine will eventually
// free this buffer through its own operator delete, and that only works if the
// block came from the shared ucrtbase heap the original allocation used.
static void* (__cdecl* g_procMalloc)(size_t);

static bool ResolveProcessMalloc()
{
    if (g_procMalloc) return true;
    static const char* mods[] = { "ucrtbase.dll", "api-ms-win-crt-heap-l1-1-0.dll" };
    for (int i = 0; i < 2; i++)
    {
        HMODULE m = GetModuleHandleA(mods[i]);
        if (!m) continue;
        FARPROC p = GetProcAddress(m, "malloc");
        if (p) { g_procMalloc = (void* (__cdecl*)(size_t))p; return true; }
    }
    return false;
}

// Grows the resource array by moving it, the way reserve() would. Safe only
// before anything has taken a Resource* into the old buffer, which is why this
// runs on the very first lookup - ahead of the engine's own init loop.
//
// The old block is deliberately left alone rather than freed: it costs a few
// tens of kilobytes per map load and removes any chance of releasing memory the
// engine still believes it owns.
static bool RelocateResourceArray(ResVector* vec, int live, int newCap)
{
    if (!ResolveProcessMalloc())
    {
        Logf("resource  cannot find the process allocator - not growing the array");
        return false;
    }

    BYTE* fresh = (BYTE*)g_procMalloc((size_t)newCap * RES_STRIDE);
    if (!fresh)
    {
        Logf("resource  allocation of %d records failed", newCap);
        return false;
    }

    memcpy(fresh, vec->begin, (size_t)live * RES_STRIDE);
    memset(fresh + (size_t)live * RES_STRIDE, 0, (size_t)(newCap - live) * RES_STRIDE);

    DWORD prot = 0;
    if (!VirtualProtect(vec, sizeof(ResVector), PAGE_READWRITE, &prot))
    {
        Logf("resource  could not write the vector header (%lu)", GetLastError());
        return false;
    }
    BYTE* old = vec->begin;
    vec->begin = fresh;
    vec->end   = fresh + (size_t)live * RES_STRIDE;
    vec->cap   = fresh + (size_t)newCap * RES_STRIDE;
    VirtualProtect(vec, sizeof(ResVector), prot, &prot);

    Logf("resource  array moved %p -> %p, capacity %d records (%d live)",
         old, fresh, newCap, live);
    return true;
}

// ---------------------------------------------------------------- cargo meshes

typedef void* (*t_CreateManagedMesh)(void*, const char*);
typedef int   (*t_MeshLoadFromFile)(void*, const char*, void*, bool);
typedef int   (*t_MeshLoadMaterial)(void*, const char*, int);

static t_CreateManagedMesh o_CreateManagedMesh;
static t_MeshLoadFromFile  o_MeshLoadFromFile;
static t_MeshLoadMaterial  o_MeshLoadMaterial;

// Exactly the three calls the engine's own table makes, in the same order.
// CreateManagedMesh caches by path, so asking twice for the same file is free
// and re-arming after a map load does not leak a mesh per load.
static void* LoadResourceMesh(const char* nmf, const char* mtl)
{
    if (!o_CreateManagedMesh || !o_MeshLoadFromFile) return NULL;

    void* mp   = g_exeBase + P_MIDDLEPOINT;
    void* mesh = o_CreateManagedMesh(mp, nmf);
    if (!mesh) return NULL;

    o_MeshLoadFromFile(mesh, nmf, mp, true);
    if (o_MeshLoadMaterial && mtl) o_MeshLoadMaterial(mesh, mtl, 0);
    return mesh;
}

// Replaces the template's mesh pointers with meshes loaded from this resource's
// own files. **The template still decides the shape**: a record whose stage
// slots are null is open-transport and has one mesh named <name>.nmf, one whose
// stage slots are filled is bulk and has four stages plus <name>_vehicle.nmf.
// Reading that off the clone rather than off a config keeps the rule in one
// place - the same place that already decides the transport class.
//
// A slot whose file is missing keeps the template's mesh, which is wrong-looking
// but drawable; the alternative is a null the engine dereferences.
static void AttachResourceMeshes(BYTE* rec, const char* name)
{
    bool bulk = false;
    for (int i = 0; i < RES_MESH_STAGES; i++)
        if (*(void**)(rec + RES_MESH_STAGE1 + i * 8)) bulk = true;

    char mtl[MAX_PATH], nmf[MAX_PATH];
    _snprintf_s(mtl, sizeof(mtl), _TRUNCATE, "resources/%s.mtl", name);

    int done = 0;
    if (bulk)
    {
        _snprintf_s(nmf, sizeof(nmf), _TRUNCATE, "resources/%s_vehicle.nmf", name);
        if (void* m = LoadResourceMesh(nmf, mtl)) { *(void**)(rec + RES_MESH_VEHICLE) = m; done++; }

        for (int i = 0; i < RES_MESH_STAGES; i++)
        {
            BYTE* slot = rec + RES_MESH_STAGE1 + i * 8;
            if (!*(void**)slot) continue;             // template has no such stage
            _snprintf_s(nmf, sizeof(nmf), _TRUNCATE, "resources/%s%d.nmf", name, i + 1);
            if (void* m = LoadResourceMesh(nmf, mtl)) { *(void**)slot = m; done++; }
        }
    }
    else if (*(void**)(rec + RES_MESH_VEHICLE))
    {
        _snprintf_s(nmf, sizeof(nmf), _TRUNCATE, "resources/%s.nmf", name);
        if (void* m = LoadResourceMesh(nmf, mtl)) { *(void**)(rec + RES_MESH_VEHICLE) = m; done++; }
    }

    Logf("resource  \"%s\" cargo meshes: %d of %s replaced", name, done,
         bulk ? "5 (bulk)" : "1 (open)");
}

// The vector at the known rva is the authority on the current array: its begin
// pointer identifies the allocation and its end bounds every lookup the engine
// makes. A map load replaces both, which is how a reload is detected - deriving
// the base from a resolved pointer instead would be wrong, because a few names
// (waste_mixed, service_material) are kept outside the array entirely.
//
// Called on every resolve, so it does nothing once armed. It retries rather
// than running once: the engine is still pushing records while building types
// are being parsed, and the slot only becomes claimable after the last
// base-game record has landed.
static void EnsureArmed()
{
    if (!g_regCount) return;

    ResVector* vec = (ResVector*)(g_exeBase + g_vecRva);
    if (!Readable(vec, sizeof(ResVector))) return;

    BYTE* base = vec->begin;
    if (!base || !Readable(base, RES_STRIDE)) return;

    if (base != g_resBase)
    {
        g_resBase    = base;
        g_nameOff    = -1;
        g_warnedSlot = false;
        for (int i = 0; i < g_regCount; i++) g_reg[i].armed = false;
        Logf("resource  array now at %p", base);
    }

    ptrdiff_t span = vec->end - base;
    ptrdiff_t cap  = vec->cap - base;
    if (span <= 0 || (span % RES_STRIDE) != 0 || cap < span) return;

    int live = (int)(span / RES_STRIDE);
    int room = (int)(cap / RES_STRIDE);
    if (live < 2 || live > 400) return;

    // Headroom past the engine's own capacity. Done first, so that everything
    // below - including the engine's init loop - only ever sees the new buffer.
    if (g_wantCapacity > room && RelocateResourceArray(vec, live, g_wantCapacity))
    {
        base = vec->begin;
        room = g_wantCapacity;
        if (base != g_resBase)
        {
            g_resBase    = base;
            g_nameOff    = -1;
            g_warnedSlot = false;
            for (int i = 0; i < g_regCount; i++) g_reg[i].armed = false;
        }
    }

    if (g_nameOff < 0)
    {
        for (int off = 0; off + 16 < RES_STRIDE; off++)
            if (memcmp(base + off, kResourceOrder[0], 8) == 0 &&
                memcmp(base + RES_STRIDE + off, kResourceOrder[1], 8) == 0)
            {
                g_nameOff = off;
                Logf("resource  name field at +0x%X, %d live records, room for %d", off, live, room);
                if (!g_layoutDone) { g_layoutDone = true; DumpRecords(base); }
                break;
            }
        if (g_nameOff < 0) return;
    }

    for (int r = 0; r < g_regCount; r++)
    {
        ResEntry& e = g_reg[r];

        // Recomputed per entry: arming one resource extends the vector, and
        // that is exactly what makes the next slot claimable in the same pass.
        live = (int)((vec->end - base) / RES_STRIDE);

        // Armed is a claim about the array in front of us, not a fact about
        // this session, so it is re-checked rather than trusted.
        //
        // A world load rebuilds the vector, and the allocator hands back the
        // same block often enough that `begin` is unchanged - so the base test
        // above sees nothing while `end` has gone back to 57 and our record has
        // been overwritten by the engine's own init. Left latched, the entry is
        // never republished: every building.ini naming it resolves to -1, and
        // the icon at +0x48 still points at a texture that was released with
        // the previous world, which is what crashed on hover.
        if (e.armed)
        {
            BYTE* have = base + (size_t)e.resolved * RES_STRIDE;
            if (e.resolved >= 0 && e.resolved < live && g_nameOff >= 0 &&
                Readable(have, RES_STRIDE) &&
                strncmp((char*)(have + g_nameOff), e.name, 32) == 0)
                continue;                             // still ours, nothing to do

            e.armed = false;
            Logf("resource  \"%s\" no longer at index %d - vector was rebuilt in place, re-arming",
                 e.name, e.resolved);
        }

        int want = e.index;
        if (want < 0)
        {
            // Auto. Waiting for the full base-game count is the whole point:
            // the engine is still pushing records while the first .ini files
            // are being parsed, and claiming a slot at that moment would move
            // the vector's end backwards and truncate its own array.
            if (live < RES_KNOWN) continue;
            want = live;
        }

        BYTE* rec = base + (size_t)want * RES_STRIDE;
        if (!Readable(rec, RES_STRIDE)) continue;

        // Already carrying our name - this array is done.
        if (want < live && strncmp((char*)(rec + g_nameOff), e.name, 32) == 0)
        {
            e.armed    = true;
            e.resolved = want;
            Logf("resource  \"%s\" already live at index %d", e.name, want);
            continue;
        }

        if (want > live) continue;                  // engine has not got there yet
        if (want < live || want >= room)
        {
            if (!g_warnedSlot)
            {
                Logf("resource  \"%s\": slot %d unusable (%d live, room %d) - fix resources.ini",
                     e.name, want, live, room);
                g_warnedSlot = true;
            }
            continue;
        }

        DWORD prot = 0;
        if (!VirtualProtect(rec, RES_STRIDE, PAGE_READWRITE, &prot))
        {
            Logf("resource  \"%s\": VirtualProtect failed (%lu)", e.name, GetLastError());
            continue;
        }

        // A freshly claimed record is all zeroes, so copy a working resource in
        // first; only then overwrite the identity fields.
        if (e.tmplIndex >= 0)
            memcpy(rec, base + (size_t)e.tmplIndex * RES_STRIDE, RES_STRIDE);

        memset(rec + g_nameOff, 0, 32);
        strncpy_s((char*)(rec + g_nameOff), 32, e.name, _TRUNCATE);
        if (e.textId) *(int*)(rec + RES_TEXTID_OFF) = e.textId;

        // Done while the record is still writable, and before the vector's end
        // moves - nothing may see this record until it is complete. The engine
        // is called into here, so a fault has to stay ours rather than take the
        // process down with a half-published resource.
        __try { AttachResourceMeshes(rec, e.name); }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            Logf("resource  \"%s\": cargo mesh load faulted - keeping the template's", e.name);
        }

        VirtualProtect(rec, RES_STRIDE, prot, &prot);

        DWORD vprot = 0;
        if (!VirtualProtect(vec, sizeof(ResVector), PAGE_READWRITE, &vprot))
        {
            Logf("resource  \"%s\": could not extend vector (%lu)", e.name, GetLastError());
            continue;
        }
        vec->end = base + (size_t)(want + 1) * RES_STRIDE;
        VirtualProtect(vec, sizeof(ResVector), vprot, &vprot);

        e.armed    = true;
        e.resolved = want;
        Logf("resource  \"%s\" published as index %d (template %d, caption %d), vector now %d",
             e.name, want, e.tmplIndex, e.textId, want + 1);
    }
}

// One-time diagnostic: full records for a few resources, so the meaning of the
// remaining fields can be worked out by diffing them.
static void DumpRecords(BYTE* base)
{
    char info[200];
    int k = _snprintf_s(info, sizeof(info), _TRUNCATE,
                        "array base %p, stride %d\r\n", base, RES_STRIDE);
    WriteTo(g_hRes, info, k);

    static const int idx[] = { 0, 15, 18, 30 };
    for (int i = 0; i < 4; i++)
    {
        char lbl[64];
        _snprintf_s(lbl, sizeof(lbl), _TRUNCATE, "record[%d] %s", idx[i], kResourceOrder[idx[i]]);
        if (Readable(base + (size_t)idx[i] * RES_STRIDE, RES_STRIDE))
            HexDump(g_hRes, lbl, base + (size_t)idx[i] * RES_STRIDE, RES_STRIDE);
    }
}

static unsigned __int64 h_ResourceGet(void* a1, void* a2, void* a3, void* a4)
{
    char n1[128], n2[128];
    bool s1 = SafeReadStr(a1, n1, sizeof(n1));
    bool s2 = SafeReadStr(a2, n2, sizeof(n2));

    const char* name = s1 ? n1 : (s2 ? n2 : NULL);
    int which = s1 ? 1 : (s2 ? 2 : 0);

    // Thread creation belongs here rather than in DllMain, which runs under the
    // loader lock. The first resolve is early enough for the probe's purposes.
    if (g_probeMap)
    {
        static LONG started = 0;
        if (InterlockedCompareExchange(&started, 1, 0) == 0)
        {
            HANDLE h = CreateThread(NULL, 0, ProbeThread, NULL, 0, NULL);
            if (h) CloseHandle(h);
        }
    }

    // Before the original, not after: the engine's own init loop resolves every
    // resource in turn and keeps what it gets back. Growing or arming the array
    // afterwards would leave those first pointers aimed at the old buffer.
    if (g_resHook >= 2)
    {
        EnterCriticalSection(&g_lock);
        EnsureArmed();
        LeaveCriticalSection(&g_lock);
    }

    unsigned __int64 r = o_ResourceGet(a1, a2, a3, a4);

    if (!name) return r;

    EnterCriticalSection(&g_lock);
    bool first = MarkSeen(name);
    LeaveCriticalSection(&g_lock);

    // Safety net: if the game could not resolve a registered name, hand back the
    // reserved record ourselves. Only ever done for slots we verified as readable.
    if (r == 0 && g_resHook >= 2 && g_resBase)
    {
        for (int i = 0; i < g_regCount; i++)
        {
            if (!g_reg[i].armed || g_reg[i].resolved < 0 ||
                strcmp(g_reg[i].name, name) != 0) continue;
            BYTE* rec = g_resBase + (size_t)g_reg[i].resolved * RES_STRIDE;
            InterlockedIncrement(&g_nInjected);
            if (first) Logf("resource  serving \"%s\" from reserved slot %d (%p)",
                            name, g_reg[i].index, rec);
            return (unsigned __int64)rec;
        }
    }

    if (first)
    {
        size_t callerRva = (size_t)((BYTE*)_ReturnAddress() - g_exeBase);
        char line[256];
        int k = _snprintf_s(line, sizeof(line), _TRUNCATE,
                            "%-28s arg%d  ret=0x%llX  caller_rva=0x%zX\r\n",
                            name, which, r, callerRva);
        EnterCriticalSection(&g_lock);
        WriteTo(g_hRes, line, k);
        LeaveCriticalSection(&g_lock);
    }
    return r;
}

// ---------------------------------------------------------------- hooked engine calls

typedef int  (__cdecl* t_ReadFileIntoBuffer)(const char*, char**, unsigned int*, bool);
typedef bool (__cdecl* t_CheckIfFileExist)(const char*, bool, bool);
typedef void (__cdecl* t_LogPrint)(char*, ...);

static t_ReadFileIntoBuffer o_ReadFile;
static t_CheckIfFileExist   o_FileExists;
static t_LogPrint           o_LogInfo, o_LogWarn, o_LogError;

static int __cdecl h_ReadFileIntoBuffer(const char* path, char** buf, unsigned int* size, bool flag)
{
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

// Every caption in the game comes through here. Ids we minted for mod
// resources are answered locally; everything else goes to the real table.
typedef wchar_t* (*t_GetString)(void*, int);
static t_GetString o_GetString;

static wchar_t* h_GetString(void* self, int id)
{
    if (id >= TML_TEXT_ID_BASE)
        for (int i = 0; i < g_regCount; i++)
            if (g_reg[i].textId == id) return g_reg[i].display;

    return o_GetString(self, id);
}

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

// ---------------------------------------------------------------- deposit registry
//
// Nothing about any individual deposit is compiled in. deposits.ini declares
// them, one section each, and all three subsystems below - the code patch, the
// minimap layer and the editor brush - iterate this one table.
//
//   [copper]
//   token         = $TYPE_MINE_COPPER
//   type          = 10
//   map           = resourcemap2
//   component     = 3
//   radius        = ore
//   building_type = 7
//   icon          = copper_ore
//   minimap       = 1
//   editor        = copper
//
// A deposit really is little more than a (texture, colour component) pair: the
// game stores richness as one byte of one channel of one of the two 1024x1024
// maps, and every per-type behaviour it has - the building.ini token, the
// search radius, the minimap overlay, the editor brush - is a lookup keyed by
// the type number. Each field above is one of those lookups.
//
// The channel is the scarce thing, not the machinery. Eight exist and the base
// game reaches six of them; see the notes on `map` and `component` in
// deposits.ini for which are actually free.

#define MAX_DEPOSITS 8

#define DEP_MAP_1  0    // resourcemap,  gameobj+0xF00
#define DEP_MAP_2  1    // resourcemap2, gameobj+0xF08

struct DepositDef
{
    char  name[32];        // section name; only ever used in the log
    char  token[64];       // the building.ini token, $TYPE_MINE_...
    int   type;            // deposit type number the whole engine keys on
    int   buildingType;    // 7 = mine, set by the parser alongside the type
    int   map;             // DEP_MAP_1 or DEP_MAP_2
    int   component;       // 0..3, the colour channel within that map
    DWORD radiusRva;       // .rdata float the search radius is copied from
    float radiusValue;     // used instead when radiusRva is 0
    char  icon[64];        // resource whose record supplies the minimap icon
    int   wantMinimap;
    char  editor[32];      // "copper" -> paint_copper / erase_copper; empty = no brush

    // derived at load
    int   editorChannel;   // the eight-value index 0x238B00 takes
    float vector[4];       // ResourceVector, selecting `component` in the shader

    // runtime
    int   minimapState;    // 0 idle, 1 hovered, 2 selected - our own copy of the
                           // vanilla per-icon field, never written into theirs
    int   minimapSlot;     // row position, counting on past the vanilla five
    int   editorColumn;    // grid column, counting on past the vanilla five
    BYTE* toolPaint;
    BYTE* toolErase;
};

static DepositDef g_dep[MAX_DEPOSITS];
static int        g_depCount;

// The search-radius constants, by the deposit that uses each. A type the table
// at 0x1DCA70 does not know gets radius zero, and a mine that searches nothing
// averages over an empty set: the building window then shows quality of source
// as -2147483648, a NaN cast to int. Naming them rather than taking a number
// means a mod deposit tracks the game's own value if a patch ever changes it.
static const struct { const char* name; DWORD rva; } kRadiusSources[] = {
    { "oil",          0x90ABFC },   // type 0
    { "ore",          0x90AD50 },   // types 1, 2, 6 - iron, coal and uranium share it
    { "bauxite",      0x90AA40 },   // type 7
    { "gravel",       0x90A9B8 },   // type 3
    { "wood",         0x90ADD0 },   // type 4
    { "water",        0x90AC9C },   // type 8
    { "watersurface", 0x90AC38 },   // type 9
};

static bool KeyIs(const char* line, const char* want) { return _stricmp(line, want) == 0; }

// Hand-parsed rather than through GetPrivateProfileString, for the same reason
// resources.ini is: the profile API is ANSI and would mangle anything a caption
// or a comment puts outside the codepage.
static void LoadDepositRegistry()
{
    char path[MAX_PATH];
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\deposits.ini", g_baseDir);

    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
    {
        Logf("deposits  no deposits.ini - no mod deposit types");
        return;
    }

    char  buf[16384];
    DWORD got = 0;
    ReadFile(h, buf, sizeof(buf) - 1, &got, NULL);
    CloseHandle(h);
    buf[got] = 0;

    DepositDef* d = NULL;
    char* ctx = NULL;
    for (char* line = strtok_s(buf, "\n", &ctx); line; line = strtok_s(NULL, "\n", &ctx))
    {
        Trim(line);
        if (!line[0] || line[0] == ';' || line[0] == '#') continue;

        if (line[0] == '[')
        {
            d = NULL;
            char* end = strchr(line, ']');
            if (!end) continue;
            *end = 0;

            if (g_depCount >= MAX_DEPOSITS)
            {
                Logf("deposits  \"%s\" ignored - only %d sections fit", line + 1, MAX_DEPOSITS);
                continue;
            }

            d = &g_dep[g_depCount++];
            memset(d, 0, sizeof(*d));
            strncpy_s(d->name, sizeof(d->name), line + 1, _TRUNCATE);

            // Defaults describe the common case: a mine reading resourcemap2
            // with the radius the ores share. -1 marks the two fields that have
            // no sensible default and must be given.
            d->type         = -1;
            d->component    = -1;
            d->buildingType = 7;
            d->map          = DEP_MAP_2;
            d->radiusRva    = 0x90AD50;
            d->wantMinimap  = 1;
            continue;
        }

        if (!d) continue;

        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char* val = eq + 1;
        Trim(line);
        Trim(val);

        if      (KeyIs(line, "token"))         strncpy_s(d->token, sizeof(d->token), val, _TRUNCATE);
        else if (KeyIs(line, "icon"))          strncpy_s(d->icon,  sizeof(d->icon),  val, _TRUNCATE);
        else if (KeyIs(line, "editor"))        strncpy_s(d->editor, sizeof(d->editor), val, _TRUNCATE);
        else if (KeyIs(line, "type"))          d->type         = (int)strtol(val, NULL, 0);
        else if (KeyIs(line, "building_type")) d->buildingType = (int)strtol(val, NULL, 0);
        else if (KeyIs(line, "component"))     d->component    = (int)strtol(val, NULL, 0);
        else if (KeyIs(line, "minimap"))       d->wantMinimap  = (int)strtol(val, NULL, 0);
        else if (KeyIs(line, "map"))
        {
            if      (KeyIs(val, "resourcemap")  || KeyIs(val, "1")) d->map = DEP_MAP_1;
            else if (KeyIs(val, "resourcemap2") || KeyIs(val, "2")) d->map = DEP_MAP_2;
            else Logf("deposits  \"%s\": unknown map \"%s\"", d->name, val);
        }
        else if (KeyIs(line, "radius"))
        {
            d->radiusRva   = 0;
            d->radiusValue = 0.0f;
            for (size_t i = 0; i < sizeof(kRadiusSources) / sizeof(kRadiusSources[0]); i++)
                if (KeyIs(val, kRadiusSources[i].name)) { d->radiusRva = kRadiusSources[i].rva; break; }
            if (!d->radiusRva)
            {
                d->radiusValue = (float)atof(val);
                if (d->radiusValue <= 0.0f)
                    Logf("deposits  \"%s\": unknown radius \"%s\"", d->name, val);
            }
        }
        else Logf("deposits  \"%s\": unknown key \"%s\"", d->name, line);
    }
}

// Drops anything that would produce a broken patch rather than letting it
// through - a bad type number here becomes spliced code, so the cost of
// guessing is a corrupted process rather than a wrong colour.
static void ValidateDeposits()
{
    int kept = 0;
    for (int i = 0; i < g_depCount; i++)
    {
        DepositDef* d = &g_dep[i];
        const char* bad = NULL;

        if (!d->token[0])
            bad = "no token";
        // 0..9 belong to the game. The upper bound is the encoding's, not a
        // policy: every compare this patches is CMP r/m32,imm8, sign-extended.
        else if (d->type < 10 || d->type > 127)
            bad = "type must be 10..127 (0..9 are the game's own, and the compare takes an imm8)";
        else if (d->component < 0 || d->component > 3)
            bad = "component must be 0..3";
        else if (!d->radiusRva && d->radiusValue <= 0.0f)
            bad = "no usable radius";

        for (int j = 0; !bad && j < kept; j++)
        {
            if      (g_dep[j].type == d->type)                bad = "duplicate type";
            else if (strcmp(g_dep[j].token, d->token) == 0)    bad = "duplicate token";
            else if (g_dep[j].map == d->map &&
                     g_dep[j].component == d->component)       bad = "duplicate channel";
        }

        if (bad)
        {
            Logf("deposits  \"%s\" rejected: %s", d->name, bad);
            continue;
        }

        // Sharing a channel with a base-game deposit is legitimate - a second
        // mine type reading iron's ore, say - but it is far more often a typo,
        // and the symptom is a mine that finds someone else's deposit.
        static const struct { int map; int comp; const char* who; } kTaken[] = {
            { DEP_MAP_1, 0, "oil"     }, { DEP_MAP_1, 1, "iron"    },
            { DEP_MAP_1, 2, "coal"    }, { DEP_MAP_2, 0, "uranium" },
            { DEP_MAP_2, 1, "bauxite" },
        };
        for (size_t k = 0; k < sizeof(kTaken) / sizeof(kTaken[0]); k++)
            if (kTaken[k].map == d->map && kTaken[k].comp == d->component)
                Logf("deposits  \"%s\" WARN  shares a channel with %s - both read the same bytes",
                     d->name, kTaken[k].who);

        // The eight-value index the texel writer at 0x238B00 takes. It decodes
        // its argument as tex = (ch - 4) < 4 ? resourcemap2 : resourcemap and
        // component = (ch + 3) & 3, so this is that mapping inverted. It agrees
        // with all six channels the base game reaches.
        d->editorChannel = (d->map == DEP_MAP_2 ? 4 : 0) | ((d->component + 1) & 3);

        for (int c = 0; c < 4; c++) d->vector[c] = (c == d->component) ? 1.0f : 0.0f;

        if (i != kept) g_dep[kept] = *d;
        kept++;
    }
    g_depCount = kept;

    int mmSlot = 5, edCol = 5;      // both grids carry five vanilla entries
    for (int i = 0; i < g_depCount; i++)
    {
        DepositDef* d = &g_dep[i];
        d->minimapSlot  = d->wantMinimap ? mmSlot++ : -1;
        d->editorColumn = d->editor[0]   ? edCol++  : -1;

        Logf("deposits  \"%s\" type %d \"%s\" -> %s component %d, radius %s, "
             "editor channel %d (slot %d, column %d)",
             d->name, d->type, d->token,
             d->map == DEP_MAP_2 ? "resourcemap2" : "resourcemap", d->component,
             d->radiusRva ? "from the game" : "fixed",
             d->editorChannel, d->minimapSlot, d->editorColumn);
    }
}

// ---------------------------------------------------------------- deposit type patch
//
// Adding a deposit type is the one thing that cannot be done by swapping a
// pointer: both places that matter are chains of comparisons compiled into the
// executable, and a new case has to be spliced in.
//
// Parser, building.ini token -> mine type number, at rva 0x10EAC8:
//     LEA  RDX,[$TYPE_MINE_BAUXITE]      48 8D 15 ..
//     LEA  RCX,[RBP+0x49A0]              token just read
//     CALL 0x14084F340                   compare
//     TEST EAX,EAX / JNZ next
//     MOV  [RBP+0x1E10],7                building type = mine
//     MOV  [RBP+0x1E18],7                deposit type
//     JMP  0x140118815                   done
//
// Sampler dispatch, deposit type -> (texture, colour component), rva 0x1DD773:
//     CMP  [RSI+0x368],6                 deposit type of this building
//     JNZ  0x1401DD7B6
//     ... load world position ...
//     MOV  R9,[0x1409941F0] / MOV R9,[R9+0xF08]    resourcemap2 texture
//     LEA  R8,[RBP+0x38] / LEA RDX,[RBP+0xB0]
//     CALL 0x140008360                   bilinear sample -> C3DFCOLOR
//     MOVSS XMM0,[RAX]                   component 0
//     MOVSS [RSP+0x5C],XMM0              deposit richness here
//
// Both sites are replaced by a jump into a cave that reproduces the original
// check and adds ours in front of it. Everything relative is computed from the
// runtime base, so only the rvas are hard-coded - and those are verified byte
// for byte before a single byte is written.

#define P_PARSER_SITE      0x10EAC8   // LEA RDX,[$TYPE_MINE_BAUXITE]
#define P_PARSER_NEXT      0x10EAF8   // next token check
#define P_PARSER_DONE      0x118815   // shared exit
#define P_STRCMP           0x84F340
#define P_STR_BAUXITE      0x8895C0
#define P_PARSER_TOKEN     0x49A0     // [rbp+..] the token just read
#define P_PARSER_BTYPE     0x1E10     // [rbp+..] building type
#define P_PARSER_DTYPE     0x1E18     // [rbp+..] deposit type

#define P_DISPATCH_SITE    0x1DD773   // CMP [RSI+0x368],6
#define P_DISPATCH_BODY6   0x1DD77C   // body of the type 6 case
#define P_DISPATCH_TAIL    0x1DD7B6   // CMP [RSI+0x368],7
#define P_GAMEOBJ          0x9941F0
#define P_SAMPLER          0x8360
#define P_DEP_TYPE_FIELD   0x368      // building object, the deposit type
#define P_MAP1_OFF         0xF00      // gameobj -> resourcemap
#define P_MAP2_OFF         0xF08      // gameobj -> resourcemap2

// Search radius by deposit type, rva 0x1DCA70 - 111 bytes, returns in XMM0.
// A type it does not know falls through to XORPS XMM0,XMM0, and a radius of
// zero means the mine finds no deposit at all: the building window then shows
// quality of source as -2147483648, a NaN converted to int.
//
//   1DCACD  83 F9 09              CMP ECX,9
//   1DCAD0  75 09                 JNZ 1DCADB
//   1DCAD2  F3 0F 10 05 ..        MOVSS XMM0,[0x14090AC38]
//   1DCADA  C3                    RET
//   1DCADB  0F 57 C0              XORPS XMM0,XMM0
//   1DCADE  C3                    RET
#define P_RADIUS_SITE      0x1DCACD
#define P_RADIUS_WATERSURF 0x90AC38   // the constant the type 9 branch returns

static const BYTE kParserOrig[]   = { 0x48, 0x8D, 0x15, 0xF1, 0xAA, 0x77, 0x00 };
static const BYTE kDispatchOrig[] = { 0x83, 0xBE, 0x68, 0x03, 0x00, 0x00, 0x06 };
static const BYTE kRadiusOrig[]   = { 0x83, 0xF9, 0x09, 0x75, 0x09 };

// Data below, code above. Both are bump-allocated and both are bounds-checked:
// with the number of cases coming out of a config file, running off the end is
// a thing a user can cause, and it must fail before a byte of the executable
// has been touched rather than halfway through.
#define CAVE_SIZE          0x2000
#define CAVE_CODE          0x800

static int   g_depositPatch;
static BYTE* g_cave;
static SIZE_T g_caveUsed;

// A cave has to sit within reach of a 32-bit displacement, or none of the calls
// and jumps back into the executable can be encoded.
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

struct Emit
{
    BYTE* p;
    BYTE* end;
    bool  overflow;

    void need(size_t n)               { if ((size_t)(end - p) < n) overflow = true; }
    void b(BYTE v)                    { need(1); if (!overflow) *p++ = v; }
    void d32(int v)                   { need(4); if (!overflow) { memcpy(p, &v, 4); p += 4; } }
    void rel32(BYTE* target)          { d32((int)(target - (p + 4))); }

    // A forward branch whose target is not known yet. Every case block below is
    // a variable length, so these are all rel32: a short jcc would silently go
    // out of range once enough deposits were declared.
    BYTE* jne32()                     { b(0x0F); b(0x85); BYTE* at = p; d32(0); return at; }
    void  land(BYTE* at)
    {
        if (overflow || !at) return;
        int rel = (int)(p - (at + 4));
        memcpy(at, &rel, 4);
    }
};

// One `if (token == "$TYPE_MINE_X") { buildingType = ..; depositType = ..; }`,
// in the shape the .ini parser's own token checks have.
static void EmitParserCase(Emit& e, const DepositDef* d, BYTE* token)
{
    e.b(0x48); e.b(0x8D); e.b(0x15); e.rel32(token);                        // lea rdx,[token]
    e.b(0x48); e.b(0x8D); e.b(0x8D); e.d32(P_PARSER_TOKEN);                 // lea rcx,[rbp+0x49a0]
    e.b(0xE8); e.rel32(g_exeBase + P_STRCMP);                               // call compare
    e.b(0x85); e.b(0xC0);                                                   // test eax,eax
    BYTE* next = e.jne32();
    e.b(0xC7); e.b(0x85); e.d32(P_PARSER_BTYPE); e.d32(d->buildingType);
    e.b(0xC7); e.b(0x85); e.d32(P_PARSER_DTYPE); e.d32(d->type);
    e.b(0xE9); e.rel32(g_exeBase + P_PARSER_DONE);
    e.land(next);
}

// One case of the type -> (texture, colour component) chain. The body is the
// game's own type-6 block with two substitutions: which map pointer is loaded
// out of the game object, and which float of the sampled colour is kept.
static void EmitDispatchCase(Emit& e, const DepositDef* d)
{
    e.b(0x83); e.b(0xBE); e.d32(P_DEP_TYPE_FIELD); e.b((BYTE)d->type);      // cmp [rsi+0x368],type
    BYTE* next = e.jne32();

    e.b(0xF2); e.b(0x0F); e.b(0x10); e.b(0x44); e.b(0x24); e.b(0x40);       // movsd xmm0,[rsp+0x40]
    e.b(0xF2); e.b(0x0F); e.b(0x11); e.b(0x45); e.b(0x38);                  // movsd [rbp+0x38],xmm0
    e.b(0x8B); e.b(0x44); e.b(0x24); e.b(0x48);                             // mov eax,[rsp+0x48]
    e.b(0x89); e.b(0x45); e.b(0x40);                                        // mov [rbp+0x40],eax
    e.b(0x4C); e.b(0x8B); e.b(0x0D); e.rel32(g_exeBase + P_GAMEOBJ);        // mov r9,[gameobj]
    e.b(0x4D); e.b(0x8B); e.b(0x89);
    e.d32(d->map == DEP_MAP_2 ? P_MAP2_OFF : P_MAP1_OFF);                   // mov r9,[r9+map]
    e.b(0x4C); e.b(0x8D); e.b(0x45); e.b(0x38);                             // lea r8,[rbp+0x38]
    e.b(0x48); e.b(0x8D); e.b(0x95); e.d32(0xB0);                           // lea rdx,[rbp+0xb0]
    e.b(0xE8); e.rel32(g_exeBase + P_SAMPLER);                              // call sampler
    e.b(0xF3); e.b(0x0F); e.b(0x10); e.b(0x40);
    e.b((BYTE)(d->component * 4));                                          // movss xmm0,[rax+c*4]
    e.b(0xF3); e.b(0x0F); e.b(0x11); e.b(0x44); e.b(0x24); e.b(0x5C);       // movss [rsp+0x5c],xmm0
    e.b(0xE9); e.rel32(g_exeBase + P_DISPATCH_TAIL);

    e.land(next);
}

// One case of the search-radius table. The value is copied out of .rdata at
// patch time rather than referenced, so a deposit that borrows the ore radius
// keeps whatever the game's own constant is.
static void EmitRadiusCase(Emit& e, const DepositDef* d, BYTE* slot)
{
    e.b(0x83); e.b(0xF9); e.b((BYTE)d->type);                               // cmp ecx,type
    BYTE* next = e.jne32();
    e.b(0xF3); e.b(0x0F); e.b(0x10); e.b(0x05); e.rel32(slot);              // movss xmm0,[slot]
    e.b(0xC3);
    e.land(next);
}

static bool PatchDepositType()
{
    if (g_depCount == 0)
    {
        Logf("patch  no deposits declared - nothing to splice");
        return false;
    }

    BYTE* parserSite   = g_exeBase + P_PARSER_SITE;
    BYTE* dispatchSite = g_exeBase + P_DISPATCH_SITE;
    BYTE* radiusSite   = g_exeBase + P_RADIUS_SITE;

    if (memcmp(parserSite,   kParserOrig,   sizeof(kParserOrig))   != 0 ||
        memcmp(dispatchSite, kDispatchOrig, sizeof(kDispatchOrig)) != 0 ||
        memcmp(radiusSite,   kRadiusOrig,   sizeof(kRadiusOrig))   != 0)
    {
        Logf("patch  site bytes differ from build v1.1.1.7 - refusing to patch");
        return false;
    }

    g_cave = AllocNear(g_exeBase, CAVE_SIZE);
    if (!g_cave) { Logf("patch  no cave within reach of the executable"); return false; }

    // --- data: one token string and one radius float per deposit ----------
    BYTE* tokenStr[MAX_DEPOSITS];
    BYTE* radiusSlot[MAX_DEPOSITS];

    BYTE* data    = g_cave;
    BYTE* dataEnd = g_cave + CAVE_CODE;

    for (int i = 0; i < g_depCount; i++)
    {
        size_t n = strlen(g_dep[i].token) + 1;
        if ((size_t)(dataEnd - data) < n) { Logf("patch  cave data full at \"%s\"", g_dep[i].name); return false; }
        memcpy(data, g_dep[i].token, n);
        tokenStr[i] = data;
        data += n;
    }

    data = (BYTE*)(((size_t)data + 3) & ~(size_t)3);       // MOVSS wants the float aligned
    for (int i = 0; i < g_depCount; i++)
    {
        if ((size_t)(dataEnd - data) < 4) { Logf("patch  cave data full at \"%s\"", g_dep[i].name); return false; }
        *(float*)data = g_dep[i].radiusRva ? *(float*)(g_exeBase + g_dep[i].radiusRva)
                                           : g_dep[i].radiusValue;
        radiusSlot[i] = data;
        data += 4;
    }

    // --- code -------------------------------------------------------------
    Emit e;
    e.p = g_cave + CAVE_CODE;
    e.end = g_cave + CAVE_SIZE;
    e.overflow = false;

    // Parser: our tokens first, then the $TYPE_MINE_BAUXITE check the jump
    // displaced, reproduced exactly.
    BYTE* parserCave = e.p;
    for (int i = 0; i < g_depCount; i++) EmitParserCase(e, &g_dep[i], tokenStr[i]);

    e.b(0x48); e.b(0x8D); e.b(0x15); e.rel32(g_exeBase + P_STR_BAUXITE);
    e.b(0x48); e.b(0x8D); e.b(0x8D); e.d32(P_PARSER_TOKEN);
    e.b(0xE8); e.rel32(g_exeBase + P_STRCMP);
    e.b(0x85); e.b(0xC0);
    e.b(0x0F); e.b(0x85); e.rel32(g_exeBase + P_PARSER_NEXT);           // jnz next token
    e.b(0xC7); e.b(0x85); e.d32(P_PARSER_BTYPE); e.d32(7);
    e.b(0xC7); e.b(0x85); e.d32(P_PARSER_DTYPE); e.d32(7);
    e.b(0xE9); e.rel32(g_exeBase + P_PARSER_DONE);

    // Dispatch: our types first, then the displaced type-6 check.
    BYTE* dispatchCave = e.p;
    for (int i = 0; i < g_depCount; i++) EmitDispatchCase(e, &g_dep[i]);

    e.b(0x83); e.b(0xBE); e.d32(P_DEP_TYPE_FIELD); e.b(0x06);           // cmp [rsi+0x368],6
    e.b(0x0F); e.b(0x85); e.rel32(g_exeBase + P_DISPATCH_TAIL);
    e.b(0xE9); e.rel32(g_exeBase + P_DISPATCH_BODY6);

    // Radius: the displaced type-9 branch first, since it is the one this jump
    // stands on, then ours, then the zero fallback the table already had.
    BYTE* radiusCave = e.p;
    {
        e.b(0x83); e.b(0xF9); e.b(0x09);                                // cmp ecx,9
        BYTE* next = e.jne32();
        e.b(0xF3); e.b(0x0F); e.b(0x10); e.b(0x05);
        e.rel32(g_exeBase + P_RADIUS_WATERSURF);                        // movss xmm0,[water surface]
        e.b(0xC3);
        e.land(next);
    }
    for (int i = 0; i < g_depCount; i++) EmitRadiusCase(e, &g_dep[i], radiusSlot[i]);

    e.b(0x0F); e.b(0x57); e.b(0xC0);                                    // xorps xmm0,xmm0
    e.b(0xC3);

    // Checked before a single byte of the executable is touched, so a cave that
    // did not fit leaves the process exactly as it was.
    if (e.overflow)
    {
        Logf("patch  %d deposits do not fit in a %d-byte cave - nothing patched",
             g_depCount, CAVE_SIZE);
        return false;
    }
    g_caveUsed = (SIZE_T)(e.p - g_cave);

    // --- redirect all three sites ----------------------------------------
    struct { BYTE* site; BYTE* cave; size_t len; const char* what; } jumps[] = {
        { parserSite,   parserCave,   sizeof(kParserOrig),   "parser"   },
        { dispatchSite, dispatchCave, sizeof(kDispatchOrig), "dispatch" },
        { radiusSite,   radiusCave,   sizeof(kRadiusOrig),   "radius"   },
    };

    for (int i = 0; i < 3; i++)
    {
        DWORD prot = 0;
        if (!VirtualProtect(jumps[i].site, jumps[i].len, PAGE_EXECUTE_READWRITE, &prot))
        {
            Logf("patch  %s site not writable (%lu)", jumps[i].what, GetLastError());
            return false;
        }
        jumps[i].site[0] = 0xE9;
        int rel = (int)(jumps[i].cave - (jumps[i].site + 5));
        memcpy(jumps[i].site + 1, &rel, 4);
        for (size_t k = 5; k < jumps[i].len; k++) jumps[i].site[k] = 0x90;
        VirtualProtect(jumps[i].site, jumps[i].len, prot, &prot);
        FlushInstructionCache(GetCurrentProcess(), jumps[i].site, jumps[i].len);
    }

    for (int i = 0; i < g_depCount; i++)
        Logf("patch  deposit type %d added: \"%s\" in building.ini, %s component %d",
             g_dep[i].type, g_dep[i].token,
             g_dep[i].map == DEP_MAP_2 ? "resourcemap2" : "resourcemap", g_dep[i].component);
    Logf("patch  cave at %p, %zu of %d bytes used (parser %p, dispatch %p, radius %p)",
         g_cave, g_caveUsed, CAVE_SIZE, parserCave, dispatchCave, radiusCave);
    return true;
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

        // If the fault lands inside or just past the resource array, say by how
        // many records - that names the undersized table directly.
        if (g_resBase)
        {
            ptrdiff_t d = (BYTE*)bad - g_resBase;
            if (d > -0x10000 && d < 0x40000)
                Logf("    that is resource-array base %+lld bytes (record %lld)",
                     (long long)d, (long long)(d / RES_STRIDE));
        }
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

// ---------------------------------------------------------------- minimap deposit buttons
//
// The minimap button row and the coloured overlay it drives are two separate
// functions, both found by decompiling around the string "gui_minimap_bauxit":
//
//   rva 0x4BFEA0  draws the five-icon row and handles clicks on it
//   rva 0x4BDDE0  draws the coloured deposit overlay for whichever icon is on
//
// Per icon, 0x4BFEA0 calls ResourceGet(self, "bauxite") and reads a texture
// pointer straight out of the resource record at +0x48 - a field
// 02-findings.md did not document before this (only +0x00 name and +0x40
// caption id were known). That is the resource's own icon, the same one
// media_soviet/resources/<name>.png already loads for every other UI use, so
// a mod button needs no new art asset: the `icon` key in deposits.ini names a
// resource and ResourceGet hands over its texture.
//
// 0x4BDDE0 reads a small on/off struct and sets two shader constants -
// ResourceVector, a float4 picking a colour channel, and MapType picking
// which deposit texture - before drawing a full-panel quad. The three
// vectors the base game already has were read back directly out of .rdata:
//
//   flag +0x04  coal      (0,0,1,0)  component 2   resourcemap
//   flag +0x08  iron      (0,1,0,0)  component 1   resourcemap
//   flag +0x0c  oil       (1,0,0,0)  component 0   resourcemap
//   flag +0x10  uranium   (1,0,0,0)  component 0   resourcemap2
//   flag +0x14  bauxite   (0,1,0,0)  component 1   resourcemap2
//   flag +0x18  -         (0,0,1,0)  component 2   resourcemap2
//
// which is exactly 02-findings.md's dispatch table by component number. The
// base game passes only the three unit vectors for components 0, 1 and 2, so
// component 3 of either map goes unused. Note the sixth flag: the overlay
// function tests +0x18 but the row draws no button for it, so that layer is
// unreachable in the stock UI.
//
// Which of the two textures gets sampled is not chosen in the shader. The
// overlay always binds resourcemap to stage 0 and then, if the selected
// layer is one of the +0x10/+0x14/+0x18 three, binds resourcemap2 over the
// top of it - same stage, same slot. A mod layer binds whichever map its
// section names, once.
//
// Both hooks below are purely additive. The original function runs first,
// through a trampoline, completely unmodified; one button per mod layer and
// the selected layer's overlay pass are appended afterward, using state kept
// in the loader's own registry and never in the six-flag struct the base game
// owns. Mutual exclusion is kept by watching that struct (drop ours the moment
// any base-game layer is picked) and by clearing it ourselves when one of ours
// is picked instead. Nothing already shipping is touched, so there is no byte
// to verify beyond each hook's own prologue.
//
// One visible consequence of appending rather than splicing: a mod quad is
// drawn after the vanilla function's tail has already drawn the minimap frame
// and the region outlines, so it sits on top of them where the base game's own
// layers sit underneath.

#define P_MINIMAP_DRAW_RVA 0x4BDDE0   // FUN_1404bdde0 - draws the coloured overlay
#define P_MINIMAP_ROW_RVA  0x4BFEA0   // FUN_1404bfea0 - draws + handles the button row

static const BYTE kMinimapDrawPrologue[] = {
    0x48, 0x8B, 0xC4,                                 // mov rax,rsp
    0x55,                                             // push rbp
    0x53,                                             // push rbx
    0x56,                                             // push rsi
    0x57,                                             // push rdi
    0x41, 0x56,                                       // push r14
    0x48, 0x8D, 0xA8, 0x28, 0xFF, 0xFF, 0xFF           // lea rbp,[rax-0xd8]
};
#define MINIMAP_DRAW_STOLEN (sizeof(kMinimapDrawPrologue))

static const BYTE kMinimapRowPrologue[] = {
    0x48, 0x8B, 0xC4,                                 // mov rax,rsp
    0x55,                                             // push rbp
    0x48, 0x8D, 0x68, 0xA1,                           // lea rbp,[rax-0x5f]
    0x48, 0x81, 0xEC, 0xF0, 0x00, 0x00, 0x00           // sub rsp,0xf0
};
#define MINIMAP_ROW_STOLEN (sizeof(kMinimapRowPrologue))

// Globals the vanilla functions already use, at the same addresses.
#define G_GAMEOBJ         0x9941F0   // +0xED8 terrain, +0xF08 resourcemap2
#define G_PANEL           0x9BE060   // the one C3D_PANEL2D the whole minimap draws through
#define G_TECHNIQUE       0x9EAD08   // current shader technique, set by the last BeginDraw
#define G_PANEL_FULLSIZE  0x909F70   // w/h passed to every Draw() call in this UI
#define G_SLOT_BG_TEX     0x9DFF38   // background box texture, shared by every button
#define G_SLOT_SEL_TEX    0x9E03E8   // "selected" badge texture, shared by every button
#define G_CLICK_FLAG      0xA54E91   // nonzero for one frame on a real click
#define G_MOUSE_OBJ       0xA54B90   // C3D_INPUT instance
#define G_RES_SELF        0x9D4F10   // "self" object every ResourceGet call in this UI uses
#define G_CLIP_HELPER_OBJ 0x9DFCC0   // passed to the per-icon clip-rect helper
#define G_DPI             0x992088
#define G_ROW_X0          0x90A9A0
#define G_ROW_STEP        0x90AA5C
#define G_ROW_Y0          0x90AB30
#define G_ICON_SCALE      0x909E6C
#define G_HITBOX_HALF     0x90A6C0
#define G_BADGE_OFFSET    0x909CF0
// 0.5f. The vanilla code loads this one constant for two unrelated jobs: the
// inset of the overlay quad inside the minimap panel, and the size of the
// "selected" badge. Both uses below read it from here.
#define G_HALF            0x909DF4
#define G_COLOR_IDLE      0x90C120   // float4 tint, not hovered
#define G_COLOR_HOVER     0x90C4E0   // float4 tint, hovered
#define G_COLOR_OVERLAY   0x90C2F0   // float4 (1,0,0,1) - the red every deposit layer is drawn in
#define G_PANEL_POS       0x9BE2F0   // 2 floats: x,y of the next Draw()
#define G_PANEL_PAD       0x9BE2F8
#define G_PANEL_SIZE      0x9BE2E8   // 2 floats: w,h of the next Draw()
#define G_PANEL_COLOR     0x9BE30C   // 4 floats: tint of the next Draw()
#define P_CLIP_HELPER     0x446150   // FUN_140446150 - not exported, internal only

#define G_TECH_GET_HANDLE 0x50       // technique vtbl+0x50  GetConstantHandle(name)->handle
#define G_TECH_SET_VEC    0x68       // technique vtbl+0x68  SetVectorConstant(handle, float4*)
#define G_TECH_SET_INT    0x88       // technique vtbl+0x88  SetIntConstant(handle, int)
#define G_TEX_BIND        0x70       // texture vtbl+0x70    Bind(stage, technique)

typedef void  (*t_MM_BeginDraw)(void*, const char*, bool);
typedef void  (*t_MM_EndDraw)(void*);
typedef void  (*t_MM_Draw)(void*, float, float, float, float, float, bool);
// GetMouseSolid returns a C3DVECTOR3 by value (12 bytes, too big for a
// register), so MSVC passes a hidden pointer to caller-allocated storage for
// the result - and, verified against the actual call site's disassembly
// (rva 0x4BFFD5: RCX = this, RDX = &local buffer, in that order - MSVC puts
// `this` before the hidden return slot for member functions, not after),
// that hidden pointer is the *second* argument here, not the first. Calling
// this with only `this` leaves RDX holding whatever the previous call left
// there, and the callee dereferences it: exactly the near-null write crash
// this hook produced the first time.
typedef void* (*t_MM_GetMouseSolid)(void*, void*);
typedef bool  (*t_MM_Collision)(void*, void*, float, float);
typedef void  (*t_MM_ClipHelper)(void*, float, float, int);
typedef void  (*t_MM_DrawRowOrOverlay)(void*);

static t_MM_BeginDraw        o_MM_BeginDraw;
static t_MM_EndDraw          o_MM_EndDraw;
static t_MM_Draw             o_MM_Draw;
static t_MM_GetMouseSolid    o_MM_GetMouseSolid;
static t_MM_Collision        o_MM_Collision;

static t_MM_DrawRowOrOverlay o_MM_DrawOverlay;   // trampoline for 0x4BDDE0
static t_MM_DrawRowOrOverlay o_MM_DrawRow;       // trampoline for 0x4BFEA0

static int g_minimapPatch;

// The six flags the base game owns, at param_1 +0x04 .. +0x18. Every mod layer
// state lives in its own DepositDef instead, so this struct is only ever read
// for mutual exclusion and written with zero - exactly what the vanilla click
// handlers already do to each other.
#define MM_VANILLA_FLAGS 6
static int* MM_Flag(BYTE* param_1, int i) { return (int*)(param_1 + 4 + 4 * i); }

static bool MM_AnyVanillaLayer(BYTE* param_1)
{
    for (int i = 0; i < MM_VANILLA_FLAGS; i++) if (*MM_Flag(param_1, i)) return true;
    return false;
}

static void MM_ClearVanillaLayers(BYTE* param_1)
{
    for (int i = 0; i < MM_VANILLA_FLAGS; i++) *MM_Flag(param_1, i) = 0;
}

static void* MM_TechConstHandle(void* tech, const char* name)
{
    void** vtbl = *(void***)tech;
    typedef void* (*t_Get)(void*, const char*);
    return ((t_Get)vtbl[G_TECH_GET_HANDLE / 8])(tech, name);
}

static void MM_TechSetVector(void* tech, void* handle, const void* v16)
{
    void** vtbl = *(void***)tech;
    typedef void (*t_Set)(void*, void*, const void*);
    ((t_Set)vtbl[G_TECH_SET_VEC / 8])(tech, handle, v16);
}

static void MM_TechSetInt(void* tech, void* handle, int v)
{
    void** vtbl = *(void***)tech;
    typedef void (*t_Set)(void*, void*, int);
    ((t_Set)vtbl[G_TECH_SET_INT / 8])(tech, handle, v);
}

static void MM_TexBind(void* texObj, void* tech)
{
    // Both checks matter: the object itself may be a stale pointer out of a
    // rebuilt table, and even a live object is useless if its vtable slot is
    // not there.
    if (!ReadablePtr(texObj, sizeof(void*))) return;
    void** vtbl = *(void***)texObj;
    if (!ReadablePtr(vtbl, G_TEX_BIND + sizeof(void*))) return;
    typedef void (*t_Bind)(void*, int, void*);
    ((t_Bind)vtbl[G_TEX_BIND / 8])(texObj, 0, tech);
}

static void MM_SetRect(float x, float y, float w, float h)
{
    float* pos = (float*)(g_exeBase + G_PANEL_POS);
    pos[0] = x; pos[1] = y;
    *(int*)(g_exeBase + G_PANEL_PAD) = 0;
    float* size = (float*)(g_exeBase + G_PANEL_SIZE);
    size[0] = w; size[1] = h;
}

static void MM_SetColor(DWORD rva)
{
    memcpy(g_exeBase + G_PANEL_COLOR, g_exeBase + rva, 16);
}

static float MM_F(DWORD rva) { return *(float*)(g_exeBase + rva); }

// Draws one mod layer's overlay quad on the minimap, exactly the way the base
// game draws uranium's or bauxite's: bind the deposit's own map, pick its
// colour component, MapType = 2 (deposit-texture mode), draw a panel-sized
// quad using the same rect the vanilla function computes from param_1.
//
// What MapType = 2 does is settled, not guessed: the technique lives in
// media_soviet/shaders_d3d11/default_panel2d.inix and its pixel shader
// disassembles to
//
//     if (MapType == 2) {
//         float4 t = Texture2DStage0.Sample(SamplerStage0, uv);
//         o.a   = dot(t, ResourceVector);     // dp4 - all four components
//         o.rgb = vertexColour;
//     }
//
// A full dp4, so ResourceVector = (0,0,0,1) really does select the alpha
// channel; the base game only ever passes the three unit vectors for
// components 0, 1 and 2, which is the only reason a fourth component looked
// unreachable. o.rgb comes from the vertex colour, which is the panel tint -
// which is why the tint has to be set to the same red the vanilla layers use
// or a mod layer would come out whatever colour the previous draw left behind.
//
// TerrainHeight and TerrainPos are deliberately not set here: the shader
// reflection marks them used only by the MapType-not-1-and-not-2 branch,
// which is the terrain-colour pass, and the vanilla call already set them
// this frame anyway.
//
// The leading EndDraw is not decoration. The vanilla function returns with a
// bracket still open - its tail is EndDraw / PrintAllTexts / BeginDraw(NULL)
// - and the vanilla function's own first two statements are exactly this
// pair, EndDraw then BeginDraw(technique). C3D_PANEL2D::Draw appends quads to
// a batch rather than drawing them, so a pass has to own its bracket.
static void DrawDepositOverlay(BYTE* param_1, const DepositDef* d)
{
    // The layer state is ours and survives a map load, so a mod layer can still
    // be the selected one when the minimap is opened somewhere that has no
    // world behind it - the terrain editor being the case that found this.
    // Neither pointer is guaranteed there, and following a null one faults
    // inside this module, where the vectored crash handler does not look.
    BYTE* gameobj = *(BYTE**)(g_exeBase + G_GAMEOBJ);
    if (!ReadablePtr(gameobj, 0xF10)) return;
    BYTE* terrain = *(BYTE**)(gameobj + 0xED8);
    if (!ReadablePtr(terrain, 0x8F0)) return;

    void* map = *(void**)(gameobj + (d->map == DEP_MAP_2 ? P_MAP2_OFF : P_MAP1_OFF));
    if (!map) return;                             // no deposit map, nothing to sample

    bool  desert  = *(int*)(terrain + 0x8EC) == 1;
    void* panel   = g_exeBase + G_PANEL;

    o_MM_EndDraw(panel);
    o_MM_BeginDraw(panel, desert ? "MinimapDesertColors" : "MinimapColors", false);

    void* tech = *(void**)(g_exeBase + G_TECHNIQUE);
    MM_TexBind(map, tech);

    MM_TechSetVector(tech, MM_TechConstHandle(tech, "ResourceVector"), d->vector);
    MM_TechSetInt(tech, MM_TechConstHandle(tech, "MapType"), 2);

    // Half, not the terrain height. An earlier version of this function read
    // the multiplier off C3D_TERRAIN::GetTerrainHeight because the decompiler
    // reuses one variable for both: the vanilla code calls GetTerrainHeight,
    // hands the result to the TerrainHeight shader constant, and then
    // overwrites the same register with the 0.5f at G_HALF before computing
    // the rect. Multiplying by a world height instead of 0.5 put the quad
    // hundreds of units off the panel, which is why the layer never appeared.
    float half = MM_F(G_HALF);
    float p48  = *(float*)(param_1 + 0x48);
    float p4c  = *(float*)(param_1 + 0x4c);
    float p50  = *(float*)(param_1 + 0x50);
    float p54  = *(float*)(param_1 + 0x54);
    float p58  = *(float*)(param_1 + 0x58);
    float sz   = p58 * half;

    MM_SetColor(G_COLOR_OVERLAY);
    MM_SetRect((p48 - p54) + sz, p4c - sz, p50, p50);

    float full = MM_F(G_PANEL_FULLSIZE);
    o_MM_Draw(panel, 0.0f, 0.0f, full, full, 0.0f, true);

    o_MM_EndDraw(panel);

    // Only now. C3D_PANEL2D::Draw does not draw: it appends the quad to the
    // batch and flushes only when the bound state forces it or when EndDraw
    // does, and the flush is what commits the shader constants. Resetting
    // MapType between Draw and EndDraw therefore lands on the quad still
    // sitting in the batch, and the layer would come out drawn through the
    // terrain-colour branch of the shader instead of the deposit branch.
    //
    // MapType lives in the technique's own constant buffer, so leaving it at
    // 2 is what the vanilla function does at rest and costs nothing; this is
    // belt and braces for any later pass that reuses MinimapColors without
    // setting it first.
    MM_TechSetInt(tech, MM_TechConstHandle(tech, "MapType"), 0);

    // The vanilla function never returns with a closed bracket: its own tail
    // (rva ~0x4BE424 onward) always re-opens with BeginDraw(panel, NULL,
    // false) and deliberately leaves it open - the button-row function that
    // runs right after never calls BeginDraw itself, it draws straight into
    // whatever bracket is already active. Skipping this step is what made
    // the button row render as tiny copies of the minimap instead of icons:
    // it was still drawing inside our "MinimapColors" bracket. Put the same
    // default, open bracket back before returning.
    o_MM_BeginDraw(panel, NULL, false);
}

// Draws one mod layer's icon in the minimap button row, below the vanilla
// five, and handles hover/click on it exactly the way those five do - reusing
// every generic global (background/badge textures, tint colours, the clip
// helper) verbatim, since none of those are resource-specific. The only
// deposit-specific things are the resource name passed to ResourceGet, the
// row slot and the layer state, all of which come out of its DepositDef;
// everything in param_1 - the six vanilla flags - is read to enforce mutual
// exclusion and written only with 0, the same as every vanilla handler
// already does to its neighbours.
//
// Simplification versus the vanilla blocks: no hover tooltip text. Building
// it calls an internal, unexported text formatter this analysis did not
// verify the ABI of, and skipping it costs only the text label under the
// cursor - the icon, its hover/selected tint and the click itself all work
// without it.
static void DrawDepositButton(BYTE* param_1, DepositDef* d)
{
    void* tech = *(void**)(g_exeBase + G_TECHNIQUE);
    void* panel = g_exeBase + G_PANEL;

    float dpi  = MM_F(G_DPI);
    float x    = *(float*)(param_1 + 0x48) - dpi * MM_F(G_ROW_X0);
    float step = dpi * MM_F(G_ROW_STEP);
    float y    = (*(float*)(param_1 + 0x4c) - *(float*)(param_1 + 0x58)) +
                 dpi * MM_F(G_ROW_Y0) + (float)d->minimapSlot * step;
    float icon = step * MM_F(G_ICON_SCALE);
    float half = dpi * MM_F(G_HITBOX_HALF);
    float full = MM_F(G_PANEL_FULLSIZE);

    // background slot
    MM_TexBind(*(void**)(g_exeBase + G_SLOT_BG_TEX), tech);
    MM_SetRect(x, y, step, step);
    MM_SetColor(G_COLOR_IDLE);
    ((t_MM_ClipHelper)(g_exeBase + P_CLIP_HELPER))(g_exeBase + G_CLIP_HELPER_OBJ, x, y, 3);

    BYTE  mouseBuf[16];   // >= sizeof(C3DVECTOR3); hidden return storage
    void* mouse   = o_MM_GetMouseSolid(g_exeBase + G_MOUSE_OBJ, mouseBuf);
    bool  hovered = o_MM_Collision(panel, mouse, half, half);

    if (hovered)
    {
        MM_SetColor(G_COLOR_HOVER);
        if (*(char*)(g_exeBase + G_CLICK_FLAG) != 0)
        {
            if (d->minimapState == 2) d->minimapState = 1;
            else
            {
                d->minimapState = 2;
                MM_ClearVanillaLayers(param_1);
                for (int i = 0; i < g_depCount; i++)
                    if (&g_dep[i] != d) g_dep[i].minimapState = 0;
            }
        }
        else if (d->minimapState != 2) d->minimapState = 1;
    }
    else if (d->minimapState != 2) d->minimapState = 0;

    o_MM_Draw(panel, 0.0f, 0.0f, full, full, 0.0f, true);

    // the resource's own icon, straight out of its record - no art asset of
    // our own needed
    // The record comes out of a table the engine rebuilds at every map load,
    // so it is checked before being followed rather than merely non-null.
    if (d->icon[0])
    {
        BYTE* record = (BYTE*)h_ResourceGet(g_exeBase + G_RES_SELF, (void*)d->icon, NULL, NULL);
        if (ReadablePtr(record, 0x50))
        {
            void* iconTex = *(void**)(record + 0x48);
            if (iconTex)
            {
                MM_TexBind(iconTex, tech);   // itself guarded; a stale record gives a stale texture
                MM_SetRect(x, y, icon, icon);
                MM_SetColor(hovered ? G_COLOR_HOVER : G_COLOR_IDLE);
                o_MM_Draw(panel, 0.0f, 0.0f, full, full, 0.0f, true);
            }
        }
    }

    if (d->minimapState == 2)
    {
        MM_TexBind(*(void**)(g_exeBase + G_SLOT_SEL_TEX), tech);
        float k1 = MM_F(G_BADGE_OFFSET), k2 = MM_F(G_HALF);
        MM_SetRect(step * k1 + x, step * k1 + y, step * k2, step * k2);
        MM_SetColor(G_COLOR_HOVER);
        o_MM_Draw(panel, 0.0f, 0.0f, full, full, 0.0f, true);
    }
}

static void DrawDepositButtons(BYTE* param_1)
{
    // Six flags, not five. The button row draws five icons, but the overlay
    // function tests one more at +0x18 - resourcemap2 component 2, a layer
    // with no button of its own - and every vanilla click handler clears it
    // along with the rest. Leaving it out of the mutual exclusion would let
    // that layer and a mod layer be on at the same time.
    if (MM_AnyVanillaLayer(param_1))
        for (int i = 0; i < g_depCount; i++) g_dep[i].minimapState = 0;

    for (int i = 0; i < g_depCount; i++)
        if (g_dep[i].minimapSlot >= 0) DrawDepositButton(param_1, &g_dep[i]);
}

static void h_MM_DrawOverlay(void* param_1)
{
    o_MM_DrawOverlay(param_1);
    if (!g_minimapPatch) return;

    // At most one can be selected - every path that sets a layer to 2 clears
    // every other - so this draws one pass, not a stack of them.
    for (int i = 0; i < g_depCount; i++)
    {
        if (g_dep[i].minimapState != 2) continue;
        __try { DrawDepositOverlay((BYTE*)param_1, &g_dep[i]); }
        __except (FaultFilter("minimap deposit overlay", GetExceptionInformation()))
        {
            g_minimapPatch = 0;
            Logf("minimap  deposit layers disabled for this session");
        }
        return;
    }
}

static void h_MM_DrawRow(void* param_1)
{
    o_MM_DrawRow(param_1);
    if (!g_minimapPatch) return;
    __try { DrawDepositButtons((BYTE*)param_1); }
    __except (FaultFilter("minimap deposit buttons", GetExceptionInformation()))
    {
        g_minimapPatch = 0;
        Logf("minimap  deposit buttons disabled for this session");
    }
}

// Resolves the handful of C3DDLL64.dll exports both hooks call by name -
// the import-table lookup already used for everything else in this file,
// so a game update that keeps these signatures needs no RVA fixed here.
static bool ResolveMinimapImports()
{
    struct { const char* sym; void** slot; } imports[] = {
        { "?BeginDraw@C3D_PANEL2D@@QEAAXPEBD_N@Z",           (void**)&o_MM_BeginDraw        },
        { "?EndDraw@C3D_PANEL2D@@QEAAXXZ",                   (void**)&o_MM_EndDraw          },
        { "?Draw@C3D_PANEL2D@@QEAAXMMMMM_N@Z",               (void**)&o_MM_Draw             },
        { "?GetMouseSolid@C3D_INPUT@@QEAA?AVC3DVECTOR3@@XZ", (void**)&o_MM_GetMouseSolid    },
        { "?Collision@C3D_PANEL2D@@QEAA_NVC3DVECTOR3@@MM@Z", (void**)&o_MM_Collision        },
    };
    for (size_t i = 0; i < sizeof(imports) / sizeof(imports[0]); i++)
    {
        void** slot = FindIatSlot(g_exe, DLL_ENGINE, imports[i].sym);
        if (!slot) { Logf("minimap  FAILED  no import slot for %s", imports[i].sym); return false; }
        *imports[i].slot = *slot;
    }
    return true;
}

static void InstallMinimapPatch()
{
    int layers = 0;
    for (int i = 0; i < g_depCount; i++) if (g_dep[i].minimapSlot >= 0) layers++;
    if (layers == 0) { Logf("minimap  no deposit declares a layer - not hooking"); return; }

    if (!ResolveMinimapImports()) return;

    bool ok1 = InstallInlineHook(g_exeBase + P_MINIMAP_DRAW_RVA, (void*)h_MM_DrawOverlay,
                                 (void**)&o_MM_DrawOverlay, kMinimapDrawPrologue,
                                 MINIMAP_DRAW_STOLEN, "minimap overlay");
    bool ok2 = InstallInlineHook(g_exeBase + P_MINIMAP_ROW_RVA, (void*)h_MM_DrawRow,
                                 (void**)&o_MM_DrawRow, kMinimapRowPrologue,
                                 MINIMAP_ROW_STOLEN, "minimap row");
    if (!ok1 || !ok2)
    {
        Logf("minimap  patch partially failed - mod minimap layers disabled");
        g_minimapPatch = 0;
        return;
    }
    Logf("minimap  %d mod layer(s) hooked", layers);
}

// ---------------------------------------------------------------- terrain editor deposit brushes
//
// The terrain editor's Resources tab draws five paint/erase pairs. Every mod
// deposit that declares one gets another, and none of it needs a code patch,
// because the editor turned out to be built almost entirely out of things that
// are already generic.
//
// Two facts make it easy.
//
// First, **tools are identified by name string, not by an enum.** The active
// tool is a pointer at editor+0xD428, and it points at the tool's descriptor,
// whose first field is its name - which is why the game can both `strcmp` it
// and read flags at +0x2B5 off the same pointer. The button drawer at 0x3826C0
// stores whatever descriptor it was handed straight into that field on a
// click, so a descriptor of our own becomes a fully first-class tool the
// moment it is drawn. Nothing has to be registered anywhere.
//
// Second, **the texel writer at 0x238B00 is already generic over the
// channel.** It takes an eight-value index and derives everything from it:
//
//     tex = (unsigned)(ch - 4) < 4 ? resourcemap2 : resourcemap;
//     switch (ch & 3) { 0: alpha  1: byte2  2: byte1  3: byte0 }
//
// so ch = map*4 + component', which is what DepositDef::editorChannel holds.
// Its only caller, 0x2350D0, maps the editor's five pairs onto 1, 2, 3, 5, 6
// with `if (2 < idx) idx++` then `idx + 1`, and that arithmetic cannot produce
// 0, 4 or 7 for any input. Those three channels are not missing a capability,
// they are missing a caller.
//
// Rather than reimplement 0x2350D0 - brush radius, strength, limit, the rate
// timer, and the guards that stop the brush painting through the open panel
// are all in there - the dispatch hook calls it with *bauxite's* index and the
// texel hook rewrites the one argument that differs. The "this map has
// bauxite" byte that call sets on the way past is saved and restored, so a map
// without bauxite does not quietly acquire it.
//
// Icons are loaded explicitly from "editor/tool_<name>.png", so a brush needs
// nothing but two PNGs in the VFS named after its `editor` key.

#define P_ED_PANEL_RVA    0x233110   // FUN_140233110 - draws the Resources tab
#define P_ED_DISPATCH_RVA 0x30D100   // FUN_14030d100 - applies the active tool, every frame
#define P_ED_CURSOR_RVA   0x2F0E70   // FUN_1402f0e70 - decides which tools get the round cursor
#define P_ED_TEXELS_RVA   0x238B00   // FUN_140238b00 - the generic deposit texel writer

#define P_ED_TOOL_FIND    0x03AAA0   // FUN_14003aaa0 - tool lookup by name
#define P_ED_DRAW_BUTTON  0x3826C0   // FUN_1403826c0 - draws one tool button
#define P_ED_PAINT        0x2350D0   // FUN_1402350d0 - one brush tick for one deposit

// Editor object fields.
#define ED_ACTIVE_TOOL    0xD428     // char* - the descriptor of the selected tool
#define ED_BRUSH_CURSOR   0x10F0     // set per frame for tools that want the round cursor

// Tool descriptor. Stride from the vector 0x3AAA0 walks; fields from the
// button drawer, which is the only thing that reads them.
#define TOOL_STRIDE       0x2D0
#define TOOL_NAME         0x00
#define TOOL_ICON_TEX     0x58       // the texture the button binds
#define TOOL_ICON_PATH    0xB4       // empty on every built-in tool - see below

// Engine globals and vtable slots the icon needs.
#define G_MIDDLEPOINT     0x9EACD0   // C3D_MIDDLEPOINT the button drawer creates textures through
#define TEX_LOAD2DFILE    0x10       // texture vtbl+0x10, slot 2: Load2DFromFile(path,0,0,0,0)

// Resources-tab layout, all floats in .rdata, all read from the game so a
// patch release that moves the grid moves our buttons with it.
#define G_ED_X_A          0x90AA40   // 50
#define G_ED_X_B          0x90AB2C   // 85
#define G_ED_Y_BASE       0x90ADD0   // 250
#define G_ED_Y_CAPTION    0x90AB14   // 80
#define G_ED_BUTTON       0x909EEC   // 0.85, the size argument every button is drawn with
#define G_ED_ROW_STEP     0x90AB44   // 90, paint row to erase row
#define G_ED_COL_STEP     0x90AB9C   // 105, resource to resource

#define ED_IDX_BAUXITE    4          // 0x2350D0's index, not the channel
#define ED_CH_BAUXITE     6          // what that index becomes by the time it reaches 0x238B00

#define ED_GAMEOBJ_BAUXITE 0x27      // "this map has bauxite"

static const BYTE kEdPanelPrologue[] = {
    0x48, 0x8B, 0xC4,                                  // mov rax,rsp
    0x48, 0x89, 0x58, 0x18,                            // mov [rax+0x18],rbx
    0x48, 0x89, 0x70, 0x20,                            // mov [rax+0x20],rsi
    0x57,                                              // push rdi
    0x48, 0x81, 0xEC, 0xB0, 0x00, 0x00, 0x00           // sub rsp,0xb0
};
static const BYTE kEdDispatchPrologue[] = {
    0x40, 0x55,                                        // push rbp
    0x41, 0x54,                                        // push r12
    0x41, 0x55,                                        // push r13
    0x41, 0x56,                                        // push r14
    0x41, 0x57,                                        // push r15
    0x48, 0x8D, 0xAC, 0x24, 0xE0, 0xDB, 0xFF, 0xFF,    // lea rbp,[rsp-0x2420]
    0xB8, 0x20, 0x25, 0x00, 0x00                       // mov eax,0x2520
};
static const BYTE kEdCursorPrologue[] = {
    0x48, 0x8B, 0xC4,                                  // mov rax,rsp
    0x48, 0x89, 0x58, 0x20,                            // mov [rax+0x20],rbx
    0x55,                                              // push rbp
    0x56,                                              // push rsi
    0x57,                                              // push rdi
    0x41, 0x55,                                        // push r13
    0x41, 0x56,                                        // push r14
    0x48, 0x8D, 0xA8, 0x88, 0xFE, 0xFF, 0xFF           // lea rbp,[rax-0x178]
};
static const BYTE kEdTexelsPrologue[] = {
    0x48, 0x8B, 0xC4,                                  // mov rax,rsp
    0x48, 0x89, 0x48, 0x08,                            // mov [rax+8],rcx
    0x55,                                              // push rbp
    0x41, 0x55,                                        // push r13
    0x48, 0x8D, 0x68, 0xC1,                            // lea rbp,[rax-0x3f]
    0x48, 0x81, 0xEC, 0xC8, 0x00, 0x00, 0x00           // sub rsp,0xc8
};

typedef void* (*t_ED_ToolFind)(void*, const char*);
// Argument slots verified against the call site at 0x233248: rcx self,
// rdx tool, r8 accumulator, xmm3 x, then y / size / flag / two bytes on the
// stack. Declaring it in that order is enough - MSVC lands every argument in
// the slot the game reads it from.
typedef float (*t_ED_DrawButton)(void* self, void* tool, void* acc,
                                 float x, float y, float size,
                                 int flag, char a, char b);
typedef void  (*t_ED_Paint)(void* self, char mode, int idx);
// The first argument really is dead: 0x238B00 reads all three coordinates out
// of the vector in rdx and never touches rcx, which the call site at 0x2352E4
// leaves holding the leftover strength value.
typedef void  (*t_ED_PaintTexels)(void* dead, float* pos, unsigned channel,
                                  float innerR, float outerR, int delta,
                                  unsigned limit, char bracket);
typedef void  (*t_ED_Void1)(void*);
typedef void* (*t_ED_CreateManagedTexture)(void*, const char*);

static t_ED_Void1       o_ED_Panel;
static t_ED_Void1       o_ED_Dispatch;
static t_ED_Void1       o_ED_Cursor;
static t_ED_PaintTexels o_ED_PaintTexels;
static t_ED_CreateManagedTexture o_ED_CreateManagedTexture;

static int  g_editorPatch;
// The engine reads pointers and floats out of these, so they need real
// alignment - a plain BYTE array is only byte-aligned and any SSE load the
// game does against one would fault.
__declspec(align(16)) static BYTE g_toolPool[MAX_DEPOSITS][2][TOOL_STRIDE];   // [.][0] paint, [.][1] erase
static bool  g_toolsReady;
// paint_bauxite's icon texture as of the last clone. The editor re-creates its
// tools and their textures every time it is entered, so a change here means the
// clones are holding a released texture and have to be rebuilt.
static void* g_toolSrcIcon;
// Index into g_dep of the brush whose paint call is in flight, -1 otherwise.
// The texel hook rewrites its argument only while this is set, so every other
// brush in the editor - bauxite's included - passes through untouched.
static int  g_brushDep = -1;

// Overwrites an inline string in a cloned descriptor without touching a byte
// past its terminator. Only the string itself is rewritten - the descriptor
// has real fields inside the space around it, so anything that pads or fills
// to a buffer length would corrupt them, which rules out strcpy_s. Refuses to
// write a string longer than the one it replaces, since that is the only thing
// that guarantees the fit.
static bool ReplaceInlineString(BYTE* tool, size_t at, const char* to, const char* label)
{
    char*  dst  = (char*)tool + at;
    size_t have = strlen(dst);
    size_t want = strlen(to);
    if (want > have)
    {
        Logf("editor   FAILED  %s: \"%s\" (%zu) longer than \"%s\" (%zu)",
             label, to, want, dst, have);
        return false;
    }
    memcpy(dst, to, want + 1);
    return true;
}

// Loads a button icon the same way the button drawer would, and for the same
// reason it cannot be left to do it.
//
// The drawer has a lazy path - if the descriptor's icon path at +0xB4 is a
// non-empty string and +0x58 is null, it calls CreateManagedTexture and
// Load2DFromFile itself. That path is for tools that come out of building.ini,
// which is the only thing that ever writes +0xB4 (through the format string
// "editor/tool_%s.png" at 0x88C580). On every built-in terrain tool the field
// is an **empty string** and the texture at +0x58 is already loaded, so
// clearing +0x58 on a clone and hoping the drawer refills it gets a null
// bind - and writing a path into a field whose buffer size is unknown is not
// a trade worth taking.
//
// Doing it here needs no assumption about the descriptor at all: both calls
// take the path as a plain argument, so the string can be ours.
static void* LoadToolIcon(const char* path)
{
    if (!o_ED_CreateManagedTexture) return NULL;
    void* tex = o_ED_CreateManagedTexture(g_exeBase + G_MIDDLEPOINT, path);
    if (!ReadablePtr(tex, sizeof(void*))) return NULL;

    void** vtbl = *(void***)tex;
    if (!ReadablePtr(vtbl, TEX_LOAD2DFILE + sizeof(void*))) return NULL;

    typedef void (*t_Load)(void*, const char*, int, int, int, int);
    ((t_Load)vtbl[TEX_LOAD2DFILE / 8])(tex, path, 0, 0, 0, 0);
    return tex;
}

// Every clone is made from the matching bauxite tool, which is the same kind of
// tool in every respect that matters, and then differs in two fields: the name,
// and the icon texture.
static bool BuildDepositTools(void* self)
{
    t_ED_ToolFind find = (t_ED_ToolFind)(g_exeBase + P_ED_TOOL_FIND);
    void* src[2] = { find(self, "paint_bauxite"), find(self, "erase_bauxite") };
    if (!src[0] || !src[1])
    {
        Logf("editor   FAILED  bauxite tools not in the registry - mod brushes disabled");
        g_editorPatch = 0;
        return false;
    }

    // Leaving to the main menu and coming back rebuilds the editor's tools from
    // 0x2E9420 and re-creates their icon textures, so clones taken in a previous
    // session hold a texture that has been released - the buttons stop drawing.
    // Bauxite's own icon pointer is the cheapest thing that tracks exactly that
    // teardown, so it is what the clones are keyed on. The tool descriptors
    // themselves are no use as a key: the vector is often rebuilt into the same
    // block, with the same names at the same addresses.
    void* srcIcon = *(void**)((BYTE*)src[0] + TOOL_ICON_TEX);
    if (g_toolsReady && srcIcon == g_toolSrcIcon) return true;
    if (g_toolsReady)
        Logf("editor   editor rebuilt (bauxite icon %p -> %p) - rebuilding mod brushes",
             g_toolSrcIcon, srcIcon);
    g_toolSrcIcon = srcIcon;
    g_toolsReady  = false;

    static const char* kVerb[2] = { "paint", "erase" };
    for (int k = 0; k < g_depCount; k++)
    {
        DepositDef* d = &g_dep[k];
        if (d->editorColumn < 0) continue;

        for (int i = 0; i < 2; i++)
        {
            BYTE* tool = g_toolPool[k][i];
            memcpy(tool, src[i], TOOL_STRIDE);

            char name[64], icon[MAX_PATH];
            _snprintf_s(name, sizeof(name), _TRUNCATE, "%s_%s", kVerb[i], d->editor);
            _snprintf_s(icon, sizeof(icon), _TRUNCATE, "editor/tool_%s.png", name);

            // The name has to fit in bauxite's, so the editor key is capped at
            // seven characters. Refusing one tool but keeping the other would
            // leave a brush that paints but cannot be turned off, so this drops
            // the whole pair.
            if (!ReplaceInlineString(tool, TOOL_NAME, name, "tool name"))
            {
                d->editorColumn = -1;
                break;
            }

            void* tex = LoadToolIcon(icon);
            // Falling back to bauxite's texture is deliberate: a button that
            // looks wrong is still a working brush, and the clone carries it.
            if (tex) *(void**)(tool + TOOL_ICON_TEX) = tex;
            else     Logf("editor   WARN  %s did not load - \"%s\" keeps bauxite's icon", icon, name);

            if (i == 0) d->toolPaint = tool;
            else        d->toolErase = tool;
        }

        if (d->editorColumn < 0) { d->toolPaint = NULL; d->toolErase = NULL; continue; }

        Logf("editor   \"%s\" tools ready: \"%s\" tex=%p, \"%s\" tex=%p, channel %d, column %d",
             d->name,
             (char*)d->toolPaint, *(void**)(d->toolPaint + TOOL_ICON_TEX),
             (char*)d->toolErase, *(void**)(d->toolErase + TOOL_ICON_TEX),
             d->editorChannel, d->editorColumn);
    }

    g_toolsReady = true;
    return true;
}

// Which deposit's brush is selected, and whether it paints or erases. The
// active tool *is* the descriptor pointer, so this is an identity test, not a
// string compare - and it stays valid even if the editor object is not, since
// it never dereferences anything but the one field it checks first.
static int ActiveDepositTool(void* self, int* modeOut)
{
    if (!g_toolsReady) return -1;
    // Only the one field is read, so only the one field is checked. Asking
    // whether the whole 54 KB editor object is readable was both wrong and
    // fragile - see the note on ReadablePtr.
    if (!ReadablePtr((const BYTE*)self + ED_ACTIVE_TOOL, sizeof(void*))) return -1;

    const void* tool = *(void**)((BYTE*)self + ED_ACTIVE_TOOL);
    for (int k = 0; k < g_depCount; k++)
    {
        if (tool && tool == g_dep[k].toolPaint) { if (modeOut) *modeOut = 1; return k; }
        if (tool && tool == g_dep[k].toolErase) { if (modeOut) *modeOut = 0; return k; }
    }
    return -1;
}

// Appends one paint/erase pair per mod deposit to the Resources tab, to the
// right of the vanilla five, on the same two rows and from the same constants
// the vanilla grid uses.
//
// The accumulator every vanilla button shares is not reachable from here: the
// original passes one stack local through all ten calls and hands it to
// 0x383BD0 before returning, which has already happened by the time this runs.
// Ours gets its own, which costs the buttons their tooltip and nothing else -
// the icon, the hover, the click and the selection badge all work.
static void DrawDepositTools(BYTE* self)
{
    if (!BuildDepositTools(self)) return;

    float dpi    = MM_F(G_DPI);
    float button = MM_F(G_ED_BUTTON);
    float x0     = dpi * MM_F(G_ED_X_A) + dpi * MM_F(G_ED_X_B);
    float xStep  = dpi * MM_F(G_ED_COL_STEP) * button;
    float yPaint = dpi * MM_F(G_ED_Y_BASE) + dpi * MM_F(G_ED_Y_CAPTION);
    float yErase = yPaint + dpi * MM_F(G_ED_ROW_STEP) * button;

    t_ED_DrawButton draw = (t_ED_DrawButton)(g_exeBase + P_ED_DRAW_BUTTON);
    for (int k = 0; k < g_depCount; k++)
    {
        DepositDef* d = &g_dep[k];
        if (d->editorColumn < 0 || !d->toolPaint || !d->toolErase) continue;

        float x = x0 + (float)d->editorColumn * xStep;
        void* acc = NULL;
        draw(self, d->toolPaint, &acc, x, yPaint, button, 0, 1, 1);
        draw(self, d->toolErase, &acc, x, yErase, button, 0, 1, 1);
    }
}

static void h_ED_Panel(void* self)
{
    o_ED_Panel(self);
    if (!g_editorPatch) return;
    __try { DrawDepositTools((BYTE*)self); }
    __except (FaultFilter("editor deposit buttons", GetExceptionInformation()))
    {
        g_editorPatch = 0;
        Logf("editor   mod brushes disabled for this session");
    }
}

static void PaintDeposit(void* self, int dep, int mode)
{
    BYTE* gameobj = *(BYTE**)(g_exeBase + G_GAMEOBJ);
    if (!ReadablePtr(gameobj, ED_GAMEOBJ_BAUXITE + 1)) return;
    BYTE saved = gameobj[ED_GAMEOBJ_BAUXITE];

    g_brushDep = dep;
    ((t_ED_Paint)(g_exeBase + P_ED_PAINT))(self, (char)mode, ED_IDX_BAUXITE);
    g_brushDep = -1;

    gameobj[ED_GAMEOBJ_BAUXITE] = saved;
}

static void h_ED_Dispatch(void* self)
{
    o_ED_Dispatch(self);
    if (!g_editorPatch) return;

    int mode = -1;
    int dep  = ActiveDepositTool(self, &mode);
    if (dep < 0) return;    // the chain we just ran knows none of our names

    __try { PaintDeposit(self, dep, mode); }
    __except (FaultFilter("editor deposit brush", GetExceptionInformation()))
    {
        g_brushDep    = -1;
        g_editorPatch = 0;
        Logf("editor   mod brushes disabled for this session");
    }
}

// Without this the brush works but paints blind: the round terrain cursor is
// drawn only for tools the strcmp chain in 0x2F0E70 recognises.
static void h_ED_Cursor(void* self)
{
    o_ED_Cursor(self);
    if (!g_editorPatch) return;
    if (ActiveDepositTool(self, NULL) >= 0 && ReadablePtr((BYTE*)self + ED_BRUSH_CURSOR, 1))
        *((BYTE*)self + ED_BRUSH_CURSOR) = 1;
}

static void h_ED_PaintTexels(void* dead, float* pos, unsigned channel,
                             float innerR, float outerR, int delta,
                             unsigned limit, char bracket)
{
    // Guarded on one of our own calls being in flight, so every other brush in
    // the editor - including bauxite's, whose index we borrowed to get here -
    // passes through untouched.
    if (g_brushDep >= 0 && g_brushDep < g_depCount && channel == ED_CH_BAUXITE)
        channel = (unsigned)g_dep[g_brushDep].editorChannel;
    o_ED_PaintTexels(dead, pos, channel, innerR, outerR, delta, limit, bracket);
}

static void InstallEditorPatch()
{
    int brushes = 0;
    for (int i = 0; i < g_depCount; i++) if (g_dep[i].editorColumn >= 0) brushes++;
    if (brushes == 0) { Logf("editor   no deposit declares a brush - not hooking"); return; }

    // Not fatal on its own: without it the buttons fall back to bauxite's
    // icon, which is cosmetic, so this only warns.
    void** slot = FindIatSlot(g_exe, DLL_ENGINE,
                              "?CreateManagedTexture@C3D_MIDDLEPOINT@@QEAAPEAVC3DAPI_TEXTURE@@PEBD@Z");
    if (slot) o_ED_CreateManagedTexture = (t_ED_CreateManagedTexture)*slot;
    else      Logf("editor   WARN  no import slot for CreateManagedTexture - buttons keep bauxite's icon");

    struct { DWORD rva; void* detour; void** tramp; const BYTE* expect; size_t stolen; const char* label; }
    hooks[] = {
        { P_ED_PANEL_RVA,    (void*)h_ED_Panel,       (void**)&o_ED_Panel,
          kEdPanelPrologue,    sizeof(kEdPanelPrologue),    "editor panel"    },
        { P_ED_DISPATCH_RVA, (void*)h_ED_Dispatch,    (void**)&o_ED_Dispatch,
          kEdDispatchPrologue, sizeof(kEdDispatchPrologue), "editor dispatch" },
        { P_ED_CURSOR_RVA,   (void*)h_ED_Cursor,      (void**)&o_ED_Cursor,
          kEdCursorPrologue,   sizeof(kEdCursorPrologue),   "editor cursor"   },
        { P_ED_TEXELS_RVA,   (void*)h_ED_PaintTexels, (void**)&o_ED_PaintTexels,
          kEdTexelsPrologue,   sizeof(kEdTexelsPrologue),   "editor texels"   },
    };

    for (size_t i = 0; i < sizeof(hooks) / sizeof(hooks[0]); i++)
    {
        if (InstallInlineHook(g_exeBase + hooks[i].rva, hooks[i].detour, hooks[i].tramp,
                              hooks[i].expect, hooks[i].stolen, hooks[i].label))
            continue;

        // Half an editor brush is worse than none: the button would select a
        // tool that never paints, or the paint would go to bauxite's channel.
        Logf("editor   patch failed at %s - mod brushes disabled", hooks[i].label);
        g_editorPatch = 0;
        return;
    }
    Logf("editor   %d mod brush pair(s) hooked", brushes);
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
    g_resHook    = GetPrivateProfileIntA("tesmioloader", "resourcehook", g_resHook,    ini);
    GetPrivateProfileStringA("tesmioloader", "trace_filter", g_traceFilter,
                             g_traceFilter, sizeof(g_traceFilter), ini);

    char rva[32] = {0};
    GetPrivateProfileStringA("tesmioloader", "resource_rva", "", rva, sizeof(rva), ini);
    if (rva[0]) g_resRva = (DWORD)strtoul(rva, NULL, 0);

    rva[0] = 0;
    GetPrivateProfileStringA("tesmioloader", "resource_vector_rva", "", rva, sizeof(rva), ini);
    if (rva[0]) g_vecRva = (DWORD)strtoul(rva, NULL, 0);

    g_wantCapacity = GetPrivateProfileIntA("tesmioloader", "resource_capacity", g_wantCapacity, ini);
    g_probeMap     = GetPrivateProfileIntA("tesmioloader", "probe_map", g_probeMap, ini);
    g_probeTexel   = GetPrivateProfileIntA("tesmioloader", "probe_texel", g_probeTexel, ini);
    g_depositPatch = GetPrivateProfileIntA("tesmioloader", "deposit_patch", g_depositPatch, ini);
    g_minimapPatch = GetPrivateProfileIntA("tesmioloader", "minimap_patch", g_minimapPatch, ini);
    g_editorPatch  = GetPrivateProfileIntA("tesmioloader", "editor_patch",  g_editorPatch,  ini);

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
    if (g_resHook)    g_hRes   = OpenLog("tesmioloader.resources.log");

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
    Logf("res hook  mode=%d rva=0x%lX", g_resHook, g_resRva);
    Logf("---");

    PatchIat(g_exe, DLL_ENGINE, SYM_READ_FILE,   (void*)h_ReadFileIntoBuffer, (void**)&o_ReadFile,   "ReadFileIntoBuffer");
    PatchIat(g_exe, DLL_ENGINE, SYM_FILE_EXISTS, (void*)h_CheckIfFileExist,   (void**)&o_FileExists, "CheckIfFileExist");
    PatchIat(g_exe, DLL_ENGINE, SYM_LOG_INFO,    (void*)h_LogInfo,            (void**)&o_LogInfo,    "C3DLog_PrintInfo");
    PatchIat(g_exe, DLL_ENGINE, SYM_LOG_WARN,    (void*)h_LogWarn,            (void**)&o_LogWarn,    "C3DLog_PrintWarning");
    PatchIat(g_exe, DLL_ENGINE, SYM_LOG_ERROR,   (void*)h_LogError,           (void**)&o_LogError,   "C3DLog_PrintError");
    PatchIat(g_exe, DLL_ENGINE, SYM_GET_STRING,  (void*)h_GetString,          (void**)&o_GetString,  "C3D_LANGUAGE::GetString");

    // Not hooks - the addresses are taken so mod resources can be given cargo
    // meshes of their own, the same three calls the engine's table makes.
    if (void** s = FindIatSlot(g_exe, DLL_ENGINE, SYM_CREATE_MESH)) o_CreateManagedMesh = (t_CreateManagedMesh)*s;
    if (void** s = FindIatSlot(g_exe, DLL_ENGINE, SYM_MESH_LOAD))   o_MeshLoadFromFile  = (t_MeshLoadFromFile)*s;
    if (void** s = FindIatSlot(g_exe, DLL_ENGINE, SYM_MESH_MTL))    o_MeshLoadMaterial  = (t_MeshLoadMaterial)*s;
    if (!o_CreateManagedMesh || !o_MeshLoadFromFile)
        Logf("resource  WARN  no import slot for the mesh loader - mod resources keep the template's cargo models");
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

    if (g_resHook)
    {
        if (g_resHook >= 2) LoadResourceRegistry();

        const char* hdr = "; name                     which-arg  return value   discovering call site\r\n";
        WriteTo(g_hRes, hdr, (int)strlen(hdr));

        void* target = g_exeBase + g_resRva;
        InstallInlineHook(target, (void*)h_ResourceGet, (void**)&o_ResourceGet,
                          kResourceGetPrologue, STOLEN_BYTES, "ResourceGet");
    }

    HookTextureVtable(engine);

    // One registry feeds all three: the code patch teaches the engine the type,
    // the minimap draws its layer, the editor paints its channel. A deposit
    // that never got through validation is in none of them.
    if (g_depositPatch || g_minimapPatch || g_editorPatch)
    {
        LoadDepositRegistry();
        ValidateDeposits();
    }

    if (g_depositPatch) PatchDepositType();
    if (g_minimapPatch) InstallMinimapPatch();
    if (g_editorPatch)  InstallEditorPatch();
    if (g_menuPatch)    PatchMenuVersion();
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
        Logf("--- shutdown: %ld redirects, %ld resource injections, %d distinct names ---",
             g_nRedirects, g_nInjected, g_seenCount);
        if (g_hLog   != INVALID_HANDLE_VALUE) CloseHandle(g_hLog);
        if (g_hReads != INVALID_HANDLE_VALUE) CloseHandle(g_hReads);
        if (g_hRes   != INVALID_HANDLE_VALUE) CloseHandle(g_hRes);
    }
    return TRUE;
}
