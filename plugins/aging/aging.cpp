// aging - tying a citizen's age to the calendar.
//
// STATE: this is the measuring half, and it is all that is here yet. It finds
// the age field and the rate it moves at; the correction lands once those two
// numbers are known. Nothing in this plugin changes the game.
//
//
// WHY A PROBE AND NOT A PATCH
//
// The field is not where the notes said it was, and both of the candidates
// static analysis offered turned out to be something else:
//
//   person+0x70    docs/02-findings.md called this the age. It is not: the
//                  per-person tick at 0x832CB0 **resets it to zero** at
//                  0x8337C0 and 0x8338F9 and accumulates the frame time into
//                  it in between. It is a state timer. What reads it is
//                  0x8368B0, which turns it into an eight-step factor through
//                  the thresholds 30/60/90/120/150/180/210 at 0x90A9B8 and on
//   person+0x65C   grows as `dt * person[0xA4] * 30.0 * k`, k in {1, 2, 5},
//                  which reads like an age and is not. It **wraps back to
//                  zero** when it passes `record[0x9C] - 1`, it is seeded at
//                  birth with `C3DRandom_Float(0, record[0x9C] * 0.5)`, and
//                  `record` is a 0xB0-byte entry reached through the object at
//                  person+0x668. A quantity that loops over a per-object frame
//                  count, seeded at a random point so nobody moves in lockstep,
//                  with 30 as its rate and 5x while moving: that is the walk
//                  animation, not a life.
//
// The game's own script API declares `Person.fAge` as a float and
// `Person_SetAge` as instruction 31004, but neither the names nor the id are
// anywhere in the executable - the API is documentation, not data - so there is
// no string to cross-reference. The person constructor at 0x823290 never writes
// an age, and neither does the per-frame tick: every `dt *` in it lands in
// +0x10, +0x28, +0x48, +0x70, +0x90, +0x65C or +0x680. So the age is advanced
// somewhere else, and reading outward from a wrong guess is how the two above
// were nearly written down as facts.
//
// docs/03-reverse-engineering.md ranks runtime observation above static
// analysis for exactly this, and it is right: one in-game week of watching real
// citizens answers "which offset is the age" and "how fast does it move"
// together, and the second number is the whole of the calibration.
//
//
// WHAT IT DOES
//
// Snapshots whole Person objects and diffs them across calendar days.
//
//   the people      the engine's live vector of Person*, at 0x9E75B8..0x9E75C0
//   the date        game+0x590, day of the year, through the world object
//                   pointer at 0x9941F0
//   the clock       an import hook on C3D_TERRAIN::Render, because it needs a
//                   per-frame call and nothing else here does. It chains: the
//                   depletion plugin hooks the same import and both survive
//
// Every time the date changes it re-reads each tracked person and reports each
// 4-byte slot whose float value moved. A field that grows by the same small
// amount every day, out of a value that looks like a human age, is the age -
// and its daily delta is the number the correction needs, because the fix is
// arithmetic from there: one year per 365 of those steps.
//
// Everything is bounded and guarded. It reads; it writes nothing.

#include "../../src/tesmio_plugin.h"

// ---------------------------------------------------------------- the sites

// std::vector<Person*> - begin, end. From docs/02-findings.md, and checked for
// sanity before anything is dereferenced.
#define RVA_PERSON_VECTOR 0xA31678  // v1.2.0.8; was 0x9E75B8

// The world object is a pointer, not the object: 0x8F60 reads it as
// `mov rsi,qword ptr [0x9941F0]`.
#define RVA_WORLD_PTR     0x9DDC88  // v1.2.0.8; was 0x9941F0

#define OFF_DAY           0x5B8  // v1.2.0.8; was 0x590. int, day of the year
#define OFF_YEAR          0x5BC  // v1.2.0.8; was 0x594. int

// operator new(0x750) in the constructor at 0x823290.
#define PERSON_SIZE       0x750

// void C3D_TERRAIN::Render(bool, C3D_CAMERA*, C3D_CAMERA*, int, int). Taken
// from the engine's export table rather than written out by hand - the first
// version of this guessed the mangling and the import lookup simply missed.
#define SYM_TERRAIN_RENDER "?Render@C3D_TERRAIN@@QEAAX_NPEAVC3D_CAMERA@@0HH@Z"

// ---------------------------------------------------------------- settings

static int g_enabled = 1;
static int g_people  = 4;       // how many citizens to follow
static int g_from    = 0xA8;    // first offset compared
static int g_to      = PERSON_SIZE;
static int g_days    = 14;      // stop after this many reports

// A slot is worth printing when it looks like it could be a person's age: a
// finite float somewhere between these, that went up rather than down. Set
// `strict = 0` to see every float that moved instead.
static float g_min    = 0.0f;
static float g_max    = 200.0f;
static int   g_strict = 1;

// ---------------------------------------------------------------- state

#define MAX_PEOPLE 16

typedef void (*t_TerrainRender)(void*, bool, void*, void*, int, int);
static t_TerrainRender o_TerrainRender;

struct Tracked
{
    BYTE* person;                       // the object we are following
    BYTE  snap[PERSON_SIZE];
};

static Tracked  g_track[MAX_PEOPLE];
static int      g_tracked;
static int      g_lastDay = -1;
static int      g_reports;
static bool     g_armed;

// ---------------------------------------------------------------- reading

static BYTE* World(void)
{
    BYTE** slot = (BYTE**)(g_exeBase + RVA_WORLD_PTR);
    if (!ReadablePtr(slot, sizeof(void*))) return NULL;
    BYTE* w = *slot;
    if (!w || !ReadablePtr(w, 0x600)) return NULL;
    return w;
}

// begin/end of the live vector, with every sanity check that costs nothing.
// A count of half a million or a stride that does not divide is a wrong
// address, not a big republic, and either has to stop this rather than be
// dereferenced.
static int People(BYTE*** outBegin)
{
    BYTE*** v = (BYTE***)(g_exeBase + RVA_PERSON_VECTOR);
    if (!ReadablePtr(v, 16)) return 0;

    BYTE** begin = (BYTE**)v[0];
    BYTE** end   = (BYTE**)v[1];
    if (!begin || end < begin) return 0;

    size_t bytes = (size_t)((BYTE*)end - (BYTE*)begin);
    if (bytes % sizeof(void*)) return 0;

    size_t n = bytes / sizeof(void*);
    if (n == 0 || n > 500000) return 0;
    if (!ReadablePtr(begin, bytes < 4096 ? bytes : 4096)) return 0;

    *outBegin = begin;
    return (int)n;
}

// ---------------------------------------------------------------- the probe

static void Arm(BYTE** begin, int count)
{
    int want = g_people;
    if (want > MAX_PEOPLE) want = MAX_PEOPLE;
    if (want > count)      want = count;

    // Spread across the vector rather than taking the first few, which in a
    // loaded save are whoever happens to sit at the front of it.
    int step = count / (want ? want : 1);
    if (step < 1) step = 1;

    g_tracked = 0;
    for (int i = 0; i < want; i++)
    {
        BYTE* p = begin[i * step];
        if (!p || !ReadablePtr(p, PERSON_SIZE)) continue;
        g_track[g_tracked].person = p;
        memcpy(g_track[g_tracked].snap, p, PERSON_SIZE);
        g_tracked++;
    }

    Logf("aging    following %d of %d citizen(s), offsets 0x%X..0x%X, %d day(s)",
         g_tracked, count, g_from, g_to, g_days);
}

static bool Sane(float f)
{
    // No NaN, no infinity - a raw object is full of pointers that decode as
    // both, and they are the bulk of what a naive diff would print.
    if (f != f) return false;
    if (f > 3.0e38f || f < -3.0e38f) return false;
    return true;
}

static void Report(int day, int year)
{
    Logf("aging    ---- day %d of year %d ----", day, year);

    for (int i = 0; i < g_tracked; i++)
    {
        BYTE* p = g_track[i].person;
        if (!ReadablePtr(p, PERSON_SIZE))
        {
            Logf("aging    person %d (%p) is gone - dropped", i, p);
            g_track[i].person = NULL;
            continue;
        }

        int printed = 0;
        for (int off = g_from; off + 4 <= g_to; off += 4)
        {
            float fNow = *(const float*)(p + off);
            float fWas = *(const float*)(g_track[i].snap + off);
            int   iNow = *(const int*)  (p + off);
            int   iWas = *(const int*)  (g_track[i].snap + off);
            if (iNow == iWas) continue;

            // The same four bytes are offered twice, as a float and as an int,
            // because an age stored either way has to be caught in one run -
            // guessing the type is how the last two candidates were wrong.
            bool asFloat = Sane(fNow) && Sane(fWas);
            bool asInt   = true;

            if (g_strict)
            {
                if (asFloat && (fNow <= fWas || fNow < g_min || fNow > g_max ||
                                fNow - fWas > g_max))
                    asFloat = false;
                if (iNow <= iWas || (float)iNow < g_min || (float)iNow > g_max)
                    asInt = false;
                if (!asFloat && !asInt) continue;
            }

            Logf("aging      person %d  +0x%03X  f %12.6f -> %12.6f (+%.6f)   i %d -> %d%s%s",
                 i, off, fWas, fNow, fNow - fWas, iWas, iNow,
                 asFloat ? "   <float>" : "", asInt ? "   <int>" : "");
            printed++;
            if (printed >= 40)
            {
                Logf("aging      person %d  ... 40 slots printed, stopping", i);
                break;
            }
        }
        if (!printed)
            Logf("aging      person %d  nothing moved that looks like an age", i);

        memcpy(g_track[i].snap, p, PERSON_SIZE);
    }
}

static void Tick(void)
{
    BYTE* world = World();
    if (!world) return;

    int day  = *(const int*)(world + OFF_DAY);
    int year = *(const int*)(world + OFF_YEAR);
    if (day < 0 || day > 400) return;             // not a loaded world yet

    BYTE** begin = NULL;
    int    count = People(&begin);
    if (count <= 0) return;

    if (!g_armed)
    {
        Arm(begin, count);
        g_armed   = true;
        g_lastDay = day;
        return;
    }

    if (day == g_lastDay) return;
    g_lastDay = day;

    Report(day, year);

    if (++g_reports >= g_days)
    {
        Logf("aging    %d day(s) reported - stopping. Raise `days` for more.", g_reports);
        g_enabled = 0;
    }
}

static void h_TerrainRender(void* self, bool a, void* cam1, void* cam2, int b, int c)
{
    o_TerrainRender(self, a, cam1, cam2, b, c);

    if (!g_enabled) return;
    __try { Tick(); }
    __except (FaultFilter("aging probe", GetExceptionInformation()))
    {
        Logf("aging    disabled after a fault");
        g_enabled = 0;
    }
}

// ---------------------------------------------------------------- setup

static void ReadSettings(void)
{
    const char* ini = "plugins\\aging.ini";
    char v[64];

    g_enabled = H->configInt(ini, "aging", "enabled", g_enabled);
    g_people  = H->configInt(ini, "aging", "people",  g_people);
    g_days    = H->configInt(ini, "aging", "days",    g_days);
    g_from    = H->configInt(ini, "aging", "from",    g_from);
    g_to      = H->configInt(ini, "aging", "to",      g_to);
    g_strict  = H->configInt(ini, "aging", "strict",  g_strict);

    if (H->configString(ini, "aging", "min", v, sizeof(v), "") && v[0]) g_min = (float)atof(v);
    if (H->configString(ini, "aging", "max", v, sizeof(v), "") && v[0]) g_max = (float)atof(v);

    if (g_people < 1) g_people = 1;
    if (g_people > MAX_PEOPLE) g_people = MAX_PEOPLE;
    if (g_days   < 1) g_days = 1;
    if (g_from   < 0) g_from = 0;
    if (g_to > PERSON_SIZE) g_to = PERSON_SIZE;
    if (g_to <= g_from) g_to = PERSON_SIZE;
}

extern "C" __declspec(dllexport) unsigned TsmPluginApiVersion(void)
{
    return TSM_API_VERSION;
}

extern "C" __declspec(dllexport) int TsmPluginInit(const TsmHost* host, TsmPluginInfo* info)
{
    TsmBind(host);
    info->name    = "aging";
    info->version = "0.1 (probe)";

    ReadSettings();
    if (!g_enabled)
    {
        Logf("aging    disabled");
        return 1;
    }
    return 0;
}

extern "C" __declspec(dllexport) int TsmPluginStart(void)
{
    if (!g_enabled) return 1;

    // An import swap, so it chains with depletion's hook on the same function
    // instead of fighting it, and needs no address of its own.
    if (!PatchIat(g_exe, DLL_ENGINE, SYM_TERRAIN_RENDER, (void*)h_TerrainRender,
                  (void**)&o_TerrainRender, "C3D_TERRAIN::Render"))
    {
        Logf("aging    no import slot for C3D_TERRAIN::Render - no probe");
        return 1;
    }

    Logf("aging    probe armed - it reads the citizens and changes nothing");
    return 0;
}

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) { return TRUE; }
