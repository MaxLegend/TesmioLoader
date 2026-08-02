// needs - resources the citizens require, as a tesmioloader plugin.
//
// The base game gives a citizen four things to buy in a shop - food, meat,
// clothes and electronics - and the list is four straight-line blocks of code
// inside one 15 KB function. There is no table to extend and no name to hook.
//
// What there is, is a *shape*. A citizen carries an array of "demands", each
// one carrying a pointer to an ordinary resource record, and the shop tick is
// completely generic over it: it walks the customer's demands, walks its own
// storage slots, and sells whatever matches. Nothing in that path knows what
// food is. So a fifth demand pointing at a fifth resource is sold by the same
// code that sells the other four, with no patch anywhere.
//
//   rva 0x836960  FUN_140836960(game, person)   the citizen's daily plan. Resets
//                 person+0x110 to zero and rebuilds the demand list from nine
//                 conditional blocks - food, meat, clothes, electronics, then
//                 five service demands (alcohol, culture, sport, church,
//                 doctor). Reached from 0x830640 when a person is at home and
//                 ready to decide what to do.
//
//   rva 0x171DA0  FUN_140171da0(game, building)  the shop tick, building type 3.
//                 Its second half is the sale: for every customer in
//                 building+0xBD8, for every demand of kind 1 or 2, find a
//                 storage slot whose resource matches and move goods across.
//                 **This is the function nothing here has to touch.**
//
//   rva 0xE40F0   FUN_1400e40f0(parser, storage, class, capacity, resource)
//                 builds a storage's slot list while building.ini is parsed. For
//                 $STORAGE_DEMAND_* it hard-codes the four names; for a plain
//                 $STORAGE it loops the whole resource vector. Hooking it is how
//                 a shop gets somewhere to put the new goods.
//
// PERSON, the fields this plugin reads or writes:
//
//   +0x0D8  eleven status floats, in the order the script VM lists them:
//           happiness, food, health, soviet, alcohol, culture, sport, religion,
//           clothing, electronic, crime
//   +0x110  demand count
//   +0x118  demand array, stride 0x80, **capacity 7**
//   +0x4F0  unsatisfied-demand count, then ten 16-byte entries at +0x4F8. The
//           planner's own prologue fills it from whatever the last list had
//           left over, which is what raises "N Citizen(s) were unable to get X".
//   +0x71C  0 citizen, 1 soviet tourist, 2 western tourist
//   size    0x750, from the operator new in the constructor at 0x823290
//
// DEMAND, 0x80 bytes:
//
//   +0x00  float  amount still wanted
//   +0x04  float  amount wanted in total
//   +0x08  int    kind. 0xF while the entry is being built; 1 and 2 are the two
//                 the shop tick serves, and the difference is urgency - a food
//                 demand of kind 1 suppresses every service demand, which is
//                 what keeps the list inside its seven slots.
//   +0x10  Resource*
//   +0x18  Target[0], 0x34 bytes - where this demand can be satisfied
//   +0x4C  Target[1]
//
// The seven-slot ceiling is why this plugin **clones** rather than builds. An
// entry synthesised from scratch would need both Targets right, and those are
// the part of the layout that is least understood; an entry memcpy'd from the
// citizen's own electronics demand is already routed to the shops that stock
// electronics, and only the resource pointer and the two amounts have to
// change. One `like =` key therefore drives both halves of the feature: the
// demand is cloned from that resource's demand, and the storage slot is added
// to exactly those storages that already stock it.
//
// Everything here is addresses for SOVIET64.exe v1.1.1.7. See docs/11-needs.md.

#include "../../src/tesmio_plugin.h"

// ---------------------------------------------------------------- the game

#define P_PERSON_PLAN_RVA   0x836960   // FUN_140836960(game, person)
#define P_STORAGE_BUILD_RVA 0x0E40F0   // FUN_1400e40f0(parser, storage, class, cap, res)
#define P_SHOP_TICK_RVA     0x171DA0   // FUN_140171da0(game, building) - building type 3
#define P_TYPE_LOAD_RVA     0x11D810   // FUN_14011d810 - "Initializing vanilla buildling types"
#define P_BUILDING_TYPES    0x9E6A30   // the vector<TYPE> that function fills
// Not a vector of pointers - a vector of the objects themselves, stride 0xBE8.
// The clear() at 0x1FF240 walks it with `lVar2 = lVar2 + 0xbe8`, and a live
// span of 40 477 440 bytes divides by 0xBE8 exactly 13 280 times.
#define TYPE_STRIDE         0xBE8
#define P_SLOT_PUSH_RVA     0x0B14E0   // std::vector<Slot>::push_back, 16-byte value
#define P_RESOURCE_VECTOR   0x9E11C0   // the resource vector: begin, end, capacity

#define RES_STRIDE          832        // 0x340
#define RES_NAME            0x00       // inline, NUL-terminated
// Per-transport-class block inside a record: the factor a storage of that class
// multiplies its capacity by, and a byte that excludes the pair outright. A
// resource whose factor is zero for a class cannot be stored in it at all,
// which is the "0.00 of 0.00 t" symptom.
#define RES_CLASS_FACTOR    0xCC       // + class * 0x20
#define RES_CLASS_BLOCKED   0xE8       // + class * 0x20
#define RES_CLASS_STRIDE    0x20

#define ST_CAPACITY         0x8C       // what $STORAGE declared
#define ST_CLASS            0x90       // RESOURCE_TRANSPORT_*
#define ST_TOKEN            0x94       // which $STORAGE_* token built this storage
#define SLOT_SIZE           0x10

// A storage carries **two** vectors of the same length, both 16 bytes per slot:
// the slots themselves at +0x00 and a parallel array at +0x18. A hex dump of a
// live 25-tonne shop storage shows them side by side -
//
//   +0x00  begin=...73670 end=...736A0 cap=...736A0    0x30 = 3 x 0x10
//   +0x18  begin=...73D70 end=...73DA0 cap=...73DA0    0x30 = 3 x 0x10
//
// - and `cap == end` in both, so appending to either reallocates. The second
// one is what the panel's per-resource "Limit amount" percentage comes from:
// growing only the first left the fourth entry reading past the end, and the
// percentage printed as -2147483648, which is INT_MIN, which is a NaN cast to
// int. Both have to grow together.
// THE BUCKET NOTHING VANILLA EVER LANDS IN
//
// rva 0x198670  FUN_140198670(game, dst, resource, amount) folds a purchase
// into a running total. It picks which total by comparing the resource against
// four cached records and falling through to a fifth bucket for anything else:
//
//   game+0xC300 -> game+0x12720      game+0xC318 -> game+0x12750
//   game+0xC310 -> game+0x12738      game+0xC320 -> game+0x12768
//   anything else                 -> game+0x12780
//
// The four are the goods the base game sells, so **the fifth branch is dead
// code in a stock game** and its vector was never constructed: `begin` is null
// while `end` is not. The first citizen to buy a modded good walks into it,
// 0x198859 loads the null begin into R8 and 0x198868 reads [R8+8].
//
// A vector with a null begin and a non-null end is not a state any live vector
// can be in, so normalising it to properly empty is safe and is all that is
// needed: the loop is guarded by `(end - begin) >> 4`, which is then zero.
#define G_GOODS_BUCKETS     0x12720    // five of them, stride 0x18
#define G_GOODS_BUCKET_OTHER 0x12780
#define G_GOODS_BUCKET_COUNT 5

#define B_STORAGES          0x970      // building -> vector<Storage>, stride 0xE0
#define STORAGE_STRIDE      0xE0
#define ST_SLOTS            0x00
#define ST_SLOTS2           0x18

// The values ST_TOKEN can take, straight off the id-to-name chain at the top of
// 0xE40F0 - the parser writes its own token number there and this copies it
// verbatim (`mov [rdi+0x94],eax` at 0xE4260). Only the demand family is named
// here; a plain warehouse is 0 and everything else is somebody else's business.
#define STOR_PLAIN           0
#define STOR_DEMAND_BASIC    2         // food (covered) + meat (cooler)
#define STOR_DEMAND_MEDIUM   3         // food + clothes
#define STOR_DEMAND_ADVANCED 4         // food + clothes + eletronics
#define STOR_DEMAND_MEDADV   5         // clothes + eletronics
#define STOR_DEMAND_HOTEL    10        // food + alcohol + meat
#define STOR_ANY            (-1)       // "wherever the donor landed"
// "nowhere - a building.ini already declares the shelf." The donor rule puts
// the goods in every storage that stocks the donor, which is exactly wrong for
// a resource meant to be sold in one building of its own: a pharmacy declares
// $STORAGE_SPECIAL ... medicine itself, and medicine has no business on a
// department store's shelf. The demand half is untouched by this - the citizen
// still wants it, and the shop tick still sells it from whatever storage has it.
#define STOR_NONE           (-2)

#define PR_STATUS           0xD8       // eleven floats; [0] is happiness
#define PR_DEMAND_N         0x110
#define PR_DEMANDS          0x118
#define PR_DEMAND_STRIDE    0x80
#define PR_TOURIST          0x71C

// The unsatisfied list the planner's own prologue fills from whatever the last
// cycle left over, capped at ten. One entry is { float amount, int kind,
// Resource* }.
#define PR_UNSAT_N          0x4F0
#define PR_UNSAT            0x4F8
#define PR_UNSAT_STRIDE     0x10
#define PR_UNSAT_MAX        10
#define UNSAT_AMOUNT        0x00
#define UNSAT_KIND          0x04
#define UNSAT_RESOURCE      0x08

// Hard ceiling, not a preference. The array runs from +0x118 and the
// unsatisfied-demand count sits at +0x4F0, so the eighth entry would begin at
// +0x498 and run over it. Configuration may lower this and may not raise it.
#define PR_DEMAND_CAP       7

#define DM_AMOUNT           0x00
#define DM_TOTAL            0x04
#define DM_KIND             0x08
#define DM_RESOURCE         0x10

#define DEMAND_KIND_URGENT  1          // the two kinds the shop tick serves
#define DEMAND_KIND_NORMAL  2

typedef void  (*t_PersonPlan)(void*, void*);
typedef void  (*t_StorageBuild)(void*, void*, int, float, void*);
typedef void  (*t_ShopTick)(void*, void*);
typedef void  (*t_TypeLoad)(void*, void*, void*);
typedef void  (*t_SlotPush)(void*, const void*);

static t_PersonPlan   o_PersonPlan;
static t_StorageBuild o_StorageBuild;
static t_ShopTick     o_ShopTick;
static t_SlotPush     o_SlotPush;

struct ResVector { BYTE* begin; BYTE* end; BYTE* cap; };

// Exactly SLOT_SIZE. **`limit` is not the quality field.** For an electric
// storage +0x0C is the node's voltage, which is where the name in
// 02-findings.md comes from, but for a goods storage it is the level the
// building wants to hold: the shop tick's first half reads
//
//     if (slot[+0x08] < slot[+0x0C] + c) -> order a delivery
//
// at 0x171E5x, and the panel prints "%.2f of %.2f %s - %.0f%%" from the pair.
// Leaving it zero is what made furniture read as a nonsense percentage: the
// shop had nothing and wanted nothing, and 0/0 came out of the divide.
struct Slot { void* res; float content; float limit; };

// ---------------------------------------------------------------- settings

// How many *kinds* of need may be declared. Unrelated to the seven-demand
// ceiling: that one is how many a single citizen carries at once, this is how
// many exist in the world. The binding limit in practice is elsewhere - the
// engine's resource vector holds 63 records against 57 base-game ones, so six
// mod resources fit - and a need may also name a vanilla resource, which costs
// no slot at all.
#define MAX_NEEDS 16

struct Need
{
    char  name[32];        // the resource the citizens are to want
    char  like[32];        // the vanilla demand it is cloned from
    float factor;          // how much of it, against the donor's amount
    int   category;        // STOR_*; STOR_ANY means "wherever the donor landed"
    char  categoryName[24];
    float chance;          // 0..1, how often a citizen takes it up at all
    float unhappiness;     // taken off happiness per cycle it went unmet

    // The donor's demand, captured whole the first time one is seen, so a
    // citizen who is not asking for the donor today can still be given this.
    // That is what makes the frequency independent - see StampDemand.
    BYTE  tmpl[PR_DEMAND_STRIDE];
    LONG  haveTmpl;

    BYTE* rec;             // resolved records, and the vector they belonged to
    BYTE* donorRec;
    BYTE* cacheBegin;      // both ends: `end` moves whenever a record is added,
    BYTE* cacheEnd;        // which is what lets a failed lookup be retried once
                           // the `resources` plugin has claimed its slot

    LONG  added;           // counters, for the periodic line
    LONG  skippedFull;
    LONG  replaced;
    LONG  storages;
    LONG  noDonor;         // citizens that carried no demand to clone from
    LONG  fromTmpl;        // ...and got the cached one instead
    LONG  punished;        // citizens whose happiness this took a bite out of
    DWORD warnedClasses;   // one "cannot be stored" line per class, not per storage
    int   missingLogged;   // one "no resource named" line per session
};

static Need g_need[MAX_NEEDS];
static int  g_needCount;

static int   g_enabled    = 1;
static int   g_doDemand   = 1;     // add the need to citizens
static int   g_doStorage  = 1;     // add the resource to the shops that stock the donor
// The type-descriptor pass has its own switch: it is the one pass that trusts
// a heuristic find (FindStoragesIn), so a fault there must not keep
// re-triggering on every later type load - which is exactly what happened
// when the __except logged "disabled" but never disabled anything.
static int   g_doTypes    = 1;     // extend the building types' own storages
static int   g_maxDemands = PR_DEMAND_CAP;
static int   g_replace    = 0;     // when the list is full: 0 skip, 1 take the donor's slot
static int   g_probe      = 0;
static float g_logSeconds = 60.0f;

static DWORD g_planThread;
static DWORD g_lastLog;
static LONG  g_probeDone;

// Diagnostics have their own budget, because both hooks run thousands of times
// per load and an unbounded line per call would be the whole log.
#define PROBE_STORAGE_LINES 40
#define PROBE_PERSON_LINES  6
static int  g_probeStorages;
static LONG g_probePersons;
static LONG g_overCap;          // citizens the game gave more demands than fit

// ---------------------------------------------------------------- resources

// The record's name is inline at +0x00 and the array has already been checked
// readable as a whole, so this is a bounded compare and no VirtualQuery - which
// matters, because a lookup that keeps missing would otherwise run once per
// citizen per plan.
static bool RecordIsNamed(const BYTE* rec, const char* name)
{
    for (int i = 0; i < 32; i++)
    {
        char c = (char)rec[RES_NAME + i];
        if (c != name[i]) return false;
        if (!c) return true;
    }
    return false;
}

static BYTE* ResourceByName(const ResVector* vec, const char* name)
{
    size_t span = (size_t)(vec->end - vec->begin);
    for (size_t i = 0; i < span / RES_STRIDE; i++)
    {
        BYTE* rec = vec->begin + i * RES_STRIDE;
        if (RecordIsNamed(rec, name)) return rec;
    }
    return NULL;
}

// The vector is heap-allocated and rebuilt at every map load at a fresh
// address, so nothing may cache a record across one. Both ends are remembered:
// `begin` moving means the whole array was relocated and every cached pointer
// is stale, `end` moving means a record was added - which is what the
// `resources` plugin does while the engine's own init loop runs, and therefore
// the one event that can turn a failed lookup into a good one.
//
// Nothing here ever asks the engine to *create* a record. It does not have to:
// the `resources` plugin publishes every name in its [list] during that init
// loop, which finishes before the first building.ini is parsed. Calling
// ResourceGet from inside the storage builder would mean re-entering another
// plugin's hook halfway through a parse, for no gain at all.
static bool ResolveNeed(Need* n)
{
    ResVector* vec = (ResVector*)(g_exeBase + P_RESOURCE_VECTOR);
    if (!ReadablePtr(vec, sizeof(*vec))) return false;
    if (!vec->begin || vec->end <= vec->begin) return false;

    size_t span = (size_t)(vec->end - vec->begin);
    if (span % RES_STRIDE) return false;
    if (!ReadablePtr(vec->begin, span)) return false;

    if (n->cacheBegin == vec->begin && n->cacheEnd == vec->end)
        return n->rec != NULL && n->donorRec != NULL;

    n->cacheBegin = vec->begin;
    n->cacheEnd   = vec->end;
    n->rec        = ResourceByName(vec, n->name);
    n->donorRec   = ResourceByName(vec, n->like);

    if (!n->rec && !n->missingLogged)
    {
        n->missingLogged = 1;
        Logf("needs    no resource named \"%s\" - declare it in plugins\\resources.ini "
             "(cloned from \"%s\") before it can be wanted", n->name, n->like);
    }
    return n->rec != NULL && n->donorRec != NULL;
}

// ---------------------------------------------------------------- the storage

static bool ListHas(const char* list, const char* word)
{
    if (!list) return true;                        // no opinion
    size_t wn = strlen(word);
    for (const char* p = list; *p; )
    {
        const char* e = strchr(p, ',');
        size_t      n = e ? (size_t)(e - p) : strlen(p);
        if (n == wn && strncmp(p, word, n) == 0) return true;
        if (!e) break;
        p = e + 1;
    }
    return false;
}

static int ListCount(const char* list)
{
    if (!list || !*list) return 0;
    int n = 1;
    for (const char* p = list; *p; p++) if (*p == ',') n++;
    return n;
}

static int SlotCount(BYTE* storage)
{
    ResVector* v = (ResVector*)storage;
    if (!v->begin || v->end < v->begin) return -1;
    return (int)((size_t)(v->end - v->begin) / SLOT_SIZE);
}

static int SlotIndexOf(BYTE* storage, const void* rec)
{
    ResVector* v = (ResVector*)storage;               // the slot vector is the storage's first field
    if (!v->begin || v->end < v->begin) return -1;

    size_t span = (size_t)(v->end - v->begin);
    if (span % SLOT_SIZE) return -1;

    for (size_t i = 0; i < span / SLOT_SIZE; i++)
        if (((Slot*)(v->begin + i * SLOT_SIZE))->res == rec) return (int)i;
    return -1;
}

// A storage's first field is a vector of 16-byte slots, and a real one always
// satisfies three things a lookalike cannot: the span is a whole number of
// slots, it is readable, and it is short - no goods storage in the game
// carries more than a handful. SlotCount checks the shape only; the
// readability half is what kept a false positive's `begin` out of the
// dereferences below.
static bool SlotVectorValid(BYTE* storage)
{
    ResVector* v = (ResVector*)storage;
    if (!v->begin || v->end < v->begin || v->cap < v->end) return false;

    size_t span = (size_t)(v->end - v->begin);
    if (span % SLOT_SIZE || span > 64 * SLOT_SIZE) return false;
    return span == 0 || ReadablePtr(v->begin, span);
}

// The invariant the heuristic scan in FindStoragesIn cannot fake: every slot
// of a real storage names a record inside the engine's resource vector. A
// triple of pointers found by accident in the type descriptor points
// anywhere else. Null slots are tolerated - they can never match the donor,
// so nothing is pushed on their account.
static bool SlotsPointAtResources(BYTE* storage, BYTE* resBegin, BYTE* resEnd)
{
    ResVector* v    = (ResVector*)storage;
    size_t     slots = (size_t)(v->end - v->begin) / SLOT_SIZE;
    for (size_t i = 0; i < slots; i++)
    {
        BYTE* res = (BYTE*)((Slot*)(v->begin + i * SLOT_SIZE))->res;
        if (!res) continue;
        if (res < resBegin || res >= resEnd) return false;
        if ((size_t)(res - resBegin) % RES_STRIDE) return false;
    }
    return true;
}

// A demand storage is built by naming the four goods outright, a plain storage
// by looping the whole resource vector. Rather than teaching this plugin which
// is which, the rule is the one that reads the same in both cases: **if the
// donor resource ended up in this storage and we did not, put us there too.**
// A warehouse that already stocks everything therefore matches the second half
// of the test and is left alone.
//
// `category` narrows that, and only ever narrows it. The donor already implies
// a category - `eletronics` lands in ADVANCED and MEDIUMADVANCED and nowhere
// else - so leaving it at `auto` is right most of the time. Naming one is for
// the case the donor cannot express: furniture in the big department stores
// but not in the small ones, both of which stock electronics.
// WHAT A BUILT BUILDING FORGETS
//
// `storage+0x94` holds the $STORAGE_* token number while building.ini is being
// parsed - the parser writes it with `mov [rdi+0x94],eax` at 0xE4260. **A built
// building does not keep it.** Every live storage reads back 0, which is the
// value for a plain $STORAGE, so a department store's shelf is indistinguishable
// from a warehouse by that field alone. Probing a real save is what settled it:
//
//   needs probe live storage token 0 class 0 cap 70.00 slots 3 donor@2 mine@-1
//
// - cap 70.00 and three slots is exactly $STORAGE_DEMAND_ADVANCED
//   RESOURCE_TRANSPORT_COVERED 70 from shop_prior.ini, and the token is 0.
//
// The composition survives, though, and it is unique per category: 0xE40F0 puts
// a fixed set of names in a COVERED demand storage and the five sets are all
// different. So the category is recovered from what is in the storage rather
// than from what built it.
//
// Only the COVERED side can be told apart this way. Every demand storage of
// class COOLER gets `meat` and nothing else, whatever token made it - that is
// one `if` at the end of 0xE40F0 - so a cooler is reported as "no opinion" and
// the donor rule alone decides.
static const struct { int token; const char* covered; } kDemandShapes[] = {
    { STOR_DEMAND_BASIC,    "food"                    },
    { STOR_DEMAND_MEDIUM,   "food,clothes"            },
    { STOR_DEMAND_ADVANCED, "food,clothes,eletronics" },
    { STOR_DEMAND_MEDADV,   "clothes,eletronics"      },
    { STOR_DEMAND_HOTEL,    "food,alcohol"            },
};
#define DEMAND_SHAPES ((int)(sizeof(kDemandShapes) / sizeof(kDemandShapes[0])))

// Every slot of one storage, by name and both floats. The vanilla neighbours
// are the reference: whatever food and clothes have in a field is what that
// field means, and a new slot that does not look like them is wrong.
static void DumpSlots(BYTE* storage)
{
    ResVector* v = (ResVector*)storage;
    if (!v->begin || v->end <= v->begin) return;
    size_t span = (size_t)(v->end - v->begin);
    if (span % SLOT_SIZE) return;

    ResVector* v2 = (ResVector*)(storage + ST_SLOTS2);
    bool have2 = ReadablePtr(v2, sizeof(*v2)) && v2->begin && v2->end > v2->begin &&
                 (size_t)(v2->end - v2->begin) == span;

    for (size_t i = 0; i < span / SLOT_SIZE; i++)
    {
        Slot* s = (Slot*)(v->begin + i * SLOT_SIZE);
        char  nm[40] = "-";
        if (s->res) SafeReadStr((BYTE*)s->res + RES_NAME, nm, sizeof(nm));
        Logf("needs    probe    slot %d  %-18s content %10.3f  limit %10.3f",
             (int)i, nm, s->content, s->limit);

        if (have2)
        {
            const DWORD* p = (const DWORD*)(v2->begin + i * SLOT_SIZE);
            Logf("needs    probe      side %d  %08X %08X %08X %08X   %10.4f %10.4f %10.4f %10.4f",
                 (int)i, p[0], p[1], p[2], p[3],
                 ((const float*)p)[0], ((const float*)p)[1],
                 ((const float*)p)[2], ((const float*)p)[3]);
        }
    }
}

// The shop panel prints a per-resource "Limit amount" percentage, and for a
// slot this plugin appended it reads -2147483648% - INT_MIN, which is what a
// NaN or an out-of-range float becomes when it is cast to int. The percentage
// is not in the slot: 16 bytes are fully accounted for by the resource pointer
// and the two floats. So it is in a second array, indexed by slot, that did not
// grow when the slot vector did.
//
// This dumps the whole 0xE0-byte storage before the push, so the array can be
// found: look for a { begin, end, capacity } triple whose span is the slot
// count times 4.
static void DumpStorageHeader(BYTE* storage, int slots)
{
    if (!ReadablePtr(storage, STORAGE_STRIDE)) return;
    Logf("needs    probe    storage %p, %d slot(s) before the push:", storage, slots);

    for (int off = 0; off < STORAGE_STRIDE; off += 16)
    {
        const BYTE* p = storage + off;
        Logf("needs    probe      +0x%02X  %08X %08X %08X %08X   %12.4f %12.4f %12.4f %12.4f",
             off,
             *(const DWORD*)(p), *(const DWORD*)(p + 4),
             *(const DWORD*)(p + 8), *(const DWORD*)(p + 12),
             *(const float*)(p), *(const float*)(p + 4),
             *(const float*)(p + 8), *(const float*)(p + 12));
    }

    // And the obvious candidates spelled out, so the answer does not have to be
    // read out of hex: any triple in the header whose span matches the slots.
    for (int off = 0; off + 24 <= STORAGE_STRIDE; off += 8)
    {
        ResVector* v = (ResVector*)(storage + off);
        if (!v->begin || v->end < v->begin || v->cap < v->end) continue;
        size_t span = (size_t)(v->end - v->begin);
        if (span == 0 || span > 4096) continue;
        if ((int)(span / 4) == slots || (int)(span / 8) == slots ||
            (int)(span / 16) == slots)
            Logf("needs    probe      +0x%02X is a vector of %zu bytes = %zu per slot",
                 off, span, span / (slots ? slots : 1));
    }
}

static int InferDemandCategory(BYTE* storage, int cls)
{
    if (cls != 0) return STOR_ANY;                 // only COVERED is distinctive

    ResVector* v = (ResVector*)storage;
    if (!v->begin || v->end <= v->begin) return STOR_ANY;
    size_t span = (size_t)(v->end - v->begin);
    if (span % SLOT_SIZE) return STOR_ANY;
    int slots = (int)(span / SLOT_SIZE);

    for (int s = 0; s < DEMAND_SHAPES; s++)
    {
        // Same size and every slot named in the shape: the shapes have no
        // subsets of one another, so one match is the answer.
        if (slots != ListCount(kDemandShapes[s].covered)) continue;

        bool all = true;
        for (int i = 0; i < slots && all; i++)
        {
            BYTE* rec = (BYTE*)((Slot*)(v->begin + (size_t)i * SLOT_SIZE))->res;
            char  nm[40];
            if (!rec || !SafeReadStr(rec + RES_NAME, nm, sizeof(nm))) { all = false; break; }
            if (!ListHas(kDemandShapes[s].covered, nm)) all = false;
        }
        if (all) return kDemandShapes[s].token;
    }
    return STOR_ANY;
}

// `live` distinguishes the two callers, and it changes exactly one field.
//
// While building.ini is parsed this runs against the building **type's**
// storage description, and there slot+0x08 is what the game itself writes -
// the resource's per-class factor times the declared capacity. On a building
// that already exists slot+0x08 is the **content**, and writing that figure
// there would conjure a warehouse full of goods out of nothing, so a live slot
// starts at zero and the shop orders its own first delivery.
static void ExtendStorage(BYTE* storage, int cls, bool live)
{
    if (cls < 0 || cls > 31) return;
    float capacity = *(float*)(storage + ST_CAPACITY);
    if (capacity <= 0.0f) return;

    // The slot vector is dereferenced several times below. A storage that
    // came from FindStoragesIn has already proven it, a live one is trusted -
    // and one cheap check here means neither has to be.
    if (!SlotVectorValid(storage)) return;

    // The token if the parser's copy still has one, otherwise whatever the
    // composition says. STOR_ANY from here means "cannot tell", and a category
    // filter does not reject what it cannot tell.
    int token = *(int*)(storage + ST_TOKEN);
    if (token <= STOR_PLAIN || token >= 64) token = InferDemandCategory(storage, cls);

    for (int i = 0; i < g_needCount; i++)
    {
        Need* n = &g_need[i];

        // `none`: the shelf is declared in a building.ini, so no storage
        // anywhere gets a slot from here. Checked before anything is resolved,
        // because there is nothing to say about a storage this need has no
        // opinion on.
        if (n->category == STOR_NONE) continue;

        bool  resolved = ResolveNeed(n);

        int donorAt = resolved ? SlotIndexOf(storage, n->donorRec) : -1;
        int mineAt  = resolved ? SlotIndexOf(storage, n->rec)      : -1;

        // Every reason a storage can be passed over, in one line. Without this
        // a storage that does not get the slot is indistinguishable from a hook
        // that never ran, which cost one whole test cycle.
        if (g_probe && g_probeStorages < PROBE_STORAGE_LINES)
        {
            g_probeStorages++;
            Logf("needs    probe  %s storage category %-2d class %-2d cap %8.2f  slots %d  "
                 "%s donor@%d mine@%d", live ? "live " : "type ",
                 token, cls, capacity, SlotCount(storage),
                 resolved ? "resolved" : "UNRESOLVED", donorAt, mineAt);
        }

        if (!resolved) continue;

        // A filter that cannot tell must not reject: STOR_ANY on either side
        // falls through to the donor rule, which is the accurate half anyway.
        if (n->category != STOR_ANY && token != STOR_ANY && n->category != token) continue;

        if (donorAt < 0) continue;

        // Already in, from an earlier session - but an earlier session may have
        // put a wrong figure in it. A slot this plugin wrote with
        // factor*capacity ended up as 24.5 t of furniture on a shelf that holds
        // 1.6 t of electronics, and that saved with the world. Anything wildly
        // out of step with the donor beside it is repaired once, on the pass
        // that first sees the building.
        if (mineAt >= 0)
        {
            if (!live) continue;
            Slot* mine  = (Slot*)(((ResVector*)storage)->begin + (size_t)mineAt  * SLOT_SIZE);
            Slot* donor = (Slot*)(((ResVector*)storage)->begin + (size_t)donorAt * SLOT_SIZE);
            if (mine->limit > donor->limit * 3.0f + 1.0f)
            {
                Logf("needs    \"%s\" had %.3f where \"%s\" has %.3f - an earlier version "
                     "wrote that; resetting to the donor's", n->name, mine->limit,
                     n->like, donor->limit);
                mine->content = donor->content;
                mine->limit   = donor->limit;
            }
            continue;
        }

        // The record may have come back from ResourceGet rather than out of the
        // span already checked, and the class block sits 0xE8 bytes in.
        if (!ReadablePtr(n->rec, RES_STRIDE)) continue;

        if (g_probe && live && n->storages == 0) DumpStorageHeader(storage, SlotCount(storage));

        // The same two numbers the game itself pushes: the record's factor for
        // this transport class, times the storage's declared capacity. A factor
        // of zero means the engine considers the pair impossible, and a slot
        // built on it would report zero capacity.
        size_t off    = (size_t)RES_CLASS_FACTOR + (size_t)cls * RES_CLASS_STRIDE;
        float  factor = *(float*)(n->rec + off);
        char   blocked = *(char*)(n->rec + RES_CLASS_BLOCKED + (size_t)cls * RES_CLASS_STRIDE);
        if (factor <= 0.0f || blocked)
        {
            // Every building type with a storage of this class would otherwise
            // say it again. The cause is always the same one: the resource was
            // cloned from a template whose transport class the donor's shops
            // cannot carry.
            if (!(n->warnedClasses & (1u << cls)))
            {
                n->warnedClasses |= (1u << cls);
                Logf("needs    \"%s\" cannot be stored in transport class %d - no slot added. "
                     "Clone it from \"%s\" in resources.ini", n->name, cls, n->like);
            }
            continue;
        }

        // CLONE THE DONOR'S SLOT, DO NOT COMPUTE ONE.
        //
        // Two guesses at what the second float means were both wrong, and the
        // shop showed for it: writing factor*capacity there put 24.5 tonnes of
        // furniture on the shelf out of nothing. The live values say what it
        // really is - with a 70 t storage, food and clothes hold 2.333 and
        // eletronics 1.633, which is capacity * factor / 15 in each case, not
        // a content and not a limit.
        //
        // So the same rule that already works for a citizen's demand applies
        // here: copy the donor's slot whole and change only the resource. It is
        // right by construction whatever the field turns out to mean, and it
        // scales itself - the donor's slot was built against this very storage.
        Slot s = *(Slot*)(((ResVector*)storage)->begin + (size_t)donorAt * SLOT_SIZE);
        s.res = n->rec;
        if (!live) s.content = factor * capacity;    // a type description carries only this

        // The parallel array first, while the slot index of the donor is still
        // the one both vectors agree on. It only grows when it already matches
        // the slot vector - anything else is a shape this was not written for.
        ResVector* v2 = (ResVector*)(storage + ST_SLOTS2);
        int slots = SlotCount(storage);
        if (ReadablePtr(v2, sizeof(*v2)) && v2->begin && v2->end > v2->begin &&
            (size_t)(v2->end - v2->begin) == (size_t)slots * SLOT_SIZE &&
            donorAt < slots)
        {
            BYTE side[SLOT_SIZE];
            memcpy(side, v2->begin + (size_t)donorAt * SLOT_SIZE, SLOT_SIZE);
            o_SlotPush(storage + ST_SLOTS2, side);
        }

        o_SlotPush(storage, &s);

        if (InterlockedIncrement(&n->storages) <= 2)
        {
            Logf("needs    \"%s\" added to a %s %s storage that stocks \"%s\" "
                 "(class %d, factor %.3f)", n->name, live ? "live" : "type",
                 n->categoryName, n->like, cls, factor);
            DumpSlots(storage);     // the neighbours settle what the two fields mean
        }
    }
}

static void h_StorageBuild(void* parser, void* storage, int cls, float capacity, void* res)
{
    o_StorageBuild(parser, storage, cls, capacity, res);
    if (!g_enabled || !g_doStorage || !storage) return;

    __try { ExtendStorage((BYTE*)storage, cls, false); }
    __except (FaultFilter("needs storage", GetExceptionInformation()))
    {
        Logf("needs    storage slots disabled after a fault");
        g_doStorage = 0;
    }
}

// ------------------------------------------------- the shops that already exist
//
// The hook above teaches the building *type*, which only helps a building the
// game has yet to construct - and only if the type was still being parsed when
// the resource existed. A shop standing in a save that was made before the need
// was declared has a storage the type no longer describes, and no amount of
// re-parsing reaches it.
//
// So the same rule is applied a second time, to the real thing: the shop tick
// hands over one building of type 3 per call, and its storages are an ordinary
// vector at building+0x970. Done once per building and then remembered, because
// this runs many times a second per shop.

#define MAX_SHOPS 512
static void* g_shopSeen[MAX_SHOPS];

static bool ShopAlreadyDone(void* b)
{
    unsigned hash = (unsigned)((((size_t)b) >> 4) * 2654435761u);
    int home = (int)(hash % MAX_SHOPS);

    for (int n = 0; n < 16; n++)
    {
        void** slot = &g_shopSeen[(home + n) % MAX_SHOPS];
        if (*slot == b) return true;
        if (!*slot) { *slot = b; return false; }
    }
    // The table is full around this hash. Claiming the home slot means the
    // building it displaces gets looked at once more, which costs one wasted
    // scan and never a duplicate slot - the "are we in it already" test is the
    // thing that guarantees that, not this table.
    g_shopSeen[home] = b;
    return false;
}

// THE PREVIEW, AND WHY IT IS STILL EMPTY
//
// A built shop stocks furniture; the build-menu preview does not, because the
// preview reads the **building type's** storage list and this plugin only ever
// reaches the built building's own.
//
// The type's list is not built by 0xE40F0 - that hook has never once fired.
// The building.ini parser at 0x10E200 carries its own copy of the whole thing:
// the $STORAGE_DEMAND_* strcmp chain is at 0x117BAE and the per-class factor
// multiply is `mulss xmm0,[rax+rbx+0xCC]` at 0x117B91, which is also the exact
// instruction that faulted when a token in a comment sent it a null resource.
//
// So the type's storages have to be found in the type descriptor at
// building+0x318, and this is the reconnaissance for that: one pass over the
// descriptor looking for a { begin, end, capacity } triple whose span divides
// by the storage stride and whose first storage looks like a storage. The
// offset it finds is what the next version can write to directly.
#define TYPEDESC_OFF   0x318
#define TYPEDESC_SCAN  0x900        // how far into the descriptor to look

// Every storage in the vector has to look like one, not just the first: the
// descriptor is big and a triple of pointers of the right shape can occur by
// accident. A wrong guess here writes into somebody else's structure.
//
// "Looks like one" was once three field checks, and it was not enough: a
// candidate passed them with a slot vector whose `begin` did not even name
// readable memory, and the first dereference faulted. The bar is now the one
// thing a real storage has and an accident does not - a readable slot vector
// whose every resource pointer lands inside the engine's resource vector.
static ResVector* FindStoragesIn(BYTE* td)
{
    if (!td || !ReadablePtr(td, TYPEDESC_SCAN)) return NULL;

    // The invariant is only as good as the resource vector itself; when that
    // cannot be read the weaker checks below still apply.
    ResVector* rv = (ResVector*)(g_exeBase + P_RESOURCE_VECTOR);
    BYTE* resBegin = NULL;
    BYTE* resEnd   = NULL;
    if (ReadablePtr(rv, sizeof(*rv)) && rv->begin && rv->end > rv->begin &&
        !((size_t)(rv->end - rv->begin) % RES_STRIDE))
    {
        resBegin = rv->begin;
        resEnd   = rv->end;
    }

    for (size_t off = 0; off + 24 <= TYPEDESC_SCAN; off += 8)
    {
        ResVector* v = (ResVector*)(td + off);
        if (!v->begin || v->end <= v->begin || v->cap < v->end) continue;

        size_t span = (size_t)(v->end - v->begin);
        if (span % STORAGE_STRIDE || span > 64 * STORAGE_STRIDE) continue;
        if (!ReadablePtr(v->begin, span)) continue;

        bool ok = true;
        for (size_t i = 0; i < span / STORAGE_STRIDE && ok; i++)
        {
            BYTE* s   = v->begin + i * STORAGE_STRIDE;
            int   cls = *(int*)(s + ST_CLASS);
            float cap = *(float*)(s + ST_CAPACITY);
            if (cls < 0 || cls > 17 || !(cap > 0.0f) || !SlotVectorValid(s)) ok = false;
            else if (resBegin && !SlotsPointAtResources(s, resBegin, resEnd)) ok = false;
        }
        if (ok) return v;
    }
    return NULL;
}

// THE TYPE'S OWN STORAGES, WHICH ARE WHAT THE INTERFACE READS
//
// Two things come from here rather than from the built building: the
// build-menu preview ("Warehouse: 12t of Food, 8.8t of Electronics") and the
// **per-resource maximum** the shop window divides by. A small shopping centre
// declares 25 t; 25 x 0.5 is the 12 t of food and 25 x 0.35 the 8.8 t of
// electronics, which is the same factor*capacity the parser writes at
// 0x117B91. Without a slot here furniture had no maximum at all, and the panel
// printed "0.58 of -0.00 tons".
//
// So the same rule is applied a third time, to the type descriptor. Once per
// type is enough - the second building of a type finds the slot already there.
static void ExtendOneType(BYTE* td)
{
    ResVector* v = FindStoragesIn(td);
    if (!v) return;

    size_t span = (size_t)(v->end - v->begin);
    for (size_t i = 0; i < span / STORAGE_STRIDE; i++)
    {
        BYTE* stor = v->begin + i * STORAGE_STRIDE;
        ExtendStorage(stor, *(int*)(stor + ST_CLASS), false);
    }
}

static void ExtendTypeStorages(BYTE* b)
{
    ExtendOneType(*(BYTE**)(b + TYPEDESC_OFF));
}

// EVERY TYPE, NOT ONLY THE ONES SOMEBODY HAS BUILT
//
// Reaching the type descriptor through a built building leaves the build-menu
// preview empty until the first shop of that type exists, which is exactly the
// symptom that was left. The types are all in one place:
//
//   rva 0x11D810   FUN_14011d810   "Initializing vanilla buildling types" -
//                  walks media_soviet/buildings_types/*.ini and the Workshop,
//                  parses each, and fills the vector<TYPE*> at 0x9E6A30.
//
// It is a vector of **pointers**, stride 8 - `mov rax,[rdx] / mov rcx,[rax +
// r15*8]` at 0x11DD50 inside that same function. Post-hooking it and walking
// the vector reaches every type the game knows, built or not, and it runs
// after "Initializing resources", so a mod resource is already published by
// then.
static void ExtendAllTypes(void)
{
    ResVector* v = (ResVector*)(g_exeBase + P_BUILDING_TYPES);
    if (!ReadablePtr(v, sizeof(*v))) { Logf("needs    type list unreadable"); return; }

    // Said out loud rather than returned silently: the first attempt at this
    // walked away without a word and the preview stayed broken, which looked
    // exactly like the hook not firing.
    Logf("needs    type list at 0x%X: %p %p %p", P_BUILDING_TYPES,
         v->begin, v->end, v->cap);

    if (!v->begin || v->end <= v->begin)
    {
        Logf("needs    type list is not a { begin, end, cap } vector - the preview "
             "fills in when the first building of a type exists instead");
        return;
    }

    size_t span = (size_t)(v->end - v->begin);
    if (span % TYPE_STRIDE)
    {
        Logf("needs    type list span %zu is not a whole number of 0x%X-byte types",
             span, TYPE_STRIDE);
        return;
    }

    int types = 0, touched = 0;
    for (size_t i = 0; i < span / TYPE_STRIDE; i++)
    {
        BYTE* td = v->begin + i * TYPE_STRIDE;
        if (!ReadablePtr(td, TYPEDESC_SCAN)) continue;
        types++;

        LONG before = 0;
        for (int k = 0; k < g_needCount; k++) before += g_need[k].storages;
        ExtendOneType(td);
        LONG after = 0;
        for (int k = 0; k < g_needCount; k++) after += g_need[k].storages;
        if (after != before) touched++;
    }
    Logf("needs    %d building type(s) scanned, %d gained a slot - the build-menu "
         "preview is right before anything is built", types, touched);
}

static void StockShop(BYTE* b)
{
    if (ShopAlreadyDone(b)) return;

    // The type first: it is what the preview and the per-resource maximum come
    // from, and doing it before the live storages means the two agree from the
    // first frame the window can be opened.
    ExtendTypeStorages(b);

    ResVector* sv = (ResVector*)(b + B_STORAGES);
    if (!ReadablePtr(sv, sizeof(*sv))) return;
    if (!sv->begin || sv->end <= sv->begin) return;

    size_t span = (size_t)(sv->end - sv->begin);
    if (span % STORAGE_STRIDE) return;
    if (!ReadablePtr(sv->begin, span)) return;

    for (size_t i = 0; i < span / STORAGE_STRIDE; i++)
    {
        BYTE* stor = sv->begin + i * STORAGE_STRIDE;
        ExtendStorage(stor, *(int*)(stor + ST_CLASS), true);
    }
}

static t_TypeLoad o_TypeLoad;

static void h_TypeLoad(void* a, void* b, void* c)
{
    o_TypeLoad(a, b, c);
    if (!g_enabled || !g_doStorage || !g_doTypes) return;

    __try { ExtendAllTypes(); }
    __except (FaultFilter("needs type storages", GetExceptionInformation()))
    {
        Logf("needs    type storages disabled after a fault - the preview stays empty "
             "until a building of that type exists");
        g_doTypes = 0;
    }
}

// AN EMPTY BUCKET IS NOT ENOUGH - IT HAS TO BE THE SAME LENGTH AS THE OTHERS
//
// The first repair here made the fifth bucket properly empty and the game still
// died in the same instruction, because the loop is not bounded by the bucket
// it reads:
//
//   14019887B  mov rax,[rbx+8] / sub rax,[rbx] / sar rax,4   ; count from ANOTHER vector
//   140198859  mov r8,[r14]                                  ; read from the fifth
//   140198868  mulss xmm0,[r8+rcx*8+8]
//
// A live save shows the four vanilla buckets holding ten 16-byte entries each
// and the fifth holding none, so the game simply assumes all five are the same
// length - which they always are when only its own four goods can be bought.
//
// So the fifth is filled to match, one entry per entry of a bucket that has
// them, copied so the fields that are not understood keep whatever shape they
// have, with the float at +0x08 - the one 0x198868 multiplies and 0x198875
// accumulates - zeroed, because that is the running total and a new good starts
// at nothing.
static void FixOtherGoodsBucket(BYTE* game)
{
    ResVector* other = (ResVector*)(game + G_GOODS_BUCKET_OTHER);
    if (!ReadablePtr(other, sizeof(*other))) return;

    size_t have = (other->begin && other->end > other->begin)
                ? (size_t)(other->end - other->begin) : 0;

    // A null begin with a non-null end is a state no live vector can be in.
    if (!other->begin && (other->end || other->cap)) { other->end = NULL; other->cap = NULL; }

    ResVector* src = NULL;
    size_t want = 0;
    for (int i = 0; i < G_GOODS_BUCKET_COUNT - 1; i++)
    {
        ResVector* b = (ResVector*)(game + G_GOODS_BUCKETS + (size_t)i * 0x18);
        if (!ReadablePtr(b, sizeof(*b)) || !b->begin || b->end <= b->begin) continue;
        size_t span = (size_t)(b->end - b->begin);
        if (span % SLOT_SIZE || span > 4096) continue;
        if (!ReadablePtr(b->begin, span)) continue;
        if (span > want) { want = span; src = b; }
    }
    if (!src || have >= want) return;

    static LONG said;
    if (InterlockedExchange(&said, 1) == 0)
        Logf("needs    the \"other goods\" purchase bucket at game+0x%X holds %zu of the "
             "%zu entries the others do - filling it, or the first citizen to buy a "
             "modded good faults at 0x198868",
             G_GOODS_BUCKET_OTHER, have / SLOT_SIZE, want / SLOT_SIZE);

    for (size_t at = have; at < want; at += SLOT_SIZE)
    {
        BYTE e[SLOT_SIZE];
        memcpy(e, src->begin + at, SLOT_SIZE);
        *(float*)(e + 8) = 0.0f;                 // the running total, not a key
        o_SlotPush(game + G_GOODS_BUCKET_OTHER, e);
    }
}

static void ProbeGoodsBuckets(BYTE* game)
{
    for (int i = 0; i < G_GOODS_BUCKET_COUNT; i++)
    {
        ResVector* v = (ResVector*)(game + G_GOODS_BUCKETS + (size_t)i * 0x18);
        if (!ReadablePtr(v, sizeof(*v))) continue;

        size_t span = (v->begin && v->end > v->begin) ? (size_t)(v->end - v->begin) : 0;
        Logf("needs    probe  goods bucket %d at game+0x%X: %zu entr(ies)",
             i, G_GOODS_BUCKETS + i * 0x18, span / SLOT_SIZE);

        // The first two entries of each, so the fields that are keys can be
        // told from the field that is a total.
        for (size_t k = 0; k < span / SLOT_SIZE && k < 2; k++)
        {
            const DWORD* p = (const DWORD*)(v->begin + k * SLOT_SIZE);
            Logf("needs    probe      [%zu] %08X %08X %08X %08X   %10.4f %10.4f %10.4f %10.4f",
                 k, p[0], p[1], p[2], p[3],
                 ((const float*)p)[0], ((const float*)p)[1],
                 ((const float*)p)[2], ((const float*)p)[3]);
        }
    }
}

static void h_ShopTick(void* game, void* building)
{
    // **Before the original, not after.** The purchase happens inside it, so a
    // repair applied afterwards is one tick too late - which is exactly how the
    // first attempt still crashed.
    if (g_enabled && game)
    {
        __try
        {
            static LONG probed;
            if (g_probe && InterlockedExchange(&probed, 1) == 0)
                ProbeGoodsBuckets((BYTE*)game);
            FixOtherGoodsBucket((BYTE*)game);
        }
        __except (FaultFilter("needs goods bucket", GetExceptionInformation())) {}
    }

    o_ShopTick(game, building);
    if (!g_enabled || !g_doStorage || !building) return;

    __try { StockShop((BYTE*)building); }
    __except (FaultFilter("needs shop stock", GetExceptionInformation()))
    {
        Logf("needs    stocking built shops disabled after a fault");
        g_doStorage = 0;
    }
}

// ---------------------------------------------------------------- the demand

static BYTE* DemandAt(BYTE* person, int i)
{
    return person + PR_DEMANDS + (size_t)i * PR_DEMAND_STRIDE;
}

static void ProbeDump(BYTE* person, int count)
{
    Logf("needs    probe  person %p: %d demand(s), tourist flag %d",
         person, count, *(int*)(person + PR_TOURIST));

    for (int i = 0; i < count && i < PR_DEMAND_CAP + 2; i++)
    {
        BYTE* d   = DemandAt(person, i);
        BYTE* rec = *(BYTE**)(d + DM_RESOURCE);
        char  nm[40] = "-";
        if (rec) SafeReadStr(rec + RES_NAME, nm, sizeof(nm));
        Logf("needs    probe    [%d] kind %-2d  %-18s %.5f of %.5f  target %d/%d",
             i, *(int*)(d + DM_KIND), nm,
             *(float*)(d + DM_AMOUNT), *(float*)(d + DM_TOTAL),
             *(int*)(d + 0x18), *(int*)(d + 0x4C));
    }

    const float* st = (const float*)(person + PR_STATUS);
    Logf("needs    probe    status  happy %.2f food %.2f health %.2f soviet %.2f "
         "alcohol %.2f culture %.2f sport %.2f religion %.2f clothes %.2f electronics %.2f",
         st[0], st[1], st[2], st[3], st[4], st[5], st[6], st[7], st[8], st[9]);
}

// A citizen's mood, the way the game itself does it
//
// A cloned demand has no status float of its own - all eleven at person+0xD8
// are spoken for - so an unmet new need cannot darken the mood the way an unmet
// vanilla one does. But the *mechanism* the game uses is not the float, it is a
// plain subtraction, and it is right there in the planner at 0x836DA9:
//
//     movss xmm0,[rbx+0xD8]     ; happiness
//     subss xmm0,xmm1           ; minus a penalty sized by how low the status is
//     movss [rbx+0xD8],xmm0
//     jbe  ...                  ; and clamped at zero
//
// so the same subtraction applies here, sized by configuration rather than by a
// status. What stands in for the status is the planner's own record of what the
// citizen failed to buy: the prologue has just filled person+0x4F0 from
// whatever the last cycle left over, and our resource being in that list means
// exactly "went shopping for it and came back without it".
static void ApplyUnhappiness(BYTE* person)
{
    int n = *(int*)(person + PR_UNSAT_N);
    if (n <= 0 || n > PR_UNSAT_MAX) return;

    float* happiness = (float*)(person + PR_STATUS);

    for (int i = 0; i < n; i++)
    {
        BYTE* e   = person + PR_UNSAT + (size_t)i * PR_UNSAT_STRIDE;
        BYTE* rec = *(BYTE**)(e + UNSAT_RESOURCE);
        if (!rec) continue;

        for (int k = 0; k < g_needCount; k++)
        {
            Need* nd = &g_need[k];
            if (nd->unhappiness <= 0.0f || rec != nd->rec) continue;

            *happiness -= nd->unhappiness;
            if (*happiness < 0.0f) *happiness = 0.0f;
            InterlockedIncrement(&nd->punished);
        }
    }
}

// A generator of this plugin's own. rand() belongs to the game's CRT and is
// stepped by the simulation; drawing from it would shift every roll the game
// makes, which is not a thing a plugin should do to a deterministic world.
static unsigned g_rngState = 0x9E3779B9u;

static float Rand01(void)
{
    unsigned x = g_rngState;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    g_rngState = x;
    return (float)(x >> 8) / 16777216.0f;
}

// The planner has just rebuilt the list from scratch, so the entries are all
// this cycle's and the count is exact. Cloning is a memcpy of the donor's whole
// 0x80 bytes followed by three writes - resource, amount, total - because
// everything else in the entry is routing that is already correct for a shop
// selling the donor.
static void ExtendDemands(BYTE* person)
{
    int count = *(int*)(person + PR_DEMAND_N);

    // A count past the array's own capacity is either a citizen the game gave
    // more demands than the seven that fit, or a person pointer that is not
    // one. Either way nothing is appended - but it is counted and said out
    // loud, because a silent return here looks exactly like a hook that never
    // fired.
    if (count < 0 || count > PR_DEMAND_CAP)
    {
        if (InterlockedIncrement(&g_overCap) == 1)
            Logf("needs    WARN  a citizen came back from the planner with %d demand(s), "
                 "past the %d that fit at person+0x118 - nothing appended for it",
                 count, PR_DEMAND_CAP);
        return;
    }

    if (g_probe && InterlockedIncrement(&g_probePersons) <= PROBE_PERSON_LINES)
        ProbeDump(person, count);

    // Before anything is appended: the list at +0x4F0 is last cycle's failures,
    // and the planner has just finished filling it.
    ApplyUnhappiness(person);

    // Round-robin rather than always from the top. With more needs declared
    // than a citizen has room for, a fixed order would mean the first line of
    // [list] always wins the last free slot and the last line is never taken up
    // at all. Rotating the starting point spreads that over the population, the
    // way `chance` already spreads one need over time.
    static LONG rotate;
    int first = (int)((unsigned)InterlockedIncrement(&rotate) % (unsigned)g_needCount);

    for (int k = 0; k < g_needCount; k++)
    {
        Need* n = &g_need[(first + k) % g_needCount];
        if (!ResolveNeed(n)) continue;

        int donor = -1;
        for (int j = 0; j < count; j++)
        {
            BYTE* d = DemandAt(person, j);
            if (*(BYTE**)(d + DM_RESOURCE) == n->rec) { donor = -2; break; }   // already ours
            if (*(BYTE**)(d + DM_RESOURCE) != n->donorRec) continue;

            int kind = *(int*)(d + DM_KIND);
            if (kind == DEMAND_KIND_URGENT || kind == DEMAND_KIND_NORMAL) donor = j;
        }
        if (donor == -2) continue;                    // already in this citizen's list

        // The donor's entry is captured the first time one is seen anywhere,
        // and from then on a citizen who is not asking for the donor today can
        // still be given the need. **That is what makes the frequency
        // independent of the donor's**: without it a citizen wants furniture
        // exactly when they want electronics and never otherwise.
        //
        // The cached entry came off some other citizen, so its amounts carry
        // that citizen's age and family; `factor` scales them and the rest is
        // routing, which is the same for everyone shopping for the same goods.
        BYTE  synth[PR_DEMAND_STRIDE];
        BYTE* src;

        if (donor >= 0)
        {
            src = DemandAt(person, donor);
            if (!n->haveTmpl)
            {
                memcpy(n->tmpl, src, PR_DEMAND_STRIDE);
                InterlockedExchange(&n->haveTmpl, 1);
                Logf("needs    \"%s\" captured a \"%s\" demand as its template - "
                     "citizens can now want it on their own", n->name, n->like);
            }
        }
        else
        {
            InterlockedIncrement(&n->noDonor);
            if (!n->haveTmpl) continue;               // nothing to stamp yet
            memcpy(synth, n->tmpl, PR_DEMAND_STRIDE);
            src = synth;
            InterlockedIncrement(&n->fromTmpl);
        }

        // How often, decided here rather than by the donor. Rolled after the
        // template exists so the draw is not wasted on citizens that could not
        // have been given it anyway.
        if (n->chance < 1.0f && Rand01() > n->chance) continue;

        BYTE* dst;

        if (count < g_maxDemands)
        {
            dst = DemandAt(person, count);
            memcpy(dst, src, PR_DEMAND_STRIDE);
            count++;
            *(int*)(person + PR_DEMAND_N) = count;
            InterlockedIncrement(&n->added);
        }
        else if (g_replace && donor >= 0)
        {
            // The seven slots are full. Taking the donor's own entry means the
            // citizen shops for one of the two goods today and the other
            // tomorrow, which is a far better failure than the need silently
            // never appearing. Only possible when the donor is really in this
            // citizen's list - a stamped one has no entry to take over.
            dst = src;
            InterlockedIncrement(&n->replaced);
        }
        else
        {
            InterlockedIncrement(&n->skippedFull);
            continue;
        }

        *(BYTE**)(dst + DM_RESOURCE) = n->rec;
        *(float*)(dst + DM_AMOUNT)   = *(float*)(src + DM_AMOUNT) * n->factor;
        *(float*)(dst + DM_TOTAL)    = *(float*)(src + DM_TOTAL)  * n->factor;
    }
}

static void LogProgress()
{
    if (g_logSeconds <= 0.0f) return;
    DWORD now = GetTickCount();
    if ((DWORD)(now - g_lastLog) <= (DWORD)(g_logSeconds * 1000.0f)) return;
    g_lastLog = now;

    for (int i = 0; i < g_needCount; i++)
    {
        Need* n = &g_need[i];
        Logf("needs    %-18s %ld added (%ld from the template), %ld replaced, "
             "%ld skipped (full), %ld without a donor, %ld storage slot(s), "
             "%ld unhappy citizen(s)",
             n->name, n->added, n->fromTmpl, n->replaced, n->skippedFull,
             n->noDonor, n->storages, n->punished);
    }
    if (g_overCap)
        Logf("needs    %ld citizen(s) carried more demands than the array holds", g_overCap);
}

static void h_PersonPlan(void* game, void* person)
{
    // The same repair from the other side, and also before the original: a
    // citizen can be given the demand here and reach a shop before any shop
    // tick this plugin sees.
    if (g_enabled && game)
    {
        __try { FixOtherGoodsBucket((BYTE*)game); }
        __except (FaultFilter("needs goods bucket", GetExceptionInformation())) {}
    }

    o_PersonPlan(game, person);
    if (!g_enabled || !g_doDemand || !person) return;

    if (!g_planThread)
    {
        g_planThread = GetCurrentThreadId();
        Logf("needs    citizen planner runs on thread %lu", g_planThread);
    }

    __try
    {
        ExtendDemands((BYTE*)person);
        LogProgress();
    }
    __except (FaultFilter("needs demand", GetExceptionInformation()))
    {
        Logf("needs    citizen demands disabled after a fault");
        g_doDemand = 0;
    }
}

// ---------------------------------------------------------------- setup

// [list] holds the content and [needs] the switches, in one file, the way
// resources.ini and deposits.ini do. Parsed by hand rather than through the
// profile API because the API cannot enumerate a section's keys.
// The `$STORAGE_DEMAND_*` family by the names the ini uses, with the goods the
// base game puts in each. The goods list is not decoration: it is what makes a
// donor that cannot be sold in the chosen category catch a warning instead of
// quietly stocking shops the citizens will never walk to.
static const struct
{
    const char* name;
    int         token;
    const char* goods;                 // comma-separated, whole-word matched
} kCategories[] = {
    { "auto",           STOR_ANY,             NULL                        },
    { "any",            STOR_ANY,             NULL                        },
    { "none",           STOR_NONE,            NULL                        },
    { "basic",          STOR_DEMAND_BASIC,    "food,meat"                 },
    { "medium",         STOR_DEMAND_MEDIUM,   "food,clothes"              },
    { "advanced",       STOR_DEMAND_ADVANCED, "food,clothes,eletronics"   },
    { "mediumadvanced", STOR_DEMAND_MEDADV,   "clothes,eletronics"        },
    { "hotel",          STOR_DEMAND_HOTEL,    "food,alcohol,meat"         },
    { "plain",          STOR_PLAIN,           NULL                        },
};
#define CATEGORY_COUNT ((int)(sizeof(kCategories) / sizeof(kCategories[0])))

static bool ParseCategory(Need* n, const char* text)
{
    for (int i = 0; i < CATEGORY_COUNT; i++)
        if (_stricmp(text, kCategories[i].name) == 0)
        {
            n->category = kCategories[i].token;
            strncpy_s(n->categoryName, sizeof(n->categoryName), kCategories[i].name, _TRUNCATE);
            return true;
        }

    // A raw token number, for the two the base game barely uses ($STORAGE_
    // DEMAND_PRISON among them) rather than inventing names for them here.
    char* endp = NULL;
    long  v = strtol(text, &endp, 0);
    if (endp && endp != text && !*endp && v >= 0 && v < 64)
    {
        n->category = (int)v;
        _snprintf_s(n->categoryName, sizeof(n->categoryName), _TRUNCATE, "token-%ld", v);
        return true;
    }

    Logf("needs    \"%s\": category \"%s\" is not one of auto, none, basic, medium, advanced, "
         "mediumadvanced, hotel - dropped", n->name, text);
    return false;
}

static void WarnCategoryMismatch(const Need* n)
{
    for (int i = 0; i < CATEGORY_COUNT; i++)
    {
        if (kCategories[i].token != n->category) continue;
        if (!kCategories[i].goods) return;
        if (ListHas(kCategories[i].goods, n->like)) return;

        Logf("needs    WARN  \"%s\" is stocked in %s shops but cloned from the \"%s\" demand, "
             "which those shops do not sell (%s do). The goods will sit where nobody "
             "goes for them", n->name, kCategories[i].name, n->like, kCategories[i].goods);
        return;
    }
}

static void LoadRegistry()
{
    char path[MAX_PATH];
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\plugins\\needs.ini", g_baseDir);

    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
    {
        Logf("needs    no plugins\\needs.ini - nothing declared");
        return;
    }

    char  buf[8192];
    DWORD got = 0;
    ReadFile(h, buf, sizeof(buf) - 1, &got, NULL);
    CloseHandle(h);
    buf[got] = 0;

    bool  inList = false;
    char* ctx = NULL;
    for (char* line = strtok_s(buf, "\n", &ctx); line; line = strtok_s(NULL, "\n", &ctx))
    {
        Trim(line);
        if (!line[0] || line[0] == ';' || line[0] == '#') continue;
        if (line[0] == '[') { inList = _strnicmp(line, "[list]", 6) == 0; continue; }
        if (!inList || g_needCount >= MAX_NEEDS) continue;

        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;

        Need* n = &g_need[g_needCount];
        memset(n, 0, sizeof(*n));
        n->factor      = 1.0f;
        n->category    = STOR_ANY;
        n->chance      = 1.0f;
        n->unhappiness = 0.0f;
        strncpy_s(n->categoryName, sizeof(n->categoryName), "demand", _TRUNCATE);

        Trim(line);
        strncpy_s(n->name, sizeof(n->name), line, _TRUNCATE);

        //  <resource> = <donor>[, <factor>[, <category>[, <chance>[, <unhappiness>]]]]
        char* field[5] = { NULL, NULL, NULL, NULL, NULL };
        field[0] = eq + 1;
        for (int f = 1; f < 5; f++)
        {
            char* c = strchr(field[f - 1], ',');
            if (!c) break;
            *c = 0;
            field[f] = c + 1;
        }
        for (int f = 0; f < 5; f++) if (field[f]) Trim(field[f]);

        strncpy_s(n->like, sizeof(n->like), field[0], _TRUNCATE);

        if (field[1] && field[1][0] && atof(field[1]) > 0.0)
            n->factor = (float)atof(field[1]);
        if (field[2] && field[2][0] && !ParseCategory(n, field[2]))
            continue;
        if (field[3] && field[3][0])
        {
            n->chance = (float)atof(field[3]);
            if (n->chance <= 0.0f) n->chance = 0.0f;
            if (n->chance >  1.0f) n->chance = 1.0f;
        }
        if (field[4] && field[4][0])
        {
            n->unhappiness = (float)atof(field[4]);
            if (n->unhappiness < 0.0f) n->unhappiness = 0.0f;
            if (n->unhappiness > 1.0f) n->unhappiness = 1.0f;
        }

        if (!n->name[0] || !n->like[0]) continue;
        if (strcmp(n->name, n->like) == 0)
        {
            Logf("needs    \"%s\" cannot be cloned from itself - dropped", n->name);
            continue;
        }
        WarnCategoryMismatch(n);
        g_needCount++;
    }
}

static void ReadSettings()
{
    const char* ini = "plugins\\needs.ini";
    char v[64];

    g_enabled   = H->configInt(ini, "needs", "enabled",  g_enabled);
    g_doDemand  = H->configInt(ini, "needs", "demand",   g_doDemand);
    g_doStorage = H->configInt(ini, "needs", "storage",  g_doStorage);
    g_probe     = H->configInt(ini, "needs", "probe",    g_probe);

    g_maxDemands = H->configInt(ini, "needs", "max_demands", g_maxDemands);
    if (g_maxDemands < 1)             g_maxDemands = 1;
    if (g_maxDemands > PR_DEMAND_CAP) g_maxDemands = PR_DEMAND_CAP;

    if (H->configString(ini, "needs", "when_full", v, sizeof(v), "skip") && v[0])
        g_replace = (_stricmp(v, "replace") == 0);

    if (H->configString(ini, "needs", "log_seconds", v, sizeof(v), "") && v[0])
        g_logSeconds = (float)atof(v);
}

extern "C" __declspec(dllexport) unsigned TsmPluginApiVersion(void)
{
    return TSM_API_VERSION;
}

extern "C" __declspec(dllexport) int TsmPluginInit(const TsmHost* host, TsmPluginInfo* info)
{
    TsmBind(host);
    info->name    = "needs";
    info->version = "1.0";

    ReadSettings();
    if (!g_enabled)
    {
        Logf("needs    enabled = 0 - citizens want only what the base game gives them");
        return 1;
    }

    LoadRegistry();
    if (g_needCount == 0)
    {
        Logf("needs    nothing in [list] - not hooking");
        return 1;
    }

    // Nothing consumed and nothing published, so there is no reason to wait for
    // Start - but the records these names refer to may be another plugin's, and
    // they do not exist until the engine asks for them, so every resolution is
    // lazy and none of it happens here.
    return 0;
}

extern "C" __declspec(dllexport) int TsmPluginStart(void)
{
    if (!g_enabled) return 1;

    o_SlotPush     = (t_SlotPush)(g_exeBase + P_SLOT_PUSH_RVA);

    // The citizen's daily plan. Post-hook: the game rebuilds the whole list, we
    // append to what it settled on.
    static const BYTE kPersonPlanPrologue[] = {
        0x48, 0x8B, 0xC4,                            // mov  rax,rsp
        0x48, 0x89, 0x48, 0x08,                      // mov  [rax+8],rcx
        0x53,                                        // push rbx
        0x56,                                        // push rsi
        0x41, 0x55,                                  // push r13
        0x41, 0x56,                                  // push r14
        0x48, 0x81, 0xEC, 0xF8, 0x00, 0x00, 0x00     // sub  rsp,0xF8
    };
    if (g_doDemand &&
        !InstallInlineHook(g_exeBase + P_PERSON_PLAN_RVA, (void*)h_PersonPlan,
                           (void**)&o_PersonPlan, kPersonPlanPrologue,
                           sizeof(kPersonPlanPrologue), "citizen demand plan"))
        g_doDemand = 0;

    // The storage builder. Runs while building.ini is parsed, so it is over
    // long before any citizen exists.
    static const BYTE kStorageBuildPrologue[] = {
        0x40, 0x55,                                  // push rbp
        0x53,                                        // push rbx
        0x56,                                        // push rsi
        0x57,                                        // push rdi
        0x41, 0x57,                                  // push r15
        0x48, 0x8D, 0x6C, 0x24, 0xD1,                // lea  rbp,[rsp-0x2F]
        0x48, 0x81, 0xEC, 0xA0, 0x00, 0x00, 0x00     // sub  rsp,0xA0
    };
    if (g_doStorage &&
        !InstallInlineHook(g_exeBase + P_STORAGE_BUILD_RVA, (void*)h_StorageBuild,
                           (void**)&o_StorageBuild, kStorageBuildPrologue,
                           sizeof(kStorageBuildPrologue), "storage slot list"))
        g_doStorage = 0;

    // The shop tick, for the buildings that already exist - a save made before
    // the need was declared has shops the type description can no longer reach.
    static const BYTE kShopTickPrologue[] = {
        0x48, 0x8B, 0xC4,                            // mov  rax,rsp
        0x55,                                        // push rbp
        0x53,                                        // push rbx
        0x57,                                        // push rdi
        0x41, 0x55,                                  // push r13
        0x41, 0x56,                                  // push r14
        0x48, 0x8D, 0x68, 0x88,                      // lea  rbp,[rax-0x78]
        0x48, 0x81, 0xEC, 0x50, 0x01, 0x00, 0x00     // sub  rsp,0x150
    };
    if (g_doStorage &&
        !InstallInlineHook(g_exeBase + P_SHOP_TICK_RVA, (void*)h_ShopTick,
                           (void**)&o_ShopTick, kShopTickPrologue,
                           sizeof(kShopTickPrologue), "shop tick"))
        Logf("needs    no shop-tick hook - only shops built after this load get the goods");

    // The building-type loader, so the build-menu preview is right before the
    // first shop of a type exists. Cosmetic on its own - the shop tick still
    // stocks whatever gets built - so a refusal here costs the preview only.
    static const BYTE kTypeLoadPrologue[] = {
        0x40, 0x55,                                        // push rbp
        0x53,                                              // push rbx
        0x57,                                              // push rdi
        0x41, 0x55,                                        // push r13
        0x41, 0x56,                                        // push r14
        0x48, 0x8D, 0xAC, 0x24, 0x00, 0xF3, 0xFF, 0xFF,    // lea  rbp,[rsp-0xD00]
        0x48, 0x81, 0xEC, 0x00, 0x0E, 0x00, 0x00           // sub  rsp,0xE00
    };
    if (g_doStorage &&
        !InstallInlineHook(g_exeBase + P_TYPE_LOAD_RVA, (void*)h_TypeLoad,
                           (void**)&o_TypeLoad, kTypeLoadPrologue,
                           sizeof(kTypeLoadPrologue), "building type loader"))
        Logf("needs    no type-loader hook - the preview fills in once a shop is built");

    if (!g_doDemand && !g_doStorage)
    {
        Logf("needs    nothing hooked - inactive");
        return 1;
    }

    for (int i = 0; i < g_needCount; i++)
        Logf("needs    %-18s like \"%s\" x%.2f, %s storages, chance %.2f, "
             "unhappiness %.3f", g_need[i].name, g_need[i].like, g_need[i].factor,
             g_need[i].categoryName, g_need[i].chance, g_need[i].unhappiness);
    Logf("needs    %d need(s), demand %s, storage %s, at most %d demand(s) per citizen (%s when full)",
         g_needCount, g_doDemand ? "on" : "off", g_doStorage ? "on" : "off",
         g_maxDemands, g_replace ? "replace" : "skip");
    return 0;
}

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) { return TRUE; }
