// accumulator - stationary batteries for the electric grid, as a tesmioloader
// plugin.
//
// The base game has no way to store electricity. It does, however, already
// move it exactly like any other resource, and that is the whole opening.
//
//
// HOW THE GAME MOVES ELECTRICITY
//
// Every building carries a vector of storages at building+0x970..0x978, stride
// 0xE0. One field of a storage is its transport class, and class 9 is
// RESOURCE_TRANSPORT_ELETRIC - the class of the `eletric` resource. A power
// plant has one, and so does every consumer.
//
//   FUN_140139a80  rva 0x139A80  the building dispatcher: one simulation tick
//                  over every building. It collects power plants (type 17) and
//                  transformers (19) into vectors of their own and calls:
//   FUN_1401b8ee0  rva 0x1B8EE0  (game, building) - walk the wires out of this
//                  building, collect every class-9 storage it can reach, and
//                  hand energy over through:
//   FUN_1401bdd40  rva 0x1BDD40  the transfer itself. Each receiver's share is
//                  (its capacity - its content) / (total shortfall), and the
//                  source may hand out at most dt * its own storage capacity.
//
// Nothing in that code knows the source has to be a power plant. The type is
// checked in one place only, and only to keep plants from being *receivers*
// (`type != 17 && type != 31`). So a battery needs no new mechanism at all:
//
//   charging     is free. A building with a large class-9 storage is filled by
//                the grid exactly like a consumer, because its storage is short
//                of its capacity and the distributor divides by shortfall.
//   discharging  is one call to FUN_1401b8ee0 for our own building, after the
//                dispatcher has run. The engine's own code pushes the charge
//                back out over the wires.
//   saving       is free. The charge lives in the game's own storage, not in a
//                counter of ours, so it goes into the save with everything else
//                and survives this plugin being removed.
//
// That leaves one real problem, and it is what most of this file is about: an
// empty battery declaring 5000 units of capacity has a shortfall of 5000 while
// the town it sits in has a shortfall of 200, so the proportional split hands
// it 96% of everything generated and blacks the town out until it is full.
//
//
// THE THROTTLE, AND WHY THERE IS NO STATE
//
// The one field the grid reads to decide how much a battery wants is its
// storage capacity, so that is what gets rewritten:
//
//   before the dispatcher   capacity = min(declared, content + charge * dt)
//                           - the shortfall the grid sees is one tick of charge
//                             rate, whatever the battery's real size is.
//   after the dispatcher    capacity = discharge rate, then call the engine's
//                           own distributor. Its `dt * capacity` ceiling is
//                           then one tick of discharge rate.
//   and then                capacity goes back to what building.ini declared.
//
// Both windows open and close inside a single call to this hook, so the
// declared capacity is saved on the stack and nothing is remembered between
// ticks. That is deliberate: a cache keyed on a building pointer would go stale
// on a world load, and every version of that bug in this project has cost a
// session (see docs/07-pitfalls.md, "Armed is a claim, not a fact"). The
// accumulators are re-found from the building vector every tick instead, which
// costs one pass over a pointer array and is always right.
//
// Discharging needs no "is the grid short" test either. FUN_1401bdd40 divides
// by the shortfall it finds and returns without moving anything when there is
// none, so a battery in a healthy grid hands out nothing on its own.
//
//
// DISCHARGING IS WHAT A TRANSFORMER DOES, NOT WHAT A PLANT DOES
//
// The first version called FUN_1401b8ee0, the power plant's distributor. It
// moved energy correctly and the battery still looked broken: the neighbouring
// substation filled once and then nothing happened, reporting 0 KV the whole
// time.
//
// A grid node hands out two separate things, and 0x1BB700's third argument
// picks which. It ends in
//
//   FUN_1401bdd40(game, b, storages, conns, buildings, what == 0, 1.0f)
//
// and that sixth argument gates the transfer: non-zero runs the tail of the
// function, which propagates slot quality - "there is voltage here" - and moves
// no energy; zero runs the body, which moves energy. The dispatcher runs both
// for every live transformer, voltage first.
//
// Energy without voltage is a dead end: nothing downstream draws, so nothing is
// short of capacity, and 0x1BDD40 divides by the shortfall it finds. The
// battery hands out one storage's worth and then correctly concludes the grid
// wants nothing.
//
// So a battery discharges the way a transformer does, through 0x1BB700, in the
// dispatcher's own order. FUN_1401d1e10 - the factory production tick that
// 0x1B8EE0 opened with, and that had to be suppressed for it - is not on this
// path at all.
//
// Everything here is addresses for SOVIET64.exe v1.1.1.7. See
// docs/10-accumulator.md.

#include "../../src/tesmio_plugin.h"

// ---------------------------------------------------------------- the game

#define P_DISPATCH_RVA    0x139A80   // FUN_140139a80(game) - one simulation tick
#define P_NODE_SHARE_RVA  0x1BB700   // FUN_1401bb700(game, building, what, priority)
#define P_SUB_SHARE_RVA   0x1B9640   // FUN_1401b9640(game, substation) - hands its
                                     // storage to the buildings in its catchment

// FUN_1401bb700's third argument decides which of the two things a grid node
// hands out, and it is the whole reason this plugin stopped calling the power
// plant's version. It ends in
//
//   FUN_1401bdd40(game, b, storages, conns, buildings, what == 0, 1.0f)
//
// and that sixth argument gates the transfer: non-zero takes the tail of the
// function, which propagates slot quality - "there is voltage here" - and moves
// no energy at all. Zero takes the main body, which moves energy.
//
// The dispatcher therefore runs both, in this order, for every transformer it
// considers live: voltage first, energy second. A battery that only did the
// second one filled its neighbour's storage once and then stopped, because
// without voltage nothing downstream draws, without draw there is no shortfall,
// and 0x1BDD40 divides by shortfall.
// The fourth argument is a per-connection priority filter, matched against the
// byte at building+0xE22+connectionIndex. -1 skips the filter - but only the
// filtered form reaches a neighbour the dispatcher has flagged as unpowered
// (`+0x1130`), which is exactly the state a substation hanging off a battery is
// in. So energy goes out priority by priority, the way the dispatcher does it
// for a priority transformer, and voltage goes out unfiltered first.
#define NODE_SHARE_VOLTAGE 0
#define NODE_SHARE_ENERGY  1
#define NODE_ANY_PRIORITY  (-1)
#define P_TIMER_OBJ       0x9D4EE0   // the C3D_TIMER the simulation steps on
#define P_TIMER_STEP      0x909B14   // 0.001f, the argument every PowerTime passes

// The electric panel - the two dials in a grid node's window.
//
//   rva 0x702610  FUN_140702610(game, window, float* x, float* y)
//
// Its parent at 0x6F4A20 calls it for building types 19, 31, 32 and 35 (the
// bitmask 0x980080000 tested against typedesc+0x360) and for anything whose
// type descriptor declares electricity use. Every panel in that window takes
// the running Y **by pointer** and leaves the next row's Y in it, which is what
// makes one more row cheap: draw at *y and add a row height back.
#define P_ELEC_PANEL_RVA  0x702610
#define P_FONTMANAGER     0x996FB0   // C3D_FONTMANAGER, by address
#define P_PANEL_FONT      0x995220   // the C3D_FONT* the panel's rows use
#define P_DPI             0x992088   // the scale every layout constant is multiplied by
#define P_ROW_STEP        0x90A9E4   // 35 - one row, and the x indent as well
#define W_BUILDING        0x240      // window -> the building it is about
#define W_COLLAPSED       0x248      // non-zero while the section is rolled up
#define PANEL_LABEL_COLOUR 0xFF990000u

#define G_BUILDINGS       0x11B08    // game -> vector<Building*> begin
#define G_BUILDINGS_END   0x11B10    // ...and end

#define B_TYPEDESC        0x318      // building -> its type descriptor
#define B_FINISHED        0x604      // construction progress; 1.0 when done
#define B_DEMOLISHED      0xEA8      // non-zero once it is going away
#define B_STORAGES        0x970      // building -> vector<Storage> begin
#define B_STORAGES_END    0x978      // ...and end, stride 0xE0

// The three floats the script VM calls fEletric_Wattage, fEletric_Voltage and
// fEletric_CurcuitBreakerCapacity are reached through pointers; +0x1100 and
// +0x1128 are the smoothed copies the two dials actually read (0x702610 opens
// with `fVar31 = *(float *)(building + 0x1128)` for the first dial).
//
// A battery is a dead end on the wire, so no current flows *through* it, so the
// solver leaves wattage at 0 - and 0x1BD4C0 computes voltage as
// 2 x wattage/breaker, which is why an accumulator that is charging perfectly
// well still reports "Building is without power supply".
#define B_WATTAGE_PTR     0x10E0
#define B_VOLTAGE_PTR     0x10E8
#define B_BREAKER_PTR     0x10F8
#define B_WATT_DIAL       0x1100
#define B_VOLT_DIAL       0x1128

// Set by the dispatcher on every grid node its solver did not reach from a
// power plant, and tested by 0x1BB700 before handing anything to a neighbour.
// Scanning .text for this offset finds exactly one writer - 0x13B02E, inside
// the dispatcher - and two readers, the dispatcher's own solver and 0x1BB9BD in
// 0x1BB700. A charged battery is a source, so it clears this for itself.
#define B_UNPOWERED       0x1130

// Wire connections: vector at +0xA10..+0xA18, stride 0x60.
//
//   conn+0x00   connection type. 7 is ELETRIC_HIGH, 8 is ELETRIC_LOW
//   conn+0x28   the line object, null when nothing is plugged in
//   line+0x20   one end, line+0x28 the other; each end holds a vector of
//               buildings at +0x70..+0x78 whose first entry is the building
//
// Read off the walk 0x1B8EE0 and 0x1BB700 both open with: they take whichever
// end does not lead back to the building they started from.
#define B_CONNECTIONS     0xA10
#define B_CONNECTIONS_END 0xA18
#define C_STRIDE          0x60
#define C_TYPE            0x00
#define C_LINE            0x28
#define L_END_A           0x20
#define L_END_B           0x28
#define E_BUILDINGS       0x70
#define E_BUILDINGS_END   0x78
#define CONN_ELETRIC_HIGH 7
#define CONN_ELETRIC_LOW  8

#define T_BUILDING_TYPE   0x360      // typedesc -> building type

#define S_STRIDE          0xE0       // one storage
#define S_SLOTS           0x00       // storage -> vector<Slot> begin; one slot
                                     // per resource the class can hold, and
                                     // class 9 holds only `eletric`
#define S_CAPACITY        0x8C       // float, what $STORAGE declared
#define S_CLASS           0x90       // int, RESOURCE_TRANSPORT_*
#define S_HANDED_OUT      0xA8       // float, what left through it this tick

#define SLOT_AMOUNT       0x08       // float, what is in it now
#define SLOT_QUALITY      0x0C       // float 0..1; a source with 0 hands out
                                     // nothing. The production tick sets it for
                                     // a power plant, and for nothing else.

#define CLASS_ELETRIC     9          // RESOURCE_TRANSPORT_ELETRIC

#define TYPE_POWERPLANT   17
#define TYPE_SUBSTATION   18
#define TYPE_TRANSFORMATOR 19

// ---------------------------------------------------------------- config

static int   g_enabled     = 1;
static float g_chargeRate  = 60.0f;   // units per second into the battery
static float g_dischargeRate = 120.0f;// units per second back out of it
static float g_minCapacity = 500.0f;  // what tells a battery from a transformer
static int   g_types[8];              // building types that may be a battery
static int   g_typeCount   = 0;
static float g_logSeconds  = 0.0f;
static int   g_probe       = 0;
static int   g_gauges      = 1;       // drive the window's two dials
static int   g_spread      = 0;       // hand a voltage to the wired neighbours
static int   g_trace       = 0;       // follow one battery's neighbours per phase
static int   g_panel       = 1;       // add the charge row to that window
static char  g_panelCaption[64] = "Charge";

// ---------------------------------------------------------------- engine calls

typedef void  (*t_Dispatch)(void*);
typedef void  (*t_NodeShare)(void*, void*, int, int);
typedef void  (*t_SubShare)(void*, void*);
typedef float (*t_PowerTime)(void*, float, bool, bool);
typedef void  (*t_ElecPanel)(void*, void*, float*, float*);
typedef void  (*t_PrintLeftUnicode)(void*, void*, float, float, unsigned long,
                                    const wchar_t*, ...);

static t_Dispatch         o_Dispatch;
static t_NodeShare        o_NodeShare;
static t_SubShare         o_SubShare;
static t_PowerTime        o_PowerTime;
static t_ElecPanel        o_ElecPanel;
static t_PrintLeftUnicode o_PrintLeftUnicode;

static DWORD g_thread;
static int   g_repowered;    // substations re-energised this tick, for the log
static double g_lastLog;
static double g_lastProbe;

// ---------------------------------------------------------------- the battery

#define MAX_ACC 256

typedef struct AccSite
{
    BYTE* building;
    BYTE* storage;      // its class-9 storage
    BYTE* slot;         // the single slot inside it
    float declaredCap;  // what building.ini asked for, saved across the tick
    float held;         // content at the top of the tick
    float charged;      // what the grid put in during the dispatcher
    float discharged;   // what went back out afterwards
    int   reached;      // neighbours handed a voltage this tick
} AccSite;

static bool IsBatteryType(int type)
{
    for (int i = 0; i < g_typeCount; i++)
        if (g_types[i] == type) return true;
    return false;
}

// The class-9 storage of a building, or null. `wantBattery` also demands the
// capacity be large enough to be a declared battery rather than the incidental
// one a transformer or a consumer carries.
static BYTE* EletricStorage(BYTE* b, bool wantBattery)
{
    BYTE* begin = *(BYTE**)(b + B_STORAGES);
    BYTE* end   = *(BYTE**)(b + B_STORAGES_END);
    if (!begin || !end || end <= begin) return NULL;

    size_t bytes = (size_t)(end - begin);
    if (bytes % S_STRIDE) return NULL;                 // not the vector we think
    size_t count = bytes / S_STRIDE;
    if (count > 64) return NULL;

    for (size_t i = 0; i < count; i++)
    {
        BYTE* s = begin + i * S_STRIDE;
        if (*(int*)(s + S_CLASS) != CLASS_ELETRIC) continue;
        if (wantBattery && *(float*)(s + S_CAPACITY) < g_minCapacity) continue;
        return s;
    }
    return NULL;
}

// Walks the game's own building vector. Called twice per tick and deliberately
// caches nothing: a building pointer is only valid for as long as the world it
// belongs to, and this plugin has no way of being told that world went away.
static int CollectAccumulators(BYTE* game, AccSite* out, int max)
{
    BYTE** begin = *(BYTE***)(game + G_BUILDINGS);
    BYTE** end   = *(BYTE***)(game + G_BUILDINGS_END);
    if (!begin || !end || end <= begin) return 0;

    size_t count = (size_t)(end - begin);
    if (count > 500000) return 0;                      // not the vector we think

    int n = 0;
    for (size_t i = 0; i < count && n < max; i++)
    {
        BYTE* b = begin[i];
        if (!b) continue;

        BYTE* td = *(BYTE**)(b + B_TYPEDESC);
        if (!td) continue;
        if (!IsBatteryType(*(int*)(td + T_BUILDING_TYPE))) continue;
        if (*(float*)(b + B_FINISHED) < 1.0f) continue;
        if (*(char*)(b + B_DEMOLISHED) != 0) continue;

        BYTE* s = EletricStorage(b, true);
        if (!s) continue;

        BYTE* slots = *(BYTE**)(s + S_SLOTS);
        if (!slots) continue;

        out[n].building    = b;
        out[n].storage     = s;
        out[n].slot        = slots;
        out[n].declaredCap = *(float*)(s + S_CAPACITY);
        out[n].held        = *(float*)(slots + SLOT_AMOUNT);
        out[n].charged     = 0.0f;
        out[n].discharged  = 0.0f;
        out[n].reached     = 0;
        n++;
    }
    return n;
}

static float TickStep(void)
{
    return o_PowerTime(g_exeBase + P_TIMER_OBJ,
                       *(float*)(g_exeBase + P_TIMER_STEP), false, false);
}

// Before the dispatcher: make the grid see a shortfall of one tick's charge
// rate instead of the whole battery, so a fresh battery cannot drink the town's
// entire generation in the first few seconds.
static void OpenChargeWindow(AccSite* a, int n, float dt)
{
    float quota = g_chargeRate * dt;
    if (quota < 0.0f) quota = 0.0f;

    for (int i = 0; i < n; i++)
    {
        float held = *(float*)(a[i].slot + SLOT_AMOUNT);
        float cap  = held + quota;
        if (cap > a[i].declaredCap) cap = a[i].declaredCap;
        if (cap < 0.0f) cap = 0.0f;
        *(float*)(a[i].storage + S_CAPACITY) = cap;
    }
}

// The building on the other end of one wire connection, or null. Whichever end
// of the line does not lead back to `b` is the neighbour - the same test both
// engine walks make.
static BYTE* WireNeighbour(BYTE* b, BYTE* conn)
{
    BYTE* line = *(BYTE**)(conn + C_LINE);
    if (!line) return NULL;

    for (int end = 0; end < 2; end++)
    {
        BYTE* ep = *(BYTE**)(line + (end ? L_END_B : L_END_A));
        if (!ep) continue;
        BYTE** begin = *(BYTE***)(ep + E_BUILDINGS);
        BYTE** e     = *(BYTE***)(ep + E_BUILDINGS_END);
        if (!begin || !e || e <= begin) continue;
        if (*begin != b) return *begin;
    }
    return NULL;
}

// Hand "there is voltage here" to every building one wire away, and clear the
// dispatcher's unpowered flag on it.
//
// 0x1BB700 propagates exactly this, and on a branch with a power plant behind
// it that is enough. It is not enough on a branch fed only by a battery, and
// the probe says why: the substation's storage sits **full** - 2.50 of 2.50 -
// with quality 0. Quality only ever rises inside a transfer, a transfer only
// happens into a shortfall, and a full storage is not short of anything. The
// grid had energy and no way to notice.
//
// So the battery says it directly, one hop out. That is all it takes: the
// engine carries it the rest of the way itself, because a substation hands its
// own quality on to everything in its catchment on the next tick.
static int SpreadVoltage(BYTE* b)
{
    BYTE* begin = *(BYTE**)(b + B_CONNECTIONS);
    BYTE* end   = *(BYTE**)(b + B_CONNECTIONS_END);
    if (!begin || !end || end <= begin) return 0;

    size_t bytes = (size_t)(end - begin);
    if (bytes % C_STRIDE) return 0;
    size_t count = bytes / C_STRIDE;
    if (count > 64) return 0;

    int reached = 0;
    for (size_t i = 0; i < count; i++)
    {
        BYTE* conn = begin + i * C_STRIDE;
        int   type = *(int*)(conn + C_TYPE);
        if (type != CONN_ELETRIC_HIGH && type != CONN_ELETRIC_LOW) continue;

        BYTE* n = WireNeighbour(b, conn);
        if (!n) continue;

        BYTE* s = EletricStorage(n, false);
        if (!s) continue;
        BYTE* slot = *(BYTE**)(s + S_SLOTS);
        if (!slot) continue;

        if (*(float*)(slot + SLOT_QUALITY) < 1.0f)
            *(float*)(slot + SLOT_QUALITY) = 1.0f;
        *(char*)(n + B_UNPOWERED) = 0;
        reached++;
    }
    return reached;
}

// ---- keeping a battery-fed substation alive --------------------------------
//
// The trace settled where the block was, and it was not where any amount of
// reading was going to put it:
//
//   trace before tick: neighbour type 18  quality 1.000  covers 2  short 0.320
//   trace after tick : neighbour type 18  quality 0.000  covers 2  short 0.320
//   trace after share: neighbour type 18  quality 1.000  covers 2  short 0.320
//
// The substation has two buildings in its catchment, they are short of 0.32,
// and it has a voltage at the top of the tick. Somewhere inside the tick that
// voltage is cleared - and 0x1B9640, the call that would have handed its
// storage to those two buildings, runs *after* that point. It hands out nothing
// because 0x1BDD40 refuses to move anything from a source whose slot quality is
// zero.
//
// Everything this plugin tried before restored the voltage after the dispatcher
// had finished, which is always too late: the clearing happens again next tick,
// still ahead of the hand-out. So the restore has to sit between the two, and
// the only seam there is 0x1B9640 itself.
//
// The hook therefore raises the quality of a substation **that a charged
// battery is wired to** back to full immediately before it distributes. Its
// catchment then draws, its storage empties, and the shortfall that creates is
// what the battery's own discharge fills after the tick - which is the loop
// closing the way it does for a power plant.
//
// Nothing else is touched: a substation with no battery on it is left exactly
// as the engine left it, dark if the engine says dark.

#define MAX_FED 64
static BYTE* g_fed[MAX_FED];
static int   g_fedCount;

static bool IsBatteryFed(BYTE* b)
{
    for (int i = 0; i < g_fedCount; i++)
        if (g_fed[i] == b) return true;
    return false;
}

// Called before the dispatcher, while the plugin still knows which batteries
// hold a charge. Rebuilt every tick, like everything else here.
static void CollectFed(AccSite* a, int n)
{
    g_fedCount = 0;
    if (g_dischargeRate <= 0.0f) return;

    for (int i = 0; i < n && g_fedCount < MAX_FED; i++)
    {
        if (*(float*)(a[i].slot + SLOT_AMOUNT) <= 0.0f) continue;

        BYTE* begin = *(BYTE**)(a[i].building + B_CONNECTIONS);
        BYTE* end   = *(BYTE**)(a[i].building + B_CONNECTIONS_END);
        if (!begin || !end || end <= begin) continue;

        size_t bytes = (size_t)(end - begin);
        if (bytes % C_STRIDE) continue;
        size_t count = bytes / C_STRIDE;
        if (count > 64) continue;

        for (size_t c = 0; c < count && g_fedCount < MAX_FED; c++)
        {
            BYTE* conn = begin + c * C_STRIDE;
            int   ct   = *(int*)(conn + C_TYPE);
            if (ct != CONN_ELETRIC_HIGH && ct != CONN_ELETRIC_LOW) continue;

            BYTE* nb = WireNeighbour(a[i].building, conn);
            if (nb && !IsBatteryFed(nb)) g_fed[g_fedCount++] = nb;
        }
    }
}

// A CHARGED BATTERY IS A SOURCE, and it has to say so before the dispatcher
// runs, not after.
//
// The dispatcher's solver starts at the power plants and walks outward, and
// every grid node it did not reach gets `+0x1130` set - "no power here". The
// next tick, that flag keeps the node out of the solver's live set, and
// 0x1BB700 refuses to hand energy to a neighbour that has it. A battery feeding
// a substation with no plant behind it therefore hit a standstill: it was
// marked unpowered because nothing upstream reached it, and being marked
// unpowered is what stopped it powering anything downstream.
//
// So it declares itself: voltage present, slot quality full, flag cleared. All
// three are what a live node looks like, and a battery with a charge in it is
// exactly that - it is the one node in the grid that does not need anything
// upstream to be true.
//
// Done before the dispatcher so the engine's own solver picks the battery up as
// part of its live set and propagates outward from it, which is what makes the
// whole branch behind the battery come alive rather than just its immediate
// neighbour. The explicit hand-out afterwards then tops that up.
static void DeclareLive(AccSite* a, int n)
{
    if (g_dischargeRate <= 0.0f) return;

    for (int i = 0; i < n; i++)
    {
        if (*(float*)(a[i].slot + SLOT_AMOUNT) <= 0.0f) continue;

        *(float*)(a[i].slot + SLOT_QUALITY) = 1.0f;

        float* volt = *(float**)(a[i].building + B_VOLTAGE_PTR);
        if (volt) *volt = 1.0f;

        *(char*)(a[i].building + B_UNPOWERED) = 0;
    }
}

// After it: hand the charge back out through the engine's own distributor, with
// the capacity standing in for the discharge rate, then put the declared
// capacity back so anything reading the storage sees the battery's real size.
static void CloseAndDischarge(BYTE* game, AccSite* a, int n)
{
    for (int i = 0; i < n; i++)
    {
        float held = *(float*)(a[i].slot + SLOT_AMOUNT);
        a[i].charged = held - a[i].held;         // what the dispatcher put in

        if (held > 0.0f && g_dischargeRate > 0.0f)
        {
            *(float*)(a[i].storage + S_CAPACITY) = g_dischargeRate;

            // A source with quality 0 hands out nothing. The production tick
            // sets this for a power plant; a battery is not one, so it is set
            // here, for the one call.
            *(float*)(a[i].slot + SLOT_QUALITY) = 1.0f;

            // Exactly what the dispatcher does for a live transformer, and in
            // this order. Voltage has to go first: without it nothing
            // downstream draws, without draw there is no shortfall, and the
            // transfer divides by shortfall - so an energy-only battery fills
            // its neighbour once and then looks broken.
            o_NodeShare(game, a[i].building, NODE_SHARE_VOLTAGE, NODE_ANY_PRIORITY);
            if (g_spread) a[i].reached = SpreadVoltage(a[i].building);
            o_NodeShare(game, a[i].building, NODE_SHARE_ENERGY,  2);
            o_NodeShare(game, a[i].building, NODE_SHARE_ENERGY,  1);
            o_NodeShare(game, a[i].building, NODE_SHARE_ENERGY,  0);

            a[i].discharged = held - *(float*)(a[i].slot + SLOT_AMOUNT);
        }

        *(float*)(a[i].storage + S_CAPACITY) = a[i].declaredCap;
    }
}

// The two dials in the building's window, and the "without power supply"
// warning next to them.
//
// Neither is wrong about the grid - they are right about a node no current
// passes through, which is exactly what a battery is. The solver leaves
// wattage at zero because nothing flows out the far side, and 0x1BD4C0 derives
// voltage from wattage. So the honest numbers have to come from here, where
// what actually moved through the storage this tick is known:
//
//   wattage  charge and discharge are both current through the battery, so the
//            dial shows their sum over dt - what it would show for a
//            transformer passing the same energy along.
//   voltage  full whenever the battery has something to give or is taking
//            something in. It is what the warning reads, and a battery holding
//            a charge is not a building without power.
static void UpdateGauges(AccSite* a, int n, float dt)
{
    if (dt <= 0.0f) return;

    for (int i = 0; i < n; i++)
    {
        float moved = a[i].charged + a[i].discharged;
        if (moved < 0.0f) moved = -moved;

        float* watt = *(float**)(a[i].building + B_WATTAGE_PTR);
        float* volt = *(float**)(a[i].building + B_VOLTAGE_PTR);
        float  held = *(float*)(a[i].slot + SLOT_AMOUNT);
        float  live = (held > 0.0f || moved > 0.0f) ? 1.0f : 0.0f;

        if (watt) *watt = moved / dt;
        if (volt) *volt = live;

        *(float*)(a[i].building + B_WATT_DIAL) = moved / dt;
        *(float*)(a[i].building + B_VOLT_DIAL) = live;
    }
}

// ---------------------------------------------------------------- trace
//
// The one thing gessing could not settle: **which phase of the tick clears a
// neighbour's voltage**. Three snapshots of the first battery's wired
// neighbours - before the dispatcher, after it, and after the discharge - say
// it outright, and each carries the fields the answer depends on:
//
//   quality    the voltage itself
//   watt       0x1BD4C0 recomputes voltage as 2 x watt/breaker for a node of
//              type 17..19 or 35, so a node with no current through it is
//              driven to zero by the engine's own arithmetic
//   covered    how many buildings are in a substation's catchment - the list
//              0x1B9640 hands energy to. Empty means it has nobody to feed,
//              which is a different failure from having nobody to feed it
//   short      what those buildings are short of. Nothing transfers into zero

#define TRACE_MAX 6

typedef struct Snap
{
    BYTE* b;
    int   type;
    float quality;
    float watt;
    float breaker;
    int   unpowered;
    int   covered;
    float shortfall;
} Snap;

static Snap g_snap[3][TRACE_MAX];
static int  g_snapCount;

static void TakeSnapshot(BYTE* battery, int phase)
{
    if (phase == 0) g_snapCount = 0;

    BYTE* begin = *(BYTE**)(battery + B_CONNECTIONS);
    BYTE* end   = *(BYTE**)(battery + B_CONNECTIONS_END);
    if (!begin || !end || end <= begin) return;

    size_t bytes = (size_t)(end - begin);
    if (bytes % C_STRIDE) return;
    size_t count = bytes / C_STRIDE;
    if (count > 64) return;

    int n = 0;
    for (size_t i = 0; i < count && n < TRACE_MAX; i++)
    {
        BYTE* conn = begin + i * C_STRIDE;
        int   ct   = *(int*)(conn + C_TYPE);
        if (ct != CONN_ELETRIC_HIGH && ct != CONN_ELETRIC_LOW) continue;

        BYTE* nb = WireNeighbour(battery, conn);
        if (!nb) continue;

        Snap* s = &g_snap[phase][n++];
        memset(s, 0, sizeof(*s));
        s->b  = nb;
        BYTE* td = *(BYTE**)(nb + B_TYPEDESC);
        s->type = td ? *(int*)(td + T_BUILDING_TYPE) : -1;

        BYTE* st = EletricStorage(nb, false);
        BYTE* sl = st ? *(BYTE**)(st + S_SLOTS) : NULL;
        if (sl) s->quality = *(float*)(sl + SLOT_QUALITY);

        float* w = *(float**)(nb + B_WATTAGE_PTR);
        float* br= *(float**)(nb + B_BREAKER_PTR);
        s->watt      = w  ? *w  : -1.0f;
        s->breaker   = br ? *br : -1.0f;
        s->unpowered = *(char*)(nb + B_UNPOWERED);

        // What this node has to hand energy to, and how badly it wants it.
        BYTE** cb = *(BYTE***)(nb + 0x10C8);
        BYTE** ce = *(BYTE***)(nb + 0x10D0);
        if (cb && ce && ce > cb)
        {
            s->covered = (int)(ce - cb);
            for (int k = 0; k < s->covered && k < 256; k++)
            {
                BYTE* c  = cb[k];
                if (!c) continue;
                BYTE* cs = EletricStorage(c, false);
                BYTE* cl = cs ? *(BYTE**)(cs + S_SLOTS) : NULL;
                if (!cl) continue;
                float miss = *(float*)(cs + S_CAPACITY) - *(float*)(cl + SLOT_AMOUNT);
                if (miss > 0.0f) s->shortfall += miss;
            }
        }
    }
    if (phase == 0) g_snapCount = n;
}

static void ReportTrace(void)
{
    static const char* kPhase[3] = { "before tick", "after tick ", "after share" };
    for (int i = 0; i < g_snapCount; i++)
    {
        for (int p = 0; p < 3; p++)
        {
            Snap* s = &g_snap[p][i];
            if (!s->b) continue;
            Logf("battery  trace %s: neighbour type %-3d  quality %.3f  watt %8.3f / %.3f  "
                 "unpowered %d  covers %d  short %.3f",
                 kPhase[p], s->type, s->quality, s->watt, s->breaker,
                 s->unpowered, s->covered, s->shortfall);
        }
    }
}

// ---------------------------------------------------------------- probe
//
// What the log needs to show before any of this can be trusted: which buildings
// carry a class-9 storage at all, what the fields hold, and whether the battery
// is being found. Off by default; it prints one block per interval.

static void ProbeGrid(BYTE* game)
{
    BYTE** begin = *(BYTE***)(game + G_BUILDINGS);
    BYTE** end   = *(BYTE***)(game + G_BUILDINGS_END);
    if (!begin || !end || end <= begin) { Logf("battery  probe: no building vector"); return; }

    size_t count = (size_t)(end - begin);
    if (count > 500000) { Logf("battery  probe: building vector looks wrong (%zu)", count); return; }

    int shown = 0, withStorage = 0;
    Logf("battery  probe: %zu building(s)", count);
    for (size_t i = 0; i < count; i++)
    {
        BYTE* b = begin[i];
        if (!b) continue;
        BYTE* td = *(BYTE**)(b + B_TYPEDESC);
        if (!td) continue;

        BYTE* s = EletricStorage(b, false);
        if (!s) continue;
        withStorage++;
        if (shown >= 24) continue;

        BYTE*  slot    = *(BYTE**)(s + S_SLOTS);
        BYTE*  slotEnd = *(BYTE**)(s + S_SLOTS + 8);
        float* watt    = *(float**)(b + B_WATTAGE_PTR);
        float* volt    = *(float**)(b + B_VOLTAGE_PTR);
        float* breaker = *(float**)(b + B_BREAKER_PTR);
        Logf("battery  probe: type %-3d %s cap %8.2f  slots %d  amount %8.2f  quality %.3f  "
             "out %.3f  W %.3f  V %.3f  breaker %.3f  dials %.3f/%.3f",
             *(int*)(td + T_BUILDING_TYPE),
             *(float*)(b + B_FINISHED) >= 1.0f ? "built  " : "site   ",
             *(float*)(s + S_CAPACITY),
             slot && slotEnd ? (int)((slotEnd - slot) / 16) : -1,
             slot ? *(float*)(slot + SLOT_AMOUNT) : 0.0f,
             slot ? *(float*)(slot + SLOT_QUALITY) : 0.0f,
             *(float*)(s + S_HANDED_OUT),
             watt ? *watt : -1.0f, volt ? *volt : -1.0f, breaker ? *breaker : -1.0f,
             *(float*)(b + B_WATT_DIAL), *(float*)(b + B_VOLT_DIAL));
        shown++;

        // For a battery, what it is actually wired to. This is the question the
        // "voltage flickers on and dies" symptom really asks: whether the
        // engine's walk out of this building reaches anything at all.
        if (!IsBatteryType(*(int*)(td + T_BUILDING_TYPE))
            || *(float*)(s + S_CAPACITY) < g_minCapacity)
            continue;

        BYTE* cb = *(BYTE**)(b + B_CONNECTIONS);
        BYTE* ce = *(BYTE**)(b + B_CONNECTIONS_END);
        size_t conns = (cb && ce && ce > cb) ? (size_t)(ce - cb) / C_STRIDE : 0;
        Logf("battery  probe: battery has %zu connection(s)", conns);
        for (size_t c = 0; c < conns && c < 16; c++)
        {
            BYTE* conn = cb + c * C_STRIDE;
            BYTE* n    = WireNeighbour(b, conn);
            BYTE* ns   = n ? EletricStorage(n, false) : NULL;
            BYTE* nslot= ns ? *(BYTE**)(ns + S_SLOTS) : NULL;
            BYTE* ntd  = n ? *(BYTE**)(n + B_TYPEDESC) : NULL;
            Logf("battery  probe:   conn type %-2d  line %s  neighbour %s type %-3d  "
                 "quality %.3f  unpowered %d",
                 *(int*)(conn + C_TYPE),
                 *(BYTE**)(conn + C_LINE) ? "yes" : "no ",
                 n ? "yes" : "no ",
                 ntd ? *(int*)(ntd + T_BUILDING_TYPE) : -1,
                 nslot ? *(float*)(nslot + SLOT_QUALITY) : -1.0f,
                 n ? *(char*)(n + B_UNPOWERED) : -1);
        }
    }
    Logf("battery  probe: %d building(s) carry an eletric storage", withStorage);
}

// ---------------------------------------------------------------- the panel
//
// One more row under the two dials:
//
//   Charge: 4210.5 / 5000  (84.2 %)
//
// The window's panels take the running Y by pointer and leave the next row's Y
// in it, so a post-hook draws at *y and hands back one row height - the frame
// grows to fit instead of the text landing on it. Same trick as the mine's
// "Deposit remaining" row, one indirection further out.

static float LayoutF(DWORD rva) { return *(float*)(g_exeBase + rva); }

static void DrawChargeRow(BYTE* win, float* px, float* py)
{
    if (*(int*)(win + W_COLLAPSED) != 0) return;

    BYTE* b = *(BYTE**)(win + W_BUILDING);
    if (!b) return;

    BYTE* td = *(BYTE**)(b + B_TYPEDESC);
    if (!td || !IsBatteryType(*(int*)(td + T_BUILDING_TYPE))) return;

    BYTE* s = EletricStorage(b, true);
    if (!s) return;
    BYTE* slot = *(BYTE**)(s + S_SLOTS);
    if (!slot) return;

    // The capacity field is a rate for the two windows the dispatcher hook
    // holds open, so the size shown here is the one building.ini declared -
    // which is what it reads as everywhere outside those windows.
    float held = *(float*)(slot + SLOT_AMOUNT);
    float cap  = *(float*)(s + S_CAPACITY);
    if (cap <= 0.0f) return;

    wchar_t caption[64], line[192];
    MultiByteToWideChar(CP_ACP, 0, g_panelCaption, -1, caption, 64);
    _snwprintf_s(line, _TRUNCATE, L"%ls: %.1f / %.0f  (%.1f %%)",
                 caption, held, cap, 100.0f * held / cap);

    void* fontMgr = g_exeBase + P_FONTMANAGER;
    void* font    = *(void**)(g_exeBase + P_PANEL_FONT);
    if (!font) return;

    float dpi = LayoutF(P_DPI);
    float x   = *px + dpi * LayoutF(P_ROW_STEP);

    o_PrintLeftUnicode(fontMgr, font, x, *py, PANEL_LABEL_COLOUR, L"%ls", line);
    *py += dpi * LayoutF(P_ROW_STEP);
}

// ---------------------------------------------------------------- the hooks

static void h_ElecPanel(void* game, void* window, float* px, float* py)
{
    o_ElecPanel(game, window, px, py);
    if (!g_enabled || !g_panel || !window || !px || !py) return;

    __try { DrawChargeRow((BYTE*)window, px, py); }
    __except (FaultFilter("accumulator panel", GetExceptionInformation()))
    {
        Logf("battery  panel row disabled after a fault");
        g_panel = 0;
    }
}

static void h_SubShare(void* game, void* substation)
{
    if (g_enabled && substation && g_fedCount > 0)
    {
        __try
        {
            BYTE* b = (BYTE*)substation;
            if (IsBatteryFed(b))
            {
                BYTE* s = EletricStorage(b, false);
                BYTE* slot = s ? *(BYTE**)(s + S_SLOTS) : NULL;
                if (slot && *(float*)(slot + SLOT_QUALITY) < 1.0f)
                {
                    *(float*)(slot + SLOT_QUALITY) = 1.0f;
                    *(char*)(b + B_UNPOWERED) = 0;
                    g_repowered++;
                }
            }
        }
        __except (FaultFilter("accumulator substation share", GetExceptionInformation()))
        {
            Logf("battery  substation hook disabled after a fault");
            g_fedCount = 0;
        }
    }
    o_SubShare(game, substation);
}

static void h_Dispatch(void* game)
{
    if (!g_enabled || !game) { o_Dispatch(game); return; }

    if (!g_thread)
    {
        g_thread = GetCurrentThreadId();
        Logf("battery  dispatcher hook runs on thread %lu", g_thread);
    }

    AccSite acc[MAX_ACC];
    int n = 0;
    float dt = 0.0f;

    __try
    {
        dt = TickStep();
        n  = CollectAccumulators((BYTE*)game, acc, MAX_ACC);
        OpenChargeWindow(acc, n, dt);
        DeclareLive(acc, n);
        CollectFed(acc, n);
        g_repowered = 0;
        if (g_trace && n > 0) TakeSnapshot(acc[0].building, 0);
    }
    __except (FaultFilter("accumulator charge window", GetExceptionInformation()))
    {
        Logf("battery  disabled after a fault opening the charge window");
        g_enabled = 0;
        n = 0;
    }

    o_Dispatch(game);

    if (!g_enabled) return;

    __try
    {
        if (g_trace && n > 0) TakeSnapshot(acc[0].building, 1);
        CloseAndDischarge((BYTE*)game, acc, n);
        if (g_trace && n > 0) TakeSnapshot(acc[0].building, 2);
        if (g_gauges) UpdateGauges(acc, n, dt);

        double now = (double)GetTickCount64() / 1000.0;
        if (g_logSeconds > 0.0f && now - g_lastLog >= g_logSeconds)
        {
            g_lastLog = now;
            if (n == 0)
                Logf("battery  no accumulator found (types %d..., capacity >= %.0f)",
                     g_typeCount ? g_types[0] : -1, g_minCapacity);
            for (int i = 0; i < n; i++)
            {
                float held = *(float*)(acc[i].slot + SLOT_AMOUNT);
                Logf("battery  %8.1f / %.0f (%.1f %%)  in %+.3f  out %+.3f  this tick "
                     "(%.2f / %.2f per second, dt %.5f), %d substation(s) re-energised",
                     held, acc[i].declaredCap,
                     acc[i].declaredCap > 0.0f ? 100.0f * held / acc[i].declaredCap : 0.0f,
                     acc[i].charged, acc[i].discharged,
                     dt > 0.0f ? acc[i].charged / dt : 0.0f,
                     dt > 0.0f ? acc[i].discharged / dt : 0.0f, dt, g_repowered);
            }
            if (g_trace) ReportTrace();
        }

        if (g_probe)
        {
            if (now - g_lastProbe >= (double)g_probe)
            {
                g_lastProbe = now;
                ProbeGrid((BYTE*)game);
            }
        }
    }
    __except (FaultFilter("accumulator discharge", GetExceptionInformation()))
    {
        Logf("battery  disabled after a fault discharging");
        g_enabled = 0;
    }
}

// ---------------------------------------------------------------- plumbing

static void ParseTypes(const char* s)
{
    g_typeCount = 0;
    while (*s && g_typeCount < 8)
    {
        while (*s == ' ' || *s == ',' || *s == '\t') s++;
        if (!*s) break;
        g_types[g_typeCount++] = atoi(s);
        while (*s && *s != ',') s++;
    }
}

extern "C" __declspec(dllexport) unsigned TsmPluginApiVersion(void)
{
    return TSM_API_VERSION;
}

extern "C" __declspec(dllexport) int TsmPluginInit(const TsmHost* host, TsmPluginInfo* info)
{
    TsmBind(host);
    info->name    = "accumulator";
    info->version = "1.0";

    const char* ini = "plugins\\accumulator.ini";
    if (!H->configInt(ini, "accumulator", "enabled", 1))
        return 1;

    char buf[128];
    H->configString(ini, "accumulator", "charge_rate", buf, sizeof(buf), "60");
    g_chargeRate = (float)atof(buf);
    H->configString(ini, "accumulator", "discharge_rate", buf, sizeof(buf), "120");
    g_dischargeRate = (float)atof(buf);
    H->configString(ini, "accumulator", "min_capacity", buf, sizeof(buf), "500");
    g_minCapacity = (float)atof(buf);
    H->configString(ini, "accumulator", "building_types", buf, sizeof(buf), "18,19");
    ParseTypes(buf);
    H->configString(ini, "accumulator", "log_seconds", buf, sizeof(buf), "0");
    g_logSeconds = (float)atof(buf);
    g_probe  = H->configInt(ini, "accumulator", "probe", 0);
    g_gauges = H->configInt(ini, "accumulator", "gauges", 1);
    g_spread = H->configInt(ini, "accumulator", "spread_voltage", 0);
    g_trace  = H->configInt(ini, "accumulator", "trace", 0);
    g_panel  = H->configInt(ini, "accumulator", "panel", 1);
    H->configString(ini, "accumulator", "panel_caption", g_panelCaption,
                    sizeof(g_panelCaption), "Charge");

    if (g_typeCount == 0) { g_types[g_typeCount++] = TYPE_TRANSFORMATOR; }
    return 0;
}

extern "C" __declspec(dllexport) int TsmPluginStart(void)
{
    void** slot = FindIatSlot(g_exe, DLL_ENGINE, "?PowerTime@C3D_TIMER@@QEAAMM_N0@Z");
    if (!slot) { Logf("battery  no import slot for C3D_TIMER::PowerTime"); return 1; }
    o_PowerTime = (t_PowerTime)*slot;

    // Called, not hooked: this is the engine's own "hand out what this grid node
    // has", and a battery discharges by asking for it the way the dispatcher
    // asks for every transformer.
    o_NodeShare = (t_NodeShare)(g_exeBase + P_NODE_SHARE_RVA);

    // The substation hand-out. Hooked, not called: the seam this plugin needs is
    // *between* the dispatcher clearing a node's voltage and that node passing
    // its storage on, and this call is the only thing in that gap.
    static const BYTE kSubSharePrologue[] = {
        0x48, 0x8B, 0xC4,                                // mov  rax,rsp
        0x48, 0x89, 0x50, 0x10,                          // mov  [rax+0x10],rdx
        0x48, 0x89, 0x48, 0x08,                          // mov  [rax+0x08],rcx
        0x55, 0x53, 0x56, 0x57,                          // push rbp, rbx, rsi, rdi
        0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57   // push r12..r15
    };
    if (!InstallInlineHook(g_exeBase + P_SUB_SHARE_RVA, (void*)h_SubShare,
                           (void**)&o_SubShare, kSubSharePrologue,
                           sizeof(kSubSharePrologue), "substation share"))
        return 1;

    static const BYTE kDispatchPrologue[] = {
        0x48, 0x8B, 0xC4,                                // mov  rax,rsp
        0x55,                                            // push rbp
        0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57,  // push r12..r15
        0x48, 0x8D, 0xA8, 0x78, 0xFA, 0xFF, 0xFF         // lea  rbp,[rax-0x588]
    };
    if (!InstallInlineHook(g_exeBase + P_DISPATCH_RVA, (void*)h_Dispatch,
                           (void**)&o_Dispatch, kDispatchPrologue,
                           sizeof(kDispatchPrologue), "building dispatcher"))
        return 1;

    // Additive and cosmetic, so a refusal here costs the row and nothing else.
    if (g_panel)
    {
        void** s = FindIatSlot(g_exe, DLL_ENGINE,
                               "?PrintLeftUnicode@C3D_FONTMANAGER@@QEAAXPEAVC3D_FONT@@MMKPEB_WZZ");
        if (!s)
        {
            Logf("battery  no import slot for PrintLeftUnicode - no charge row");
            g_panel = 0;
        }
        else
        {
            o_PrintLeftUnicode = (t_PrintLeftUnicode)*s;

            static const BYTE kElecPanelPrologue[] = {
                0x4C, 0x8B, 0xDC,                            // mov  r11,rsp
                0x55, 0x56,                                  // push rbp, rsi
                0x41, 0x54,                                  // push r12
                0x49, 0x8D, 0xAB, 0xA8, 0xFE, 0xFF, 0xFF     // lea  rbp,[r11-0x158]
            };
            if (!InstallInlineHook(g_exeBase + P_ELEC_PANEL_RVA, (void*)h_ElecPanel,
                                   (void**)&o_ElecPanel, kElecPanelPrologue,
                                   sizeof(kElecPanelPrologue), "electric panel"))
                g_panel = 0;
        }
    }

    Logf("battery  charge %.0f/s, discharge %.0f/s, battery is a class-9 storage "
         "of at least %.0f on a building of type %d%s",
         g_chargeRate, g_dischargeRate, g_minCapacity,
         g_typeCount ? g_types[0] : -1, g_typeCount > 1 ? " (or another listed)" : "");
    return 0;
}

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) { return TRUE; }
