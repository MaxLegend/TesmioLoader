// resources - resources the base game does not have, as a tesmioloader plugin.
//
// The engine keeps its resources in one global std::vector of 832-byte records
// and resolves every name in every .ini through a single function. That makes
// the whole subsystem reachable from two places:
//
//   rva 0x2AA7C0  ResourceGet(self, name, ...)  the choke point. Hooked inline;
//                 a name resources.ini claims is answered with a record of our
//                 own, everything else falls through to the real lookup.
//   rva 0x9E11C0  the vector object - begin, end, capacity. Publishing a record
//                 is writing `end`; the engine allocates 63 and fills 57, so
//                 six mod resources fit without moving anything.
//
// A new record is a clone of a template's, with its own name, caption id and
// cargo meshes. Only the icon is found by name - every mesh path in the base
// game is a literal, which is why AttachResourceMeshes makes the same three
// engine calls the game's own resource table makes.
//
// Everything here is addresses for SOVIET64.exe v1.1.1.7. See
// docs/04-adding-resources.md.

#include "../../src/tesmio_plugin.h"

// The three calls the engine's own resource table makes to give a record its
// cargo geometry. Used to give a mod resource meshes of its own instead of the
// template's - see AttachResourceMeshes.
#define SYM_CREATE_MESH "?CreateManagedMesh@C3D_MIDDLEPOINT@@QEAAPEAVC3D_MESH@@PEBD@Z"
#define SYM_MESH_LOAD   "?LoadFromFile@C3D_MESH@@QEAAHPEBDPEAVC3D_MIDDLEPOINT@@_N@Z"
#define SYM_MESH_MTL    "?LoadMaterial@C3D_MESH@@QEAAHPEBDH@Z"
#define SYM_GET_STRING  "?GetString@C3D_LANGUAGE@@QEAAPEA_WH@Z"

// C3D_MIDDLEPOINT, the object every managed asset is created through.
#define P_MIDDLEPOINT   0x9EACD0

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

static int    g_resHook = 1;      // 0 off, 1 observe only, 2 observe + inject
static DWORD  g_resRva  = DEFAULT_RESOURCEGET_RVA;
static LONG   g_nInjected;
static HANDLE g_hRes = INVALID_HANDLE_VALUE;

// The moved code writes its dumps through this name.
static void WriteTo(HANDLE h, const char* s, int len) { TsmWrite(h, s, len); }

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
// ---------------------------------------------------------------- the plugin

// Published for anything that needs to know which index a mod resource ended up
// with - a building's storage, a cargo model, a UI panel. -1 until the engine's
// init loop has run and the record has actually been claimed.
static int         svc_Count(void)  { return g_regCount; }
static const char* svc_Name(int i)  { return (i >= 0 && i < g_regCount) ? g_reg[i].name : NULL; }
static int         svc_Index(int i) { return (i >= 0 && i < g_regCount) ? g_reg[i].resolved : -1; }
static int         svc_IndexOf(const char* n)
{
    if (!n) return -1;
    for (int i = 0; i < g_regCount; i++)
        if (_stricmp(g_reg[i].name, n) == 0) return g_reg[i].resolved;
    return -1;
}
static const TsmResourceApi kResourceApi = { svc_Count, svc_Name, svc_Index, svc_IndexOf };

extern "C" __declspec(dllexport) unsigned TsmPluginApiVersion(void)
{
    return TSM_API_VERSION;
}

extern "C" __declspec(dllexport) int TsmPluginInit(const TsmHost* host, TsmPluginInfo* info)
{
    TsmBind(host);
    info->name    = "resources";
    info->version = "1.0";

    const char* ini = "plugins\\resources.ini";
    char v[64];

    g_resHook = H->configInt(ini, "resources", "hook", g_resHook);
    if (!g_resHook)
    {
        Logf("resource  hook = 0 - no mod resources");
        return 1;
    }

    if (H->configString(ini, "resources", "resource_rva", v, sizeof(v), "") && v[0])
        g_resRva = (DWORD)strtoul(v, NULL, 0);
    if (H->configString(ini, "resources", "resource_vector_rva", v, sizeof(v), "") && v[0])
        g_vecRva = (DWORD)strtoul(v, NULL, 0);
    g_wantCapacity = H->configInt(ini, "resources", "resource_capacity", g_wantCapacity);

    g_hRes = TsmOpenLog("tesmioloader.resources.log");
    const char* hdr = "; name                     which-arg  return value   discovering call site\r\n";
    WriteTo(g_hRes, hdr, (int)strlen(hdr));

    if (g_resHook >= 2) LoadResourceRegistry();

    // Captions for mod resources. Every string in the game comes through here,
    // so it is hooked whatever mode we are in - the ids we mint are answered
    // locally and nothing else is touched.
    PatchIat(g_exe, DLL_ENGINE, SYM_GET_STRING, (void*)h_GetString,
             (void**)&o_GetString, "C3D_LANGUAGE::GetString");

    // Not hooks - the addresses are taken so mod resources can be given cargo
    // meshes of their own, the same three calls the engine's table makes.
    if (void** s = FindIatSlot(g_exe, DLL_ENGINE, SYM_CREATE_MESH)) o_CreateManagedMesh = (t_CreateManagedMesh)*s;
    if (void** s = FindIatSlot(g_exe, DLL_ENGINE, SYM_MESH_LOAD))   o_MeshLoadFromFile  = (t_MeshLoadFromFile)*s;
    if (void** s = FindIatSlot(g_exe, DLL_ENGINE, SYM_MESH_MTL))    o_MeshLoadMaterial  = (t_MeshLoadMaterial)*s;
    if (!o_CreateManagedMesh || !o_MeshLoadFromFile)
        Logf("resource  WARN  no import slot for the mesh loader - mod resources keep the template's cargo models");

    if (!InstallInlineHook(g_exeBase + g_resRva, (void*)h_ResourceGet,
                           (void**)&o_ResourceGet, kResourceGetPrologue,
                           STOLEN_BYTES, "ResourceGet"))
        return 1;

    Logf("resource  hook mode=%d rva=0x%lX, %d name(s) declared", g_resHook, g_resRva, g_regCount);
    H->provide(TSM_SERVICE_RESOURCES, TSM_RESOURCES_VERSION, &kResourceApi);
    return 0;
}

BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_DETACH && g_hRes != INVALID_HANDLE_VALUE)
        CloseHandle(g_hRes);
    return TRUE;
}
