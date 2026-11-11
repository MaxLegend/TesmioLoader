// cities - a per-city radius and shape, as a tesmioloader plugin.
//
// A city in this game is a "name point": a name, a world position, and a list
// of the buildings that belong to it. Which buildings those are is decided in
// exactly one function, and it is decided by one hard-coded number.
//
//   0x3305F0  NamePoint* FindCity(Game*, const C3DVECTOR3* pos, bool create)
//
//     limit = [0x90B214]            ; 1e6  - the radius SQUARED, so 1000 m
//     best  = [0x90B23C]            ; 1e10 - the "nothing yet" sentinel
//     for (np : game->namepoints)   ; game+0x10348 .. +0x10350, vector<NamePoint*>
//         if (np[0x118]) continue                       ; transient skip flag
//         d = C3DCalcDistanceSq(pos, np+0x04)           ; 3D, squared
//         if (limit <= d) continue
//         if (best  <= d) continue
//         best = d; result = np
//     if (result) return result
//     if (!create) return 0
//     ... pick a random unused name, AddNamePoint, ReassignBuildingsNear ...
//
// So the radius is one `movss xmm7,[rip+disp32]` reading 1e6, shared by every
// city on the map, and there is nowhere in the NamePoint that a per-city figure
// could live. That is the whole of what this plugin changes.
//
//
// THE SHAPE OF THE PATCH
//
// The search is replaced, the creation is not. An inline hook on 0x3305F0 runs
// this plugin's own search - per-city radius, circle or square - and returns
// what it finds. When it finds nothing and `create` is set it calls the
// original, which is the game's own "invent a name, allocate a NamePoint, push
// it into the vector, reassign the neighbourhood" path. **Nothing about
// creating a city is reimplemented here**, which is the point: a city this
// plugin causes to exist is indistinguishable from one the game made.
//
// For that to work the original's own search has to fail, or it would answer
// from the vanilla 1000 m before ever reaching the creation half. The one
// `movss` at 0x330658 is therefore repointed at a float this plugin owns,
// holding 0.0f: `comiss xmm7,xmm0 / jbe skip` then skips every candidate,
// whatever the distance. The literal at 0x90B214 is left alone - it sits in the
// shared pool with unrelated readers, exactly as 480.0f does for `walking`.
//
// The reassignment is replaced outright (0x32FEA0), for two reasons. It carries
// its own copy of the 1000 m figure as a box pre-filter, and - more
// importantly - it dereferences the *old* city without checking it for null.
// Vanilla never has a building without a city; this plugin can produce one for
// as long as it takes to adopt it, so the null-safe version is not optional.
//
//
// WHERE THE RADIUS LIVES
//
// Nowhere in the game's own structures. The NamePoint is 0x1C0 bytes with no
// spare field whose meaning is known, and its save record is a fixed 0x130
// bytes the loader reads field by field:
//
//   +0x00 float x,y,z      +0x0C wchar name[128]   +0x10C u32   +0x110 u32
//   +0x114 u8              +0x118 u32 buildingCount +0x11C u32 hasBlob
//
// followed by `buildingCount` u32 building indices and, when hasBlob, 0x80
// more bytes. So this plugin keeps its own table, keyed by NamePoint*, and
// serialises it **appended to namepoints.bin after everything the game
// writes**, keyed by position in the vector - which is the same key the game's
// own loader uses (record i -> vector[i]). The game reads exactly as many
// records as there are name points and then stops, so the extra block is
// invisible to it and a save stays loadable with the plugin removed.
//
// The two call sites are rewritten rather than hooked: `call 0x333440` at
// 0x42DB95 saves, `call 0x3337D0` at 0x431D56 loads, and both hand over the
// FILE* this plugin needs. Same technique as the deposits plugin's world-map
// save. fread/fwrite are taken from the game's own import table, because a
// FILE* from the game's CRT means nothing to the plugin's own.
//
//
// THE WINDOW
//
// 0x7CEDB0 builds the city window - the one with "Date established" and the
// "Allow citizens to move in" checkboxes - and leaves its bottom edge in
// window+0x250. A post-hook reads that, draws a slider and a checkbox there
// with the game's own widgets (0x272780 is the 0..100 slider, 0x273AF0 the
// checkbox), and writes the new bottom back, which is what makes the frame grow
// by two rows instead of clipping them. Same trick as `depletion`'s reserve row
// on the mine window.
//
// A window is a city window when window+0x248 == 3; window+0x240 is then the
// NamePoint. Both come from the dispatcher at 0x6EA0E3, which is also where the
// city window's relationship with the city hall lives: a working $TYPE_CITYHALL
// (building type 0x27) in the city makes the dispatcher draw that building's
// own panel inside the city window as well.
//
//
// WHAT HAPPENS TO A BUILDING THAT FALLS OUTSIDE
//
// Shrinking a city can leave buildings with no city at all, and the game has no
// concept of that. After every change the plugin rebuilds the whole assignment
// (RebuildAll) and then walks the buildings once more; anything still without a
// city is handed to FindCity(create=true) - the game's own creation path, with
// the game's own random name and the game's own "date established". Creating
// one city adopts every orphan inside its radius, so the pass converges in as
// many creations as there are genuinely separate clusters.
//
// Everything here is addresses for SOVIET64.exe v1.1.1.9, verified byte for
// byte before anything is written. See docs/14-cities.md.

#include "../../src/tesmio_plugin.h"

// ---------------------------------------------------------------- the game

// The game object is a static, not a pointer: the save loader reaches the same
// vectors both ways, `[0x9E5258]` and `[gameobj+0x10348]`, and 0x9E5258 -
// 0x10348 is 0x9D4F10. Confirmed again at 0x4380AB, `lea rcx,[0x9D4F10]` right
// before a call that takes the game as its first argument.
#define P_GAMEOBJ            0x9D4F10

#define G_CITY_BEGIN         0x10348   // vector<NamePoint*>
#define G_CITY_END           0x10350
#define G_BLD_BEGIN          0x11B08   // vector<Building*>
#define G_BLD_END            0x11B10

// NamePoint, 0x1C0 bytes, operator new'd by AddNamePoint at 0x32F3B0.
#define NP_POS               0x04      // float x, y, z
#define NP_NAME              0x10      // wchar_t[128]
#define NP_SKIP              0x118     // set while the "can I delete this" scan runs
#define NP_BLD               0x1A0     // vector<Building*>: begin, end, capacity

// Building.
#define B_CITY               0x108     // NamePoint*, or null
#define B_TYPEDESC           0x318     // the type record; null on scenery
#define B_NODE               0x320     // C3D_NODE, for GetPosition
#define B_BBOX               0x5D0     // ->+0x20 min, ->+0x28 max, floats at +4

// ---------------------------------------------------------------- functions

// v1.1.1.9 rvas throughout this block. Old values are noted per line;
// docs/02-findings.md has the derivation. P_FIND_CITY/P_REASSIGN/P_CITY_WINDOW
// are re-verified by their prologue at load; P_RENUMBER/P_VEC_PUSH/P_SLIDER/
// P_CHECKBOX are not - see the note by BindImports.
#define P_FIND_CITY          0x330610  // v1.1.1.13; was 0x330690. NamePoint* (Game*, const Vec3*, bool create)
#define P_REASSIGN           0x32FEC0  // v1.1.1.13; was 0x32FF40. void (Game*, const Vec3*)
#define P_RENUMBER           0x330330  // v1.1.1.13; was 0x3303B0. void (ignored, NamePoint*) - house numbers
#define P_VEC_PUSH           0xB0E90   // unchanged. void (vector<T*>*, T** value) - push_back
#define P_CITY_WINDOW        0x7CF100  // v1.1.1.13; was 0x7CEF60. void (Game*, Window*, int captionId)
#define P_SAVE_CITIES        0x333460  // v1.1.1.13; was 0x3334E0. void (Game*, FILE*)
#define P_LOAD_CITIES        0x3337F0  // v1.1.1.13; was 0x333870. void (Game*, FILE*)
#define P_SAVE_CALL          0x42DBC5  // v1.1.1.13; was 0x42DC35. the one `call 0x3334E0`
#define P_LOAD_CALL          0x431D86  // v1.1.1.13; was 0x431DF6. the one `call 0x333870`
#define P_SLIDER             0x272770  // v1.1.1.13; was 0x2727F0. the 0..100 slider with - and + buttons
// The checkbox the city window itself uses for "Allow citizens to move into
// this city", one row above ours. It takes its **label as a string** rather
// than a text id, so it needs no language entry, and - the reason it is used
// here rather than the plainer 0x273AF0 - its only gates are the hit test and
// its own `live` argument:
//
//   0x273AF0   if (hit && game[0xAD74] == -1) { if (latch) { latch = 0; toggle } }
//   0x395650   if (hit && live)               { if (latch) {            toggle } }
//
// `game+0xAD74` is the widget currently being edited or dragged; the window
// dispatcher clears `window+0x20` whenever it is set (`0x6EA214`), so anything
// that claims it makes 0x273AF0 inert for the rest of the frame. 0x273AF0 also
// **consumes** the latch, which the labelled one leaves alone.
#define P_CHECKBOX           0x395630  // v1.1.1.13; was 0x3956F0
#define P_WORLD_LOAD         0x430F50  // v1.1.1.13; was 0x430FC0. void (…) - the whole world load
#define P_WORLD_LOAD_CALL    0x2942EB  // v1.1.1.13; was 0x29436B. the one `call 0x430FC0`
#define P_MOUSE_DOWN         0xA54E90  // non-zero while the left button is held (a .data
                                       // global - v1.1.1.9 did not move .data at all)

// The one `movss xmm7,[rip+disp32]` that loads the search limit, and the two
// that load the 1000 m box the delete checks pre-filter with. 0x32FEA0's own
// copy is not in the list: that whole function is replaced. v1.1.1.9 rvas;
// old values noted per line.
#define RVA_LIMIT_SITE       0x330678  // v1.1.1.13; was 0x3306F8
#define RVA_LIMIT_CONST      0x90B208  // v1.1.1.13; was 0x90B1FC. 1e6 = 1000 m squared
#define RVA_BOX_DELETE       0x32F59D  // v1.1.1.13; was 0x32F61D. in DeleteCity
#define RVA_BOX_CANDELETE    0x32F882  // v1.1.1.13; was 0x32F902. in CanDeleteCity
#define RVA_BOX_CONST        0x90B08C  // v1.1.1.13; was 0x90B07C. 1000

#define VANILLA_LIMIT_SQ     1000000.0f
#define VANILLA_BOX          1000.0f
#define VANILLA_RADIUS       1000.0f
#define SENTINEL_FAR         1e10f     // what the vanilla search starts `best` at

// ---------------------------------------------------------------- the dots
//
// The blue grid the game paints on the ground while a city window is open, in
// `0x30D100` at `0x3125A0`. It is a **scan**, not a shape:
//
//   if (window[0x248] != 3) return          ; only the first window, and only a city
//   np = window[0x240]
//   for (x = np.x - R;  x <= np.x + R + 5;  x += 50)
//       h = sqrtf(R*R - (x - np.x)^2)       ; the chord of the circle at this x
//       for (z = np.z - h;  z <= np.z + h + 5;  z += 50)
//           if (FindCity(&(x,?,z), 0) == np)  draw a dot
//
// so **the dot is drawn by asking the search**, and the search is this plugin's.
// What is wrong is only the region it walks: a circle of the vanilla 1000 m,
// which is why raising the radius past that showed nothing new and why a square
// looked exactly like a circle - the corners are simply never sampled.
//
// Five sites. R twice (start bound and end bound), R squared once (the chord),
// and the 50 m step twice - the step has to follow the radius or a 5 km city
// walks 200x200 points, each running the search over every city on the map.
// v1.1.1.9 rvas; old values noted per line.
#define RVA_OVL_R_START      0x312341  // v1.1.1.13; was 0x3123C1. movss xmm11,[0x90B07C]
#define RVA_OVL_RSQ          0x3125FC  // v1.1.1.13; was 0x31267C. movss xmm11,[0x90B1FC]
#define RVA_OVL_STEP_Z       0x312731  // v1.1.1.13; was 0x3127B1. addss xmm6 ,[0x90AA28]
#define RVA_OVL_STEP_X       0x312750  // v1.1.1.13; was 0x3127D0. addss xmm8 ,[0x90AA28]
#define RVA_OVL_STEP_CONST   0x90AA38  // v1.1.1.13; was 0x90AA28. 50
#define VANILLA_OVL_STEP     50.0f

// The x loop's END bound is the one that could not be repointed, and it is
// worth writing down why. It reads the same 1000 into **xmm9**:
//
//   0x31273E  movaps xmm0,xmm2          ; xmm0 = np.x
//   0x312741  movss  xmm9,[0x90B094]    ; 1000
//   0x31274A  addss  xmm0,xmm9
//   0x31274F  addss  xmm0,xmm13         ; + 5, slack at the exact boundary
//   0x312754  comiss xmm0,xmm8
//   0x312758  jae    <next column>
//
// and **xmm9 survives the block**: `divss xmm0,xmm9` at 0x314CC2, ten kilobytes
// later in the same function, divides an unrelated integer by it. That is what
// the two `movss xmm9,[0x90B094]` at 0x312760 and 0x31276B are for - they put
// 1000 back on the paths that skip the loop. Repointing the read would have
// quietly changed that division every time a city window was open.
//
// So the comparison is rewritten instead of the constant: fourteen bytes become
// `addss xmm0,[our radius]` + the same `comiss`, and xmm9 keeps its 1000. The
// five-metre slack goes with it, which is invisible against a fifty-metre grid.
// xmm11 and the two step reads have no such downstream reader - checked the
// same way - which is why those three are ordinary repoints.
#define RVA_OVL_END_CMP      0x31276A  // v1.1.1.13; was 0x3127EA
static const BYTE kOvlEndCmp[] = {
    0xF3, 0x41, 0x0F, 0x58, 0xC1,       // addss  xmm0,xmm9
    0xF3, 0x41, 0x0F, 0x58, 0xC5,       // addss  xmm0,xmm13
    0x41, 0x0F, 0x2F, 0xC0              // comiss xmm0,xmm8
};

// A square of half-extent r has its corners at r*sqrt(2), so a circular scan of
// that radius covers all of it. Everything outside the square is rejected by
// the search anyway, so the scan region only ever has to be a superset.
#define SQUARE_REACH         1.41422f

// ---------------------------------------------------------------- the window

#define W_HIDDEN             0x01      // non-zero while the panel is not drawn
#define W_X                  0x04
#define W_Y                  0x08
#define W_ACTIVE             0x20      // the window accepts input
#define W_OFFX               0x28
#define W_OFFY               0x2C
#define W_OBJ                0x240     // NamePoint* on a city window
#define W_KIND               0x248     // 3 == city window
#define W_BOTTOM             0x250     // the running Y, written at the end
#define W_KIND_CITY          3

#define P_DPI                0x992088  // every layout constant is multiplied by this
#define P_FONTMGR            0x996FB0  // C3D_FONTMANAGER, by address
#define P_ROW_FONT           0x994200  // the C3D_FONT* the city window's rows use
// v1.1.1.9; .rdata moved by a uniform -0x18, each value confirmed still there.
#define LAYOUT_15            0x90A8DC  // v1.1.1.13; was 0x90A8CC. 15
#define LAYOUT_20            0x90A938  // v1.1.1.13; was 0x90A928. 20 - a checkbox
#define LAYOUT_25            0x90A978  // v1.1.1.13; was 0x90A968. 25 - how far 0x395650 draws ABOVE its y
#define LAYOUT_35            0x90A9DC  // v1.1.1.13; was 0x90A9CC. 35 - one row
#define ROW_COLOUR           0xFF990000u
#define P_WIDGET_BUSY        0xAD74    // game -> the widget being edited, or -1
#define P_CLICK_LATCH        0xA54E91  // a click nobody has consumed yet

// ---------------------------------------------------------------- prologues

// Checked byte for byte before anything is written. All three are position
// independent - the host copies them into a trampoline without relocating.
static const BYTE kFindCityPrologue[] = {
    0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x18, 0x55, 0x56, 0x57,
    0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57
};
static const BYTE kReassignPrologue[] = {
    0x48, 0x8B, 0xC4, 0x48, 0x89, 0x50, 0x10, 0x48, 0x89, 0x48, 0x08,
    0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57
};
static const BYTE kWindowPrologue[] = {
    0x48, 0x8B, 0xC4, 0x55, 0x56, 0x57,
    0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57
};

// ---------------------------------------------------------------- types

typedef void*  (*t_FindCity)(void*, const float*, unsigned char);
typedef void   (*t_Reassign)(void*, const float*);
typedef void   (*t_Renumber)(void*, void*);
typedef void   (*t_VecPush)(void*, void*);
typedef void   (*t_CityWindow)(void*, void*, int);
typedef void   (*t_CityIo)(void*, void*);
typedef float* (*t_VecGetter)(void*, float*);
typedef void   (*t_PrintLeftUnicode)(void*, void*, float, float, unsigned long,
                                     const wchar_t*, ...);
typedef size_t (*t_Fread)(void*, size_t, size_t, void*);
typedef size_t (*t_Fwrite)(const void*, size_t, size_t, void*);

// The game's own widgets. Both draw, hit-test and mutate `value` themselves,
// and both return non-zero on the frame the player changed it.
typedef int (*t_Slider)(void* game, float x, float y, int textId, unsigned* value,
                        float scale, char enabled, int step, char drag, char* hover);
// The labelled checkbox, argument for argument off the city window's own call
// at 0x7CF7F9: box size in `size`, the label drawn `size`-relative to its right,
// a null font meaning the panel default, and the last three exactly as the game
// passes them. `live` is `window+0x20`.
typedef int (*t_Checkbox)(void* game, float x, float y, unsigned* value,
                          const wchar_t* label, unsigned colour, void* font,
                          float size, float labelDy, char live, char p11, char p12);

static t_FindCity         o_FindCity;
static t_Reassign         o_Reassign;
static t_Renumber         o_Renumber;
static t_VecPush          o_VecPush;
static t_CityWindow       o_CityWindow;
static t_CityIo           o_SaveCities;
static t_CityIo           o_LoadCities;
static t_VecGetter        o_NodeGetPosition;
static t_PrintLeftUnicode o_PrintLeftUnicode;
static t_Fread            o_Fread;
static t_Fwrite           o_Fwrite;
static t_Slider           o_Slider;
static t_Checkbox         o_Checkbox;

// ---------------------------------------------------------------- settings

static int   g_enabled     = 1;
static int   g_window      = 1;        // the two extra rows
static int   g_adopt       = 1;        // create a city for whatever falls outside
static float g_defRadius   = VANILLA_RADIUS;
static int   g_defSquare   = 0;
static float g_radiusMin   = 200.0f;
static float g_radiusMax   = 5000.0f;
static int   g_step        = 2;        // slider percent per - / + click
static float g_labelX      = 10.0f;    // both in unscaled layout units, from the
static float g_widgetX     = 200.0f;   // panel's own left edge
static float g_checkDy     = 25.0f;    // pushes the checkbox back onto its row
static int   g_overlay     = 1;        // make the ground dots follow radius and shape
static float g_overlayStep = 0.0f;     // 0: scale the grid with the radius
static int   g_probe       = 0;

// The neutered search limit and the widened box, in memory this plugin owns.
// AllocNear puts them within +-2GB of the executable, which is what makes a
// 32-bit displacement reach.
#define SLOT_ZERO   0
#define SLOT_BOX    1
#define SLOT_OVL_R  2
#define SLOT_OVL_RSQ 3
#define SLOT_OVL_STEP 4
static float* g_slot;

static bool g_haveSearch   = false;    // the FindCity hook is in
static bool g_haveNeutered = false;    // and the original's own search is off

// ---------------------------------------------------------------- the table
//
// One entry per city, keyed by NamePoint*. Cities are counted in tens, so a
// linear scan is both fast enough and impossible to get wrong; the alternative,
// keying by index, breaks the moment the game erases one out of the middle.

#define MAX_CITIES 512

struct CityCfg
{
    void* city;
    float radius;
    int   square;
};

static CityCfg g_cfg[MAX_CITIES];
static int     g_cfgCount;
static float   g_maxRadius = VANILLA_RADIUS;   // drives the reassignment box

static CityCfg* CfgFind(void* city)
{
    for (int i = 0; i < g_cfgCount; i++)
        if (g_cfg[i].city == city) return &g_cfg[i];
    return NULL;
}

static CityCfg* CfgFor(void* city)
{
    CityCfg* c = CfgFind(city);
    if (c) return c;
    if (g_cfgCount >= MAX_CITIES) return NULL;
    c = &g_cfg[g_cfgCount++];
    c->city   = city;
    c->radius = g_defRadius;
    c->square = g_defSquare;
    return c;
}

static void CfgClear(void)
{
    g_cfgCount = 0;
    g_maxRadius = g_defRadius > VANILLA_RADIUS ? g_defRadius : VANILLA_RADIUS;
}

// A deleted city leaves an entry behind, and the allocator will hand its
// address out again. Dropping anything no longer in the vector is what stops a
// brand-new city inheriting the radius of one that stood somewhere else.
static void CfgPurge(BYTE* game)
{
    void** begin = *(void***)(game + G_CITY_BEGIN);
    void** end   = *(void***)(game + G_CITY_END);
    if (!begin) { g_cfgCount = 0; return; }

    int kept = 0;
    for (int i = 0; i < g_cfgCount; i++)
    {
        bool live = false;
        for (void** it = begin; it < end && !live; it++)
            if (*it == g_cfg[i].city) live = true;
        if (live) g_cfg[kept++] = g_cfg[i];
    }
    g_cfgCount = kept;
}

// The box every reassignment pre-filters with has to be at least as wide as the
// widest city, or a building the widest city should have taken is never looked
// at. Never narrower than the game's own 1000, so nothing the base game did
// stops working.
static void RecomputeMaxRadius(void)
{
    float m = VANILLA_RADIUS;
    for (int i = 0; i < g_cfgCount; i++)
        if (g_cfg[i].radius > m) m = g_cfg[i].radius;
    g_maxRadius = m;
    if (g_slot) g_slot[SLOT_BOX] = m;
}

// ---------------------------------------------------------------- vectors
//
// A city's building list is an ordinary three-pointer vector. Growing one has
// to go through the game's own push_back (0xB0E90): the buffer is freed by the
// game's allocator, so it must have been allocated by it. Shrinking allocates
// nothing and is done here.

static void VecErase(BYTE* vec, void* item)
{
    void** begin = *(void***)(vec);
    void** end   = *(void***)(vec + 8);
    if (!begin || begin == end) return;

    void** w = begin;
    for (void** r = begin; r < end; r++)
        if (*r != item) *w++ = *r;
    *(void***)(vec + 8) = w;
}

static void VecClear(BYTE* vec)
{
    *(void***)(vec + 8) = *(void***)(vec);
}

// ---------------------------------------------------------------- geometry
//
// Exactly what 0x32FEA0 computes for each building, in the same order: the
// node position, overwritten by the bounding box midpoint when the building
// has no type record (scenery, villages, the imported civ_* set).

static bool BuildingCentre(BYTE* b, float* out)
{
    if (!ReadablePtr(b + B_BBOX, 8)) return false;

    float tmp[4] = { 0, 0, 0, 0 };
    const float* p = o_NodeGetPosition(b + B_NODE, tmp);
    if (!p) return false;
    out[0] = p[0]; out[1] = p[1]; out[2] = p[2];

    if (*(void**)(b + B_TYPEDESC) != NULL) return true;

    BYTE* bb = *(BYTE**)(b + B_BBOX);
    if (!bb || !ReadablePtr(bb + 0x20, 16)) return true;
    const float* mn = *(const float**)(bb + 0x20);
    const float* mx = *(const float**)(bb + 0x28);
    if (!mn || !mx) return true;
    if (!ReadablePtr(mn, 16) || !ReadablePtr(mx, 16)) return true;

    out[0] = (mn[1] + mx[1]) * 0.5f;
    out[1] = (mn[2] + mx[2]) * 0.5f;
    out[2] = (mn[3] + mx[3]) * 0.5f;
    return true;
}

// ---------------------------------------------------------------- the search
//
// The replacement for the loop inside 0x3305F0. With every city on the default
// 1000 m circle this is bit for bit the vanilla decision: the same squared 3D
// distance, the same strict `<`, the same 1e10 starting point, the same "skip
// anything flagged at +0x118".

static void* SearchCity(BYTE* game, const float* pos)
{
    void** begin = *(void***)(game + G_CITY_BEGIN);
    void** end   = *(void***)(game + G_CITY_END);
    if (!begin || begin >= end) return NULL;

    void* best  = NULL;
    float bestD = SENTINEL_FAR;

    for (void** it = begin; it < end; it++)
    {
        BYTE* np = (BYTE*)*it;
        if (!np) continue;
        if (*(BYTE*)(np + NP_SKIP) != 0) continue;

        const float* p = (const float*)(np + NP_POS);
        float dx = pos[0] - p[0];
        float dy = pos[1] - p[1];
        float dz = pos[2] - p[2];
        float d  = dx * dx + dy * dy + dz * dz;

        CityCfg* c = CfgFor(np);
        float r = c ? c->radius : g_defRadius;
        int   q = c ? c->square : g_defSquare;

        if (q)
        {
            // A square of side 2r, so it circumscribes the circle of the same
            // radius: the slider means the same distance in both shapes and
            // switching shape can only ever take buildings in, never drop them.
            float ax = dx < 0 ? -dx : dx;
            float az = dz < 0 ? -dz : dz;
            if (ax >= r || az >= r) continue;
        }
        else
        {
            if (d >= r * r) continue;
        }

        if (bestD <= d) continue;
        bestD = d;
        best  = np;
    }
    return best;
}

// ---------------------------------------------------------------- assignment

static void Touch(void** list, int* n, void* city)
{
    if (!city) return;
    for (int i = 0; i < *n; i++) if (list[i] == city) return;
    if (*n < 256) list[(*n)++] = city;
}

// Every building, every city, from scratch: recompute each building's city,
// rebuild every city's building list in the same order the buildings are in,
// and renumber. Cheap enough to be the answer to every "something changed"
// - one pass over the buildings times the number of cities.
static int RebuildAll(BYTE* game)
{
    void** cb = *(void***)(game + G_CITY_BEGIN);
    void** ce = *(void***)(game + G_CITY_END);
    void** bb = *(void***)(game + G_BLD_BEGIN);
    void** be = *(void***)(game + G_BLD_END);
    if (!cb || !bb) return 0;

    for (void** it = cb; it < ce; it++)
        if (*it) VecClear((BYTE*)*it + NP_BLD);

    int orphans = 0;
    for (void** it = bb; it < be; it++)
    {
        BYTE* b = (BYTE*)*it;
        if (!b) continue;

        float c[3];
        if (!BuildingCentre(b, c)) continue;

        BYTE* city = (BYTE*)SearchCity(game, c);
        *(void**)(b + B_CITY) = city;
        if (city) o_VecPush(city + NP_BLD, it);
        else      orphans++;
    }

    for (void** it = cb; it < ce; it++)
        if (*it) o_Renumber(game, *it);

    return orphans;
}

// The replacement for 0x32FEA0. Same job - reassign every building near `pos`
// - with two differences: the box follows the widest city instead of a fixed
// 1000, and a building whose old city is null does not take the process down.
static void ReassignNear(BYTE* game, const float* pos)
{
    void** bb = *(void***)(game + G_BLD_BEGIN);
    void** be = *(void***)(game + G_BLD_END);
    if (!bb) return;

    float half = g_maxRadius;
    void* touched[256];
    int   nTouched = 0;

    for (void** it = bb; it < be; it++)
    {
        BYTE* b = (BYTE*)*it;
        if (!b) continue;

        float c[3];
        if (!BuildingCentre(b, c)) continue;
        if (c[0] < pos[0] - half || c[0] > pos[0] + half) continue;
        if (c[2] < pos[2] - half || c[2] > pos[2] + half) continue;

        BYTE* was = *(BYTE**)(b + B_CITY);
        BYTE* now = (BYTE*)SearchCity(game, c);
        *(void**)(b + B_CITY) = now;

        if (was != now)
        {
            if (was) VecErase(was + NP_BLD, b);
            if (now) o_VecPush(now + NP_BLD, it);
            Touch(touched, &nTouched, was);
        }
        Touch(touched, &nTouched, now);
    }

    for (int i = 0; i < nTouched; i++)
        o_Renumber(game, touched[i]);
}

// ---------------------------------------------------------------- adoption
//
// Whatever is left without a city gets one, through the game's own creation
// path - random name off the unused pool, "date established" from the current
// game date, the whole thing. Creating one adopts every orphan inside its
// radius, so the loop is over clusters, not over buildings.

static bool g_adopting = false;

static int AdoptOrphans(BYTE* game)
{
    if (g_adopting || !g_adopt || !g_haveSearch) return 0;

    g_adopting = true;
    int made = 0;

    for (int pass = 0; pass < MAX_CITIES; pass++)
    {
        void** bb = *(void***)(game + G_BLD_BEGIN);
        void** be = *(void***)(game + G_BLD_END);
        BYTE*  victim = NULL;
        float  c[3] = { 0, 0, 0 };

        for (void** it = bb; it < be && !victim; it++)
        {
            BYTE* b = (BYTE*)*it;
            if (!b) continue;
            if (*(void**)(b + B_CITY) != NULL) continue;
            if (!BuildingCentre(b, c)) continue;
            victim = b;
        }
        if (!victim) break;

        // create=1. The original's own search is neutered, so this always
        // reaches the creation half; the reassignment it runs afterwards comes
        // straight back through this plugin and picks the orphans up.
        void* fresh = o_FindCity(game, c, 1);
        if (!fresh)
        {
            Logf("cities   the game would not create a city at %.0f, %.0f - giving up",
                 c[0], c[2]);
            break;
        }
        made++;

        // The creation path reassigns only the neighbourhood; make sure the
        // building that asked really did end up somewhere, or the loop spins.
        if (*(void**)(victim + B_CITY) == NULL)
        {
            *(void**)(victim + B_CITY) = fresh;
            o_VecPush((BYTE*)fresh + NP_BLD, (void*)&victim);
            o_Renumber(game, fresh);
        }
    }

    g_adopting = false;
    return made;
}

// One "the player moved something" step: rebuild the assignment with the new
// numbers, then give whatever fell outside a city of its own.
// The purge is deliberately **not** here. Reclaiming the entries of deleted
// cities is worth doing once a save, not on every slider release: it is
// quadratic in the number of cities, and it is the one thing in this plugin
// that can take a setting away from a city that is still on screen.
static void ApplyChange(BYTE* game)
{
    RecomputeMaxRadius();
    int orphans = RebuildAll(game);
    int made    = 0;
    if (orphans > 0) made = AdoptOrphans(game);

    void** cb = *(void***)(game + G_CITY_BEGIN);
    void** ce = *(void***)(game + G_CITY_END);
    Logf("cities   reassigned: %d building(s) outside every city, %d new city(-ies), %d total",
         orphans, made, (int)(ce - cb));
}

// ---------------------------------------------------------------- detours

static void* h_FindCity(void* game, const float* pos, unsigned char create)
{
    void* found = NULL;
    __try { found = SearchCity((BYTE*)game, pos); }
    __except (FaultFilter("cities find", GetExceptionInformation())) { found = NULL; }

    if (found) return found;
    if (!create) return NULL;
    return o_FindCity(game, pos, 1);
}

static void h_Reassign(void* game, const float* pos)
{
    __try { ReassignNear((BYTE*)game, pos); }
    __except (FaultFilter("cities reassign", GetExceptionInformation()))
    {
        Logf("cities   reassignment faulted - falling back to the game's own");
        o_Reassign(game, pos);
    }
}

// ---------------------------------------------------------------- persistence
//
// Appended to namepoints.bin, after everything the game writes. The game reads
// exactly one 0x130-byte record per name point and stops, so this is invisible
// to it: a save written with the plugin still loads without it.

#define BLOB_MAGIC   "TSMCITY1"
#define BLOB_VERSION 1u

struct BlobHeader
{
    char     magic[8];
    unsigned version;
    unsigned count;
};

struct BlobEntry
{
    float    radius;
    unsigned flags;        // bit 0: square
};

// Neither serialiser is handed the game object reliably: 0x3337D0 ignores its
// first argument entirely and reads the vectors as globals, so the call site at
// 0x431D56 never bothers to load rcx. The static is the only thing that is true
// at both sites - `lea rcx,[0x9D4F10]` is how the loader itself addresses the
// game two hundred instructions later.
static BYTE* GameObject(void) { return g_exeBase + P_GAMEOBJ; }

static void h_SaveCities(void* game, void* file)
{
    o_SaveCities(game, file);
    if (!o_Fwrite) return;

    __try
    {
        BYTE*  g  = GameObject();
        CfgPurge(g);
        void** cb = *(void***)(g + G_CITY_BEGIN);
        void** ce = *(void***)(g + G_CITY_END);
        unsigned n = (unsigned)(ce - cb);

        BlobHeader h;
        memcpy(h.magic, BLOB_MAGIC, 8);
        h.version = BLOB_VERSION;
        h.count   = n;
        o_Fwrite(&h, sizeof(h), 1, file);

        for (unsigned i = 0; i < n; i++)
        {
            CityCfg*  c = CfgFor(cb[i]);
            BlobEntry e;
            e.radius = c ? c->radius : g_defRadius;
            e.flags  = (unsigned)((c ? c->square : g_defSquare) ? 1 : 0);
            o_Fwrite(&e, sizeof(e), 1, file);
        }
        Logf("cities   saved %u radius record(s) into namepoints.bin", n);
    }
    __except (FaultFilter("cities save", GetExceptionInformation()))
    {
        Logf("cities   save block faulted - the world itself is unaffected");
    }
}

// Set by the load hook, consumed at the end of the world load: the radii are
// known long before the buildings are safe to walk.
static bool g_settle;

static void h_LoadCities(void* game, void* file)
{
    CfgClear();
    g_settle = false;
    o_LoadCities(game, file);
    if (!o_Fread) return;

    __try
    {
        BYTE*  g  = GameObject();
        void** cb = *(void***)(g + G_CITY_BEGIN);
        void** ce = *(void***)(g + G_CITY_END);
        unsigned have = (unsigned)(ce - cb);

        BlobHeader h;
        memset(&h, 0, sizeof(h));
        if (o_Fread(&h, sizeof(h), 1, file) != 1 ||
            memcmp(h.magic, BLOB_MAGIC, 8) != 0)
        {
            Logf("cities   no radius block in this save - %u city(-ies) on the default %.0f m",
                 have, g_defRadius);
        }
        else if (h.version != BLOB_VERSION)
        {
            Logf("cities   radius block is version %u, this plugin writes %u - ignored",
                 h.version, BLOB_VERSION);
        }
        else
        {
            unsigned n = h.count < have ? h.count : have;
            if (h.count != have)
                Logf("cities   radius block holds %u record(s) for %u city(-ies) - reading %u",
                     h.count, have, n);
            for (unsigned i = 0; i < n; i++)
            {
                BlobEntry e;
                if (o_Fread(&e, sizeof(e), 1, file) != 1) { n = i; break; }
                CityCfg* c = CfgFor(cb[i]);
                if (!c) break;
                if (e.radius > 1.0f && e.radius < 100000.0f) c->radius = e.radius;
                c->square = (e.flags & 1u) ? 1 : 0;
            }
            Logf("cities   loaded %u radius record(s) from namepoints.bin", n);
        }

        RecomputeMaxRadius();

        // **Not a rebuild here.** Everything that assigned a building to a city
        // before this point did it on default numbers, so one is needed - but
        // the buildings are not finished being loaded yet: version 1.0 rebuilt
        // here and faulted inside the game's own renumbering at 0x3303FF,
        // reading `type+0x200` off a `building+0x318` that was not a type
        // record yet. It is deferred to the end of the world load instead.
        g_settle = true;

        if (g_probe)
        {
            for (unsigned i = 0; i < have; i++)
            {
                BYTE* np = (BYTE*)cb[i];
                CityCfg* c = CfgFor(np);
                int n = (int)((*(void***)(np + NP_BLD + 8) - *(void***)(np + NP_BLD)));
                Logf("cities   probe %2u  %-24ls  r=%.0f %-6s  %d building(s)",
                     i, (const wchar_t*)(np + NP_NAME),
                     c ? c->radius : g_defRadius,
                     (c && c->square) ? "square" : "circle", n);
            }
        }
    }
    __except (FaultFilter("cities load", GetExceptionInformation()))
    {
        Logf("cities   load block faulted - every city stays on the default radius");
    }
}

// ---------------------------------------------------------------- settling
//
// The one place a rebuild is both needed and safe. `LoadNamePoints` knows the
// radii but runs while the buildings are still being put together; the world
// loader's last act is the base game's own "give every city-less building a
// city" pass, so by the time it returns everything is constructed and every
// pointer is real.
//
// Its single call site is rewritten rather than its prologue stolen - the same
// technique as the two serialisers, and for the same reason: nothing has to be
// relocated and a game update makes it refuse rather than corrupt anything.

typedef void (*t_WorldLoad)(void*, void*, void*, void*);
static t_WorldLoad o_WorldLoad;

static void h_WorldLoad(void* a, void* b, void* c, void* d)
{
    o_WorldLoad(a, b, c, d);
    if (!g_settle) return;
    g_settle = false;

    __try
    {
        BYTE* g = GameObject();
        RecomputeMaxRadius();
        int orphans = RebuildAll(g);
        int made    = (orphans > 0) ? AdoptOrphans(g) : 0;

        void** cb = *(void***)(g + G_CITY_BEGIN);
        void** ce = *(void***)(g + G_CITY_END);
        Logf("cities   settled after load: %d outside every city, %d new, %d total",
             orphans, made, (int)(ce - cb));

        if (g_probe)
        {
            cb = *(void***)(g + G_CITY_BEGIN);
            ce = *(void***)(g + G_CITY_END);
            for (void** it = cb; it < ce; it++)
            {
                BYTE* np = (BYTE*)*it;
                CityCfg* cfg = CfgFor(np);
                int n = (int)(*(void***)(np + NP_BLD + 8) - *(void***)(np + NP_BLD));
                Logf("cities   probe %2d  r=%.0f %-6s  %d building(s)",
                     (int)(it - cb), cfg ? cfg->radius : g_defRadius,
                     (cfg && cfg->square) ? "square" : "circle", n);
            }
        }
    }
    __except (FaultFilter("cities settle", GetExceptionInformation()))
    {
        Logf("cities   the pass after loading faulted - assignments are the game's own");
    }
}

// ---------------------------------------------------------------- the rows

static float LayoutF(DWORD rva) { return *(float*)(g_exeBase + rva); }

static unsigned RadiusToPercent(float r)
{
    float span = g_radiusMax - g_radiusMin;
    if (span <= 0.0f) return 0;
    float p = (r - g_radiusMin) * 100.0f / span;
    if (p < 0.0f)   p = 0.0f;
    if (p > 100.0f) p = 100.0f;
    return (unsigned)(p + 0.5f);
}

static float PercentToRadius(unsigned p)
{
    if (p > 100) p = 100;
    return g_radiusMin + (g_radiusMax - g_radiusMin) * (float)p / 100.0f;
}

// Set while a drag is in progress, so the whole map is not reassigned on every
// mouse-move frame. The pass runs on the first frame the value stops moving -
// including the frame the window closes, which is why the flush lives in the
// hook rather than in the row drawing.
static void* g_dirtyCity;
static int   g_changedThisFrame;

// The dot grid walks whatever region these three describe, and the search
// decides which points inside it get a dot - so they only have to be a superset
// of the city. The overlay is drawn for the first window and only when that
// window is a city's, so the city on screen is the one the window hook just
// drew; worst case they are one frame apart.
//
// The step follows the radius, because the scan is quadratic in it: vanilla
// walks 40x40 points at 1000 m and 50 m, and a 5 km city at the same step would
// walk 200x200 - each of them asking the search about every city on the map.
static void SetOverlayFor(const CityCfg* c)
{
    if (!g_slot || !g_overlay || !c) return;

    float r = c->radius;
    if (c->square) r *= SQUARE_REACH;

    float step = g_overlayStep;
    if (step <= 0.0f)
    {
        step = r / 20.0f;
        if (step < VANILLA_OVL_STEP) step = VANILLA_OVL_STEP;
    }

    g_slot[SLOT_OVL_R]    = r;
    g_slot[SLOT_OVL_RSQ]  = r * r;
    g_slot[SLOT_OVL_STEP] = step;
}

static void DrawCityRows(BYTE* game, BYTE* win)
{
    if (*(char*)(win + W_HIDDEN) != 0) return;
    if (*(int*)(win + W_KIND) != W_KIND_CITY) return;

    BYTE* city = *(BYTE**)(win + W_OBJ);
    if (!city || !ReadablePtr(city + NP_BLD, 16)) return;

    CityCfg* cfg = CfgFor(city);
    if (!cfg) return;

    float dpi  = LayoutF(P_DPI);
    float step = dpi * LayoutF(LAYOUT_35);
    float left = (*(float*)(win + W_X) - dpi * LayoutF(LAYOUT_15))
               + *(float*)(win + W_OFFX) + dpi * LayoutF(LAYOUT_35);
    float y    = *(float*)(win + W_BOTTOM);

    void* fontMgr = g_exeBase + P_FONTMGR;
    void* font    = *(void**)(g_exeBase + P_ROW_FONT);
    char  live    = *(char*)(win + W_ACTIVE);

    int buildings = (int)(*(void***)(city + NP_BLD + 8) - *(void***)(city + NP_BLD));

    // ---- the radius, as the game's own 0..100 slider
    unsigned pct = RadiusToPercent(cfg->radius);
    unsigned was = pct;

    if (font)
    {
        wchar_t line[96];
        _snwprintf_s(line, _TRUNCATE, L"City radius: %.0f m   (%d building(s))",
                     cfg->radius, buildings);
        o_PrintLeftUnicode(fontMgr, font, left + dpi * g_labelX, y, ROW_COLOUR,
                           L"%ls", line);
    }
    if (o_Slider)
        o_Slider(game, left + dpi * g_widgetX, y, -1, &pct, dpi,
                 live, g_step, live, NULL);
    if (pct != was)
    {
        cfg->radius = PercentToRadius(pct);
        g_dirtyCity = city;
        g_changedThisFrame = 1;
    }
    y += step;

    // ---- the shape. The widget draws its own label, to the right of the box.
    //
    // It also draws **above** the y it is handed - the box at `y - DPI*25` and
    // the label at `y - DPI*37` - so a row placed at the running Y lands a
    // third of a row too high and sits on the slider. The offset is added back
    // so the box lands on its own row; `checkbox_dy` is there to nudge it.
    unsigned sq    = cfg->square ? 1u : 0u;
    unsigned sqWas = sq;
    if (o_Checkbox)
    {
        o_Checkbox(game, left + dpi * g_labelX, y + dpi * g_checkDy, &sq,
                   L"Square city area",
                   ROW_COLOUR, NULL, LayoutF(LAYOUT_20), 0.0f, live, 1, 0);
        if (sq != sqWas)
        {
            cfg->square = sq ? 1 : 0;
            g_dirtyCity = city;
            g_changedThisFrame = 1;
        }
    }
    y += step;

    *(float*)(win + W_BOTTOM) = y;

    SetOverlayFor(cfg);

    // Everything the two widgets are gated on, so a run with probe = 1 says
    // which gate is shut rather than only that nothing happened.
    if (g_probe)
        Logf("cities   probe window  cfg %d/%d  r=%.0f %s  pct %u->%u  sq %u->%u  n=%d"
             "  live=%d busy=%d latch=%d held=%d",
             (int)(cfg - g_cfg), g_cfgCount, cfg->radius,
             cfg->square ? "square" : "circle", was, pct, sqWas, sq, buildings,
             (int)live, *(const int*)(game + P_WIDGET_BUSY),
             (int)*(const BYTE*)(g_exeBase + P_CLICK_LATCH),
             (int)*(const BYTE*)(g_exeBase + P_MOUSE_DOWN));
}

static void h_CityWindow(void* game, void* window, int caption)
{
    o_CityWindow(game, window, caption);
    if (!g_window || !window) return;

    g_changedThisFrame = 0;
    __try { DrawCityRows((BYTE*)game, (BYTE*)window); }
    __except (FaultFilter("cities window", GetExceptionInformation()))
    {
        Logf("cities   window rows disabled after a fault");
        g_window = 0;
    }

    // One pass, once the value has stopped moving **and the button is up**. A
    // drag fires every frame, and reassigning the whole map twenty times a
    // second is both wasted work and a stutter the player feels as the slider
    // fighting back.
    bool held = *(const BYTE*)(g_exeBase + P_MOUSE_DOWN) != 0;
    if (!g_changedThisFrame && !held && g_dirtyCity)
    {
        g_dirtyCity = NULL;
        __try { ApplyChange((BYTE*)game); }
        __except (FaultFilter("cities apply", GetExceptionInformation()))
        {
            Logf("cities   reassignment after a change faulted");
        }
    }
}

// ---------------------------------------------------------------- patching

static bool WriteCode(void* at, const void* bytes, size_t n, const char* what)
{
    DWORD prot = 0;
    if (!VirtualProtect(at, n, PAGE_EXECUTE_READWRITE, &prot))
    {
        Logf("cities   VirtualProtect failed at %p (%s)", at, what);
        return false;
    }
    memcpy(at, bytes, n);
    VirtualProtect(at, n, prot, &prot);
    FlushInstructionCache(GetCurrentProcess(), at, n);
    return true;
}

// Points one RIP-relative float read at a float this plugin owns. Refuses
// unless the opcode, the address the displacement resolves to and the value in
// there are all exactly what this build is known to have - the same three
// checks the walking plugin makes, and for the same reason: every literal
// involved sits in the shared pool with unrelated readers, so overwriting one
// would move far more than cities.
//
// `op` is the whole instruction up to the displacement, which is what tells a
// `movss xmm7,[rip]` (F3 0F 10 3D, eight bytes) from a `movss xmm11,[rip]`
// (F3 44 0F 10 1D, nine) and from the `addss` the dot grid steps with.
static bool Repoint(DWORD siteRva, const BYTE* op, size_t opLen,
                    DWORD constRva, float vanilla, float* slot, const char* label)
{
    BYTE*  site   = g_exeBase + siteRva;
    size_t insLen = opLen + 4;

    if (!ReadablePtr(site, insLen) || memcmp(site, op, opLen) != 0)
    {
        Logf("cities   %s: rva 0x%X is %02X %02X %02X %02X %02X, not the expected read - refusing",
             label, siteRva, site[0], site[1], site[2], site[3], site[4]);
        return false;
    }

    DWORD resolved = (DWORD)(siteRva + insLen + *(const int*)(site + opLen));
    if (resolved != constRva)
    {
        Logf("cities   %s: rva 0x%X reads 0x%X, expected 0x%X - refusing",
             label, siteRva, resolved, constRva);
        return false;
    }

    float now = *(const float*)(g_exeBase + constRva);
    if (now != vanilla)
    {
        Logf("cities   %s: 0x%X holds %g, expected %g - refusing",
             label, constRva, now, vanilla);
        return false;
    }

    __int64 rel = (BYTE*)slot - (site + insLen);
    if (rel < (__int64)INT_MIN || rel > (__int64)INT_MAX)
    {
        Logf("cities   %s: slot %p is out of rel32 range of %p - refusing",
             label, slot, site);
        return false;
    }

    int disp = (int)rel;
    if (!WriteCode(site + opLen, &disp, sizeof(disp), label)) return false;

    Logf("cities   %-18s rva 0x%X now reads %p (%g)", label, siteRva, slot, *slot);
    return true;
}

// The dot grid's x-loop end test, rewritten to compare against our own radius
// while leaving xmm9 alone. See the note beside RVA_OVL_END_CMP.
static bool PatchDotsEndBound(float* slot)
{
    BYTE* site = g_exeBase + RVA_OVL_END_CMP;

    if (!ReadablePtr(site, sizeof(kOvlEndCmp)) ||
        memcmp(site, kOvlEndCmp, sizeof(kOvlEndCmp)) != 0)
    {
        Logf("cities   dots end: rva 0x%X is not the expected compare - refusing",
             RVA_OVL_END_CMP);
        return false;
    }

    // addss xmm0,[rip+disp32] is eight bytes; the displacement is from its end.
    __int64 rel = (BYTE*)slot - (site + 8);
    if (rel < (__int64)INT_MIN || rel > (__int64)INT_MAX)
    {
        Logf("cities   dots end: slot %p out of rel32 range of %p - refusing", slot, site);
        return false;
    }

    BYTE code[sizeof(kOvlEndCmp)];
    code[0] = 0xF3; code[1] = 0x0F; code[2] = 0x58; code[3] = 0x05;   // addss xmm0,[rip]
    int disp = (int)rel;
    memcpy(code + 4, &disp, 4);
    code[8] = 0x41; code[9] = 0x0F; code[10] = 0x2F; code[11] = 0xC0; // comiss xmm0,xmm8
    code[12] = 0x90; code[13] = 0x90;

    if (!WriteCode(site, code, sizeof(code), "dots end")) return false;

    Logf("cities   %-18s rva 0x%X now compares against %p (%g)",
         "dots end", RVA_OVL_END_CMP, slot, *slot);
    return true;
}

// The five encodings the sites above use.
static const BYTE kMovssX7[]   = { 0xF3, 0x0F, 0x10, 0x3D };        // movss xmm7,[rip]
static const BYTE kMovssX6[]   = { 0xF3, 0x0F, 0x10, 0x35 };        // movss xmm6,[rip]
static const BYTE kMovssX11[]  = { 0xF3, 0x44, 0x0F, 0x10, 0x1D };  // movss xmm11,[rip]
static const BYTE kAddssX6[]   = { 0xF3, 0x0F, 0x58, 0x35 };        // addss xmm6,[rip]
static const BYTE kAddssX8[]   = { 0xF3, 0x44, 0x0F, 0x58, 0x05 };  // addss xmm8,[rip]

// Rewrites the rel32 of one direct call so it lands in a cave holding an
// absolute jump to `detour`. Verified by decoding the call rather than by
// comparing bytes: what has to be true is that the site is a direct call *to
// this function*, and that survives the caller moving.
static bool PatchCall(DWORD siteRva, DWORD targetRva, void* detour,
                      void** origOut, const char* label)
{
    BYTE* site   = g_exeBase + siteRva;
    BYTE* target = g_exeBase + targetRva;

    if (!ReadablePtr(site, 5)) { Logf("cities   %s: call site unreadable", label); return false; }

    int rel = 0;
    memcpy(&rel, site + 1, 4);
    if (site[0] != 0xE8 || site + 5 + rel != target)
    {
        Logf("cities   %s: rva 0x%X is not `call %p` - wrong game build, refusing",
             label, siteRva, target);
        return false;
    }

    BYTE* cave = AllocNear(g_exeBase, 16);
    if (!cave) { Logf("cities   %s: no cave within reach", label); return false; }

    cave[0] = 0xFF; cave[1] = 0x25;                   // jmp qword ptr [rip+0]
    memset(cave + 2, 0, 4);
    memcpy(cave + 6, &detour, sizeof(detour));

    __int64 delta = cave - (site + 5);
    int newRel = (int)delta;
    if (delta != (__int64)newRel)
    {
        Logf("cities   %s: cave %p out of rel32 range of the call site", label, cave);
        return false;
    }

    DWORD prot = 0;
    if (!VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &prot))
    {
        Logf("cities   %s: call site not writable (%lu)", label, GetLastError());
        return false;
    }
    memcpy(site + 1, &newRel, 4);
    VirtualProtect(site, 5, prot, &prot);
    FlushInstructionCache(GetCurrentProcess(), site, 5);

    *origOut = target;                                // called by address, no trampoline
    Logf("cities   %-18s call at rva 0x%X -> %p (original %p)", label, siteRva, detour, target);
    return true;
}

// ---------------------------------------------------------------- settings

static void ReadSettings(void)
{
    const char* ini = "plugins\\cities.ini";
    char buf[64];

    g_enabled   = H->configInt(ini, "cities", "enabled",   g_enabled);
    g_window    = H->configInt(ini, "cities", "window",    g_window);
    g_adopt     = H->configInt(ini, "cities", "adopt",     g_adopt);
    g_defSquare = H->configInt(ini, "cities", "square",    g_defSquare);
    g_step      = H->configInt(ini, "cities", "step",      g_step);
    g_overlay   = H->configInt(ini, "cities", "overlay",   g_overlay);
    g_probe     = H->configInt(ini, "cities", "probe",     g_probe);

    H->configString(ini, "cities", "radius", buf, (int)sizeof(buf), "1000");
    g_defRadius = (float)atof(buf);
    H->configString(ini, "cities", "radius_min", buf, (int)sizeof(buf), "200");
    g_radiusMin = (float)atof(buf);
    H->configString(ini, "cities", "radius_max", buf, (int)sizeof(buf), "5000");
    g_radiusMax = (float)atof(buf);
    H->configString(ini, "cities", "label_x", buf, (int)sizeof(buf), "10");
    g_labelX = (float)atof(buf);
    H->configString(ini, "cities", "widget_x", buf, (int)sizeof(buf), "200");
    g_widgetX = (float)atof(buf);
    H->configString(ini, "cities", "checkbox_dy", buf, (int)sizeof(buf), "25");
    g_checkDy = (float)atof(buf);
    H->configString(ini, "cities", "overlay_step", buf, (int)sizeof(buf), "0");
    g_overlayStep = (float)atof(buf);
    if (g_overlayStep < 0.0f)   g_overlayStep = 0.0f;
    if (g_overlayStep > 500.0f) g_overlayStep = 500.0f;

    if (g_radiusMin < 25.0f)          g_radiusMin = 25.0f;
    if (g_radiusMax <= g_radiusMin)   g_radiusMax = g_radiusMin + 100.0f;
    if (g_radiusMax > 20000.0f)       g_radiusMax = 20000.0f;
    if (g_defRadius < g_radiusMin)    g_defRadius = g_radiusMin;
    if (g_defRadius > g_radiusMax)    g_defRadius = g_radiusMax;
    if (g_step < 1)                   g_step = 1;
    if (g_step > 50)                  g_step = 50;
}

// The game object is a static and this plugin reaches it by address, so the
// address is checked the way every other constant here is: the save loader's
// own `lea rcx,[gameobj]` two instructions before it asks FindCity for a
// city-less building's city. If that no longer resolves to P_GAMEOBJ, nothing
// below is safe to run.
#define RVA_GAMEOBJ_LEA 0x43969A  // v1.1.1.13; was 0x43970A

static bool VerifyGameObject(void)
{
    BYTE* site = g_exeBase + RVA_GAMEOBJ_LEA;
    if (!ReadablePtr(site, 7)) return false;
    if (site[0] != 0x48 || site[1] != 0x8D || site[2] != 0x0D) return false;

    DWORD resolved = (DWORD)(RVA_GAMEOBJ_LEA + 7 + *(const int*)(site + 3));
    if (resolved != P_GAMEOBJ)
    {
        Logf("cities   game object: rva 0x%X points at 0x%X, expected 0x%X - refusing",
             RVA_GAMEOBJ_LEA, resolved, P_GAMEOBJ);
        return false;
    }
    return true;
}

static bool BindImports(void)
{
    if (void** s = FindIatSlot(g_exe, DLL_ENGINE,
            "?GetPosition@C3D_NODE@@QEAA?AVC3DVECTOR3@@XZ"))
        o_NodeGetPosition = (t_VecGetter)*s;
    else
    {
        Logf("cities   no import slot for C3D_NODE::GetPosition - cannot place a building");
        return false;
    }

    if (void** s = FindIatSlot(g_exe, DLL_ENGINE,
            "?PrintLeftUnicode@C3D_FONTMANAGER@@QEAAXPEAVC3D_FONT@@MMKPEB_WZZ"))
        o_PrintLeftUnicode = (t_PrintLeftUnicode)*s;
    else
    {
        Logf("cities   no import slot for PrintLeftUnicode - no window rows");
        g_window = 0;
    }

    if (void** s = FindIatSlot(g_exe, DLL_STDIO, "fread"))  o_Fread  = (t_Fread)*s;
    if (void** s = FindIatSlot(g_exe, DLL_STDIO, "fwrite")) o_Fwrite = (t_Fwrite)*s;
    if (!o_Fread || !o_Fwrite)
        Logf("cities   no fread/fwrite import - radii will not survive a save");

    o_Renumber = (t_Renumber)(g_exeBase + P_RENUMBER);
    o_VecPush  = (t_VecPush) (g_exeBase + P_VEC_PUSH);
    o_Slider   = (t_Slider)  (g_exeBase + P_SLIDER);
    o_Checkbox = (t_Checkbox)(g_exeBase + P_CHECKBOX);
    return true;
}

// ---------------------------------------------------------------- entry

extern "C" __declspec(dllexport) unsigned TsmPluginApiVersion(void)
{
    return TSM_API_VERSION;
}

extern "C" __declspec(dllexport) int TsmPluginInit(const TsmHost* host, TsmPluginInfo* info)
{
    TsmBind(host);
    info->name    = "cities";
    info->version = "1.0";

    ReadSettings();
    if (!g_enabled)
    {
        Logf("cities   enabled = 0 - every city keeps the fixed %.0f m", VANILLA_RADIUS);
        return 1;                        // nothing hooked, no reason to stay loaded
    }
    return 0;
}

extern "C" __declspec(dllexport) int TsmPluginStart(void)
{
    if (!g_enabled) return 1;
    if (!VerifyGameObject())
    {
        Logf("cities   the game object is not where this build puts it - inactive");
        return 1;
    }
    if (!BindImports()) return 1;

    CfgClear();

    // One allocation for both floats: the game reads them on every search and
    // every reassignment, so they have to outlive this call and stay put.
    g_slot = (float*)AllocNear(g_exeBase, 64);
    if (!g_slot)
    {
        Logf("cities   no allocation within reach of the executable - not patching");
        return 1;
    }
    g_slot[SLOT_ZERO]     = 0.0f;
    g_slot[SLOT_BOX]      = g_maxRadius;
    g_slot[SLOT_OVL_R]    = VANILLA_BOX;
    g_slot[SLOT_OVL_RSQ]  = VANILLA_LIMIT_SQ;
    g_slot[SLOT_OVL_STEP] = VANILLA_OVL_STEP;

    if (g_probe)
        Logf("cities   probe: limit 0x%X = %g, box 0x%X = %g",
             RVA_LIMIT_CONST, *(const float*)(g_exeBase + RVA_LIMIT_CONST),
             RVA_BOX_CONST,   *(const float*)(g_exeBase + RVA_BOX_CONST));

    // The search first. If it does not install, the constant below must stay
    // as it is: a neutered limit with the vanilla search still in charge would
    // mean no building ever belongs to any city.
    g_haveSearch = InstallInlineHook(g_exeBase + P_FIND_CITY, (void*)h_FindCity,
                                     (void**)&o_FindCity, kFindCityPrologue,
                                     sizeof(kFindCityPrologue), "city search");
    if (!g_haveSearch)
    {
        Logf("cities   the search could not be hooked - inactive");
        return 1;
    }

    g_haveNeutered = Repoint(RVA_LIMIT_SITE, kMovssX7, sizeof(kMovssX7),
                             RVA_LIMIT_CONST, VANILLA_LIMIT_SQ,
                             &g_slot[SLOT_ZERO], "search limit");
    if (!g_haveNeutered)
        Logf("cities   the original search is still live: a building outside every city"
             " may be adopted by one within 1000 m instead of getting its own");

    if (!InstallInlineHook(g_exeBase + P_REASSIGN, (void*)h_Reassign,
                           (void**)&o_Reassign, kReassignPrologue,
                           sizeof(kReassignPrologue), "city reassign"))
    {
        // Its own box is 1000 and it dereferences a null old city, so the two
        // repointed delete boxes below are what is left to keep it honest.
        o_Reassign = (t_Reassign)(g_exeBase + P_REASSIGN);
        Logf("cities   reassignment not hooked - the game's own runs, and it is"
             " neither null-safe nor wider than 1000 m");
    }

    Repoint(RVA_BOX_DELETE, kMovssX6, sizeof(kMovssX6),
            RVA_BOX_CONST, VANILLA_BOX, &g_slot[SLOT_BOX], "delete box");
    Repoint(RVA_BOX_CANDELETE, kMovssX6, sizeof(kMovssX6),
            RVA_BOX_CONST, VANILLA_BOX, &g_slot[SLOT_BOX], "candelete box");

    // The dot grid. All five or none: a scan whose radius and chord disagree
    // takes the square root of a negative number and paints nothing at all.
    if (g_overlay)
    {
        bool ok = Repoint(RVA_OVL_R_START, kMovssX11, sizeof(kMovssX11),
                          RVA_BOX_CONST, VANILLA_BOX, &g_slot[SLOT_OVL_R], "dots radius")
               && PatchDotsEndBound(&g_slot[SLOT_OVL_R])
               && Repoint(RVA_OVL_RSQ, kMovssX11, sizeof(kMovssX11),
                          RVA_LIMIT_CONST, VANILLA_LIMIT_SQ, &g_slot[SLOT_OVL_RSQ], "dots chord")
               && Repoint(RVA_OVL_STEP_Z, kAddssX6, sizeof(kAddssX6),
                          RVA_OVL_STEP_CONST, VANILLA_OVL_STEP, &g_slot[SLOT_OVL_STEP], "dots step z")
               && Repoint(RVA_OVL_STEP_X, kAddssX8, sizeof(kAddssX8),
                          RVA_OVL_STEP_CONST, VANILLA_OVL_STEP, &g_slot[SLOT_OVL_STEP], "dots step x");
        if (!ok)
        {
            Logf("cities   the ground overlay is only partly patched - it may paint the"
                 " wrong area. Set overlay = 0 if it looks wrong");
        }
    }

    if (g_window)
    {
        if (!InstallInlineHook(g_exeBase + P_CITY_WINDOW, (void*)h_CityWindow,
                               (void**)&o_CityWindow, kWindowPrologue,
                               sizeof(kWindowPrologue), "city window"))
        {
            Logf("cities   window not hooked - the radius can only come from the ini");
            g_window = 0;
        }
    }

    if (o_Fwrite && !PatchCall(P_SAVE_CALL, P_SAVE_CITIES, (void*)h_SaveCities,
                               (void**)&o_SaveCities, "city save"))
        o_SaveCities = NULL;
    if (o_Fread && !PatchCall(P_LOAD_CALL, P_LOAD_CITIES, (void*)h_LoadCities,
                              (void**)&o_LoadCities, "city load"))
        o_LoadCities = NULL;

    // Only worth having if the radii are read in the first place.
    if (o_LoadCities && !PatchCall(P_WORLD_LOAD_CALL, P_WORLD_LOAD, (void*)h_WorldLoad,
                                  (void**)&o_WorldLoad, "world load"))
        Logf("cities   no settle pass after loading - a save whose radii differ from the"
             " defaults keeps the assignment the game made on the way in");

    Logf("cities   default %.0f m %s, slider %.0f..%.0f m, adopt %s",
         g_defRadius, g_defSquare ? "square" : "circle",
         g_radiusMin, g_radiusMax, g_adopt ? "on" : "off");
    return 0;
}

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) { return TRUE; }
