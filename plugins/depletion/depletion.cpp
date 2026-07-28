// depletion - deposits that run out, as a tesmioloader plugin.
//
// In the base game a deposit is infinite. A mine samples the map once when it
// is built, averages the richness it found into "quality of source", and from
// then on produces at that rate forever - nothing ever writes the richness back
// down.
//
//   rva 0x1B3690  FUN_1401b3690(game, building)   one mine, one tick. Called
//                 from the building dispatcher at 0x139A80 for every building
//                 whose type is 7. (0x1B1220, its twin, is the type 92 water
//                 well; that one already resamples itself and is left alone.)
//
//   building+0xDDC   production rate this tick, recomputed every call as
//                    workers * quality * max. Zero when the mine is stopped,
//                    unpowered, unstaffed or its storage is full, so
//                    integrating it charges only for output that really
//                    happened.
//   building+0xDF8   quality of source, 0..1. Written once at construction and
//                    then never again for an ore mine - so it is free for this
//                    plugin to own.
//
// So: integrate the production rate, and take the tonnage out of the deposit
// map itself. Draining the texture rather than a private counter is what makes
// depletion persist through a save, show under the minimap overlay, and be
// shared by every mine over the same patch.
//
//
// WHY THE TEXTURE IS NEVER TOUCHED FROM THE TICK
//
// The first version of this plugin resampled each mine from the tick by calling
// the game's own scan at 0x1DD190, the way the water branch of 0x1B3690 does.
// **That crashed the graphics driver**, and the reason is worth writing down
// because nothing about it is visible from the game's side.
//
// `TextureAccessOpen` on `C3DAPI_D3D11_TEXTURE` is not a lock. Disassembled:
//
//   ID3D11Device::CreateTexture2D(&desc, 0, &staging)   // USAGE_STAGING, CPU rw
//   ID3D11DeviceContext::CopyResource(staging, gpuTex)  // vtbl+0x178
//   ID3D11DeviceContext::Map(staging, ...)              // vtbl+0x70
//
// and `TextureAccessClose` is `Unmap` plus the `CopyResource` back. That is the
// **immediate context**, which is not thread-safe and must not be touched from
// a simulation thread while the renderer is using it - and it drags the whole
// 4 MB map across the bus in both directions every time.
//
// The base game never does this from a building tick. Every caller of 0x1DD190
// that maps a texture is a main-thread path: building placement (0x2BAD70,
// 0x7ABBD0) and the terrain editor (0x30D100). The one tick-side caller is the
// water branch - and water, deposit type 8 or 9, is excluded from both of that
// function's texture guards (`type < 3` and `type - 6 < 2`), so it is precisely
// the one deposit type that touches no texture at all. Taking it as a template
// missed exactly the thing that made it safe.
//
// So the work is split:
//
//   the tick     arithmetic only. Integrates production, spends it against a
//                reserve held in this plugin, and writes quality of source.
//                Reads two floats out of the building and nothing else.
//   the flusher  seeds a mine's reserve from the map, and writes accumulated
//                depletion back into it. Runs from an import hook on
//                C3D_TERRAIN::Render - unambiguously the render thread, and
//                rate-limited, so one Map/Unmap pair covers many seconds of
//                mining across every mine at once.
//
// Everything here is addresses for SOVIET64.exe v1.1.1.7. See
// docs/08-depletion.md.

#include "../../src/tesmio_plugin.h"

// The deposit registry, if the `deposits` plugin is installed. Consumed in
// Start rather than Init, because that is the phase at which every plugin has
// published. Null when it is not there, which only means no mod deposits - the
// base game's own keep working.
static const TsmDepositApi* D;

// ---------------------------------------------------------------- the game

#define P_MINE_TICK_RVA   0x1B3690   // FUN_1401b3690 - one mine, one tick
#define P_DEPOSIT_RADIUS  0x1DCA70   // FUN_1401dca70 - deposit type -> search radius
#define P_GAMEOBJ         0x9941F0   // the pointer to the main game object
#define P_TIMER_OBJ       0x9D4EE0   // the C3D_TIMER the whole simulation steps on
#define P_TIMER_STEP      0x909B14   // 0.001f, the argument every PowerTime call passes
#define P_PROD_PERIOD     0x90AA90   // 60.0f, what the rate is divided by per tick

#define P_MAP1_OFF        0xF00      // gameobj -> resourcemap
#define P_MAP2_OFF        0xF08      // gameobj -> resourcemap2
#define P_TERRAIN_OFF     0xED8      // gameobj -> C3D_TERRAIN
#define P_TERRAIN_MASK    0x158      // C3D_TERRAIN -> its mask texture
                                     // Read off the type-3 case at 0x1DD910:
                                     //   mov r9,[rcx+0x158] / call the sampler
                                     //   movss xmm0,[rax+8]      component 2

#define B_TYPEDESC        0x318      // building -> its type descriptor
#define B_NODE            0x320      // building -> its C3D_NODE
#define B_PRODRATE        0xDDC      // production rate this tick
#define B_QUALITY         0xDF8      // quality of source, 0..1

#define T_BUILDING_TYPE   0x360      // typedesc -> building type; 7 is a mine
#define T_DEPOSIT_TYPE    0x368      // typedesc -> deposit type

#define TEX_SLOT_OPEN     16         // vtbl+0x80  TextureAccessOpen
#define TEX_SLOT_CLOSE    18         // vtbl+0x90  TextureAccessClose
#define TEX_SLOT_GETTEXEL 20         // vtbl+0xA0  TextureAccesGetTexel
#define TEX_SLOT_SETTEXEL 23         // vtbl+0xB8  TextureAccesSetTexel
#define TEX_WIDTH_OFF     0x14
#define TEX_HEIGHT_OFF    0x18

#define SYM_TERRAIN_RENDER "?Render@C3D_TERRAIN@@QEAAX_NPEAVC3D_CAMERA@@0HH@Z"

// ---- the mine's own information panel -----------------------------------
//
// rva 0x786AC0  FUN_140786ac0(game, window, ...)  builds the panel that shows
// "Quality of source" and "Current production per workday". Found by scanning
// .text for reads of building+0xDF8 and matching the label ids it passes to
// C3D_LANGUAGE::GetString: 0xC82 is "Quality of source", 0xC83 is "Current
// production per workday" (tools/assets/btf.py resolves an id to its text).
//
// Its layout is a running Y and a fixed X, both plain locals:
//
//   x = DPI*[0x90ABB0] + (window[0x04] - DPI*[0x90A8E4] + window[0x28] + DPI*[0x90A9E4])
//   y = DPI*[0x90AA08] + window[0x08] + window[0x2C]        first row
//   y += DPI*[0x90A9E4]                                     after each row
//
// and it ends with `window[0x250] = y`. **That field is the panel's height**,
// so a post-hook can read where the game stopped, draw one more row there, and
// write the new bottom back - which is what makes the window grow by exactly
// one line instead of the text landing on the frame.
#define P_MINE_PANEL_RVA  0x786AC0
#define P_FONTMANAGER     0x996FB0   // C3D_FONTMANAGER, by address
#define P_PANEL_FONT      0x995220   // the C3D_FONT* the panel's rows use
#define P_DPI             0x992088   // the scale every layout constant is multiplied by
#define P_ROW_STEP        0x90A9E4   // 35 - one row, and the x indent as well
#define P_X_BACK          0x90A8E4   // 15
#define P_X_INDENT        0x90ABB0   // 110
#define W_BUILDING        0x240      // window -> the building it is about
#define W_BOTTOM          0x250      // window -> the running Y, written at the end
#define W_HIDDEN          0x01       // non-zero while the panel is not drawn
#define W_X               0x04
#define W_Y               0x08
#define W_OFFX            0x28
#define W_OFFY            0x2C
#define PANEL_LABEL_COLOUR 0xFF990000u   // what every label row on this panel uses

// The map a deposit's channel lives in. The first two match TSM_MAP_*; the
// third has no deposits.ini spelling because the spliced dispatch chain has no
// case that loads it - only the base game's gravel does.
#define MAP_RESOURCEMAP   0
#define MAP_RESOURCEMAP2  1
#define MAP_TERRAIN       2
#define MAP_COUNT         3

typedef void   (*t_MineTick)(void*, void*);
typedef void   (*t_MinePanel)(void*, void*, void*);
typedef void   (*t_PrintLeftUnicode)(void*, void*, float, float, unsigned long,
                                     const wchar_t*, ...);
typedef void   (*t_TerrainRender)(void*, bool, void*, void*, int, int);
typedef float  (*t_DepositRadius)(int);
typedef float* (*t_VecGetter)(void*, float*);          // hidden-return C3DVECTOR2/3
typedef float  (*t_PowerTime)(void*, float, bool, bool);
typedef void   (*t_TexVoid)(void*);
typedef DWORD  (*t_TexGetTexel)(void*, int, int);
typedef void   (*t_TexSetTexel)(void*, int, int, DWORD);

static t_MineTick      o_MineTick;
static t_MinePanel     o_MinePanel;
static t_PrintLeftUnicode o_PrintLeftUnicode;
static t_TerrainRender o_TerrainRender;
static t_DepositRadius o_DepositRadius;
static t_VecGetter     o_NodeGetPosition;
static t_VecGetter     o_TerrainGetOffset;
static t_VecGetter     o_TerrainGetSize;
static t_PowerTime     o_PowerTime;
static t_TexVoid       o_MaskTextureOpen;   // C3D_TERRAIN's, not the texture's own
static t_TexVoid       o_MaskTextureClose;

// ---------------------------------------------------------------- settings

static int   g_enabled        = 1;
static float g_tonnesPerTexel = 1200.0f;
static float g_flushSeconds   = 5.0f;     // real seconds between map writes
static float g_logSeconds     = 60.0f;    // real seconds between progress lines; 0 = off
static int   g_panel          = 1;        // the extra row in the mine's window
static char  g_panelCaption[64] = "Deposit remaining";
static char  g_vanillaList[192] = "oil,iron,coal,uranium,bauxite,gravel:30000";

#define MAX_DEPLETABLE 24

struct DepletableDef
{
    char  name[32];
    int   type;
    int   map;
    int   component;
    float tonnesPerTexel;
};
static DepletableDef g_depletable[MAX_DEPLETABLE];
static int           g_depletableCount;

// Every base-game deposit a mine draws a stock from. Wood (4) is left out
// because it regrows on its own, and water (8, 9) because it is not a stock.
//
// Gravel is the odd one and is deliberately last. It does not live in a
// resource map at all: the type-3 case samples component 2 of the **terrain's
// own mask texture**, the same one the editor's material brush paints, so
// working a gravel pit down visibly wears the ground texture away under it.
// That is arguably what a gravel pit looks like, but it is a real change to the
// terrain and not just to an invisible channel - drop `gravel` from `vanilla`
// to keep the base game's behaviour.
//
// Its search radius is 30 against an ore mine's 210, so its footprint is a few
// dozen texels rather than a few hundred and it needs a much larger figure per
// texel to last a comparable time. Hence the `name:tonnes` syntax.
static const struct { const char* name; int type; int map; int component; } kVanillaDeposits[] = {
    { "oil",     0, MAP_RESOURCEMAP,  0 },
    { "iron",    1, MAP_RESOURCEMAP,  1 },
    { "coal",    2, MAP_RESOURCEMAP,  2 },
    { "uranium", 6, MAP_RESOURCEMAP2, 0 },
    { "bauxite", 7, MAP_RESOURCEMAP2, 1 },
    { "gravel",  3, MAP_TERRAIN,      2 },
};
#define VANILLA_DEPOSITS ((int)(sizeof(kVanillaDeposits) / sizeof(kVanillaDeposits[0])))

// ---------------------------------------------------------------- per-mine state
//
// One unit is one step of one texel's value, so a footprint's whole reserve is
// the sum of its texel values - an integer count of things the flusher can
// actually take away. Tonnes only ever appear on the way in.
//
// `reserve` and `spent` are the model; the texture is where `spent` eventually
// lands. Between flushes the two disagree by less than a flush interval's
// output, and after a reload the model is rebuilt from the texture, so the
// texture is always the thing that survives.

#define MAX_MINE_STATES 256

#define MINE_NEW       0    // seen by the tick, not yet seeded from the map
#define MINE_ACTIVE    1
#define MINE_EXHAUSTED 2
#define MINE_DEAD      3    // no footprint, or seeding failed; leave it alone

struct MineState
{
    void*  building;
    DWORD  lastSeen;
    int    state;
    int    dep;             // index into g_depletable
    float  baseQuality;     // quality of source when it was seeded
    double reserve0;        // units under it then
    double reserve;         // units under it now
    double spent;           // units mined but not yet written to the map
    int    x0, y0, w, h;    // footprint, as a texel bounding box
    float  cxT, cyT, rT;    // footprint centre and radius, in texels
    int    cursor;          // sweep position within the box
    int    logged;
    DWORD  lastLog;
};
static MineState g_mine[MAX_MINE_STATES];
static DWORD     g_mineTick;
static LONG      g_pending;          // mines needing a seed or a write
static DWORD     g_tickThread;       // logged once, to settle which thread is which
static DWORD     g_renderThread;

static const DepletableDef* DepletableFor(int type)
{
    for (int i = 0; i < g_depletableCount; i++)
        if (g_depletable[i].type == type) return &g_depletable[i];
    return NULL;
}

static int DepletableIndex(int type)
{
    for (int i = 0; i < g_depletableCount; i++)
        if (g_depletable[i].type == type) return i;
    return -1;
}

static MineState* MineStateFor(void* b)
{
    unsigned hash = (unsigned)((((size_t)b) >> 4) * 2654435761u);
    int home = (int)(hash % MAX_MINE_STATES);
    int freeSlot = -1, oldest = home;

    for (int n = 0; n < 16; n++)
    {
        int i = (home + n) % MAX_MINE_STATES;
        if (g_mine[i].building == b) { g_mine[i].lastSeen = g_mineTick; return &g_mine[i]; }
        if (!g_mine[i].building && freeSlot < 0) freeSlot = i;
        if (g_mine[i].lastSeen < g_mine[oldest].lastSeen) oldest = i;
    }

    int i = freeSlot >= 0 ? freeSlot : oldest;
    memset(&g_mine[i], 0, sizeof(g_mine[i]));
    g_mine[i].building = b;
    g_mine[i].lastSeen = g_mineTick;
    g_mine[i].state    = MINE_NEW;
    return &g_mine[i];
}

// ---------------------------------------------------------------- the maps

// Both maps and the terrain are read off the global at 0x9941F0 rather than off
// the tick's own first argument. The two are almost certainly the same object -
// every function in the game uses them interchangeably - but "almost certainly"
// is not a good enough basis for a pointer that ends up on the left of a store.
static BYTE* GameObject()
{
    return *(BYTE**)(g_exeBase + P_GAMEOBJ);
}

static void* TerrainObject()
{
    BYTE* game = GameObject();
    return game ? *(void**)(game + P_TERRAIN_OFF) : NULL;
}

static void* DepositTexture(int map)
{
    if (map == MAP_TERRAIN)
    {
        void* terrain = TerrainObject();
        return terrain ? *(void**)((BYTE*)terrain + P_TERRAIN_MASK) : NULL;
    }
    BYTE* game = GameObject();
    if (!game) return NULL;
    return *(void**)(game + (map == MAP_RESOURCEMAP2 ? P_MAP2_OFF : P_MAP1_OFF));
}

// The two resource maps are bracketed through their own vtable, the terrain
// mask through C3D_TERRAIN - exactly as 0x1DD190 does for each. Both end in
// ID3D11DeviceContext::Map, so this only ever runs on the render thread.
static void DepositTextureAccess(int map, void* tex, bool open)
{
    if (map == MAP_TERRAIN)
    {
        void* terrain = TerrainObject();
        t_TexVoid fn = open ? o_MaskTextureOpen : o_MaskTextureClose;
        if (terrain && fn) fn(terrain);
        return;
    }
    void**    vt = *(void***)tex;
    t_TexVoid fn = (t_TexVoid)vt[open ? TEX_SLOT_OPEN : TEX_SLOT_CLOSE];
    if (fn) fn(tex);
}

// Where the component of one texel sits inside the word GetTexel returns.
// Straight out of the editor brush at 0x238B00: channel & 3 == 0 is the alpha
// byte, which is component 3; 1, 2 and 3 are components 0, 1 and 2 at bits 16,
// 8 and 0.
static int ComponentShift(int component)
{
    return component == 3 ? 24 : (16 - component * 8);
}

// The sampler's mapping at 0x8360, and the brush's at 0x238B00, are the same
// one: normalise the world position against the terrain's offset and size, then
// scale by the texture's own dimensions.
static bool WorldToTexel(void* tex, float wx, float wz, float* txOut, float* tyOut)
{
    void* terrain = TerrainObject();
    if (!terrain || !tex || !o_TerrainGetOffset || !o_TerrainGetSize) return false;

    float ob[4] = {0}, sb[4] = {0};
    const float* off  = o_TerrainGetOffset(terrain, ob);
    const float* size = o_TerrainGetSize(terrain, sb);
    if (!off || !size || size[0] == 0.0f || size[1] == 0.0f) return false;

    int tw = *(int*)((BYTE*)tex + TEX_WIDTH_OFF);
    int th = *(int*)((BYTE*)tex + TEX_HEIGHT_OFF);
    if (tw <= 0 || th <= 0) return false;

    *txOut = (wx - off[0]) / size[0] * (float)tw;
    *tyOut = (wz - off[2]) / size[1] * (float)th;
    return true;
}

// ---------------------------------------------------------------- the tick
//
// Arithmetic only. No engine call but PowerTime, no texture, no allocation -
// whatever thread this is on, it cannot upset anything.

static void DepleteMine(BYTE* b)
{
    BYTE* typeDesc = *(BYTE**)(b + B_TYPEDESC);
    if (!typeDesc || !ReadablePtr(typeDesc + T_BUILDING_TYPE, 12)) return;
    if (*(int*)(typeDesc + T_BUILDING_TYPE) != 7) return;

    int dep = DepletableIndex(*(int*)(typeDesc + T_DEPOSIT_TYPE));
    if (dep < 0) return;

    MineState* st = MineStateFor(b);
    if (!st) return;

    if (st->state == MINE_NEW)
    {
        st->dep = dep;
        InterlockedExchange(&g_pending, 1);      // the flusher will seed it
        return;
    }
    if (st->state == MINE_DEAD) return;

    // The rate the tick just settled on: zero unless the mine really produced,
    // because 0x1B3690 has already scaled it by workers, power, shift count and
    // how much room the storage had left.
    float rate = *(float*)(b + B_PRODRATE);
    if (rate > 0.0f && st->state == MINE_ACTIVE)
    {
        float step = o_PowerTime(g_exeBase + P_TIMER_OBJ,
                                 *(float*)(g_exeBase + P_TIMER_STEP), false, false);
        float tonnes = step / *(float*)(g_exeBase + P_PROD_PERIOD) * rate;
        double units = (double)tonnes * 255.0 / g_depletable[st->dep].tonnesPerTexel;

        st->reserve -= units;
        st->spent   += units;
        if (st->reserve <= 0.0) { st->reserve = 0.0; st->state = MINE_EXHAUSTED; }
        if (st->spent >= 1.0) InterlockedExchange(&g_pending, 1);
    }

    // Quality of source is the weighted average richness under the mine, and a
    // uniform drain lowers that average in proportion to what has been taken -
    // so this is not an approximation of the game's own formula, it is what the
    // formula gives once the map is drawn down evenly.
    double left = st->reserve0 > 0.0 ? st->reserve / st->reserve0 : 0.0;
    if (left < 0.0) left = 0.0;
    if (left > 1.0) left = 1.0;
    *(float*)(b + B_QUALITY) = st->baseQuality * (float)left;
}

static void h_MineTick(void* game, void* building)
{
    o_MineTick(game, building);
    if (!g_enabled || !building) return;

    if (!g_tickThread)
    {
        g_tickThread = GetCurrentThreadId();
        Logf("deplete  mine tick runs on thread %lu (render thread %lu)",
             g_tickThread, g_renderThread);
    }

    g_mineTick++;
    __try { DepleteMine((BYTE*)building); }
    __except (FaultFilter("deposit depletion tick", GetExceptionInformation()))
    {
        Logf("deplete  disabled after a fault");
        g_enabled = 0;
    }
}

// ---------------------------------------------------------------- the flusher
//
// Render thread, rate-limited. Everything that maps the deposit map happens
// here, once per pass, for every mine that needs it.

static bool MineFootprint(MineState* st, void* tex, BYTE* b)
{
    float pb[4] = {0};
    const float* pos = o_NodeGetPosition(b + B_NODE, pb);
    if (!pos) return false;

    BYTE* typeDesc = *(BYTE**)(b + B_TYPEDESC);
    if (!typeDesc) return false;
    float radius = o_DepositRadius(*(int*)(typeDesc + T_DEPOSIT_TYPE));
    if (radius <= 0.0f) return false;

    float cx, cy, ex, ey;
    if (!WorldToTexel(tex, pos[0], pos[2], &cx, &cy)) return false;
    if (!WorldToTexel(tex, pos[0] + radius, pos[2] + radius, &ex, &ey)) return false;

    float rx = ex - cx, ry = ey - cy;
    float rT = (rx > ry ? rx : ry);
    if (rT < 1.0f) rT = 1.0f;

    int tw = *(int*)((BYTE*)tex + TEX_WIDTH_OFF);
    int th = *(int*)((BYTE*)tex + TEX_HEIGHT_OFF);
    int x0 = (int)(cx - rT), x1 = (int)(cx + rT) + 1;
    int y0 = (int)(cy - rT), y1 = (int)(cy + rT) + 1;
    if (x0 < 0) x0 = 0;  if (x1 > tw) x1 = tw;
    if (y0 < 0) y0 = 0;  if (y1 > th) y1 = th;
    if (x1 <= x0 || y1 <= y0) return false;

    st->x0 = x0; st->y0 = y0; st->w = x1 - x0; st->h = y1 - y0;
    st->cxT = cx; st->cyT = cy; st->rT = rT;
    st->cursor = 0;
    return true;
}

static bool InFootprint(const MineState* st, int tx, int ty)
{
    float dx = (float)tx + 0.5f - st->cxT;
    float dy = (float)ty + 0.5f - st->cyT;
    return dx * dx + dy * dy <= st->rT * st->rT;
}

// The reserve is simply the sum of the component over every texel the mine can
// reach: a count of the steps that can still be taken away from it.
static void SeedMine(MineState* st, void* tex, BYTE* b, t_TexGetTexel get, int shift, DWORD mask)
{
    if (!MineFootprint(st, tex, b)) { st->state = MINE_DEAD; return; }

    double sum = 0.0;
    for (int ty = st->y0; ty < st->y0 + st->h; ty++)
        for (int tx = st->x0; tx < st->x0 + st->w; tx++)
            if (InFootprint(st, tx, ty))
                sum += (double)((get(tex, tx, ty) & mask) >> shift);

    st->baseQuality = *(float*)(b + B_QUALITY);
    st->reserve0    = sum;
    st->reserve     = sum;
    st->spent       = 0.0;
    st->state       = sum > 0.0 ? MINE_ACTIVE : MINE_EXHAUSTED;

    if (!st->logged)
    {
        st->logged = 1;
        const DepletableDef* d = &g_depletable[st->dep];
        Logf("deplete  %s mine: %d x %d texel box, %.0f units under it "
             "(%.0f t), quality %.3f",
             d->name, st->w, st->h, sum, sum * d->tonnesPerTexel / 255.0,
             st->baseQuality);
    }
}

// Take whole units out of the map, one texel value each, sweeping the footprint
// with a cursor so the drain spreads over the whole area instead of boring a
// hole under the middle of the building.
static void WriteMine(MineState* st, void* tex, t_TexGetTexel get, t_TexSetTexel set,
                      int shift, DWORD mask, int texW, int texH)
{
    int cells = st->w * st->h;
    if (cells <= 0) return;

    while (st->spent >= 1.0)
    {
        bool took = false;
        for (int n = 0; n < cells; n++)
        {
            int i = st->cursor;
            st->cursor = (st->cursor + 1) % cells;

            int tx = st->x0 + i % st->w;
            int ty = st->y0 + i / st->w;
            if (tx < 0 || ty < 0 || tx >= texW || ty >= texH) continue;
            if (!InFootprint(st, tx, ty)) continue;

            DWORD c = get(tex, tx, ty);
            DWORD v = (c & mask) >> shift;
            if (v == 0) continue;

            set(tex, tx, ty, (c & ~mask) | ((v - 1) << shift));
            took = true;
            break;
        }
        if (!took)
        {
            // Nothing left anywhere in the footprint - somebody else mined it
            // out from under us. Drop the debt rather than spin on it.
            st->state = MINE_EXHAUSTED;
            st->reserve = 0.0;
            st->spent = 0.0;
            break;
        }
        st->spent -= 1.0;
    }
}

// One map, one Map/Unmap pair, every mine on it.
static void FlushMap(int map)
{
    int first = -1;
    for (int i = 0; i < MAX_MINE_STATES; i++)
    {
        MineState* st = &g_mine[i];
        if (!st->building || st->state == MINE_DEAD) continue;
        if (st->dep < 0 || st->dep >= g_depletableCount) continue;
        if (g_depletable[st->dep].map != map) continue;
        if (st->state == MINE_NEW || st->spent >= 1.0) { first = i; break; }
    }
    if (first < 0) return;

    void* tex = DepositTexture(map);
    if (!tex) return;
    int texW = *(int*)((BYTE*)tex + TEX_WIDTH_OFF);
    int texH = *(int*)((BYTE*)tex + TEX_HEIGHT_OFF);
    if (texW <= 0 || texH <= 0) return;

    void**        vt  = *(void***)tex;
    t_TexGetTexel get = (t_TexGetTexel)vt[TEX_SLOT_GETTEXEL];
    t_TexSetTexel set = (t_TexSetTexel)vt[TEX_SLOT_SETTEXEL];
    if (!get || !set) return;

    DepositTextureAccess(map, tex, true);
    for (int i = 0; i < MAX_MINE_STATES; i++)
    {
        MineState* st = &g_mine[i];
        if (!st->building || st->state == MINE_DEAD) continue;
        if (st->dep < 0 || st->dep >= g_depletableCount) continue;
        if (g_depletable[st->dep].map != map) continue;

        int shift  = ComponentShift(g_depletable[st->dep].component);
        DWORD mask = 0xFFu << shift;

        if (st->state == MINE_NEW)
            SeedMine(st, tex, (BYTE*)st->building, get, shift, mask);
        else if (st->spent >= 1.0)
            WriteMine(st, tex, get, set, shift, mask, texW, texH);

    }
    DepositTextureAccess(map, tex, false);
}

// Until there is a readout in the mine's own window this is the only way to
// watch a deposit go down: at the default figure a mine holds years of output,
// so nothing moves on the minute scale and "is it working at all" needs an
// answer with numbers in it. It reads no texture, so it sits outside the
// bracket - and outside FlushMap, which returns early whenever there is nothing
// to write, which is exactly when this line is most wanted.
static void LogProgress()
{
    if (g_logSeconds <= 0.0f) return;
    DWORD now = GetTickCount();

    for (int i = 0; i < MAX_MINE_STATES; i++)
    {
        MineState* st = &g_mine[i];
        if (!st->building || st->state == MINE_NEW || st->state == MINE_DEAD) continue;
        if (st->reserve0 <= 0.0) continue;
        if (st->dep < 0 || st->dep >= g_depletableCount) continue;
        if ((DWORD)(now - st->lastLog) <= (DWORD)(g_logSeconds * 1000.0f)) continue;
        if (!ReadablePtr((BYTE*)st->building + B_QUALITY, 4)) continue;

        st->lastLog = now;
        const DepletableDef* d = &g_depletable[st->dep];
        double perTonne = d->tonnesPerTexel / 255.0;
        Logf("deplete  %s mine: %.2f%% left, %.0f of %.0f t, quality %.3f%s",
             d->name, 100.0 * st->reserve / st->reserve0,
             st->reserve * perTonne, st->reserve0 * perTonne,
             *(float*)((BYTE*)st->building + B_QUALITY),
             st->state == MINE_EXHAUSTED ? "  EXHAUSTED" : "");
    }
}

static void FlushAll()
{
    for (int map = 0; map < MAP_COUNT; map++) FlushMap(map);
    LogProgress();
}

static void h_TerrainRender(void* self, bool a, void* cam1, void* cam2, int b, int c)
{
    if (g_enabled)
    {
        if (!g_renderThread) g_renderThread = GetCurrentThreadId();

        // Before the terrain draws, not in the middle of it - and only when
        // there is work, because Open drags the whole map to system memory and
        // Close drags it back.
        static DWORD lastFlush;
        DWORD now = GetTickCount();
        if (InterlockedCompareExchange(&g_pending, 0, 1) == 1 ||
            (DWORD)(now - lastFlush) > (DWORD)(g_flushSeconds * 1000.0f))
        {
            if ((DWORD)(now - lastFlush) > 250)      // never more than 4 per second
            {
                lastFlush = now;
                __try { FlushAll(); }
                __except (FaultFilter("deposit depletion flush", GetExceptionInformation()))
                {
                    Logf("deplete  disabled after a fault in the flush");
                    g_enabled = 0;
                }
            }
            else InterlockedExchange(&g_pending, 1);  // try again next frame
        }
    }
    o_TerrainRender(self, a, cam1, cam2, b, c);
}

// ---------------------------------------------------------------- the panel row
//
// One more row on the mine's own window, under "Current production per
// workday". Drawn from a post-hook, so the game has already laid the panel out
// and left its bottom edge in window[0x250]; this reads it, prints there, and
// puts the new bottom back.

static float LayoutF(DWORD rva) { return *(float*)(g_exeBase + rva); }

// Tonnes read better with a unit than with seven digits. The thresholds are
// where the extra digits stop carrying information, not where the SI prefix
// changes.
static void FormatTonnes(double t, wchar_t* out, size_t n)
{
    if (t >= 1000000.0)     _snwprintf_s(out, n, _TRUNCATE, L"%.2f Mt", t / 1000000.0);
    else if (t >= 10000.0)  _snwprintf_s(out, n, _TRUNCATE, L"%.1f kt", t / 1000.0);
    else                    _snwprintf_s(out, n, _TRUNCATE, L"%.0f t",  t);
}

static MineState* FindMine(void* b)
{
    unsigned hash = (unsigned)((((size_t)b) >> 4) * 2654435761u);
    int home = (int)(hash % MAX_MINE_STATES);
    for (int n = 0; n < 16; n++)
    {
        MineState* st = &g_mine[(home + n) % MAX_MINE_STATES];
        if (st->building == b) return st;
    }
    return NULL;
}

static void DrawReserveRow(BYTE* win)
{
    // The panel builder's own first test: nothing is drawn while this is set.
    if (*(char*)(win + W_HIDDEN) != 0) return;

    BYTE* b = *(BYTE**)(win + W_BUILDING);
    if (!b || !ReadablePtr(b + B_TYPEDESC, 8)) return;

    MineState* st = FindMine(b);
    if (!st || st->reserve0 <= 0.0) return;
    if (st->state != MINE_ACTIVE && st->state != MINE_EXHAUSTED) return;
    if (st->dep < 0 || st->dep >= g_depletableCount) return;

    double perTonne = g_depletable[st->dep].tonnesPerTexel / 255.0;
    double left     = st->reserve * perTonne;
    double total    = st->reserve0 * perTonne;
    double pct      = 100.0 * st->reserve / st->reserve0;

    wchar_t leftTxt[32], totalTxt[32], caption[64], line[192];
    FormatTonnes(left, leftTxt, 32);
    FormatTonnes(total, totalTxt, 32);
    MultiByteToWideChar(CP_ACP, 0, g_panelCaption, -1, caption, 64);
    _snwprintf_s(line, _TRUNCATE, L"%ls: %ls / %ls  (%.1f %%)",
                 caption, leftTxt, totalTxt, pct);

    float dpi = LayoutF(P_DPI);
    float x = dpi * LayoutF(P_X_INDENT)
            + (*(float*)(win + W_X) - dpi * LayoutF(P_X_BACK))
            + *(float*)(win + W_OFFX) + dpi * LayoutF(P_ROW_STEP);
    float y = *(float*)(win + W_BOTTOM);

    void*  fontMgr = g_exeBase + P_FONTMANAGER;
    void*  font    = *(void**)(g_exeBase + P_PANEL_FONT);
    if (!font) return;

    o_PrintLeftUnicode(fontMgr, font, x, y, PANEL_LABEL_COLOUR, L"%ls", line);

    // Give the window the row back, so the frame grows instead of clipping it.
    *(float*)(win + W_BOTTOM) = y + dpi * LayoutF(P_ROW_STEP);
}

static void h_MinePanel(void* game, void* window, void* p3)
{
    o_MinePanel(game, window, p3);
    if (!g_enabled || !g_panel || !window) return;

    __try { DrawReserveRow((BYTE*)window); }
    __except (FaultFilter("deposit depletion panel", GetExceptionInformation()))
    {
        Logf("deplete  panel row disabled after a fault");
        g_panel = 0;
    }
}

// ---------------------------------------------------------------- setup

static void ReadSettings()
{
    const char* ini = "plugins\\depletion.ini";
    char v[192];

    g_enabled = H->configInt(ini, "depletion", "enabled", g_enabled);

    if (H->configString(ini, "depletion", "tonnes_per_texel", v, sizeof(v), "") && v[0]
        && atof(v) > 0.0)
        g_tonnesPerTexel = (float)atof(v);

    if (H->configString(ini, "depletion", "flush_seconds", v, sizeof(v), "") && v[0]
        && atof(v) > 0.0)
        g_flushSeconds = (float)atof(v);

    if (H->configString(ini, "depletion", "log_seconds", v, sizeof(v), "") && v[0])
        g_logSeconds = (float)atof(v);

    g_panel = H->configInt(ini, "depletion", "panel", g_panel);
    H->configString(ini, "depletion", "panel_caption", g_panelCaption,
                    sizeof(g_panelCaption), g_panelCaption);

    if (H->configString(ini, "depletion", "vanilla", v, sizeof(v), "") && v[0])
        strncpy_s(g_vanillaList, sizeof(g_vanillaList), v, _TRUNCATE);
}

static void Add(const char* name, int type, int map, int component, float tonnes)
{
    if (g_depletableCount >= MAX_DEPLETABLE) return;
    DepletableDef* t = &g_depletable[g_depletableCount++];
    strncpy_s(t->name, sizeof(t->name), name, _TRUNCATE);
    t->type           = type;
    t->map            = map;
    t->component      = component;
    t->tonnesPerTexel = tonnes;
}

static void BuildTable()
{
    // "oil,iron,gravel:30000" - a name, optionally with its own figure per
    // texel. Tokenised rather than searched, so a name is matched whole.
    char list[192];
    strncpy_s(list, sizeof(list), g_vanillaList, _TRUNCATE);
    _strlwr_s(list, sizeof(list));
    bool all  = (strcmp(list, "all") == 0 || strcmp(list, "1") == 0);
    bool none = (strcmp(list, "none") == 0 || strcmp(list, "0") == 0 || !list[0]);

    bool  want[VANILLA_DEPOSITS];
    float per [VANILLA_DEPOSITS];
    for (int i = 0; i < VANILLA_DEPOSITS; i++) { want[i] = all && !none; per[i] = g_tonnesPerTexel; }

    if (!all && !none)
    {
        char* ctx = NULL;
        for (char* tok = strtok_s(list, ",;", &ctx); tok; tok = strtok_s(NULL, ",;", &ctx))
        {
            float own = 0.0f;
            if (char* colon = strchr(tok, ':'))
            {
                *colon = 0;
                own = (float)atof(colon + 1);
            }
            Trim(tok);

            bool known = false;
            for (int i = 0; i < VANILLA_DEPOSITS; i++)
            {
                if (strcmp(tok, kVanillaDeposits[i].name) != 0) continue;
                want[i] = true;
                if (own > 0.0f) per[i] = own;
                known = true;
            }
            if (!known && tok[0]) Logf("deplete  vanilla: no base-game deposit \"%s\"", tok);
        }
    }

    for (int i = 0; i < VANILLA_DEPOSITS; i++)
        if (want[i]) Add(kVanillaDeposits[i].name, kVanillaDeposits[i].type,
                         kVanillaDeposits[i].map, kVanillaDeposits[i].component, per[i]);

    // Mod deposits come from the `deposits` plugin's registry, already
    // validated. A section's `deplete` key is one that plugin has no use for
    // and hands through untouched: a number overrides the global figure, 0
    // exempts it. Without that plugin there are simply no mod deposits.
    if (!D) return;
    for (int i = 0, n = D->count(); i < n; i++)
    {
        TsmDeposit dep;
        if (!D->get(i, &dep)) continue;
        if (dep.buildingType != 7) continue;          // only mines tick here

        float tonnes = g_tonnesPerTexel;
        if (const char* s = D->setting(i, "deplete"))
        {
            double v = atof(s);
            if (v <= 0.0) continue;                    // "deplete = 0"
            tonnes = (float)v;
        }
        Add(dep.name, dep.type,
            dep.map == TSM_MAP_RESOURCEMAP2 ? MAP_RESOURCEMAP2 : MAP_RESOURCEMAP,
            dep.component, tonnes);
    }
}

static bool ResolveImports()
{
    struct { const char* sym; t_VecGetter* out; } vecs[] = {
        { "?GetPosition@C3D_NODE@@QEAA?AVC3DVECTOR3@@XZ",       &o_NodeGetPosition  },
        { "?GetOffset@C3D_TERRAIN@@QEAA?AVC3DVECTOR3@@XZ",      &o_TerrainGetOffset },
        { "?GetTerrainSize@C3D_TERRAIN@@QEAA?AVC3DVECTOR2@@XZ", &o_TerrainGetSize   },
    };
    for (size_t i = 0; i < sizeof(vecs) / sizeof(vecs[0]); i++)
    {
        void** slot = FindIatSlot(g_exe, DLL_ENGINE, vecs[i].sym);
        if (!slot) { Logf("deplete  no import slot for %s", vecs[i].sym); return false; }
        *vecs[i].out = (t_VecGetter)*slot;
    }

    void** slot = FindIatSlot(g_exe, DLL_ENGINE, "?PowerTime@C3D_TIMER@@QEAAMM_N0@Z");
    if (!slot) { Logf("deplete  no import slot for C3D_TIMER::PowerTime"); return false; }
    o_PowerTime = (t_PowerTime)*slot;

    // Only gravel needs these, so a miss drops gravel rather than the feature.
    if (void** s = FindIatSlot(g_exe, DLL_ENGINE, "?MaskTextureOpen@C3D_TERRAIN@@QEAAXXZ"))
        o_MaskTextureOpen = (t_TexVoid)*s;
    if (void** s = FindIatSlot(g_exe, DLL_ENGINE, "?MaskTextureClose@C3D_TERRAIN@@QEAAXXZ"))
        o_MaskTextureClose = (t_TexVoid)*s;
    if (!o_MaskTextureOpen || !o_MaskTextureClose)
    {
        int kept = 0;
        for (int i = 0; i < g_depletableCount; i++)
            if (g_depletable[i].map != MAP_TERRAIN) g_depletable[kept++] = g_depletable[i];
        if (kept != g_depletableCount)
            Logf("deplete  no import slot for C3D_TERRAIN::MaskTexture* - gravel dropped");
        g_depletableCount = kept;
    }

    if (void** s = FindIatSlot(g_exe, DLL_ENGINE,
            "?PrintLeftUnicode@C3D_FONTMANAGER@@QEAAXPEAVC3D_FONT@@MMKPEB_WZZ"))
        o_PrintLeftUnicode = (t_PrintLeftUnicode)*s;
    else
    {
        Logf("deplete  no import slot for PrintLeftUnicode - no panel row");
        g_panel = 0;
    }

    o_DepositRadius = (t_DepositRadius)(g_exeBase + P_DEPOSIT_RADIUS);
    return true;
}

extern "C" __declspec(dllexport) unsigned TsmPluginApiVersion(void)
{
    return TSM_API_VERSION;
}

extern "C" __declspec(dllexport) int TsmPluginInit(const TsmHost* host, TsmPluginInfo* info)
{
    TsmBind(host);
    info->name    = "depletion";
    info->version = "1.1";

    // Nothing but configuration here. The deposit registry is another plugin's
    // and may not have published yet, so the work is in Start.
    ReadSettings();
    if (!g_enabled)
    {
        Logf("deplete  enabled = 0 - deposits stay infinite");
        return 1;                       // nothing hooked, no reason to stay loaded
    }
    return 0;
}

extern "C" __declspec(dllexport) int TsmPluginStart(void)
{
    if (!g_enabled) return 1;

    // Every plugin's Init has run, so this is either there or genuinely absent.
    D = (const TsmDepositApi*)H->consume(TSM_SERVICE_DEPOSITS, TSM_DEPOSITS_VERSION);
    if (!D) Logf("deplete  no deposits plugin - only the base game's own run out");

    BuildTable();
    if (g_depletableCount == 0)
    {
        Logf("deplete  nothing declared depletable - not hooking");
        return 1;
    }
    if (!ResolveImports() || g_depletableCount == 0)
    {
        Logf("deplete  cannot resolve what it needs - not hooking");
        return 1;
    }

    // The flusher first: everything that maps the deposit map runs there, and
    // the tick is harmless on its own.
    if (!PatchIat(g_exe, DLL_ENGINE, SYM_TERRAIN_RENDER, (void*)h_TerrainRender,
                  (void**)&o_TerrainRender, "C3D_TERRAIN::Render"))
    {
        Logf("deplete  no import slot for C3D_TERRAIN::Render - not hooking");
        return 1;
    }

    static const BYTE kMineTickPrologue[] = {
        0x48, 0x8B, 0xC4,                            // mov  rax,rsp
        0x57,                                        // push rdi
        0x41, 0x56,                                  // push r14
        0x41, 0x57,                                  // push r15
        0x48, 0x81, 0xEC, 0x20, 0x01, 0x00, 0x00     // sub  rsp,0x120
    };
    if (!InstallInlineHook(g_exeBase + P_MINE_TICK_RVA, (void*)h_MineTick,
                           (void**)&o_MineTick, kMineTickPrologue,
                           sizeof(kMineTickPrologue), "mine tick"))
        return 1;

    // Additive and cosmetic, so a refusal here costs the row and nothing else.
    if (g_panel)
    {
        static const BYTE kMinePanelPrologue[] = {
            0x48, 0x8B, 0xC4,                            // mov  rax,rsp
            0x55, 0x56, 0x57,                            // push rbp, rsi, rdi
            0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57,  // push r12..r15
            0x48, 0x8D, 0xA8, 0xF8, 0xF4, 0xFF, 0xFF     // lea  rbp,[rax-0xB08]
        };
        if (!InstallInlineHook(g_exeBase + P_MINE_PANEL_RVA, (void*)h_MinePanel,
                               (void**)&o_MinePanel, kMinePanelPrologue,
                               sizeof(kMinePanelPrologue), "mine panel"))
            g_panel = 0;
    }

    static const char* kMapName[] = { "resourcemap ", "resourcemap2", "terrain mask" };
    for (int i = 0; i < g_depletableCount; i++)
        Logf("deplete  %-10s type %-3d %s component %d, %.0f t per saturated texel",
             g_depletable[i].name, g_depletable[i].type,
             kMapName[g_depletable[i].map], g_depletable[i].component,
             g_depletable[i].tonnesPerTexel);
    Logf("deplete  %d deposit type(s) will run out, map written every %.0f s",
         g_depletableCount, g_flushSeconds);
    return 0;
}

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) { return TRUE; }
