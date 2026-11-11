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
//   rva 0x2A92D0  the price pass. Walks the vector and writes every record's
//                 price from the production chains. Bracketed, so [base_price]
//                 lands before it and [price] after it.
//   rva 0x185470  the customhouse's own tick, building type 20. Post-hooked so
//                 an already-standing customhouse picks up a resource this
//                 plugin declared after it was built - see CUSTOMS below.
//
// A new record is a clone of a template's, with its own name, caption id and
// cargo meshes. Only the icon is found by name - every mesh path in the base
// game is a literal, which is why AttachResourceMeshes makes the same three
// engine calls the game's own resource table makes.
//
// CUSTOMS. A customhouse's trade storages are plain $STORAGE lines, one per
// transport class, and the game builds that slot list exactly once - while
// building.ini is parsed - by walking whatever this vector looks like at that
// moment. A customhouse already standing (or built in an earlier session)
// therefore freezes its trade list at whatever resources existed then: every
// name [list] declares afterwards is otherwise perfectly tradeable - matching
// transport class, a computed price, trucks that will deliver it - but has no
// slot to be sold from. See the ExtendCustomsStorage block below, which reruns
// that same walk against the live vector once per customhouse.
//
// Everything here is addresses for SOVIET64.exe v1.1.1.9. See
// docs/04-adding-resources.md.

#include "../../src/tesmio_plugin.h"

// The three calls the engine's own resource table makes to give a record its
// cargo geometry. Used to give a mod resource meshes of its own instead of the
// template's - see AttachResourceMeshes.
#define SYM_CREATE_MESH "?CreateManagedMesh@C3D_MIDDLEPOINT@@QEAAPEAVC3D_MESH@@PEBD@Z"
#define SYM_MESH_LOAD   "?LoadFromFile@C3D_MESH@@QEAAHPEBDPEAVC3D_MIDDLEPOINT@@_N@Z"
#define SYM_MESH_MTL    "?LoadMaterial@C3D_MESH@@QEAAHPEBDH@Z"
#define SYM_GET_STRING  "?GetString@C3D_LANGUAGE@@QEAAPEA_WH@Z"

// Asked before any of the three above is called with a path. The import slot
// already carries the loader's own VFS-aware hook, so a file that exists only
// under tesmioloader\vfs answers yes. Paths are relative to media_soviet, the
// same form CreateManagedMesh takes.
#define SYM_FILE_EXISTS "?C3DHelp_CheckIfFileExist@@YA_NPEBD_N1@Z"

// C3D_MIDDLEPOINT, the object every managed asset is created through.
#define P_MIDDLEPOINT   0x9EACD0

// SOVIET64.exe v1.1.1.9. Verified against the prologue below before hooking, so
// a game update makes the hook refuse to install rather than corrupt the process.
// Was 0x2AA7C0 in v1.1.1.7; the prologue found it again 0x70 further on.
#define DEFAULT_RESOURCEGET_RVA 0x2AA7B0   // v1.1.1.13; was 0x2AA830

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

// The engine's own cache of hand-picked records, and the reason relocation used
// to look unsafe.
//
// ResourceGet is `(gameObject, name)` and reads its array out of `self+0xC2B0`
// - which *is* this vector. The game object is the static at rva 0x9D4F10 and
// 0x9D4F10 + 0xC2B0 == 0x9E11C0, so the vector object is a field of it and
// everything else here is expressed relative to the vector rather than to a
// second hard-coded address.
//
// Immediately behind those three pointers, at `self+0xC2C8`..`+0xC488`, sit 57
// qwords the engine fills at rva 0x2A82B0 with one ResourceGet per name - the
// records it wants by hand instead of by lookup. Fifty-three of them point into
// the array; the first is `workers`, index 0, so it is bit-for-bit equal to
// `begin`. **That is the "second structure holding the array base"
// 07-pitfalls.md warned about** - not a container, a cached record that a
// memory scan cannot tell from the base pointer. The other four -
// `self+0xC3B8`, `+0xC3C0`, `+0xC418`, `+0xC420` - are the standalone records
// ResourceGet compares against before it scans anything, and never point into
// the array at all.
//
// Ordering already keeps this consistent: the cache is filled through our own
// hook and EnsureArmed runs before the original, so the very first lookup the
// engine makes already returns a pointer into the new buffer. Carrying the
// block across anyway costs one loop and removes the argument.
#define RES_CACHE_OFF   0x18                  // from the vector object
#define RES_CACHE_COUNT 57

// Floor on how many records the array should have room for, from
// `resource_capacity`. 0 means "whatever [list] needs" - the plugin sizes the
// array itself and relocates when the engine's own 63 are not enough. A
// negative value pins the array to the engine's allocation and refuses to move
// it, which is the behaviour every version before this one had.
static int g_wantCapacity = 0;

// Set once the array in front of us is big enough, so a failed relocation is
// reported once rather than on every lookup.
static bool g_warnedCapacity;

// Offset of the localisation id inside a record, found by diffing the records
// of workers/coal/rawiron/alcohol - the only field that differed and stayed
// stable across runs (518 / 508 / 524 / 512).
#define RES_TEXTID_OFF 0x40

// ---------------------------------------------------------------- money
//
// **Four floats and one integer, and between them they are the whole of what
// the trade table shows.** Read off the engine's own resource table at
// 0x2A1D60, which builds every record in a stack buffer whose rbp is
// `record + 0xC0`, and confirmed field by field against the `$Economy_*`
// blocks a save writes into `media_soviet/save/<name>/stats.ini`.
//
//   +0x44  the price *kind*, and the first thing the pass below branches on:
//          -1 workers, -2 eletric, -3 vehicles, -4 trains, -5 heat, and for
//          everything else 0 raw, 1 manufactured, 2 consumer good.
//   +0x58  price in RUB   +0x5C  price in USD
//   +0x78  base in RUB    +0x7C  base in USD
//
// The *price* is what the interface divides and multiplies: the trade window's
// buy figure is the price times the record's own 1.05 at +0x8C and its sell
// figure the same price times 0.95 at +0x88. Both come from one number per
// currency, which is why forcing that number is enough to move every screen
// that quotes a resource. It is not a constant - `PriceRecompute` below
// overwrites all of them from the production chains, and the values the
// resource table ships are only what a resource is worth before any building
// type has been read.
//
// The *base* is the `$Economy_Base*` a save carries, and it is an input to
// that solver rather than a result: only fifteen resources in the base game
// have one, and they are the ones that come out of the ground or cannot be
// produced at all - `rawiron` 4.5/4.5, `uranium` 5.2/4.2, `explosives`
// 15/13, `water` 0.3/0.2. Which of the two floats is which currency was
// settled on those three, because they are the only ones whose pair differs.
//
// **A resource no building type produces prices at zero**, whatever its base
// is: the solver at 0x2A9470 walks the building types looking for one whose
// output is this resource and returns the register it zeroed on the way in
// when it finds none. That is the whole of the reported symptom - copper is
// mined, concentrated, smelted and refined, so all four price themselves,
// while a name declared in `[list]` and used by nothing at all cannot.
#define RES_KIND_OFF   0x44
#define RES_PRICE_RUB  0x58
#define RES_PRICE_USD  0x5C
#define RES_BASE_RUB   0x78
#define RES_BASE_USD   0x7C

// The resource's icon texture, filled by the UI pass at 0x2960DE from
// "resources/<name>.png" - the one asset the engine finds by name.
#define RES_ICON_OFF   0x48

// ---------------------------------------------------------------- the rest of the record
//
// **The whole 832-byte record, and not one byte of it guessed.** The engine's
// resource table at 0x2A1D60 builds each record in one stack buffer whose rbp is
// `record + 0xC0` and commits it with `add qword ptr [rbx+8],0x340` (records
// 0..34) or `call 0x449350(vector, record)` (35..56). Reading that code back and
// replaying every write through rbp gives the exact contents of all 57 base-game
// records, which is how the map below was made rather than by diffing live
// memory - see tools/pe/restable.py, which prints it.
//
// Only 30 of the 832 bytes are ever written per resource. Everything else is
// zero at push time and filled at runtime: the icon at +0x48, the previous
// price at +0x70/+0x74, the previous base at +0x80/+0x84 and the five mesh
// pointers from +0x318. **That is what makes a record with no template
// possible** - a from-scratch record is a zeroed one plus this list.
//
//   +0x00  name, inline, 32 bytes         +0x40  caption id     +0x44  price kind
//   +0x50  \ set on nine records only: nuclearfuelburned 127, waste_toxic 84,
//   +0x54  / usagewater 1, the four landfill wastes 0.3..1. Reader not found.
//   +0x58/+0x5C  price RUB/USD            +0x78/+0x7C  base price RUB/USD
//   +0x88 and +0xA8  two identical 0x20-byte market blocks, below
//   +0xC8  one byte, below
//   +0xCC  eighteen 0x20-byte transport-class blocks, below
//   +0x30C the material family, below     +0x310 0.3 everywhere but workers
//                                                and eletric, which are 1.0
//
// **Two market blocks, byte-identical in shape**, at +0x88 and +0xA8:
//
//   +0x00 sell multiplier, 0.95     +0x04 buy multiplier, 1.05
//   +0x10, +0x14  two large figures - eletric 40000/60000, coal 9500/9500,
//                 iron 6500/7500, and 200/350 (first block) with 500/500
//                 (second) on twelve resources including every waste
//   +0x18 0.5, or 0.65 on coal, 0.75 on chemicals/bitumen, 0.85 on the liquids
//   +0x1C 1.0 - the only field in the whole record that is the same in all 57
//
// The pair at +0x88/+0x8C is the one 02-findings.md already identified: the
// trade window quotes the price times +0x8C to buy and times +0x88 to sell. The
// second block carries the same two multipliers and a different pair of large
// figures, so **which block is which currency is not established here** - the
// naming below follows the order of the price pair at +0x58/+0x5C and nothing
// stronger. Three resources are untradeable - workers, vehicles, trains - and
// they are exactly the three whose blocks are entirely zero but for +0xA4/+0xC4.
#define RES_MARKET_RUB    0x88
#define RES_MARKET_USD    0xA8
#define RES_MARKET_SELL   0x00
#define RES_MARKET_BUY    0x04
#define RES_MARKET_LIM1   0x10
#define RES_MARKET_LIM2   0x14
#define RES_MARKET_K      0x18
#define RES_MARKET_ONE    0x1C

// A byte, and its distribution is exact: 1 on plants, chemicals, uf6,
// nuclearfuel, nuclearfuelburned, fabric, alcohol, food, clothes, meat,
// ecomponents, mcomponents, plastics, eletronics and explosives, 0 on the other
// 42. **Those fifteen are precisely the resources whose primary transport class
// is COVERED, COOLER, NUCLEAR1 or NUCLEAR2**, and they are also the fifteen with
// no cargo geometry of their own in media_soviet/resources - the goods a lorry
// carries as a crate or a container. The rule reproduces all 57 records with no
// exception, which is why `packed = auto` below can derive it; what reads the
// byte has not been established.
#define RES_PACKED_OFF    0xC8

// Eighteen transport-class blocks, one per RESOURCE_TRANSPORT_* in
// media_soviet/scripts/SOVIETInstructions.txt (COVERED 0 .. WASTE 17,
// RESOURCE_TRANSPORT_NUM 18), 0x20 bytes each, +0xCC..+0x30C.
//
// The count is not inferred from the enum: the table's own prologue zeroes the
// array with `mov ecx,0x12` - eighteen - and 0x2A1B80, which every resource
// block calls before writing its classes, clears exactly eighteen blocks of
// [+0x00,+0x14) plus the flag at +0x1C. A resource declares one class, sometimes
// two, and the rest stay zero.
//
//   +0x00  capacity factor. Proven: the building.ini parser multiplies a
//          $STORAGE capacity by it (`mulss xmm0,[rax+rbx+0xCC]` at 0x117B91),
//          which is why a storage whose class differs from the resource's own
//          reports 0.00 of 0.00 t.
//   +0x04  \ two figures the base game sets per class rather than per resource:
//   +0x08  / GRAVEL 0.5/30 on nine of thirteen, OPEN 1.25/2.5, OIL and WATER
//          5/5, CONCRETE 5/20, ELETRIC and HEATING 1/1. What reads them has not
//          been established, so kTransportClasses ships the base game's own
//          numbers per class and they are only worth overriding deliberately.
//   +0x0C  \ 0.05 on exactly the classes that move a liquid through a pipe -
//   +0x10  / OIL, WATER, SEWAGE - and zero on the other fifteen.
//   +0x1C  a byte. Set on cement/alumina/meat GENERAL and livestock COVERED and
//          GENERAL, always on a *second* class and never on a primary one. The
//          customhouse sync below reads it as "not tradeable in this class",
//          which is the one behaviour that depends on it.
#define RES_CLASS_BASE    0xCC
#define RES_CLASS_STRIDE  0x20
#define RES_CLASS_COUNT   18
#define RES_CLASS_FACTOR  0xCC   // == RES_CLASS_BASE + 0x00, the name the customs code uses
#define RES_CLASS_F1      0x04
#define RES_CLASS_F2      0x08
#define RES_CLASS_F3      0x0C
#define RES_CLASS_F4      0x10
#define RES_CLASS_FLAG    0x1C
#define RES_CLASS_BLOCKED 0xE8   // == RES_CLASS_BASE + RES_CLASS_FLAG

// An int, -1 on workers, eletric, heat, water and usagewater - the five with no
// material form - and 10..19 on the other 52. **Each of the ten values holds
// exactly one waste resource**: 10 waste_gravel with gravel, bricks, cement,
// asphalt and concrete; 11 waste_steel with steel, iron and mcomponents; 12
// waste_aluminium with the bauxite chain; 13 waste_plastic with plastics and
// electronics; 14 waste_bio with plants; 15 fertiliser with food, meat and
// livestock; 16 waste_burnable with wood and boards; 17 waste_toxic with the
// chemicals, fuels and nuclear chain; 18 waste_other with fabric, clothes,
// vehicles and trains; 19 waste_ash with coal. So it is a material family and
// the sorted-waste mapping is the obvious thing it would be for, but what reads
// it has not been established - hence -1, a value five base records carry, as
// the default.
#define RES_FAMILY_OFF    0x30C
#define RES_FAMILY_FIRST  10

// 1.0 on workers and eletric, 0.3 on the other 55, including heat and water.
// Unidentified.
#define RES_TAIL_OFF      0x310

// C3D_GAME::RecomputeResourcePrices(game). Loops the whole resource vector and
// writes +0x58 and +0x5C for every record, skipping kind -1 and pinning kind
// -5 to 1.0. Called from world init at 0x28ED78 and from the economy module at
// 0x2FBB95, each time followed by 0x2A9F40.
// v1.1.1.9; was 0x2A92D0. Confirmed by what it reads rather than by the shift:
// the function still walks the resource vector through self+0xC2B0/+0xC2B8.
#define DEFAULT_PRICEPASS_RVA 0x2A92C0   // v1.1.1.13; was 0x2A9340

static const BYTE kPricePassPrologue[] = {
    0x48, 0x89, 0x6C, 0x24, 0x18,   // mov  [rsp+18h], rbp
    0x57,                           // push rdi
    0x41, 0x56,                     // push r14
    0x41, 0x57,                     // push r15
    0x48, 0x83, 0xEC, 0x30          // sub  rsp, 30h
};                                  // exactly 14, the minimum the host allows

static DWORD g_priceRva = DEFAULT_PRICEPASS_RVA;

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
    int     tmplIndex;   // -1 when there is no template at all
    wchar_t display[64];
    int     textId;
    bool    armed;
    bool    custom;      // built from a zeroed record rather than cloned
};

// How many names [list] may declare. Nothing in the engine chooses this number
// - the array is grown to whatever is asked for - so it is only the size of the
// registry this plugin keeps, about 220 bytes an entry. It is deliberately far
// past anything that would be sensible content: what actually runs out first is
// the game's own tolerance for resources it was never built with, and that
// shows up when a modded good is *used* rather than when it is declared. See
// the purchase bucket in docs/07-pitfalls.md.
#define RES_MAX_ENTRIES 256

static ResEntry g_reg[RES_MAX_ENTRIES];
static int      g_regCount;
static bool     g_regOverflow;

// One resource's money, from `[base_price]` and `[price]`. Keyed by name and
// **not by [list] index**, so the same two sections can retune a base-game
// resource; nothing in what follows cares whether a record is ours.
struct PriceEntry
{
    char  name[64];
    float baseRub, baseUsd;
    float priceRub, priceUsd;
    bool  hasBase, hasPrice;
};

static PriceEntry g_price[RES_MAX_ENTRIES];
static int        g_priceCount;
static int        g_priceHook = 1;      // 0 off, 1 install the bracket
static int        g_priceReport = 1;    // log the table after every recompute

static PriceEntry* PriceEntryFor(const char* name)
{
    for (int i = 0; i < g_priceCount; i++)
        if (_stricmp(g_price[i].name, name) == 0) return &g_price[i];
    if (g_priceCount >= RES_MAX_ENTRIES) return NULL;

    PriceEntry& e = g_price[g_priceCount++];
    memset(&e, 0, sizeof(e));
    strncpy_s(e.name, sizeof(e.name), name, _TRUNCATE);
    return &e;
}

// "<rub>[, <usd>]". One number sets both, which is what most of the base
// game's own base prices do - only uranium, explosives, water, usagewater and
// the waste resources price the two currencies apart.
static bool ParseMoneyPair(char* v, float* rub, float* usd)
{
    Trim(v);
    if (!v[0]) return false;

    char* second = strchr(v, ',');
    if (second) *second++ = 0;
    Trim(v);
    if (!v[0]) return false;

    *rub = (float)atof(v);
    *usd = *rub;
    if (second)
    {
        Trim(second);
        if (second[0]) *usd = (float)atof(second);
    }
    return true;
}

// ---------------------------------------------------------------- a record with no template
//
// The other half of [list]. A resource declared as `<name> = custom` starts from
// a zeroed record instead of a clone, and everything the record holds comes from
// its own `[custom:<name>]` section, defaulted to the base game's own numbers.
//
// **Both halves run the same code.** A `[custom:<name>]` section is a list of
// (offset, type, value) writes applied to the record after it has been zeroed
// *or* cloned, so the same section can also retune one field of a template-based
// resource - `[custom:copper_ore]` with `kind = 1` on an entry cloned from
// rawiron - and the clone path is untouched for anything that does not have one.
//
// Keyed by name rather than by [list] position, the way PriceEntryFor is, so a
// section may appear anywhere in the file, before or after the entry it
// configures.

// The eighteen transport classes and, per class, the numbers the base game uses
// for them. Every figure here was read out of the resource table rather than
// invented: where the base game disagrees with itself the commonest value wins,
// and the comment says how wide the spread was.
struct TransportClass
{
    const char* name;
    float factor, f1, f2, f3, f4;
};

// **0.049999997f, not 0.05f.** The game's own constant for the liquid classes is
// 0x3D4CCCCC and `0.05f` compiles to 0x3D4CCCCD - the base game's compiler
// truncated the decimal towards zero where a modern one rounds to nearest, so
// every float constant in the executable can be one ULP low. It makes no
// difference to anything the field does; it is spelled exactly here so that
// tools/pe/restable.py --verify can report a rebuilt record as byte-for-byte
// identical, which is the check that says nothing in the layout was missed.
#define RES_LIQUID_F 0.049999997f

static const TransportClass kTransportClasses[RES_CLASS_COUNT] = {
    { "covered",   0.50f, 2.50f,  2.50f, 0.0f, 0.0f },  // 12 users, factor 0.25..0.8
    { "open",      1.00f, 1.25f,  2.50f, 0.0f, 0.0f },  // 8 users, wood and boards exactly this
    { "gravel",    1.00f, 0.50f, 30.00f, 0.0f, 0.0f },  // 9 of 13 users exactly this
    { "oil",       1.00f, 5.00f,  5.00f, RES_LIQUID_F, RES_LIQUID_F },  // all four identical
    { "cement",    1.00f, 1.50f,  1.50f, 0.0f, 0.0f },  // cement, alumina
    { "cooler",    0.50f, 3.50f,  3.50f, 0.0f, 0.0f },  // meat
    { "livestock", 0.75f, 4.00f,  4.00f, 0.0f, 0.0f },  // livestock
    { "passanger", 1.00f, 5.00f,  5.00f, 0.0f, 0.0f },  // workers - the game's own spelling
    { "concrete",  1.00f, 5.00f, 20.00f, 0.0f, 0.0f },  // concrete
    { "eletric",   1.00f, 1.00f,  1.00f, 0.0f, 0.0f },  // eletric - the game's own spelling
    { "vehicles",  1.00f, 1.00f,  1.00f, 0.0f, 0.0f },  // vehicles, trains
    { "general",   1.00f, 1.25f,  2.50f, 0.0f, 0.0f },  // mirrors the primary, see below
    { "nuclear1",  1.00f, 2.50f,  2.50f, 0.0f, 0.0f },  // uf6
    { "nuclear2",  1.00f, 2.50f,  2.50f, 0.0f, 0.0f },  // nuclearfuel(+burned)
    { "heating",   1.00f, 1.00f,  1.00f, 0.0f, 0.0f },  // heat
    { "water",     1.00f, 5.00f,  5.00f, RES_LIQUID_F, RES_LIQUID_F },  // water, usagewater
    { "sewage",    1.00f, 5.00f,  5.00f, RES_LIQUID_F, RES_LIQUID_F },  // water, usagewater
    { "waste",     1.00f, 2.50f,  2.50f, 0.0f, 0.0f },  // 6 of 10 users exactly this
};

// A class by name, by the game's own $STORAGE token, or by number. Accepting the
// token form means a line can be copied straight out of a building.ini.
static int TransportClassIndex(const char* s)
{
    if (!s || !s[0]) return -1;
    if (_strnicmp(s, "RESOURCE_TRANSPORT_", 19) == 0) s += 19;

    if (s[0] >= '0' && s[0] <= '9')
    {
        int n = atoi(s);
        return (n >= 0 && n < RES_CLASS_COUNT) ? n : -1;
    }
    for (int i = 0; i < RES_CLASS_COUNT; i++)
        if (_stricmp(kTransportClasses[i].name, s) == 0) return i;
    return -1;
}

// The ten material families at +0x30C, named after the one waste resource each
// of them contains. A number is accepted too, and -1 means none.
static const char* kFamilyNames[10] = {
    "gravel", "steel", "aluminium", "plastic", "bio",
    "food",   "burnable", "toxic", "other", "ash"
};

static int FamilyIndex(const char* s)
{
    if (!s || !s[0]) return -1;
    if (_stricmp(s, "none") == 0) return -1;
    if (s[0] == '-' || (s[0] >= '0' && s[0] <= '9')) return atoi(s);

    if (_strnicmp(s, "waste_", 6) == 0) s += 6;
    for (int i = 0; i < 10; i++)
        if (_stricmp(kFamilyNames[i], s) == 0) return RES_FAMILY_FIRST + i;
    return -2;                                   // said something, meant nothing
}

// Which cargo geometry to look for. `auto` is the files on disk for a custom
// record and the template's own mesh slots for a clone - the shape has to come
// from somewhere, and a zeroed record cannot answer it.
enum { CARGO_AUTO = 0, CARGO_NONE, CARGO_BULK, CARGO_OPEN };

enum { RW_F32, RW_I32, RW_U8 };

struct ResWrite
{
    unsigned short off;
    unsigned char  kind;
    float          f;
    int            i;
};

// 96 writes is six full transport classes plus every named field twice over;
// the whole record only has 30 bytes of content to write.
#define RES_MAX_WRITES 96

struct CustomEntry
{
    char     name[64];
    ResWrite w[RES_MAX_WRITES];
    int      nWrites;
    int      cargo;          // CARGO_*
    int      primary;        // the class `transport =` named, or -1
    bool     packedSet;      // an explicit `packed =` beats the auto rule
    bool     overflow;
};

static CustomEntry g_custom[RES_MAX_ENTRIES];
static int         g_customCount;
static int         g_customReport = 0;   // dump every built record, field by field

static CustomEntry* CustomLookup(const char* name)
{
    for (int i = 0; i < g_customCount; i++)
        if (_stricmp(g_custom[i].name, name) == 0) return &g_custom[i];
    return NULL;
}

static CustomEntry* CustomFor(const char* name)
{
    if (CustomEntry* e = CustomLookup(name)) return e;
    if (g_customCount >= RES_MAX_ENTRIES) return NULL;

    CustomEntry& e = g_custom[g_customCount++];
    memset(&e, 0, sizeof(e));
    strncpy_s(e.name, sizeof(e.name), name, _TRUNCATE);
    e.cargo   = CARGO_AUTO;
    e.primary = -1;
    return &e;
}

static void Emit(CustomEntry* c, int off, int kind, float f, int i)
{
    if (off < 0 || off + (kind == RW_U8 ? 1 : 4) > RES_STRIDE)
    {
        Logf("registry  \"%s\": offset 0x%X is outside the %d-byte record - ignored",
             c->name, off, RES_STRIDE);
        return;
    }
    if (c->nWrites >= RES_MAX_WRITES)
    {
        if (!c->overflow)
        {
            c->overflow = true;
            Logf("registry  \"%s\": more than %d field writes - the rest are ignored",
                 c->name, RES_MAX_WRITES);
        }
        return;
    }
    ResWrite& w = c->w[c->nWrites++];
    w.off  = (unsigned short)off;
    w.kind = (unsigned char)kind;
    w.f    = f;
    w.i    = i;
}

static void EmitF(CustomEntry* c, int off, float v) { Emit(c, off, RW_F32, v, 0); }
static void EmitI(CustomEntry* c, int off, int v)   { Emit(c, off, RW_I32, 0.0f, v); }
static void EmitB(CustomEntry* c, int off, int v)   { Emit(c, off, RW_U8,  0.0f, v); }

// "a, b, c" -> up to `max` floats, returning how many were actually there.
// Destructive on `v`, like everything else in this parser.
static int ParseFloatList(char* v, float* out, int max)
{
    int n = 0;
    while (v && *v && n < max)
    {
        char* next = strchr(v, ',');
        if (next) *next++ = 0;
        Trim(v);
        if (!v[0]) break;
        out[n++] = (float)atof(v);
        v = next;
    }
    return n;
}

// One transport class, with as much or as little of it as the line gave.
// `general` with no numbers mirrors the primary class, because that is what ten
// of the twelve base-game resources declaring a second class do.
static void EmitClass(CustomEntry* c, int cls, const float* v, int n)
{
    const TransportClass& t = kTransportClasses[cls];
    const TransportClass* d = &t;

    if (cls == 11 && n == 0 && c->primary >= 0)      // GENERAL, mirroring
        d = &kTransportClasses[c->primary];

    int base = RES_CLASS_BASE + cls * RES_CLASS_STRIDE;
    EmitF(c, base,                 n > 0 ? v[0] : d->factor);
    EmitF(c, base + RES_CLASS_F1,  n > 1 ? v[1] : d->f1);
    EmitF(c, base + RES_CLASS_F2,  n > 2 ? v[2] : d->f2);
    EmitF(c, base + RES_CLASS_F3,  d->f3);
    EmitF(c, base + RES_CLASS_F4,  d->f4);
    EmitB(c, base + RES_CLASS_FLAG, n > 3 ? (int)v[3] : 0);
}

// One `key = value` line of a [custom:<name>] section. Unknown keys are called
// out rather than skipped silently: a misspelt field that quietly does nothing
// is exactly the class of bug this project keeps re-learning.
static void ParseCustomLine(CustomEntry* c, char* key, char* value)
{
    Trim(key);
    Trim(value);

    float v[6];

    // `transport` and `class` are the same line; the first also records which
    // class is the primary one, which is what `general` mirrors and what the
    // packed byte is derived from.
    if (_stricmp(key, "transport") == 0 || _stricmp(key, "class") == 0)
    {
        bool primary = (key[0] == 't' || key[0] == 'T');

        char* rest = strchr(value, ',');
        if (rest) *rest++ = 0;
        Trim(value);

        int cls = TransportClassIndex(value);
        if (cls < 0)
        {
            Logf("registry  \"%s\": unknown transport class \"%s\"", c->name, value);
            return;
        }
        if (primary) c->primary = cls;

        int n = rest ? ParseFloatList(rest, v, 4) : 0;
        EmitClass(c, cls, v, n);

        // The measured rule: 1 on every base-game resource whose primary class
        // is COVERED, COOLER, NUCLEAR1 or NUCLEAR2, 0 on the other 42.
        if (primary && !c->packedSet)
            EmitB(c, RES_PACKED_OFF,
                  (cls == 0 || cls == 5 || cls == 12 || cls == 13) ? 1 : 0);
        return;
    }
    if (_stricmp(key, "kind") == 0)   { EmitI(c, RES_KIND_OFF, atoi(value)); return; }
    if (_stricmp(key, "packed") == 0)
    {
        c->packedSet = true;
        EmitB(c, RES_PACKED_OFF, _stricmp(value, "auto") == 0 ? 0 : atoi(value));
        return;
    }
    if (_stricmp(key, "family") == 0)
    {
        int f = FamilyIndex(value);
        if (f == -2) { Logf("registry  \"%s\": unknown family \"%s\"", c->name, value); return; }
        EmitI(c, RES_FAMILY_OFF, f);
        return;
    }
    if (_stricmp(key, "price") == 0 || _stricmp(key, "base_price") == 0)
    {
        // The same two numbers [price] and [base_price] take, but written into
        // the record when it is built rather than around the engine's own pass.
        // Those two sections are still the ones that survive a recompute.
        float a = 0, b = 0;
        if (!ParseMoneyPair(value, &a, &b)) return;
        bool base = (key[0] == 'b' || key[0] == 'B');
        EmitF(c, base ? RES_BASE_RUB : RES_PRICE_RUB, a);
        EmitF(c, base ? RES_BASE_USD : RES_PRICE_USD, b);
        return;
    }
    if (_stricmp(key, "trade_mult") == 0)
    {
        int n = ParseFloatList(value, v, 2);
        if (n < 2) { Logf("registry  \"%s\": trade_mult needs <sell>, <buy>", c->name); return; }
        EmitF(c, RES_MARKET_RUB + RES_MARKET_SELL, v[0]);
        EmitF(c, RES_MARKET_RUB + RES_MARKET_BUY,  v[1]);
        EmitF(c, RES_MARKET_USD + RES_MARKET_SELL, v[0]);
        EmitF(c, RES_MARKET_USD + RES_MARKET_BUY,  v[1]);
        return;
    }
    if (_stricmp(key, "market_rub") == 0 || _stricmp(key, "market_usd") == 0)
    {
        int n = ParseFloatList(value, v, 3);
        if (n < 2)
        {
            Logf("registry  \"%s\": %s needs at least two figures", c->name, key);
            return;
        }
        int m = (_stricmp(key, "market_rub") == 0) ? RES_MARKET_RUB : RES_MARKET_USD;
        EmitF(c, m + RES_MARKET_LIM1, v[0]);
        EmitF(c, m + RES_MARKET_LIM2, v[1]);
        if (n > 2) EmitF(c, m + RES_MARKET_K, v[2]);
        return;
    }
    if (_stricmp(key, "cargo") == 0)
    {
        c->cargo = _stricmp(value, "none") == 0 ? CARGO_NONE
                 : _stricmp(value, "bulk") == 0 ? CARGO_BULK
                 : _stricmp(value, "open") == 0 ? CARGO_OPEN
                 : CARGO_AUTO;
        if (c->cargo == CARGO_AUTO && _stricmp(value, "auto") != 0)
            Logf("registry  \"%s\": cargo \"%s\" is not none/bulk/open/auto - "
                 "taking it as auto", c->name, value);
        return;
    }
    if (_stricmp(key, "field") == 0)
    {
        // The escape hatch, and the reason the unidentified fields need no
        // invented names: `field = 0x50, f, 84` writes a float, `i` an int, `b`
        // a byte, anywhere in the 832 bytes.
        char* t = strchr(value, ',');
        if (!t) { Logf("registry  \"%s\": field needs <offset>, f|i|b, <value>", c->name); return; }
        *t++ = 0;
        char* val = strchr(t, ',');
        if (!val) { Logf("registry  \"%s\": field needs <offset>, f|i|b, <value>", c->name); return; }
        *val++ = 0;
        Trim(value); Trim(t); Trim(val);

        int off = (int)strtol(value, NULL, 0);
        if (t[0] == 'f' || t[0] == 'F') EmitF(c, off, (float)atof(val));
        else if (t[0] == 'b' || t[0] == 'B') EmitB(c, off, (int)strtol(val, NULL, 0));
        else EmitI(c, off, (int)strtol(val, NULL, 0));
        return;
    }

    Logf("registry  \"%s\": unknown field \"%s\" - see plugins\\resources.ini", c->name, key);
}

// Everything a record needs that is not identity, assets or money: the shape of
// a working resource with no class declared yet. Written before the entry's own
// list, so any line in [custom:<name>] overrides it.
//
// The figures are the base game's commonest: the two market blocks of twelve
// resources including every waste, a family of -1 (workers, eletric, heat,
// water, usagewater), and the 0.3 at +0x310 that 55 of 57 records carry.
static void WriteDefaultRecord(BYTE* rec)
{
    memset(rec, 0, RES_STRIDE);

    *(int*)  (rec + RES_KIND_OFF)  = 0;
    *(float*)(rec + RES_PRICE_RUB) = 10.0f;
    *(float*)(rec + RES_PRICE_USD) = 10.0f;

    static const int markets[2] = { RES_MARKET_RUB, RES_MARKET_USD };
    static const float lim[2][2] = { { 200.0f, 350.0f }, { 500.0f, 500.0f } };
    for (int b = 0; b < 2; b++)
    {
        BYTE* m = rec + markets[b];
        *(float*)(m + RES_MARKET_SELL) = 0.95f;
        *(float*)(m + RES_MARKET_BUY)  = 1.05f;
        *(float*)(m + RES_MARKET_LIM1) = lim[b][0];
        *(float*)(m + RES_MARKET_LIM2) = lim[b][1];
        *(float*)(m + RES_MARKET_K)    = 0.5f;
        *(float*)(m + RES_MARKET_ONE)  = 1.0f;
    }

    *(int*)  (rec + RES_FAMILY_OFF) = -1;
    *(float*)(rec + RES_TAIL_OFF)   = 0.3f;
}

static void ApplyCustomWrites(BYTE* rec, CustomEntry* c)
{
    for (int i = 0; i < c->nWrites; i++)
    {
        ResWrite& w = c->w[i];
        switch (w.kind)
        {
        case RW_F32: *(float*)(rec + w.off) = w.f; break;
        case RW_I32: *(int*)  (rec + w.off) = w.i; break;
        default:     *(rec + w.off) = (BYTE)w.i;   break;
        }
    }
}

// What the record actually says, after everything has been applied - the one
// thing that answers "did my .ini reach the engine". Only the fields the base
// game ever writes are printed, because the rest are zero by construction.
static void ReportRecord(BYTE* rec, const char* name)
{
    Logf("record    \"%s\" kind %d  price %.2f/%.2f  base %.2f/%.2f  packed %d  "
         "family %d  +310 %.2f", name,
         *(int*)(rec + RES_KIND_OFF),
         *(float*)(rec + RES_PRICE_RUB), *(float*)(rec + RES_PRICE_USD),
         *(float*)(rec + RES_BASE_RUB),  *(float*)(rec + RES_BASE_USD),
         *(rec + RES_PACKED_OFF), *(int*)(rec + RES_FAMILY_OFF),
         *(float*)(rec + RES_TAIL_OFF));

    for (int b = 0; b < 2; b++)
    {
        BYTE* m = rec + (b ? RES_MARKET_USD : RES_MARKET_RUB);
        Logf("record    \"%s\" market %s  sell %.2f buy %.2f  %.0f / %.0f  k %.2f",
             name, b ? "usd" : "rub",
             *(float*)(m + RES_MARKET_SELL), *(float*)(m + RES_MARKET_BUY),
             *(float*)(m + RES_MARKET_LIM1), *(float*)(m + RES_MARKET_LIM2),
             *(float*)(m + RES_MARKET_K));
    }

    int classes = 0;
    for (int k = 0; k < RES_CLASS_COUNT; k++)
    {
        BYTE* cb = rec + RES_CLASS_BASE + k * RES_CLASS_STRIDE;
        if (*(float*)cb == 0.0f) continue;
        classes++;
        Logf("record    \"%s\" class %-9s [%2d] factor %.3f  %.2f / %.2f  %.2f / %.2f  flag %d",
             name, kTransportClasses[k].name, k,
             *(float*)cb, *(float*)(cb + RES_CLASS_F1), *(float*)(cb + RES_CLASS_F2),
             *(float*)(cb + RES_CLASS_F3), *(float*)(cb + RES_CLASS_F4),
             *(cb + RES_CLASS_FLAG));
    }
    if (!classes)
        Logf("record    WARN  \"%s\" can be carried by no transport class at all - every "
             "storage that names it will report 0.00 of 0.00 t. Set `transport =` in "
             "[custom:%s]", name, name);
}

// Parsed by hand rather than through the profile API: display names are UTF-8
// and GetPrivateProfileString would mangle anything outside the ANSI codepage.
static void LoadResourceRegistry()
{
    char path[MAX_PATH];
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\plugins\\resources.ini", g_baseDir);

    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
    {
        Logf("registry  no plugins\\resources.ini - nothing to inject");
        return;
    }

    // The whole file, however long it is. A fixed 8 KB buffer used to be enough
    // for six resources and the comments around them; it is not a bound worth
    // keeping once [list] can be any length, and a truncated read would drop
    // entries silently rather than complain.
    LARGE_INTEGER size = { 0 };
    GetFileSizeEx(h, &size);
    if (size.QuadPart <= 0 || size.QuadPart > 4 * 1024 * 1024)
    {
        CloseHandle(h);
        Logf("registry  plugins\\resources.ini is %lld bytes - refusing to read it",
             (long long)size.QuadPart);
        return;
    }

    char* buf = (char*)malloc((size_t)size.QuadPart + 1);
    if (!buf) { CloseHandle(h); Logf("registry  out of memory reading resources.ini"); return; }

    DWORD got = 0;
    ReadFile(h, buf, (DWORD)size.QuadPart, &got, NULL);
    CloseHandle(h);
    buf[got] = 0;

    enum { SECT_NONE, SECT_LIST, SECT_BASE, SECT_PRICE, SECT_CUSTOM };

    int          inSection = SECT_NONE;
    CustomEntry* custom    = NULL;
    char*        ctx = NULL;
    for (char* line = strtok_s(buf, "\n", &ctx); line; line = strtok_s(NULL, "\n", &ctx))
    {
        Trim(line);
        if (!line[0] || line[0] == ';' || line[0] == '#') continue;
        // [list] is the content, [base_price] and [price] are its money,
        // [custom:<name>] is one resource's record field by field, and
        // [resources] is the plugin's own settings; they share a file so a
        // feature is one file. Any other section is somebody else's and is
        // skipped.
        if (line[0] == '[')
        {
            custom    = NULL;
            inSection = _strnicmp(line, "[list]", 6)        == 0 ? SECT_LIST
                      : _strnicmp(line, "[base_price]", 12) == 0 ? SECT_BASE
                      : _strnicmp(line, "[price]", 7)       == 0 ? SECT_PRICE
                      : SECT_NONE;

            if (inSection == SECT_NONE && _strnicmp(line, "[custom:", 8) == 0)
            {
                char who[64];
                strncpy_s(who, sizeof(who), line + 8, _TRUNCATE);
                if (char* end = strchr(who, ']')) *end = 0;
                Trim(who);

                custom = who[0] ? CustomFor(who) : NULL;
                if (custom) inSection = SECT_CUSTOM;
                else Logf("registry  \"%s\" - no room for another [custom:] section", who);
            }
            continue;
        }
        if (inSection == SECT_NONE) continue;

        if (inSection == SECT_CUSTOM)
        {
            char* eq = strchr(line, '=');
            if (!eq) continue;
            *eq = 0;
            ParseCustomLine(custom, line, eq + 1);
            continue;
        }

        if (inSection != SECT_LIST)
        {
            char* eq = strchr(line, '=');
            if (!eq) continue;
            *eq = 0;
            Trim(line);
            if (!line[0]) continue;

            PriceEntry* p = PriceEntryFor(line);
            if (!p)
            {
                Logf("registry  more than %d priced names - \"%s\" ignored",
                     RES_MAX_ENTRIES, line);
                continue;
            }

            float a = 0, b = 0;
            if (!ParseMoneyPair(eq + 1, &a, &b))
            {
                Logf("registry  \"%s\": %s needs <rub>[, <usd>]", line,
                     inSection == SECT_BASE ? "[base_price]" : "[price]");
                continue;
            }
            if (inSection == SECT_BASE)
            {
                p->baseRub = a; p->baseUsd = b; p->hasBase = true;
                Logf("registry  \"%s\" base price %.2f RUB / %.2f USD", p->name, a, b);
            }
            else
            {
                p->priceRub = a; p->priceUsd = b; p->hasPrice = true;
                Logf("registry  \"%s\" price %.2f RUB / %.2f USD, forced", p->name, a, b);
            }
            continue;
        }

        if (g_regCount >= RES_MAX_ENTRIES)
        {
            if (!g_regOverflow)
            {
                g_regOverflow = true;
                Logf("registry  [list] has more than %d entries - the rest are ignored. "
                     "Raise RES_MAX_ENTRIES in plugins/resources/resources.cpp", RES_MAX_ENTRIES);
            }
            continue;
        }

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

        // [<slot>,]<template|custom>[,<display name>]
        //
        // The slot is optional, and leaving it out is the better default:
        // hard-coding 57 and 58 only holds as long as nothing else claims them
        // first, and how many resources the base game ships is not something a
        // mod should have to know. A leading field that is not a number means
        // the value starts at the template, which is what makes the short form
        // unambiguous without a second syntax.
        //
        // `custom` (or `none`) in the template field means there is no donor at
        // all: the record is built from zero out of [custom:<name>]. The two
        // forms are the same syntax and the same registry - only what happens at
        // arm time differs.
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
                if (_stricmp(value, "custom") == 0 || _stricmp(value, "none") == 0)
                    e.custom = true;
                else
                {
                    e.tmplIndex = CanonicalIndex(value);
                    // A misspelt template used to leave the record all zeroes
                    // and the resource inert, with one line in the log. Building
                    // it from scratch is strictly better - it is at least a
                    // working record - and keeps the resource count, which the
                    // save format depends on, exactly as it was.
                    if (e.tmplIndex < 0)
                    {
                        e.custom = true;
                        Logf("registry  \"%s\": \"%s\" is not a base-game resource - building the "
                             "record from scratch. Say `custom` if that was the intent",
                             e.name, value);
                    }
                }
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

        // No template, however the line said so, means built from scratch.
        if (e.tmplIndex < 0) e.custom = true;

        // A clone inherits the template's caption id, which is why one is
        // optional there. A record built from zero has 0 in that field and would
        // resolve to whatever string id 0 is, so it gets an id of its own and
        // its own name as the display text.
        if (e.custom && !e.textId)
        {
            MultiByteToWideChar(CP_UTF8, 0, e.name, -1, e.display,
                                sizeof(e.display) / sizeof(e.display[0]));
            e.textId = TML_TEXT_ID_BASE + g_regCount;
        }

        char how[48];
        if (e.custom) strncpy_s(how, sizeof(how), "no template", _TRUNCATE);
        else _snprintf_s(how, sizeof(how), _TRUNCATE, "template %d", e.tmplIndex);

        if (e.index < 0)
            Logf("registry  \"%s\" -> next free slot, %s, text id %d",
                 e.name, how, e.textId);
        else
            Logf("registry  \"%s\" -> slot %d, %s, text id %d",
                 e.name, e.index, how, e.textId);
        g_regCount++;
    }
    free(buf);
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

// Carries the engine's cached record pointers over to a moved array. A qword in
// the cache block that lands exactly on a record boundary inside the old buffer
// is one of ours; the four standalone records and any slot the engine has not
// filled yet point elsewhere and are left alone.
//
// Normally rebases nothing, because relocation happens on the first lookup and
// the cache is still all zeroes at that point. It is the belt to the ordering's
// braces, and the line it logs is worth reading: a non-zero count means the
// array moved later than it should have.
static void RebaseResourceCache(ResVector* vec, BYTE* from, BYTE* to, int live)
{
    void** cache = (void**)((BYTE*)vec + RES_CACHE_OFF);
    const size_t bytes = RES_CACHE_COUNT * sizeof(void*);
    if (!Readable(cache, bytes)) return;

    DWORD prot = 0;
    if (!VirtualProtect(cache, bytes, PAGE_READWRITE, &prot))
    {
        Logf("resource  WARN  could not write the record cache (%lu) - %d pointer(s) may dangle",
             GetLastError(), RES_CACHE_COUNT);
        return;
    }

    int moved = 0;
    for (int i = 0; i < RES_CACHE_COUNT; i++)
    {
        BYTE*  p = (BYTE*)cache[i];
        size_t d = (size_t)(p - from);
        if (!p || p < from || d >= (size_t)live * RES_STRIDE || (d % RES_STRIDE) != 0) continue;
        cache[i] = to + d;
        moved++;
    }
    VirtualProtect(cache, bytes, prot, &prot);

    if (moved)
        Logf("resource  %d cached record pointer(s) rebased - the array moved after the engine "
             "had already resolved them", moved);
}

// Grows the resource array by moving it, the way reserve() would. Safe only
// before anything has taken a Resource* into the old buffer, which is why this
// runs on the very first lookup - ahead of the engine's own init loop - and why
// the caller refuses once anything of ours is already published.
//
// The old block is deliberately left alone rather than freed: it costs a few
// tens of kilobytes once per process and removes any chance of releasing memory
// the engine still believes it owns. **Once** per process, not per map load:
// the engine reuses the buffer across worlds - a world change is `end = begin`
// at rva 0x25EC7A, a clear() and not a destructor - so a capacity raised here
// survives every later load and nothing moves a second time.
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

    RebaseResourceCache(vec, old, fresh, live);

    Logf("resource  array moved %p -> %p, capacity %d records (%d live)",
         old, fresh, newCap, live);
    return true;
}

// ---------------------------------------------------------------- cargo meshes

typedef void* (*t_CreateManagedMesh)(void*, const char*);
typedef int   (*t_MeshLoadFromFile)(void*, const char*, void*, bool);
typedef int   (*t_MeshLoadMaterial)(void*, const char*, int);
typedef bool  (__cdecl* t_FileExists)(const char*, bool, bool);

static t_CreateManagedMesh o_CreateManagedMesh;
static t_MeshLoadFromFile  o_MeshLoadFromFile;
static t_MeshLoadMaterial  o_MeshLoadMaterial;
static t_FileExists        o_FileExists;

// The engine's own helper, with the argument pair every caller in the game
// passes. Missing it is not fatal - it only means the asset check falls back to
// "assume it is there", which is what this plugin did before.
static bool AssetExists(const char* path)
{
    return o_FileExists ? o_FileExists(path, false, true) : true;
}

// Exactly the three calls the engine's own table makes, in the same order.
// CreateManagedMesh caches by path, so asking twice for the same file is free
// and re-arming after a map load does not leak a mesh per load.
// **The existence check is not an optimisation, it is the whole safety of this
// function.** Handing CreateManagedMesh a path that is not there leaves an
// empty mesh - node array null, node count non-zero - registered in the
// middlepoint's cache, and LoadMaterial on it faults inside
// C3D_MIDDLEPOINT::CreateManagedMaterial. Catching that fault is not enough:
// the engine has already been damaged, and the game dies on the first frame it
// renders, in C3D_MESH::Render, twenty seconds and a whole world load later.
// One resource declared in [list] with no files on disk cost exactly that. See
// docs/07-pitfalls.md.
static void* LoadResourceMesh(const char* nmf, const char* mtl)
{
    if (!o_CreateManagedMesh || !o_MeshLoadFromFile) return NULL;
    if (!AssetExists(nmf)) return NULL;

    void* mp   = g_exeBase + P_MIDDLEPOINT;
    void* mesh = o_CreateManagedMesh(mp, nmf);
    if (!mesh) return NULL;

    // A mesh that failed to load is the same empty object, so the material is
    // only asked for once there is geometry to put it on.
    //
    // NOT A BOOL. Every resource with real files under media_soviet/resources
    // reported "0 of N replaced" - drawn as the template - even though the
    // .nmf is byte-identical to its donor's own (working) mesh and the VFS log
    // showed every fopen for it succeeding through the redirect, twice per
    // file, matching a full fopen/fseek/ftell/fread/fclose parse disassembled
    // directly in C3DDLL64.dll at rva 0xA84C0. The mangled export settles it:
    // `?LoadFromFile@C3D_MESH@@QEAA H PEBD PEAVC3D_MIDDLEPOINT@@ _N @Z` returns
    // `H` - `int` - not `_N` - `bool`. The disassembly's only two return sites
    // agree: the full-parse path ends in `xor eax,eax` (0 on success), and the
    // one failure path found (`fopen` itself returning null) returns `eax=1`.
    // So this is a C-style error code, zero meaning no error, and the original
    // `if (!loaded)` had success and failure backwards - discarding every mesh
    // that had in fact just finished loading, and falling back to the
    // template's geometry silently, with no warning that would have looked
    // like a bug rather than a missing asset.
    int rc = o_MeshLoadFromFile(mesh, nmf, mp, true);
    if (rc != 0)
    {
        Logf("resource  \"%s\": LoadFromFile error %d", nmf, rc);
        return NULL;
    }
    if (o_MeshLoadMaterial && mtl && AssetExists(mtl)) o_MeshLoadMaterial(mesh, mtl, 0);
    return mesh;
}

// Which of the three cargo shapes a resource has, from the files that are
// actually on disk. This is what a record with no template has to use, because a
// zeroed record cannot be asked - and it is the same rule the stock folder
// follows: bulk resources ship <name>1..4.nmf plus <name>_vehicle.nmf, open ones
// exactly <name>.nmf, and the fifteen that travel packed ship no mesh at all.
static int CargoShapeFromFiles(const char* name)
{
    char p[MAX_PATH];

    // Without the engine's existence helper AssetExists answers yes to
    // everything, which would make this pick `bulk` for a resource with no files
    // at all - and handing CreateManagedMesh a path that is not there is the one
    // mistake that kills the process a whole world load later. No answer is the
    // only safe answer; say `cargo = bulk` explicitly if that is really wanted.
    if (!o_FileExists)
    {
        Logf("resource  \"%s\": no way to test for cargo files - assuming none. Say "
             "`cargo =` in [custom:%s] to override", name, name);
        return CARGO_NONE;
    }

    _snprintf_s(p, sizeof(p), _TRUNCATE, "resources/%s1.nmf", name);
    if (AssetExists(p)) return CARGO_BULK;

    _snprintf_s(p, sizeof(p), _TRUNCATE, "resources/%s.nmf", name);
    if (AssetExists(p)) return CARGO_OPEN;

    return CARGO_NONE;
}

// The template's own mesh slots: stages filled means bulk, only the vehicle slot
// means open, all five null means the template has no cargo geometry either.
static int CargoShapeFromRecord(BYTE* rec)
{
    for (int i = 0; i < RES_MESH_STAGES; i++)
        if (*(void**)(rec + RES_MESH_STAGE1 + i * 8)) return CARGO_BULK;
    return *(void**)(rec + RES_MESH_VEHICLE) ? CARGO_OPEN : CARGO_NONE;
}

// Gives the record cargo meshes loaded from this resource's own files.
//
// **The shape is decided by the caller**, and how depends on where the record
// came from: a clone reads it off the template's mesh slots, which is what keeps
// the rule in the same place that decides the transport class, and a
// from-scratch record reads it off the files on disk. Either can be overridden
// with `cargo =` in [custom:<name>].
//
// For a clone, a slot whose file is missing keeps the template's mesh - wrong
// looking but drawable, where the alternative is a null the engine dereferences.
// For a from-scratch record there is nothing to fall back to, so a bulk shape
// with a missing stage leaves that stage null, exactly as the base game's own
// packed resources do for all five.
static void AttachResourceMeshes(BYTE* rec, const char* name, int shape)
{
    char mtl[MAX_PATH], nmf[MAX_PATH];
    _snprintf_s(mtl, sizeof(mtl), _TRUNCATE, "resources/%s.mtl", name);

    int done = 0, want = 0;
    if (shape == CARGO_BULK)
    {
        want = 1 + RES_MESH_STAGES;
        _snprintf_s(nmf, sizeof(nmf), _TRUNCATE, "resources/%s_vehicle.nmf", name);
        if (void* m = LoadResourceMesh(nmf, mtl)) { *(void**)(rec + RES_MESH_VEHICLE) = m; done++; }

        for (int i = 0; i < RES_MESH_STAGES; i++)
        {
            BYTE* slot = rec + RES_MESH_STAGE1 + i * 8;
            _snprintf_s(nmf, sizeof(nmf), _TRUNCATE, "resources/%s%d.nmf", name, i + 1);
            if (void* m = LoadResourceMesh(nmf, mtl)) { *(void**)slot = m; done++; }
        }
    }
    else if (shape == CARGO_OPEN)
    {
        want = 1;
        _snprintf_s(nmf, sizeof(nmf), _TRUNCATE, "resources/%s.nmf", name);
        if (void* m = LoadResourceMesh(nmf, mtl)) { *(void**)(rec + RES_MESH_VEHICLE) = m; done++; }
    }

    // No cargo geometry at all is a perfectly ordinary resource - food, clothes,
    // eletronics and the other twelve that travel packed are like that, and only
    // their icon is ever drawn. So this is not a warning; the warning is a shape
    // that was asked for and has no files behind it.
    if (shape == CARGO_NONE)
    {
        Logf("resource  \"%s\" has no cargo geometry - media_soviet/resources/%s.png is "
             "the only asset it needs", name, name);
        return;
    }

    Logf("resource  \"%s\" cargo meshes: %d of %d (%s)", name, done, want,
         shape == CARGO_BULK ? "bulk" : "open");
    if (done == 0)
        Logf("resource  WARN  \"%s\" has no cargo geometry under media_soviet/resources - "
             "add %s.nmf and %s.mtl, or say `cargo = none` in [custom:%s]",
             name, name, name, name);
}

// How many records the array has to hold for everything in [list] to fit.
//
// Deliberately **not** a function of how much of [list] is already published:
// it is the base game's own count plus every declared entry, so it returns the
// same number before and after arming and the array is therefore moved once and
// never again. Working from `live + still to do` instead would grow the answer
// as records land and relocate on every pass.
//
// `live` only ever raises it, for the day a game update ships more than 57
// records of its own and the constant below stops being the whole base game.
static int NeededCapacity(int live)
{
    int pending = 0;
    int need    = RES_KNOWN + g_regCount;

    for (int i = 0; i < g_regCount; i++)
    {
        if (g_reg[i].armed) continue;
        if (g_reg[i].index < 0) pending++;
        else if (g_reg[i].index + 1 > need) need = g_reg[i].index + 1;
    }
    if (live + pending > need) need = live + pending;
    if (g_wantCapacity > need) need = g_wantCapacity;      // the .ini's floor
    return need;
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
        g_resBase       = base;
        g_nameOff       = -1;
        g_warnedSlot    = false;
        g_warnedCapacity = false;
        for (int i = 0; i < g_regCount; i++) g_reg[i].armed = false;
        Logf("resource  array now at %p", base);
    }

    ptrdiff_t span = vec->end - base;
    ptrdiff_t cap  = vec->cap - base;
    if (span <= 0 || (span % RES_STRIDE) != 0 || cap < span) return;

    // A sanity bound on the vector, not a limit on [list]: anything outside it
    // means the three pointers are not a vector of records at all. Derived from
    // the registry size so raising that cannot quietly turn this into the next
    // ceiling.
    int live = (int)(span / RES_STRIDE);
    int room = (int)(cap / RES_STRIDE);
    if (live < 2 || live > RES_KNOWN + RES_MAX_ENTRIES + 64) return;

    // The name offset, before anything else needs it. It is a property of the
    // record layout rather than of the allocation, so it survives the array
    // moving and is only ever looked for once.
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

    // Armed is a claim about the array in front of us, not a fact about this
    // session, so it is re-checked rather than trusted.
    //
    // A world load rebuilds the vector, and the allocator hands back the same
    // block often enough that `begin` is unchanged - so the base test above sees
    // nothing while `end` has gone back to 57 and our record has been
    // overwritten by the engine's own init. Left latched, the entry is never
    // republished: every building.ini naming it resolves to -1, and the icon at
    // +0x48 still points at a texture that was released with the previous world,
    // which is what crashed on hover.
    //
    // Done as its own pass rather than inside the publishing loop, because both
    // the capacity below and the guard on moving the array ask how much of
    // [list] is really live and would otherwise be reading stale flags.
    for (int r = 0; r < g_regCount; r++)
    {
        ResEntry& e = g_reg[r];
        if (!e.armed) continue;

        BYTE* have = base + (size_t)e.resolved * RES_STRIDE;
        if (e.resolved >= 0 && e.resolved < live &&
            Readable(have, RES_STRIDE) &&
            strncmp((char*)(have + g_nameOff), e.name, 32) == 0)
            continue;                                 // still ours, nothing to do

        e.armed = false;
        Logf("resource  \"%s\" no longer at index %d - vector was rebuilt in place, re-arming",
             e.name, e.resolved);
    }

    // Room for the whole of [list], past the 63 records the engine allocates.
    //
    // Done before anything is published, so that everything downstream -
    // including the engine's own init loop, which resolves forty records by name
    // immediately after building the array and caches what it gets back - only
    // ever sees the new buffer.
    //
    // Refused once anything of ours is already live in this array: by then the
    // building-type parser has taken Resource* of its own into it and moving the
    // buffer would strand them. It cannot happen in practice, because nothing is
    // published until the array is big enough; the guard is there to keep it
    // that way.
    int need = NeededCapacity(live);
    if (need > room && g_wantCapacity >= 0)
    {
        bool anyArmed = false;
        for (int i = 0; i < g_regCount; i++) if (g_reg[i].armed) anyArmed = true;

        if (anyArmed)
        {
            if (!g_warnedCapacity)
            {
                g_warnedCapacity = true;
                Logf("resource  WARN  need room for %d records and have %d, but %d are already "
                     "live - not moving the array now", need, room, live);
            }
        }
        else if (RelocateResourceArray(vec, live, need))
        {
            base = vec->begin;
            room = need;
            g_resBase = base;
        }
        else if (!g_warnedCapacity)
        {
            g_warnedCapacity = true;
            Logf("resource  WARN  could not grow the array to %d records - only %d of the %d "
                 "names in [list] will fit", need, room - live, g_regCount);
        }
    }

    for (int r = 0; r < g_regCount; r++)
    {
        ResEntry& e = g_reg[r];
        if (e.armed) continue;

        // Recomputed per entry: arming one resource extends the vector, and
        // that is exactly what makes the next slot claimable in the same pass.
        live = (int)((vec->end - base) / RES_STRIDE);

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
        if (want >= room)
        {
            // The array should have been grown above, so getting here means the
            // relocation was refused or failed. Say which, rather than pointing
            // at resources.ini - nothing in it is wrong.
            if (!g_warnedSlot)
            {
                Logf("resource  \"%s\": no room at index %d (%d live, room %d) - the array was "
                     "not grown%s", e.name, want, live, room,
                     g_wantCapacity < 0 ? " because resource_capacity is negative" : "");
                g_warnedSlot = true;
            }
            continue;
        }
        if (want < live)
        {
            if (!g_warnedSlot)
            {
                Logf("resource  \"%s\": slot %d is taken (%d live) - fix resources.ini",
                     e.name, want, live);
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

        CustomEntry* cfg = CustomLookup(e.name);

        // Two ways to fill a freshly claimed record, and it is all zeroes to
        // begin with either way.
        //
        // **Clone** - copy a working resource in and correct it. What the
        // template really supplies is the 30 bytes of content the engine's own
        // resource table writes: the transport classes, the two market blocks,
        // the kind, the family. Nothing else in the record is per-resource.
        //
        // **From scratch** - write those same 30 bytes ourselves. Every offset
        // below is relative to a name at +0x00, which is the one thing the layout
        // is anchored on, so a record built here is refused outright if the name
        // field turned up anywhere else.
        if (e.custom)
        {
            if (g_nameOff != 0)
            {
                Logf("resource  \"%s\": the name field is at +0x%X, not +0, so the record "
                     "layout is not the one this build knows - refusing to build it from "
                     "scratch", e.name, g_nameOff);
                VirtualProtect(rec, RES_STRIDE, prot, &prot);
                continue;
            }
            WriteDefaultRecord(rec);
        }
        else
            memcpy(rec, base + (size_t)e.tmplIndex * RES_STRIDE, RES_STRIDE);

        // The entry's own field list, which a clone may also have - one line of
        // [custom:<name>] over a cloned record is how a template is corrected
        // rather than replaced.
        if (cfg) ApplyCustomWrites(rec, cfg);

        // Identity last, so no `field =` line can reach the name or the caption.
        memset(rec + g_nameOff, 0, 32);
        strncpy_s((char*)(rec + g_nameOff), 32, e.name, _TRUNCATE);
        if (e.textId) *(int*)(rec + RES_TEXTID_OFF) = e.textId;

        int shape = cfg ? cfg->cargo : CARGO_AUTO;
        if (shape == CARGO_AUTO)
            shape = e.custom ? CargoShapeFromFiles(e.name) : CargoShapeFromRecord(rec);

        // Done while the record is still writable, and before the vector's end
        // moves - nothing may see this record until it is complete. The engine
        // is called into here, so a fault has to stay ours rather than take the
        // process down with a half-published resource.
        __try { AttachResourceMeshes(rec, e.name, shape); }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            Logf("resource  \"%s\": cargo mesh load faulted - the cargo slots keep whatever "
                 "they had", e.name);
        }

        // The icon is the one asset the engine finds by name, in a UI pass of
        // its own at 0x2960DE, and the one this plugin therefore cannot supply
        // a fallback for. Saying so here is worth a line: a resource with no
        // icon looks like a resource that did not publish.
        //
        // **It is not optional on a record built from scratch.** A clone
        // inherits the template's texture pointer at +0x48 and is merely drawn
        // wrongly without a .png of its own; a from-scratch record has null
        // there until that pass runs. The pass overwrites +0x48 for every record
        // in the vector without releasing what was there - `mov [rbx+0x48],rax`
        // at 0x296172 - so the pointer is not owned by the record and seeding it
        // from record 0 costs nothing and removes the null.
        {
            char png[MAX_PATH];
            _snprintf_s(png, sizeof(png), _TRUNCATE, "resources/%s.png", e.name);
            if (!AssetExists(png))
                Logf("resource  WARN  \"%s\" has no icon - put a 48x48 RGBA PNG at "
                     "media_soviet/%s (tesmioloader\\vfs\\media_soviet\\resources\\%s.png)",
                     e.name, png, e.name);

            if (e.custom && !*(void**)(rec + RES_ICON_OFF))
                *(void**)(rec + RES_ICON_OFF) = *(void**)(base + RES_ICON_OFF);
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
        if (e.custom)
            Logf("resource  \"%s\" published as index %d (built from scratch%s, caption %d), "
                 "vector now %d", e.name, want, cfg ? "" : " with NO [custom:] section",
                 e.textId, want + 1);
        else
            Logf("resource  \"%s\" published as index %d (template %d%s, caption %d), "
                 "vector now %d", e.name, want, e.tmplIndex,
                 cfg ? " + [custom:] overrides" : "", e.textId, want + 1);

        // Every field of the record as it actually stands, which is the only
        // thing that separates "the .ini did not reach the engine" from "the
        // engine does not do what the field was thought to do". Always for a
        // record with no template - there is no donor to compare it against -
        // and on request for a clone.
        if (e.custom || g_customReport) ReportRecord(rec, e.name);
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

// ---------------------------------------------------------------- prices

// The record carrying a name, mod or base game, read out of the live vector.
// Deliberately not cached: a world load rebuilds the array, sometimes in the
// same block, and this runs a handful of times a session rather than per tick.
static BYTE* FindRecord(const char* name)
{
    if (g_nameOff < 0) return NULL;

    ResVector* vec = (ResVector*)(g_exeBase + g_vecRva);
    if (!Readable(vec, sizeof(ResVector))) return NULL;

    BYTE* base = vec->begin;
    ptrdiff_t span = vec->end - base;
    if (!base || span <= 0 || (span % RES_STRIDE) != 0) return NULL;

    int live = (int)(span / RES_STRIDE);
    if (live < 2 || live > RES_KNOWN + RES_MAX_ENTRIES + 64) return NULL;

    // One query for the whole array rather than one per record: this is called
    // three times per declared name per recompute, and Readable walks regions.
    if (!Readable(base, (size_t)live * RES_STRIDE)) return NULL;

    for (int i = 0; i < live; i++)
    {
        BYTE* rec = base + (size_t)i * RES_STRIDE;
        if (strncmp((char*)(rec + g_nameOff), name, 32) == 0) return rec;
    }
    return NULL;
}

// The records are heap memory and already writable, so this is belt and
// braces - the same call EnsureArmed makes before it edits a record, kept so
// that a page which somehow is not writable turns into a silent no-op rather
// than an access violation on the render thread.
static bool WriteRecordFloats(BYTE* rec, int offA, float a, int offB, float b)
{
    DWORD prot = 0;
    if (!VirtualProtect(rec, RES_STRIDE, PAGE_READWRITE, &prot)) return false;
    *(float*)(rec + offA) = a;
    *(float*)(rec + offB) = b;
    VirtualProtect(rec, RES_STRIDE, prot, &prot);
    return true;
}

static bool g_warnedNoLayout;

// Before the engine's pass, because the solver reads the base on its way in.
// Re-applied on every recompute rather than once at arm time: a save carries
// `$Economy_Base*` and puts the game's own numbers back into the record, and
// the drift at 0x2FB390 multiplies whatever is there by a random walk twice a
// period. Pinning it here makes the .ini the last word on both.
static void ApplyBasePrices(void)
{
    for (int i = 0; i < g_priceCount; i++)
    {
        PriceEntry& p = g_price[i];
        if (!p.hasBase) continue;

        BYTE* rec = FindRecord(p.name);
        if (!rec) continue;
        WriteRecordFloats(rec, RES_BASE_RUB, p.baseRub, RES_BASE_USD, p.baseUsd);
    }
}

// After it, because the pass overwrites +0x58/+0x5C for every record it does
// not skip. This is the half that gives a price to a resource no building type
// produces - for those the solver returns a zeroed register and the base is
// never consulted at all.
static void ApplyForcedPrices(void)
{
    for (int i = 0; i < g_priceCount; i++)
    {
        PriceEntry& p = g_price[i];
        if (!p.hasPrice) continue;

        BYTE* rec = FindRecord(p.name);
        if (!rec)
        {
            Logf("price     \"%s\" has no record - nothing forced. Is it in [list], "
                 "and did it publish?", p.name);
            continue;
        }
        if (!WriteRecordFloats(rec, RES_PRICE_RUB, p.priceRub, RES_PRICE_USD, p.priceUsd))
            Logf("price     \"%s\": record is not writable (%lu)", p.name, GetLastError());
    }
}

static void ReportOnePrice(const char* name)
{
    BYTE* rec = FindRecord(name);
    if (!rec) { Logf("price     %-20s not in the vector", name); return; }

    Logf("price     %-20s %10.2f RUB %10.2f USD   base %.2f / %.2f   kind %d",
         name,
         *(float*)(rec + RES_PRICE_RUB), *(float*)(rec + RES_PRICE_USD),
         *(float*)(rec + RES_BASE_RUB),  *(float*)(rec + RES_BASE_USD),
         *(int*)(rec + RES_KIND_OFF));
}

// What the engine actually decided, which is the only thing that answers "why
// is this one zero". A price of 0.00 with a non-zero base means nothing
// produces the resource; a price of 0.00 with a zero base means neither does.
static void ReportPrices(void)
{
    if (!g_priceReport) return;

    for (int i = 0; i < g_regCount; i++)
        ReportOnePrice(g_reg[i].name);

    for (int i = 0; i < g_priceCount; i++)
    {
        bool listed = false;
        for (int r = 0; r < g_regCount; r++)
            if (_stricmp(g_reg[r].name, g_price[i].name) == 0) listed = true;
        if (!listed) ReportOnePrice(g_price[i].name);
    }
}

// The bracket. Base in before the pass, forced price out after it, and the
// report last - one hook rather than two, because both halves are keyed to the
// same call and doing them anywhere else means racing whatever wrote last.
typedef void (*t_PricePass)(void*);
static t_PricePass o_PricePass;

static void h_PricePass(void* game)
{
    EnterCriticalSection(&g_lock);
    if (g_nameOff < 0 && !g_warnedNoLayout)
    {
        g_warnedNoLayout = true;
        Logf("price     the record layout is not known yet - no price is applied "
             "to this pass. Expect the next one to work");
    }
    __try { ApplyBasePrices(); }
    __except (FaultFilter("resources price base", GetExceptionInformation())) {}
    LeaveCriticalSection(&g_lock);

    o_PricePass(game);

    EnterCriticalSection(&g_lock);
    __try { ApplyForcedPrices(); ReportPrices(); }
    __except (FaultFilter("resources price force", GetExceptionInformation())) {}
    LeaveCriticalSection(&g_lock);
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
                            name, g_reg[i].resolved, rec);
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

// ---------------------------------------------------------------- customs
//
// A customhouse (media_soviet/buildings_types/zoll_*.ini, $TYPE_CUSTOMHOUSE)
// declares its trade storages as plain $STORAGE lines, one per transport
// class - confirmed against zoll_sahy.ini, whose $STORAGE lines name no
// resource at all, unlike a shop's $STORAGE_DEMAND_* or a pharmacy's
// $STORAGE_SPECIAL. That slot list is built once, while building.ini is
// parsed - rva 0xE40F0, FUN_1400e40f0(parser, storage, class, capacity,
// resource), documented in full in plugins/needs/needs.cpp - by walking
// whatever this vector looks like at that moment. A customhouse already
// standing on a map, or built in an earlier session, freezes its trade list
// at whatever resources existed then: every name [list] above declares
// afterwards is otherwise perfectly tradeable - matching transport class, a
// computed price, trucks that will happily deliver it - but has no slot to be
// sold from. Confirmed empirically before this was written: demolishing and
// rebuilding an affected customhouse fixes it immediately.
//
// The fix reruns that same "does this storage have a slot for every resource
// its class can carry" walk against the *live* resource vector, once per
// customhouse, the first time each one ticks after this plugin loads. Every
// structure offset below (B_STORAGES, ST_CLASS, ST_CAPACITY,
// P_CUSTOMS_SLOT_PUSH_RVA, the two-parallel-vectors gotcha) is the one
// plugins/needs/needs.cpp already found for a shop's storage; only the tick
// address and the walk itself are new here.
//
//   rva 0x185470  FUN_140185470(game, building) - the type's own tick, and
//                 the counterpart of the shop tick (rva 0x171DA0) needs.cpp
//                 hooks for the same reason. Confirmed by disassembly rather
//                 than taken on faith: the building-type dispatcher (rva
//                 0x139A80) calls it only from
//                 `cmp dword ptr [rax+0x360],0x14 / jne .. / call 0x185470`
//                 at rva 0x13E30A - 0x14 is BUILDINGTYPE_CUSTOMHOUSE, and
//                 `rax` there is `building+0x318`, the same type-descriptor
//                 pointer TYPEDESC_OFF names in needs.cpp. The function also
//                 spawns tourists on its own timer and tail-jumps into a
//                 second, shared function afterwards; neither matters here -
//                 a post-hook only needs it called once per customhouse per
//                 tick, which the dispatcher already guarantees.
#define P_CUSTOMHOUSE_TICK_RVA 0x185500  // v1.1.1.13; was 0x1854E0

static const BYTE kCustomsTickPrologue[] = {
    0x48, 0x8B, 0xC4,                            // mov  rax,rsp
    0x48, 0x89, 0x58, 0x18,                      // mov  [rax+0x18],rbx
    0x56,                                        // push rsi
    0x48, 0x81, 0xEC, 0x90, 0x00, 0x00, 0x00     // sub  rsp,0x90
};

#define P_CUSTOMS_SLOT_PUSH_RVA 0x0B14E0   // std::vector<Slot>::push_back, 16-byte value

#define B_STORAGES         0x970      // building -> vector<Storage>, stride 0xE0
#define STORAGE_STRIDE     0xE0
#define ST_SLOTS2          0x18
#define ST_CAPACITY        0x8C
#define ST_CLASS           0x90
#define SLOT_SIZE          0x10

// RES_CLASS_FACTOR, RES_CLASS_BLOCKED and RES_CLASS_STRIDE are the record's own
// transport-class layout and are declared with the rest of it at the top of this
// file. Both readings are the same two fields of the same 0x20-byte block: the
// capacity factor the storage code multiplies by, and the flag this walk treats
// as "not tradeable in this class".

struct Slot { void* res; float content; float limit; };

typedef void (*t_CustomsTick)(void*, void*);
typedef void (*t_SlotPush)(void*, const void*);

static t_CustomsTick o_CustomsTick;
static t_SlotPush    o_CustomsSlotPush;

static int   g_customsHook    = 1;
static int   g_customsProbe   = 0;
static DWORD g_customsTickRva = P_CUSTOMHOUSE_TICK_RVA;
static LONG  g_customsSlotsAdded;

static int CustomsSlotCount(BYTE* storage)
{
    ResVector* v = (ResVector*)storage;
    if (!v->begin || v->end < v->begin) return -1;
    return (int)((size_t)(v->end - v->begin) / SLOT_SIZE);
}

static int CustomsSlotIndexOf(BYTE* storage, const void* rec)
{
    ResVector* v = (ResVector*)storage;
    if (!v->begin || v->end < v->begin) return -1;

    size_t span = (size_t)(v->end - v->begin);
    if (span % SLOT_SIZE) return -1;

    for (size_t i = 0; i < span / SLOT_SIZE; i++)
        if (((Slot*)(v->begin + i * SLOT_SIZE))->res == rec) return (int)i;
    return -1;
}

// One trade storage. Walks the live resource vector and adds a slot for
// every resource this storage's transport class can carry and does not
// already have - which, on a customhouse built before that resource existed,
// is every resource declared since. Reads through g_vecRva rather than a
// second hard-coded vector address, so a `resource_vector_rva` override above
// applies here too.
static void ExtendCustomsStorage(BYTE* storage)
{
    int cls = *(int*)(storage + ST_CLASS);
    if (cls < 0 || cls > 31) return;

    float capacity = *(float*)(storage + ST_CAPACITY);
    if (capacity <= 0.0f) return;

    ResVector* rv = (ResVector*)(g_exeBase + g_vecRva);
    if (!ReadablePtr(rv, sizeof(*rv)) || !rv->begin || rv->end < rv->begin) return;
    size_t resCount = (size_t)(rv->end - rv->begin) / RES_STRIDE;

    int slots = CustomsSlotCount(storage);
    if (slots < 0) return;

    // DO NOT CLONE AN EXISTING SLOT'S NUMBERS HERE - unlike a shop's slot,
    // where `limit` is a stocking target unrelated to any one resource's
    // identity, a customhouse's per-resource fields are its *trade*
    // configuration: the script API exposes bImport/bExport on the storage
    // itself (media_soviet/scripts/SOVIETInstructions.txt, struct Storage),
    // which means the per-slot content/limit this plugin writes is what
    // decides how much of *that* resource to move, at whatever direction the
    // storage is already set to. An earlier version copied `limit` from
    // whatever slot happened to be first in the storage - so a brand-new
    // resource silently inherited an unrelated, already-configured resource's
    // trade amount, and the customhouse began moving it - spending money on
    // an import nobody asked for - before a single truck had ever arrived
    // with it. A limit of zero is what an unconfigured resource in a real
    // customhouse already looks like: present in the list, tradeable, and
    // inert until the player sets a real amount from the trade panel, exactly
    // as for any other resource there. Only the trade *amount* is left at
    // zero, though - the resource identity in the parallel array below is not
    // optional, see the save-crash note further down.
    ResVector* v2 = (ResVector*)(storage + ST_SLOTS2);

    for (size_t i = 0; i < resCount; i++)
    {
        BYTE* rec = rv->begin + i * RES_STRIDE;
        if (!ReadablePtr(rec, RES_STRIDE)) continue;

        size_t off     = (size_t)RES_CLASS_FACTOR + (size_t)cls * RES_CLASS_STRIDE;
        float  factor  = *(float*)(rec + off);
        char   blocked = *(char*)(rec + RES_CLASS_BLOCKED + (size_t)cls * RES_CLASS_STRIDE);
        if (factor <= 0.0f || blocked) continue;

        if (CustomsSlotIndexOf(storage, rec) >= 0) continue;   // already tradeable here

        // The parallel array only grows when it still matches the slot
        // count - anything else is a shape this was not written for, exactly
        // the guard needs.cpp uses on the same pair of vectors.
        bool sideOk = ReadablePtr(v2, sizeof(*v2)) && v2->end >= v2->begin &&
                      (size_t)(v2->end - v2->begin) == (size_t)CustomsSlotCount(storage) * SLOT_SIZE;

        // ST_SLOTS2 IS A SECOND vector<Slot>, NOT AN OPAQUE BLOB - confirmed by
        // disassembling the customhouse's own save-serialiser, rva 0x1EC470: it
        // walks *this* array (storage+0x18/+0x20, not the main one at +0x00),
        // and for every 16-byte entry reads +0x00 as a resource pointer whose
        // name it copies byte-by-byte into the save. A version that pushed an
        // all-zero entry here - meant to stop a new resource inheriting a
        // neighbour's trade amount, see the money fix below - left that pointer
        // null, and crashed every save that touched an augmented customhouse
        // the moment the save writer reached it (ACCESS_VIOLATION reading
        // address 0 at that exact `mov al,[rcx]`). The fix is the same shape as
        // the main slot with the same resource, not an empty one.
        Slot s = { rec, 0.0f, 0.0f };     // never conjure stock, and never a trade amount

        if (sideOk) o_CustomsSlotPush(storage + ST_SLOTS2, &s);
        o_CustomsSlotPush(storage, &s);

        g_customsSlotsAdded++;

        if (g_customsProbe)
        {
            char nm[40] = "?";
            SafeReadStr(rec, nm, sizeof(nm));
            Logf("customs   storage %p class %d: added \"%s\" (class factor %.3f, "
                 "starts untraded)", storage, cls, nm, factor);
        }
    }
}

// ------------------------------------------------- the customhouses that already exist

#define MAX_CUSTOMHOUSES 64
static void* g_customsSeen[MAX_CUSTOMHOUSES];

static bool CustomsAlreadyDone(void* b)
{
    unsigned hash = (unsigned)((((size_t)b) >> 4) * 2654435761u);
    int home = (int)(hash % MAX_CUSTOMHOUSES);

    for (int n = 0; n < 16; n++)
    {
        int i = (home + n) % MAX_CUSTOMHOUSES;
        if (g_customsSeen[i] == b) return true;
        if (g_customsSeen[i] == NULL) { g_customsSeen[i] = b; return false; }
    }
    return false;   // table full - process again rather than skip forever
}

static void h_CustomsTick(void* game, void* building)
{
    o_CustomsTick(game, building);
    if (!g_customsHook || !building) return;
    if (CustomsAlreadyDone(building)) return;

    __try
    {
        BYTE*      b        = (BYTE*)building;
        ResVector* storages = (ResVector*)(b + B_STORAGES);
        if (!ReadablePtr(storages, sizeof(*storages)) || !storages->begin) return;

        size_t count = (size_t)(storages->end - storages->begin) / STORAGE_STRIDE;
        for (size_t i = 0; i < count; i++)
            ExtendCustomsStorage(storages->begin + i * STORAGE_STRIDE);

        Logf("customs   customhouse %p synced (%ld slot(s) added so far, all customhouses)",
             building, g_customsSlotsAdded);
    }
    __except (FaultFilter("customs storage", GetExceptionInformation()))
    {
        Logf("customs   disabled after a fault");
        g_customsHook = 0;
    }
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
    info->version = "1.7";

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
    // Read as a string, not through configInt: GetPrivateProfileInt answers 0
    // for anything negative, which would silently turn `resource_capacity = -1`
    // - "never move the array" - into "size it automatically", the opposite.
    if (H->configString(ini, "resources", "resource_capacity", v, sizeof(v), "") && v[0])
        g_wantCapacity = (int)strtol(v, NULL, 0);

    g_priceHook    = H->configInt(ini, "resources", "price_hook", g_priceHook);
    g_priceReport  = H->configInt(ini, "resources", "price_report", g_priceReport);
    g_customReport = H->configInt(ini, "resources", "custom_report", g_customReport);
    if (H->configString(ini, "resources", "price_pass_rva", v, sizeof(v), "") && v[0])
        g_priceRva = (DWORD)strtoul(v, NULL, 0);

    g_customsHook  = H->configInt(ini, "customs", "hook",  g_customsHook);
    g_customsProbe = H->configInt(ini, "customs", "probe", g_customsProbe);
    if (H->configString(ini, "customs", "customhouse_tick_rva", v, sizeof(v), "") && v[0])
        g_customsTickRva = (DWORD)strtoul(v, NULL, 0);

    g_hRes = TsmOpenLog("tesmioloader.resources.log");
    const char* hdr = "; name                     which-arg  return value   discovering call site\r\n";
    WriteTo(g_hRes, hdr, (int)strlen(hdr));

    if (g_resHook >= 2) LoadResourceRegistry();

    // Not hooks - the addresses are taken so mod resources can be given cargo
    // meshes of their own, the same three calls the engine's table makes.
    if (void** s = FindIatSlot(g_exe, DLL_ENGINE, SYM_CREATE_MESH)) o_CreateManagedMesh = (t_CreateManagedMesh)*s;
    if (void** s = FindIatSlot(g_exe, DLL_ENGINE, SYM_MESH_LOAD))   o_MeshLoadFromFile  = (t_MeshLoadFromFile)*s;
    if (void** s = FindIatSlot(g_exe, DLL_ENGINE, SYM_MESH_MTL))    o_MeshLoadMaterial  = (t_MeshLoadMaterial)*s;
    if (void** s = FindIatSlot(g_exe, DLL_ENGINE, SYM_FILE_EXISTS)) o_FileExists        = (t_FileExists)*s;
    if (!o_FileExists)
        Logf("resource  WARN  no import slot for C3DHelp_CheckIfFileExist - a mod resource with "
             "no assets will fault the engine instead of being skipped");
    if (!o_CreateManagedMesh || !o_MeshLoadFromFile)
        Logf("resource  WARN  no import slot for the mesh loader - mod resources keep the template's cargo models");

    if (!InstallInlineHook(g_exeBase + g_resRva, (void*)h_ResourceGet,
                           (void**)&o_ResourceGet, kResourceGetPrologue,
                           STOLEN_BYTES, "ResourceGet"))
        return 1;

    // Captions for mod resources. Every string in the game comes through here,
    // so it is hooked whatever mode we are in - the ids we mint are answered
    // locally and nothing else is touched. Installed only now, after
    // ResourceGet is confirmed: LoadOnePlugin's contract is that a plugin
    // returning non-zero from Init has hooked nothing, because it then calls
    // FreeLibrary on this DLL - a GetString hook installed before the
    // ResourceGet refusal above would have pointed the game's import slot at
    // code that had just been unmapped, and the next menu string would have
    // jumped into freed memory instead of crashing cleanly.
    PatchIat(g_exe, DLL_ENGINE, SYM_GET_STRING, (void*)h_GetString,
             (void**)&o_GetString, "C3D_LANGUAGE::GetString");

    // The price bracket. Installed whenever there is something to say - an
    // override to apply or a table to print - and skipped entirely otherwise,
    // so a .ini with no money in it costs nothing. A refusal here is not fatal:
    // resources still exist, they are just priced by the engine alone.
    bool wantPrices = false;
    for (int i = 0; i < g_priceCount; i++)
        if (g_price[i].hasBase || g_price[i].hasPrice) wantPrices = true;

    if (g_priceHook && (wantPrices || g_priceReport))
    {
        if (InstallInlineHook(g_exeBase + g_priceRva, (void*)h_PricePass,
                              (void**)&o_PricePass, kPricePassPrologue,
                              sizeof(kPricePassPrologue), "resource price pass"))
            Logf("price     bracket on 0x%lX: %d name(s) priced, report %s",
                 g_priceRva, g_priceCount, g_priceReport ? "on" : "off");
        else
            Logf("price     WARN  no hook on the price pass - [base_price] and "
                 "[price] do nothing this session");
    }

    // The customhouse tick. Post-hook: let the game run its own frame first,
    // then top up whatever that customhouse's trade storages are still
    // missing. Declining this hook only means an existing customhouse stays
    // blind to a resource declared after it was built - new construction is
    // unaffected, because 0xE40F0 already sees everything live at that point.
    if (g_customsHook)
    {
        o_CustomsSlotPush = (t_SlotPush)(g_exeBase + P_CUSTOMS_SLOT_PUSH_RVA);

        if (InstallInlineHook(g_exeBase + g_customsTickRva, (void*)h_CustomsTick,
                              (void**)&o_CustomsTick, kCustomsTickPrologue,
                              sizeof(kCustomsTickPrologue), "customhouse tick"))
            Logf("customs   hooked - existing customhouses will pick up resources "
                 "declared after they were built, the next time each one ticks");
        else
        {
            g_customsHook = 0;
            Logf("customs   no customhouse-tick hook - existing customhouses stay "
                 "blind to resources added after they were built");
        }
    }
    else
        Logf("customs   hook = 0 - customhouses trade only what they were built with");

    int nCustom = 0;
    for (int i = 0; i < g_regCount; i++) if (g_reg[i].custom) nCustom++;
    Logf("resource  %d name(s) declared: %d cloned from a template, %d built from scratch, "
         "%d [custom:] section(s) read", g_regCount, g_regCount - nCustom, nCustom, g_customCount);

    Logf("resource  hook mode=%d rva=0x%lX, %d name(s) declared, array %s",
         g_resHook, g_resRva, g_regCount,
         g_wantCapacity < 0 ? "left at the engine's own size" :
         g_wantCapacity > 0 ? "grown to [list] or the configured floor, whichever is larger"
                            : "grown to fit [list]");
    H->provide(TSM_SERVICE_RESOURCES, TSM_RESOURCES_VERSION, &kResourceApi);
    return 0;
}

BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_DETACH && g_hRes != INVALID_HANDLE_VALUE)
        CloseHandle(g_hRes);
    return TRUE;
}
