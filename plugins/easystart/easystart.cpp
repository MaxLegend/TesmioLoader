// easystart - a citizen's needs arrive with the century, as a tesmioloader plugin.
//
// A map that starts pre-populated hands the player a town whose citizens want
// electricity, running water, central heating and a cinema on the first day.
// They lived without all of it the day before. This plugin holds each need back
// until a year in `plugins/easystart.ini`.
//
// It holds back the **consequences only**. A cinema built in 1925 is still
// walked to, a shop still sells clothes, a pub still serves; those things
// simply do not feed happiness yet, and their absence costs none.
//
// A need has four faces in this game and all four have to be gated, which is
// what the first version got wrong: it gated the first and left the other three
// visible.
//
//   1. the citizen's own demand list      -> happiness and health
//   2. the rate a building subtracts      -> happiness and health
//   3. the counters a building panel draws -> "56 Citizen(s) without power"
//   4. a real mechanic                    -> children in school, parents at work
//
//
// 1. WHAT A NEED IS, AND WHERE THE LIST OF THEM CAME FROM
//
// The game keeps an itemised ledger of why happiness and health moved, and the
// statistics window draws it. Both drawers walk a float array immediately
// followed by an int array of the same length, with a base language id
// incremented once per entry, so one `mov r12d,<id>` gives the whole table:
//
//   happiness  23 reasons, ids 54031..54053
//   health     12 reasons, ids 54003..54014
//
// Scanning `.text` for `inc dword ptr [rip+X]` landing in either count array
// maps every reason to the code that raises it. That scan is the map of this
// feature. Two negatives fell out of it and both matter: **there is no
// "no sewage" reason** - a full sewage store stops the building drawing water
// and the player sees *No water*, so sewage rides on the water gate - and
// reason 4, **"Job", has no writer at all**.
//
// The nine demand-carried needs are all decided in one function:
//
//   rva 0x83A6D0   FUN(game, person)
//
// one caller, inside the per-person tick right after the daily planner. It
// walks `person+0x118` and picks the reason from two fields and nothing else:
//
//   demand+0x08 == 3    alcohol       demand+0x10 == game[0xC300]  food
//   demand+0x08 == 4    church        demand+0x10 == game[0xC310]  meat
//   demand+0x08 == 5    culture       demand+0x10 == game[0xC318]  clothes
//   demand+0x08 == 6    sport         demand+0x10 == game[0xC320]  electronics
//   demand+0x08 == 10   hospital
//
// So a demand that is not in the array when this runs affects nothing, while
// one that is there everywhere else still routes the citizen to the pub. The
// gate is therefore a **pre/post bracket and no patch**: compact the locked
// entries out, call the original, put the array back byte for byte.
//
// Removing the entry removes the bonus with it. That is deliberate, and it is
// what "the early cinema changes nothing" means.
//
// The **status floats have to be held** or the unlock is a catastrophe: it is
// the *planner* that decays them, so twenty years of locked culture would rot
// that status to zero and the republic would turn miserable on the day the
// cinema starts counting. Health is never pinned - it is the citizen's real
// health - and neither is happiness, which is the thing being protected.
//
// Alcohol runs backwards and was pinned wrong through 1.1: fStatusAlcohol is
// how much a citizen has been drinking, not how content they are, and it
// feeds the health ledger's "Drinking alcohol" reason directly - the same
// list that "Starving" and "Pollution" are on. Holding it at `locked_status`
// (1.0 by default) told the health system every citizen was drinking at
// capacity for as long as alcohol stayed locked, which is what the "status
// bar at 100%" observed on the 1.0 save actually was. It is now pinned to 0
// unconditionally - the one status where zero, not the configured level, is
// what "this need does not exist yet" means.
//
//
// 2. THE BUILDING SIDE IS TWELVE RATES IN .rdata
//
//   0x1BC210   the living tick        happiness: no electricity, no water
//   0x488B80   interior temperature   happiness and health
//   0x1B0950   drinking water         health
//
// Each computes its per-person amount **once before the loop over the
// inhabitants**, scales it by the "Unsatisfied citizens reaction" setting at
// game+0x5C8, and guards the ledger write with `ucomiss amount,0 / je`. So a
// zeroed amount is a complete no-op - no subtraction, no clamp, and nothing in
// the statistics either. Each amount is finalised by one rip-relative load of a
// .rdata constant, and repointing that displacement at a value this plugin owns
// is the whole patch. Overwriting the constants in place is wrong: 0x909ABC and
// 0x909AC4 are each read by two blocks.
//
//
// 3. THE PANEL COUNTS SEPARATELY, AND THAT IS WHAT WAS STILL SHOWING
//
// The living building's window draws "N Citizen(s) currently without power" and
// "N Citizen(s) are currently without drinking water" from four floats the
// living tick snapshots at the top of every pass:
//
//   building+0x1104 -> +0x1108 |  +0x110C -> +0x1110
//   building+0x1114 -> +0x1118 |  +0x111C -> +0x1120
//
// The panel takes `max(+0x1108, +0x1110, +0x1118)` for power and `+0x1120` for
// water. **The tick never reads the snapshots back** - it works from the
// accumulators - so zeroing them in a post-hook is invisible to the simulation
// and empties both lines. That is why this is a post-hook on the tick rather
// than a patch on the panel: one place, and the building's own state stays
// consistent with what is drawn.
//
// "Building is without power supply" is a third line and comes from a query,
// `0x1BBFE0(game, building)`, whose result the panel tests with `test al,al`.
// That query has twenty callers deep in the simulation, so it is not hooked:
// the panel's own `call rel32` at 0x76147B is rewritten to a thunk instead, and
// every other caller is untouched.
//
//
// 4. SCHOOL AND KINDERGARTEN ARE ONE SETTING, AND IT SPLITS IN TWO
//
// Neither raises a happiness or a health reason, so neither is a need in the
// sense above. They are the game's own "Education simulation" option, which
// lives at **game+0x5DC** - `0` is Complex, `1` is Simple - and whose help text
// (language id 713) is exactly the two behaviours wanted: *children
// automatically reach basic education (no elementary schools needed); parents
// can work even while their children are under 6 (no kindergarten needed)*.
//
// Writing that field is not an option - it is saved with the world and read by
// windows - so the seven places the *simulation* tests it are rewritten to
// `cmp dword ptr [rip+ours], imm8`, which is the same seven bytes (eight with a
// SIB byte, plus a nop). Each site keeps its own immediate; only the memory
// operand moves. And because the sites divide cleanly, **school and
// kindergarten get separate years**:
//
//   school        0x823AE9, 0x824116, 0x824145, 0x834D65   education at birth
//                 0x7618E7                                 the panel's warning
//   kindergarten  0x824AEB   a parent of a child under 6 searching for a place
//                 0x761928                                 the panel's warning
//
// The five sites in the statistics window and the renderer are left alone.
//
// Addresses are for SOVIET64.exe **v1.1.1.9** and every one is compared against
// the bytes this build is known to have before anything is written. The first
// version of this plugin was derived against v1.1.1.7 and every code address in
// it was wrong by +0x70, +0xA0 or +0x1E0. See docs/16-easystart.md.

#include "../../src/tesmio_plugin.h"

// ---------------------------------------------------------------- the needs

enum
{
    NEED_FOOD = 0,
    NEED_MEAT,
    NEED_CLOTHES,
    NEED_ELECTRONICS,
    NEED_ALCOHOL,
    NEED_RELIGION,
    NEED_CULTURE,
    NEED_SPORT,
    NEED_HEALTH,
    NEED_ELECTRICITY,
    NEED_WATER,
    NEED_HEATING,
    NEED_SCHOOL,
    NEED_KINDERGARTEN,
    NEED_COUNT
};

// The key in [unlock], the status float the need owns as an index into the
// eleven at person+0xD8, and the year it defaults to. Meat shares the food
// status; the utilities and the two education keys have none; health is the
// citizen's real health and is never written.
static const struct
{
    const char* key;
    int         status;
    int         year;
} kNeeds[NEED_COUNT] = {
    { "food",          1, 1921 },   // fStatusFood, and it is satiety
    { "meat",         -1, 1927 },
    { "clothes",       8, 1927 },
    { "electronics",   9, 1955 },
    { "alcohol",       4, 1932 },   // fStatusAlcohol - inverted, see PinStatuses
    { "religion",      7, 1921 },
    { "culture",       5, 1940 },
    { "sport",         6, 1932 },
    { "health",       -1, 1940 },   // fStatusHealth, deliberately not pinned
    { "electricity",  -1, 1945 },
    { "water",        -1, 1950 },
    { "heating",      -1, 1950 },
    { "school",       -1, 1932 },
    { "kindergarten", -1, 1945 },
};

#define YEAR_ALWAYS  0            // unlocked from the first day
#define YEAR_NEVER   0x7FFFFFFF   // `off` - never matters at all

// ---------------------------------------------------------------- offsets

#define GAME_YEAR        0x594    // int, the absolute year: 1970 in a fresh save
#define GAME_RES_FOOD    0xC300   // the four cached shop-goods records
#define GAME_RES_MEAT    0xC310
#define GAME_RES_CLOTHES 0xC318
#define GAME_RES_ELECTR  0xC320

#define PERSON_STATUS    0xD8     // eleven floats
#define PERSON_DCOUNT    0x110
#define PERSON_DEMANDS   0x118
#define PERSON_UNSAT     0x4F0    // count, then 16-byte entries at +0x4F8
#define PERSON_UNSAT_E   0x4F8

#define DEMAND_STRIDE    0x80
#define DEMAND_MAX       7        // (0x4F0 - 0x118) / 0x80, arithmetic not a check
#define DEMAND_KIND      0x08
#define DEMAND_RES       0x10

#define UNSAT_STRIDE     0x10     // { float amount, int kind, Resource* }
#define UNSAT_KIND       0x04
#define UNSAT_RES        0x08
#define UNSAT_MAX        10

#define PERSON_MONEY_SPENT 0x734  // 02-findings.md: money this person has spent

#define PERSON_SIZE      0x750    // operator new(0x750) in the constructor

// A resource record is 832 bytes and opens with its name.
#define RESOURCE_NAME    0x00

// The living building's four "how many went without" snapshots, which are what
// the window prints. The tick writes them from its accumulators and never reads
// them back.
#define BLD_OUT_POWER_A  0x1108
#define BLD_OUT_POWER_B  0x1110
#define BLD_OUT_POWER_C  0x1118
#define BLD_OUT_WATER    0x1120

// The game object is a static, not a pointer to one - see docs/14-cities.md
// and docs/15-daynight.md, both of which already rely on this address.
#define P_GAMEOBJ           0x9D4F10

// std::vector<Person*>, a global on its own (not inside the game object) - see
// 02-findings.md, "The person". Begin at +0, end 8 bytes later; the element
// count is (end-begin)/8, decided at runtime.
#define PERSON_VEC_BEGIN    0x9E75B8
#define PERSON_VEC_END      0x9E75C0

// std::vector<Building*>, every building on the map - game+0x11B08 (begin),
// +0x11B10 (end). See docs/14-cities.md.
#define GAME_BUILDING_VEC   0x11B08

// The type lives on the building's *type descriptor*, not on the building
// itself - confirmed both by depletion.cpp/accumulator.cpp (which already read
// it this way, commented "typedesc -> building type") and by the vanilla
// planner's own disassembly: `mov rdx,[person+0x20]` (the building) /
// `mov r8,[rdx+0x318]` (its type descriptor) / `mov esi,[r8+0x360]` (the type).
// Reading +0x360 straight off the building, as this file did through 1.2,
// reads an unrelated field and never matches - see docs/07-pitfalls.md.
#define B_TYPEDESC          0x318
#define BUILDING_TYPE       0x360
#define BUILDINGTYPE_LIVING 2        // SOVIETInstructions.txt: BUILDINGTYPE_LIVING = 2,
                                      // and no other type shares the number

static int BuildingTypeOf(const BYTE* b)
{
    if (!b || !ReadablePtr(b, B_TYPEDESC + 8)) return -1;
    const BYTE* td = *(const BYTE* const*)(b + B_TYPEDESC);
    if (!td || !ReadablePtr(td, BUILDING_TYPE + 4)) return -1;
    return *(const int*)(td + BUILDING_TYPE);
}

// ---------------------------------------------------------------- the sites

// The evaluator. rcx is the world, rdx the person; its return value is dead at
// the one call site.
#define RVA_EVALUATE 0x83A870   // v1.1.1.13; was 0x83A6D0
static const BYTE kEvaluatePrologue[] = {
    0x48, 0x8B, 0xC4,               // mov  rax,rsp
    0x55, 0x56, 0x57,               // push rbp / rsi / rdi
    0x41, 0x54, 0x41, 0x55,         // push r12 / r13
    0x41, 0x56, 0x41, 0x57          // push r14 / r15
};

// The living building's tick: (game, building, flag).
#define RVA_LIVING_TICK 0x1BC190   // v1.1.1.13; was 0x1BC210
static const BYTE kLivingPrologue[] = {
    0x48, 0x8B, 0xC4,               // mov rax,rsp
    0x48, 0x89, 0x58, 0x10,         // mov [rax+0x10],rbx
    0x48, 0x89, 0x70, 0x18,         // mov [rax+0x18],rsi
    0x48, 0x89, 0x78, 0x20,         // mov [rax+0x20],rdi
    0x55                            // push rbp
};

// "Building is without power supply": the panel's own call, and the query it
// calls. Only this one call site is redirected.
#define RVA_POWER_CALL  0x76161B   // v1.1.1.13; was 0x76147B
#define RVA_HAS_POWER   0x1BBF60   // v1.1.1.13; was 0x1BBFE0
static const BYTE kHasPowerHead[] = { 0x83, 0xB9, 0xB0, 0x05, 0x00, 0x00, 0x00 };

// One repointed rate. `wide` marks a double rather than a float, which the two
// electricity sites are: they go cvtss2sd / mulsd / cvtpd2ps.
struct Rate
{
    DWORD       rva;
    const BYTE* op;
    int         opLen;
    DWORD       constRva;
    double      vanilla;
    int         wide;
    int         need;
    const char* label;
};

static const BYTE kMulsdXmm1[]   = { 0xF2, 0x0F, 0x59, 0x0D };       // mulsd xmm1,[rip]
static const BYTE kMulssXmm3[]   = { 0xF3, 0x0F, 0x59, 0x1D };       // mulss xmm3,[rip]
static const BYTE kMulssXmm2[]   = { 0xF3, 0x0F, 0x59, 0x15 };       // mulss xmm2,[rip]
static const BYTE kMulssXmm0[]   = { 0xF3, 0x0F, 0x59, 0x05 };       // mulss xmm0,[rip]
static const BYTE kMulssXmm1[]   = { 0xF3, 0x0F, 0x59, 0x0D };       // mulss xmm1,[rip]
static const BYTE kMovssXmm7[]   = { 0xF3, 0x0F, 0x10, 0x3D };       // movss xmm7,[rip]
static const BYTE kMovssXmm8[]   = { 0xF3, 0x44, 0x0F, 0x10, 0x05 }; // movss xmm8,[rip]

static const Rate kRates[] = {
    // 0x1BC210, the living tick: happiness for electricity and for water. Two
    // electricity branches, split on how the shortfall is measured.
    { 0x1BC31B, kMulsdXmm1, 4, 0x909E40, 0.00021666666666666666, 1, NEED_ELECTRICITY, "electricity a"  },
    { 0x1BC4F5, kMulsdXmm1, 4, 0x909E68, 0.00033333333333333332, 1, NEED_ELECTRICITY, "electricity b"  },
    { 0x1BC6CB, kMulssXmm3, 4, 0x909B00, 0.0009999999310821295,  0, NEED_WATER,       "water happy"    },

    // 0x488B80, interior temperature: happiness in xmm7, health in xmm8, and a
    // second pair for the colder setting.
    { 0x488B7A, kMovssXmm7, 4, 0x909B20, 0.001500000013038516,   0, NEED_HEATING,     "heat happy a"   },
    { 0x488BAC, kMovssXmm7, 4, 0x909B0C, 0.0011699999449774623,  0, NEED_HEATING,     "heat happy b"   },
    { 0x488B96, kMovssXmm8, 5, 0x909B28, 0.0017000000225380063,  0, NEED_HEATING,     "heat health a"  },
    { 0x488BB4, kMovssXmm8, 5, 0x909B18, 0.0013259999686852098,  0, NEED_HEATING,     "heat health b"  },

    // 0x1B0950, drinking water: health, in two blocks. The second picks its
    // rate from the difficulty; the first has a bonus term for clean water,
    // which is zeroed as well - a locked need must not pay either way.
    { 0x1B0B44, kMulssXmm2, 4, 0x909AC4, 0.0002500000118743628,  0, NEED_WATER,       "water health a" },
    { 0x1B0B51, kMulssXmm0, 4, 0x909ACC, 0.00031250002211891115, 0, NEED_WATER,       "water health b" },
    { 0x1B0F82, kMulssXmm1, 4, 0x909AB0, 0.0001875000016298145,  0, NEED_WATER,       "water health c" },
    { 0x1B0F92, kMulssXmm1, 4, 0x909AC4, 0.0002500000118743628,  0, NEED_WATER,       "water health d" },
    { 0x1B0FA2, kMulssXmm1, 4, 0x909ACC, 0.00031250002211891115, 0, NEED_WATER,       "water health e" },
};

#define RATE_COUNT ((int)(sizeof(kRates) / sizeof(kRates[0])))

// Every place the *simulation* tests "Education simulation" at game+0x5DC. The
// five in the statistics window (0x8BB2A, 0x8BDAE, 0x8BF8D), the renderer
// (0x326A50) and the editor (0x382A8B) are deliberately absent.
struct EduSite
{
    DWORD rva;
    int   need;
    const char* label;
};

static const EduSite kEduSites[] = {
    { 0x823C89, NEED_SCHOOL,       "school: basic education at birth"   },
    { 0x8242B6, NEED_SCHOOL,       "school: education for a new citizen" },
    { 0x8242E5, NEED_SCHOOL,       "school: the same, second branch"    },
    { 0x834F05, NEED_SCHOOL,       "school: education on arrival"       },
    { 0x761A87, NEED_SCHOOL,       "school: the panel's warning"        },
    { 0x824C8B, NEED_KINDERGARTEN, "kindergarten: a parent's job search" },
    { 0x761AC8, NEED_KINDERGARTEN, "kindergarten: the panel's warning"  },
};

#define EDU_SITE_COUNT ((int)(sizeof(kEduSites) / sizeof(kEduSites[0])))

// ---------------------------------------------------------------- settings

static int   g_enabled     = 1;
static int   g_demands     = 1;
static int   g_utilities   = 1;
static int   g_counters    = 1;
static int   g_pinStatus   = 1;
static int   g_clearUnsat  = 1;
static float g_lockedLevel = 1.0f;
static int   g_probe       = 0;

static int   g_unlock[NEED_COUNT];

// [unlock_resource]: a mod good declared by `needs` gets a year of its own.
#define MOD_MAX 32
static struct { char name[48]; int year; } g_mod[MOD_MAX];
static int g_modCount = 0;

// ---------------------------------------------------------------- state

typedef void (*t_Evaluate)(void* game, void* person);
typedef char (*t_LivingTick)(void* game, void* building, char flag);
typedef char (*t_HasPower)(void* game, void* building);

static t_Evaluate   o_Evaluate;
static t_LivingTick o_LivingTick;
static t_HasPower   g_HasPower;      // called, never hooked

// Everything the rewritten instructions read. One allocation within +-2GB of
// the executable, because that is what makes a 32-bit displacement reach.
static BYTE*  g_slots;
static int*   g_eduFlag;             // [0] school, [1] kindergarten

static int    g_year       = -1;
static int    g_locked[NEED_COUNT];
static int    g_anyLocked  = 0;
static int    g_ratesOk[RATE_COUNT];
static int    g_powerCall  = 0;
static int    g_swept      = 0;      // the once-per-load population sweep, below

// ---------------------------------------------------------------- patching

static bool WriteCode(void* at, const void* bytes, size_t n, const char* what)
{
    DWORD prot = 0;
    if (!VirtualProtect(at, n, PAGE_EXECUTE_READWRITE, &prot))
    {
        Logf("easystart  VirtualProtect failed at %p (%s)", at, what);
        return false;
    }
    memcpy(at, bytes, n);
    VirtualProtect(at, n, prot, &prot);
    FlushInstructionCache(GetCurrentProcess(), at, n);
    return true;
}

// g_slots layout: two education flags, then eight bytes per rate.
static int*   EduSlot(int i)  { return (int*)(g_slots + 4 * i); }
static BYTE*  RateSlot(int i) { return g_slots + 16 + (size_t)i * 8; }

static void WriteRate(int i, double v)
{
    if (kRates[i].wide) *(double*)RateSlot(i) = v;
    else                *(float*)RateSlot(i)  = (float)v;
}

static bool RepointRate(int i)
{
    const Rate* r    = &kRates[i];
    BYTE*       site = g_exeBase + r->rva;

    if (memcmp(site, r->op, (size_t)r->opLen) != 0)
    {
        Logf("easystart  %s: rva 0x%X is %02X %02X %02X %02X, not the expected load"
             " - refusing", r->label, r->rva, site[0], site[1], site[2], site[3]);
        return false;
    }

    DWORD resolved = (DWORD)(r->rva + r->opLen + 4 + *(const int*)(site + r->opLen));
    if (resolved != r->constRva)
    {
        Logf("easystart  %s: rva 0x%X reads 0x%X, expected 0x%X - refusing",
             r->label, r->rva, resolved, r->constRva);
        return false;
    }

    double now = r->wide ? *(const double*)(g_exeBase + r->constRva)
                         : (double)*(const float*)(g_exeBase + r->constRva);
    if (now != r->vanilla)
    {
        Logf("easystart  %s: 0x%X holds %.12g, expected %.12g - refusing",
             r->label, r->constRva, now, r->vanilla);
        return false;
    }

    BYTE*   slot = RateSlot(i);
    __int64 rel  = slot - (site + r->opLen + 4);
    if (rel < (__int64)INT_MIN || rel > (__int64)INT_MAX)
    {
        Logf("easystart  %s: slot %p out of rel32 range of %p - refusing",
             r->label, slot, site);
        return false;
    }

    WriteRate(i, r->vanilla);        // vanilla until the first tick reports a year
    int disp = (int)rel;
    if (!WriteCode(site + r->opLen, &disp, sizeof(disp), r->label)) return false;

    Logf("easystart  rate %-15s rva 0x%X now reads %p (was 0x%X = %.12g)",
         r->label, r->rva, slot, r->constRva, r->vanilla);
    return true;
}

// Rewrites one `cmp dword ptr [reg+0x5DC], imm8` into `cmp dword ptr [rip+X],
// imm8`, keeping the site's own immediate. Three encodings appear:
//
//   83 /7 mod=10 disp32 imm8            7 bytes  ->  7, exactly
//   41 83 /7 mod=10 disp32 imm8         8 bytes  ->  7 + nop
//   83 /7 mod=10 rm=100 SIB disp32 imm8 8 bytes  ->  7 + nop
#define GAME_EDUCATION_SIM 0x5DC

static bool RepointEduSite(const EduSite* s, int* flag)
{
    BYTE* site   = g_exeBase + s->rva;
    int   len    = 0;
    int   dispAt = 0;

    if (site[0] == 0x83 && (site[1] >> 6) == 2 && ((site[1] >> 3) & 7) == 7)
    {
        // 83 modrm [sib] disp32 imm8
        dispAt = ((site[1] & 7) == 4) ? 3 : 2;
        len    = dispAt + 5;
    }
    else if (site[0] == 0x41 && site[1] == 0x83 && (site[2] >> 6) == 2
             && ((site[2] >> 3) & 7) == 7 && (site[2] & 7) != 4)
    {
        // REX.B 83 modrm disp32 imm8
        dispAt = 3;
        len    = 8;
    }
    else
    {
        Logf("easystart  %s (0x%X): %02X %02X %02X is not cmp dword[reg+d32],imm8"
             " - refusing", s->label, s->rva, site[0], site[1], site[2]);
        return false;
    }

    DWORD disp = *(const DWORD*)(site + dispAt);
    BYTE  imm  = site[dispAt + 4];

    if (disp != GAME_EDUCATION_SIM)
    {
        Logf("easystart  %s (0x%X): compares +0x%X, expected +0x%X - refusing",
             s->label, s->rva, disp, GAME_EDUCATION_SIM);
        return false;
    }
    if (imm > 1)
    {
        Logf("easystart  %s (0x%X): immediate %d, expected 0 or 1 - refusing",
             s->label, s->rva, imm);
        return false;
    }

    __int64 rel = (BYTE*)flag - (site + 7);
    if (rel < (__int64)INT_MIN || rel > (__int64)INT_MAX)
    {
        Logf("easystart  %s (0x%X): flag %p out of rel32 range - refusing",
             s->label, s->rva, flag);
        return false;
    }

    BYTE out[8];
    out[0] = 0x83;                       // cmp r/m32, imm8
    out[1] = 0x3D;                       // mod=00 reg=/7 rm=101 -> rip+disp32
    *(int*)(out + 2) = (int)rel;
    out[6] = imm;                        // the site's own immediate, unchanged
    out[7] = 0x90;                       // only written for the 8-byte forms

    if (!WriteCode(site, out, (size_t)len, "education gate")) return false;
    Logf("easystart  edu  %-36s rva 0x%X now reads %p (cmp ...,%d)",
         s->label, s->rva, flag, imm);
    return true;
}

// ---------------------------------------------------------------- the year

static bool Locked(int need) { return g_locked[need] != 0; }

static void SyncYear(BYTE* game)
{
    int year = *(const int*)(game + GAME_YEAR);
    if (year == g_year) return;
    g_year = year;

    g_anyLocked = 0;
    for (int i = 0; i < NEED_COUNT; i++)
    {
        g_locked[i] = (year < g_unlock[i]) ? 1 : 0;
        if (g_locked[i]) g_anyLocked = 1;
    }
    for (int i = 0; i < g_modCount; i++)
        if (year < g_mod[i].year) g_anyLocked = 1;

    if (g_utilities)
        for (int i = 0; i < RATE_COUNT; i++)
            if (g_ratesOk[i])
                WriteRate(i, Locked(kRates[i].need) ? 0.0 : kRates[i].vanilla);

    // 0 is Complex - the mechanic is live; 1 is Simple - it is not.
    if (g_eduFlag)
    {
        g_eduFlag[0] = Locked(NEED_SCHOOL)       ? 1 : 0;
        g_eduFlag[1] = Locked(NEED_KINDERGARTEN) ? 1 : 0;
    }

    char line[512];
    int  n = _snprintf_s(line, sizeof(line), _TRUNCATE,
                         "easystart  year %d, still to come:", year);
    int  any = 0;
    for (int i = 0; i < NEED_COUNT && n > 0 && n < (int)sizeof(line) - 24; i++)
        if (g_locked[i])
        {
            any = 1;
            n += _snprintf_s(line + n, sizeof(line) - n, _TRUNCATE, " %s", kNeeds[i].key);
        }
    for (int i = 0; i < g_modCount && n > 0 && n < (int)sizeof(line) - 56; i++)
        if (year < g_mod[i].year)
        {
            any = 1;
            n += _snprintf_s(line + n, sizeof(line) - n, _TRUNCATE, " %s", g_mod[i].name);
        }

    if (any) Logf("%s", line);
    else     Logf("easystart  year %d, every declared need is live", year);
}

// ---------------------------------------------------------------- demands

// Resolving a mod resource means reading its name out of the record, and
// SafeReadStr costs a VirtualQuery. This runs for every citizen on every tick,
// so the answer is cached: a 16-entry direct-mapped table keyed on the record
// pointer, holding both hits and misses.
#define MODC_SLOTS 16
static struct { const void* res; int mod; } g_modCache[MODC_SLOTS];

static int ModIndexOf(const void* res)
{
    unsigned k = (unsigned)(((ULONG_PTR)res >> 6) & (MODC_SLOTS - 1));
    if (g_modCache[k].res == res) return g_modCache[k].mod;

    int found = -1;
    char name[48];
    if (SafeReadStr((const BYTE*)res + RESOURCE_NAME, name, sizeof(name)))
        for (int i = 0; i < g_modCount; i++)
            if (_stricmp(name, g_mod[i].name) == 0) { found = i; break; }

    g_modCache[k].res = res;
    g_modCache[k].mod = found;
    return found;
}

static int NeedOfDemand(BYTE* game, const BYTE* demand, int* modLocked)
{
    *modLocked = 0;

    switch (*(const int*)(demand + DEMAND_KIND))
    {
    case 3:  return NEED_ALCOHOL;
    case 4:  return NEED_RELIGION;
    case 5:  return NEED_CULTURE;
    case 6:  return NEED_SPORT;
    case 10: return NEED_HEALTH;
    case 1:
    case 2:  break;
    default: return -1;
    }

    const void* res = *(const void* const*)(demand + DEMAND_RES);
    if (!res) return -1;

    if (res == *(const void* const*)(game + GAME_RES_FOOD))    return NEED_FOOD;
    if (res == *(const void* const*)(game + GAME_RES_MEAT))    return NEED_MEAT;
    if (res == *(const void* const*)(game + GAME_RES_CLOTHES)) return NEED_CLOTHES;
    if (res == *(const void* const*)(game + GAME_RES_ELECTR))  return NEED_ELECTRONICS;

    if (g_modCount)
    {
        int i = ModIndexOf(res);
        if (i >= 0)
        {
            *modLocked = (g_year < g_mod[i].year) ? 1 : 0;
            return -2;
        }
    }
    return -1;
}

static bool DemandIsLocked(BYTE* game, const BYTE* demand)
{
    int mod  = 0;
    int need = NeedOfDemand(game, demand, &mod);
    if (need == -2) return mod != 0;
    if (need < 0)   return false;
    return Locked(need);
}

// The planner left whatever the last cycle could not satisfy at +0x4F0, and
// that list is what raises "N Citizen(s) were unable to get X".
static void ClearUnsatisfied(BYTE* game, BYTE* person)
{
    int n = *(const int*)(person + PERSON_UNSAT);
    if (n <= 0 || n > UNSAT_MAX) return;

    int keep = 0;
    for (int i = 0; i < n; i++)
    {
        BYTE* e = person + PERSON_UNSAT_E + (size_t)i * UNSAT_STRIDE;

        // The entry carries the same kind and the same record the demand did,
        // in a shorter form, and only those two fields are read - so a stub is
        // enough to ask the same question.
        BYTE stub[DEMAND_RES + 8];
        memset(stub, 0, sizeof(stub));
        *(int*)(stub + DEMAND_KIND)  = *(const int*)(e + UNSAT_KIND);
        *(void**)(stub + DEMAND_RES) = *(void**)(e + UNSAT_RES);

        if (DemandIsLocked(game, stub)) continue;

        if (keep != i)
            memcpy(person + PERSON_UNSAT_E + (size_t)keep * UNSAT_STRIDE, e, UNSAT_STRIDE);
        keep++;
    }
    if (keep != n) *(int*)(person + PERSON_UNSAT) = keep;
}

static void PinStatuses(BYTE* person)
{
    for (int i = 0; i < NEED_COUNT; i++)
    {
        if (!g_locked[i] || kNeeds[i].status < 0) continue;

        // Every status but alcohol reads "how content is this citizen" - 1.0 is
        // the goal. Alcohol reads "how much has this citizen been drinking" and
        // feeds a health penalty directly, so its own goal while locked is 0,
        // never the configured level.
        float level = (i == NEED_ALCOHOL) ? 0.0f : g_lockedLevel;
        *(float*)(person + PERSON_STATUS + (size_t)kNeeds[i].status * 4) = level;
    }
}

// ---------------------------------------------------------------- the sweep
//
// Both hooks only ever touch a person or a building that the game's own tick
// hands them, and the simulation does not visit its whole population in one
// frame - a large map spreads it over many. Left alone, a citizen or a
// building that already existed when the world loaded shows its pre-plugin
// state (an unpinned, effectively random status float; an unlocked-looking
// warning) until its own turn comes round, which for a big city can take
// noticeably longer than a fresh, just-built one ever does. `probe`'s watch
// list is what caught this: ten citizens converged to a pinned status within
// seconds of each other, but nothing says the *whole* population does.
//
// So the moment a world's first year is known, this walks the two vectors the
// game itself keeps - every person and every building - once, and applies the
// pinned status (not the demand-hiding: that is inherently transient, and the
// citizen's own very next tick already brackets it correctly) and the cleared
// warning counters directly. Nothing here is a hook; it runs once, reads two
// std::vectors the same way `cities` and `resources` already read others, and
// touches only what a real tick would have touched anyway.
static void SweepExistingCitizens(void)
{
    if (!ReadablePtr(g_exeBase + PERSON_VEC_BEGIN, 16)) return;
    void** begin = *(void***)(g_exeBase + PERSON_VEC_BEGIN);
    void** end   = *(void***)(g_exeBase + PERSON_VEC_END);
    if (!begin || !end || end < begin) return;

    int total = (int)(end - begin);
    int done  = 0;
    for (void** it = begin; it != end; it++)
    {
        BYTE* p = (BYTE*)*it;
        if (!p || !ReadablePtr(p, PERSON_SIZE)) continue;

        __try
        {
            PinStatuses(p);
            done++;
        }
        __except (FaultFilter("easystart citizen sweep", GetExceptionInformation()))
        {
        }
    }
    Logf("easystart  sweep: %d of %d existing citizen(s) had a locked status"
         " pinned immediately, rather than waiting for their own next tick",
         done, total);
}

static void SweepExistingBuildings(void* game)
{
    BYTE* g = (BYTE*)game;
    if (!ReadablePtr(g + GAME_BUILDING_VEC, 16)) return;
    void** begin = *(void***)(g + GAME_BUILDING_VEC);
    void** end   = *(void***)(g + GAME_BUILDING_VEC + 8);
    if (!begin || !end || end < begin) return;

    int total = (int)(end - begin);
    int done  = 0;
    for (void** it = begin; it != end; it++)
    {
        BYTE* b = (BYTE*)*it;
        if (!b || !ReadablePtr(b, BLD_OUT_WATER + 4)) continue;

        __try
        {
            if (BuildingTypeOf(b) == BUILDINGTYPE_LIVING)
            {
                if (Locked(NEED_ELECTRICITY))
                {
                    *(float*)(b + BLD_OUT_POWER_A) = 0.0f;
                    *(float*)(b + BLD_OUT_POWER_B) = 0.0f;
                    *(float*)(b + BLD_OUT_POWER_C) = 0.0f;
                }
                if (Locked(NEED_WATER))
                    *(float*)(b + BLD_OUT_WATER) = 0.0f;
                done++;
            }
        }
        __except (FaultFilter("easystart building sweep", GetExceptionInformation()))
        {
        }
    }
    Logf("easystart  sweep: %d of %d building(s) are living quarters, and had"
         " their power/water counters cleared immediately", done, total);
}

// Diagnostic only: how many times the evaluator ran at all, and how many
// distinct buildings it ran for. `person+0x20` is the building the person is
// in - see 02-findings.md - and a heartbeat every few seconds is what tells
// "the hook never fires for old buildings" from "nothing has happened yet",
// which log inspection alone could not settle.
#define SEEN_BUILDINGS_MAX 256
static void*    g_seenBuildings[SEEN_BUILDINGS_MAX];
static int      g_seenBuildingCount = 0;
static unsigned g_evalCalls   = 0;
static unsigned g_lockedCalls = 0;
static ULONGLONG g_heartbeatAt = 0;

static void NoteBuilding(void* building)
{
    if (!building || g_seenBuildingCount >= SEEN_BUILDINGS_MAX) return;
    for (int i = 0; i < g_seenBuildingCount; i++)
        if (g_seenBuildings[i] == building) return;
    g_seenBuildings[g_seenBuildingCount++] = building;
}

// A fixed handful of citizens, captured the first time each is seen and then
// re-read live on every heartbeat - not the "saved" snapshot ProbeSample takes,
// the actual current person+0x118 - so the SAME citizen can be watched across
// minutes of play. This is what tells "the demand list is stale from the save
// and has not been replanned yet" from "the gate is stuck", because it shows
// whether the list ever changes shape at all, independent of locking.
#define WATCH_MAX 10
static void* g_watchPerson[WATCH_MAX];
static void* g_watchBuilding[WATCH_MAX];
static int   g_watchCount = 0;

static void NoteWatch(void* person, void* building)
{
    if (g_watchCount >= WATCH_MAX) return;
    for (int i = 0; i < g_watchCount; i++)
        if (g_watchPerson[i] == person) return;
    g_watchPerson[g_watchCount]   = person;
    g_watchBuilding[g_watchCount] = building;
    g_watchCount++;
}

static void DumpWatch(void* game)
{
    for (int i = 0; i < g_watchCount; i++)
    {
        BYTE* p = (BYTE*)g_watchPerson[i];
        if (!ReadablePtr(p, PERSON_SIZE))
        {
            Logf("easystart  probe  watch[%d] %p is no longer readable", i, p);
            continue;
        }

        int  n    = *(const int*)(p + PERSON_DCOUNT);
        char buf[192];
        buf[0]    = 0;    // n==0 leaves the loop below empty; %s must not read garbage
        int  off  = 0;
        int  show = (n < 0) ? 0 : (n > 3 ? 3 : n);
        for (int k = 0; k < show && off < (int)sizeof(buf) - 24; k++)
        {
            const BYTE* d    = p + PERSON_DEMANDS + (size_t)k * DEMAND_STRIDE;
            int         kind = *(const int*)(d + DEMAND_KIND);
            int         mod  = 0;
            int         need = NeedOfDemand((BYTE*)game, d, &mod);

            // need<0 on a shop-good kind (1 or 2) means the resource pointer
            // did not match game+0xC300.."0x320 - print its own name instead
            // of guessing whether that is really food misclassified or a
            // genuinely different good.
            char nm[24] = "";
            if (need < 0 && (kind == 1 || kind == 2))
            {
                const void* res = *(const void* const*)(d + DEMAND_RES);
                if (res) SafeReadStr((const BYTE*)res + RESOURCE_NAME, nm, sizeof(nm));
            }
            off += _snprintf_s(buf + off, sizeof(buf) - off, _TRUNCATE,
                               " [%d]kind%d/need%d%s%s", k, kind, need,
                               nm[0] ? "=" : "", nm);
        }
        const float* s = (const float*)(p + PERSON_STATUS);
        Logf("easystart  probe  watch[%d] %p (building %p): %d demand(s)%s "
             " food %.2f clothing %.2f alcohol %.2f culture %.2f",
             i, p, g_watchBuilding[i], n, buf, s[1], s[8], s[4], s[5]);

        // Diagnostic for "food never arrives for a pre-existing citizen": is the
        // status float genuinely frozen bit-for-bit, or drifting below what
        // %.2f shows? And does this citizen's building read as a living
        // building at all - through the type descriptor, the way BuildingTypeOf
        // now reads it, not straight off the building the way this line used to.
        int buildingType = BuildingTypeOf((const BYTE*)g_watchBuilding[i]);

        int unsatN = *(const int*)(p + PERSON_UNSAT);
        char ubuf[128];
        int  uoff = 0;
        ubuf[0] = 0;
        if (unsatN > 0 && unsatN <= UNSAT_MAX)
            for (int u = 0; u < unsatN && uoff < (int)sizeof(ubuf) - 16; u++)
                uoff += _snprintf_s(ubuf + uoff, sizeof(ubuf) - uoff, _TRUNCATE,
                                     " kind%d", *(const int*)(p + PERSON_UNSAT_E +
                                     (size_t)u * UNSAT_STRIDE + UNSAT_KIND));

        Logf("easystart  probe  watch[%d]   food %.8f (raw bits %08X)  building type %d"
             "  unsatisfied %d(%s)  money-spent %.2f",
             i, s[1], *(const DWORD*)(s + 1), buildingType, unsatN, ubuf,
             *(const float*)(p + PERSON_MONEY_SPENT));
    }
}

static void Heartbeat(void* game, int year)
{
    ULONGLONG now = GetTickCount64();
    if (g_heartbeatAt && now - g_heartbeatAt < 5000) return;
    g_heartbeatAt = now;

    Logf("easystart  probe  heartbeat: %u evaluate call(s) so far, %u with"
         " something locked, %d distinct building(s) seen, year %d",
         g_evalCalls, g_lockedCalls, g_seenBuildingCount, year);
    DumpWatch(game);
}

// Up to eight distinct citizens, not just the first, and the building they are
// in - so a citizen from a pre-existing house can be told apart from one in a
// freshly built one instead of guessing from a single sample.
#define PROBE_SAMPLES 8
static void ProbeSample(void* game, BYTE* person, const BYTE* saved, int before, int after)
{
    static int done = 0;
    if (done >= PROBE_SAMPLES) return;
    done++;

    void* building = *(void* const*)(person + 0x20);
    Logf("easystart  probe  person %p (building %p): %d demand(s), %d hidden,"
         " %d left for the evaluator, year %d",
         person, building, before, before - after, after, *(const int*)((BYTE*)game + GAME_YEAR));
    for (int i = 0; i < before; i++)
    {
        const BYTE* d = saved + (size_t)i * DEMAND_STRIDE;
        int   kind = *(const int*)(d + DEMAND_KIND);
        int   mod  = 0;
        int   need = NeedOfDemand((BYTE*)game, d, &mod);
        Logf("easystart  probe    [%d] kind %-3d res %p  need %d  %.5f of %.5f", i,
             kind, *(const void* const*)(d + DEMAND_RES), need,
             *(const float*)d, *(const float*)(d + 4));
    }
    const float* s = (const float*)(person + PERSON_STATUS);
    Logf("easystart  probe    status happy %.2f food %.2f health %.2f soviet %.2f"
         " alcohol %.2f culture %.2f sport %.2f religion %.2f clothing %.2f"
         " electronic %.2f",
         s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7], s[8], s[9]);
}

// ---------------------------------------------------------------- the hooks

static void h_Evaluate(void* game, void* person)
{
    BYTE* g = (BYTE*)game;
    BYTE* p = (BYTE*)person;

    BYTE saved[DEMAND_MAX * DEMAND_STRIDE];

    // volatile because it is written inside a __try and read after it: a
    // non-volatile local modified in a guarded block has an indeterminate value
    // once the handler has run, and the restore below is exactly that case.
    volatile int savedCount = -1;

    __try
    {
        SyncYear(g);

        // Once per world load, not once per process: g_year is only ever -1
        // before the first real year is read, and SyncYear has just set it.
        if (!g_swept)
        {
            g_swept = 1;
            if (g_pinStatus) SweepExistingCitizens();
            if (g_counters)  SweepExistingBuildings(g);
        }

        g_evalCalls++;
        if (g_probe)
        {
            void* building = *(void* const*)(p + 0x20);
            NoteBuilding(building);
            NoteWatch(p, building);
            Heartbeat(g, g_year);
        }

        if (g_demands && g_anyLocked)
        {
            int n = *(const int*)(p + PERSON_DCOUNT);
            if (n > 0 && n <= DEMAND_MAX)
            {
                // Scan before copying: most citizens carry nothing locked, and
                // this runs for every person on every tick.
                int locked = 0;
                for (int i = 0; i < n; i++)
                    if (DemandIsLocked(g, p + PERSON_DEMANDS + (size_t)i * DEMAND_STRIDE))
                    { locked = 1; break; }

                if (locked)
                {
                    g_lockedCalls++;
                    memcpy(saved, p + PERSON_DEMANDS, (size_t)n * DEMAND_STRIDE);
                    savedCount = n;      // set before the edit, so a fault still restores

                    int keep = 0;
                    for (int i = 0; i < n; i++)
                    {
                        const BYTE* d = saved + (size_t)i * DEMAND_STRIDE;
                        if (DemandIsLocked(g, d)) continue;
                        if (keep != i)
                            memcpy(p + PERSON_DEMANDS + (size_t)keep * DEMAND_STRIDE,
                                   d, DEMAND_STRIDE);
                        keep++;
                    }
                    *(int*)(p + PERSON_DCOUNT) = keep;
                    if (g_probe) ProbeSample(g, p, saved, n, keep);
                }
            }

            if (g_clearUnsat) ClearUnsatisfied(g, p);
        }
    }
    __except (FaultFilter("easystart demand gate", GetExceptionInformation()))
    {
    }

    o_Evaluate(game, person);

    __try
    {
        if (savedCount >= 0)
        {
            memcpy(p + PERSON_DEMANDS, saved, (size_t)savedCount * DEMAND_STRIDE);
            *(int*)(p + PERSON_DCOUNT) = savedCount;
        }
        if (g_pinStatus && g_anyLocked) PinStatuses(p);
    }
    __except (FaultFilter("easystart demand restore", GetExceptionInformation()))
    {
    }
}

// The four figures the living building's window prints as "N Citizen(s)
// currently without ...". The tick snapshots them at the top of every pass and
// never reads them back, so clearing them here is invisible to the simulation.
static char h_LivingTick(void* game, void* building, char flag)
{
    char r = o_LivingTick(game, building, flag);

    __try
    {
        if (g_counters && g_anyLocked)
        {
            BYTE* b = (BYTE*)building;
            if (Locked(NEED_ELECTRICITY))
            {
                *(float*)(b + BLD_OUT_POWER_A) = 0.0f;
                *(float*)(b + BLD_OUT_POWER_B) = 0.0f;
                *(float*)(b + BLD_OUT_POWER_C) = 0.0f;
            }
            if (Locked(NEED_WATER))
                *(float*)(b + BLD_OUT_WATER) = 0.0f;
        }
    }
    __except (FaultFilter("easystart living counters", GetExceptionInformation()))
    {
    }
    return r;
}

// "Building is without power supply", and only in the panel that asks here.
// The panel does `call HasPower; test al,al; jne <skip the warning>` - nonzero
// means "has power". While electricity is locked every building must read as
// powered, so the return here is 1, not 0 - returning 0 does the opposite of
// what this hook exists for and forces the warning on instead of off.
static char h_PanelHasPower(void* game, void* building)
{
    if (g_counters && Locked(NEED_ELECTRICITY)) return 1;
    return g_HasPower(game, building);
}

// ---------------------------------------------------------------- settings

static int ParseYear(const char* v, int fallback)
{
    char t[64];
    _snprintf_s(t, sizeof(t), _TRUNCATE, "%s", v);
    Trim(t);
    if (!t[0]) return fallback;
    if (_stricmp(t, "off") == 0 || _stricmp(t, "never") == 0) return YEAR_NEVER;
    if (_stricmp(t, "always") == 0 || _stricmp(t, "on") == 0) return YEAR_ALWAYS;
    int y = atoi(t);
    return (y < 0) ? fallback : y;
}

static void ReadSettings()
{
    const char* ini = "plugins\\easystart.ini";
    char v[256];

    g_enabled    = H->configInt(ini, "easystart", "enabled",           g_enabled);
    g_demands    = H->configInt(ini, "easystart", "demands",           g_demands);
    g_utilities  = H->configInt(ini, "easystart", "utilities",         g_utilities);
    g_counters   = H->configInt(ini, "easystart", "warnings",          g_counters);
    g_pinStatus  = H->configInt(ini, "easystart", "pin_status",        g_pinStatus);
    g_clearUnsat = H->configInt(ini, "easystart", "clear_unsatisfied", g_clearUnsat);
    g_probe      = H->configInt(ini, "easystart", "probe",             g_probe);

    if (H->configString(ini, "easystart", "locked_status", v, sizeof(v), "") && v[0])
        g_lockedLevel = (float)atof(v);
    if (g_lockedLevel < 0.0f) g_lockedLevel = 0.0f;
    if (g_lockedLevel > 1.0f) g_lockedLevel = 1.0f;

    for (int i = 0; i < NEED_COUNT; i++)
    {
        char def[32];
        if (kNeeds[i].year == YEAR_NEVER) strcpy_s(def, sizeof(def), "off");
        else _snprintf_s(def, sizeof(def), _TRUNCATE, "%d", kNeeds[i].year);

        H->configString(ini, "unlock", kNeeds[i].key, v, sizeof(v), def);
        g_unlock[i] = ParseYear(v, kNeeds[i].year);
    }

    if (H->configString(ini, "unlock_resource", "list", v, sizeof(v), "") && v[0])
    {
        char* ctx = 0;
        for (char* t = strtok_s(v, ",", &ctx); t && g_modCount < MOD_MAX;
             t = strtok_s(0, ",", &ctx))
        {
            Trim(t);
            if (!t[0]) continue;
            char year[32];
            H->configString(ini, "unlock_resource", t, year, sizeof(year), "off");
            _snprintf_s(g_mod[g_modCount].name, sizeof(g_mod[0].name), _TRUNCATE, "%s", t);
            g_mod[g_modCount].year = ParseYear(year, YEAR_NEVER);
            g_modCount++;
        }
    }
}

// Rewrites the panel's own `call rel32` at the one site that draws "Building is
// without power supply". A rel32 cannot reach this DLL, so it is pointed at a
// 14-byte absolute jump in the cave instead.
static bool RedirectPowerCall(void)
{
    BYTE* site = g_exeBase + RVA_POWER_CALL;
    if (site[0] != 0xE8)
    {
        Logf("easystart  power warning: rva 0x%X is %02X, not a call - refusing",
             RVA_POWER_CALL, site[0]);
        return false;
    }

    DWORD target = (DWORD)(RVA_POWER_CALL + 5 + *(const int*)(site + 1));
    if (target != RVA_HAS_POWER)
    {
        Logf("easystart  power warning: rva 0x%X calls 0x%X, expected 0x%X - refusing",
             RVA_POWER_CALL, target, RVA_HAS_POWER);
        return false;
    }
    if (memcmp(g_exeBase + RVA_HAS_POWER, kHasPowerHead, sizeof(kHasPowerHead)) != 0)
    {
        Logf("easystart  power warning: 0x%X is not the query this build has"
             " - refusing", RVA_HAS_POWER);
        return false;
    }

    BYTE* stub = AllocNear(g_exeBase, 32);
    if (!stub)
    {
        Logf("easystart  power warning: no cave within reach - refusing");
        return false;
    }
    BYTE jmp[14] = { 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00 };
    void* fn = (void*)h_PanelHasPower;
    memcpy(jmp + 6, &fn, 8);
    memcpy(stub, jmp, sizeof(jmp));

    __int64 rel = stub - (site + 5);
    if (rel < (__int64)INT_MIN || rel > (__int64)INT_MAX)
    {
        Logf("easystart  power warning: stub %p out of rel32 range - refusing", stub);
        return false;
    }

    g_HasPower = (t_HasPower)(g_exeBase + RVA_HAS_POWER);
    int disp = (int)rel;
    if (!WriteCode(site + 1, &disp, sizeof(disp), "power warning")) return false;

    Logf("easystart  power warning: the panel's call at 0x%X now goes through %p",
         RVA_POWER_CALL, stub);
    return true;
}

// ---------------------------------------------------------------- lifecycle

extern "C" __declspec(dllexport) unsigned TsmPluginApiVersion(void)
{
    return TSM_API_VERSION;
}

extern "C" __declspec(dllexport) int TsmPluginInit(const TsmHost* host, TsmPluginInfo* info)
{
    TsmBind(host);
    info->name    = "easystart";
    info->version = "1.2";

    ReadSettings();
    if (!g_enabled)
    {
        Logf("easystart  disabled");
        return 1;
    }
    return 0;
}

extern "C" __declspec(dllexport) int TsmPluginStart(void)
{
    size_t bytes = 16 + (size_t)RATE_COUNT * 8 + 16;
    g_slots = AllocNear(g_exeBase, bytes);
    if (!g_slots)
    {
        Logf("easystart  no allocation within reach of the executable - inactive");
        return 1;
    }
    memset(g_slots, 0, bytes);
    g_eduFlag = EduSlot(0);
    g_eduFlag[0] = 0;                    // Complex until the first tick decides
    g_eduFlag[1] = 0;

    int rates = 0;
    if (g_utilities)
        for (int i = 0; i < RATE_COUNT; i++)
            if ((g_ratesOk[i] = RepointRate(i) ? 1 : 0) != 0) rates++;

    int edu = 0, eduWanted = 0;
    for (int i = 0; i < EDU_SITE_COUNT; i++)
    {
        if (g_unlock[kEduSites[i].need] == YEAR_NEVER) continue;
        eduWanted++;
        if (RepointEduSite(&kEduSites[i], EduSlot(kEduSites[i].need == NEED_SCHOOL ? 0 : 1)))
            edu++;
    }
    if (eduWanted)
        Logf("easystart  education: %d of %d site(s) repointed%s", edu, eduWanted,
             edu == eduWanted ? "" : " - PARTIAL, the simulation may disagree with itself");

    bool hooked = InstallInlineHook(g_exeBase + RVA_EVALUATE, (void*)h_Evaluate,
                                   (void**)&o_Evaluate, kEvaluatePrologue,
                                   sizeof(kEvaluatePrologue), "citizen need evaluation");
    if (!hooked)
        Logf("easystart  the evaluator hook did not install - the nine demand needs"
             " are NOT gated");

    bool ticked = false;
    if (g_counters)
    {
        ticked = InstallInlineHook(g_exeBase + RVA_LIVING_TICK, (void*)h_LivingTick,
                                  (void**)&o_LivingTick, kLivingPrologue,
                                  sizeof(kLivingPrologue), "living building tick");
        if (!ticked)
            Logf("easystart  the living tick hook did not install - the panel will"
                 " still count citizens without power and water");
        if (!RedirectPowerCall())
            Logf("easystart  \"Building is without power supply\" is left as it was");
    }

    if (!hooked && !rates && !edu && !ticked)
    {
        Logf("easystart  nothing could be installed - inactive");
        return 1;
    }

    Logf("easystart  ready: demand gate %s, living counters %s, %d of %d utility"
         " rate(s), locked status held at %.2f",
         hooked ? "on" : "OFF", ticked ? "on" : "off", rates, RATE_COUNT, g_lockedLevel);

    for (int i = 0; i < NEED_COUNT; i++)
    {
        if (g_unlock[i] == YEAR_NEVER)
            Logf("easystart    %-13s off - never gated", kNeeds[i].key);
        else if (g_unlock[i] == YEAR_ALWAYS)
            Logf("easystart    %-13s live from the start", kNeeds[i].key);
        else
            Logf("easystart    %-13s from %d", kNeeds[i].key, g_unlock[i]);
    }
    for (int i = 0; i < g_modCount; i++)
        Logf("easystart    %-13s from %d (mod resource)", g_mod[i].name, g_mod[i].year);

    return 0;
}

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) { return TRUE; }
