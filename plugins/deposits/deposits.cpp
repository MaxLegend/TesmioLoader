// deposits - deposit types the base game does not have, as a tesmioloader
// plugin, and the three subsystems one declaration drives.
//
// A deposit really is little more than a (texture, colour component) pair: the
// game stores richness as one byte of one channel of one of two 1024x1024 maps,
// and every per-type behaviour it has is a lookup keyed by the type number.
// deposits.ini declares them and this one table feeds all three:
//
//   the code patch   splices the type into the building.ini parser, the
//                    type-to-channel dispatch and the search-radius table. The
//                    only place in this project that emits instructions, and it
//                    emits them in a loop over the registry.
//   the minimap      a button and an overlay layer per deposit, from two
//                    additive inline hooks. No code patch: the overlay shader
//                    selects its channel with a full dp4, so all four
//                    components were reachable from the start.
//   the editor       a paint/erase pair per deposit, from four more additive
//                    hooks. Also no code patch: the engine's brush primitive is
//                    generic over an eight-value channel index and three of the
//                    eight simply had no caller.
//
// The registry is also published as a service, so another plugin can read what
// deposits.ini declared - and carry its own per-deposit keys in the same file.
// `depletion` does both.
//
// Everything here is addresses for SOVIET64.exe v1.1.1.7. See
// docs/05-deposits.md.

#include "../../src/tesmio_plugin.h"

// C3D_MIDDLEPOINT, the object every managed asset is created through. Both the
// resource table at 0x2A1D60 and the editor's button drawer pass this same
// address.
#define P_MIDDLEPOINT   0x9EACD0

// ResourceGet, for the one thing here that needs it: the minimap button takes
// its icon straight out of a resource record. Called at the patched entry point
// rather than through the resources plugin, which is what makes a mod resource
// name resolve without either plugin knowing about the other - if `resources`
// is installed its hook is on that address, and if it is not, the base game's
// own lookup answers and a mod icon simply has no record.
#define P_RESOURCEGET   0x2AA7C0
typedef unsigned __int64 (*t_ResourceGet)(void*, void*, void*, void*);

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
#define DEPOSIT_EXTRAS 8    // plugin-owned keys kept per section

#define DEP_MAP_1  0    // resourcemap,  gameobj+0xF00
#define DEP_MAP_2  1    // resourcemap2, gameobj+0xF08
#define DEP_MAP_TERRAIN 2  // the terrain's own mask, terrain+0x158 - gravel's, and
                           // reachable only from the depletion table: the spliced
                           // dispatch chain has no case that loads it.

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

    // Anything in the section the loader itself has no use for, kept verbatim
    // so a plugin can carry its own per-deposit settings in the same file. The
    // loader has no opinion on what these mean and does not warn about them.
    char  extraKey[DEPOSIT_EXTRAS][32];
    char  extraVal[DEPOSIT_EXTRAS][64];
    int   extraCount;

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
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\plugins\\deposits.ini", g_baseDir);

    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
    {
        Logf("deposits  no plugins\\deposits.ini - no mod deposit types");
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

            // [deposits] is the plugin's own settings; every other section is a
            // deposit. They share a file so a feature is one file, and the name
            // of the plugin is the one name a deposit may not have.
            if (_stricmp(line + 1, "deposits") == 0) continue;

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
        else if (d->extraCount < DEPOSIT_EXTRAS)
        {
            // Not an error. A key the loader does not use belongs to whichever
            // plugin asked for it, and it reaches that plugin through
            // TsmHost::depositSetting.
            strncpy_s(d->extraKey[d->extraCount], 32, line, _TRUNCATE);
            strncpy_s(d->extraVal[d->extraCount], 64, val,  _TRUNCATE);
            d->extraCount++;
        }
        else Logf("deposits  \"%s\": no room for \"%s\" - %d plugin keys per section",
                  d->name, line, DEPOSIT_EXTRAS);
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
        t_ResourceGet resourceGet = (t_ResourceGet)(g_exeBase + P_RESOURCEGET);
        BYTE* record = (BYTE*)resourceGet(g_exeBase + G_RES_SELF, (void*)d->icon, NULL, NULL);
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

// ---------------------------------------------------------------- the plugin

// The registry, published for anything else that has to know what deposits.ini
// declared. `setting` hands back the keys this plugin has no use for, which is
// how another plugin carries its own per-deposit settings in the same file
// without either having to know about the other.
static int svc_Count(void) { return g_depCount; }

static int svc_Get(int i, TsmDeposit* out)
{
    if (i < 0 || i >= g_depCount || !out) return 0;
    const DepositDef* d = &g_dep[i];
    out->name         = d->name;
    out->token        = d->token;
    out->type         = d->type;
    out->buildingType = d->buildingType;
    out->map          = d->map == DEP_MAP_2 ? TSM_MAP_RESOURCEMAP2 : TSM_MAP_RESOURCEMAP;
    out->component    = d->component;
    out->radius       = d->radiusRva ? *(float*)(g_exeBase + d->radiusRva) : d->radiusValue;
    out->icon         = d->icon;
    return 1;
}

static const char* svc_Setting(int i, const char* key)
{
    if (i < 0 || i >= g_depCount || !key) return NULL;
    const DepositDef* d = &g_dep[i];
    for (int k = 0; k < d->extraCount; k++)
        if (_stricmp(d->extraKey[k], key) == 0) return d->extraVal[k];
    return NULL;
}

static const TsmDepositApi kDepositApi = { svc_Count, svc_Get, svc_Setting };

extern "C" __declspec(dllexport) unsigned TsmPluginApiVersion(void)
{
    return TSM_API_VERSION;
}

extern "C" __declspec(dllexport) int TsmPluginInit(const TsmHost* host, TsmPluginInfo* info)
{
    TsmBind(host);
    info->name    = "deposits";
    info->version = "1.0";

    const char* ini = "plugins\\deposits.ini";
    g_depositPatch = H->configInt(ini, "deposits", "code_patch", g_depositPatch);
    g_minimapPatch = H->configInt(ini, "deposits", "minimap",    g_minimapPatch);
    g_editorPatch  = H->configInt(ini, "deposits", "editor",     g_editorPatch);

    // The registry is read whatever the three switches say: another plugin may
    // want to know what was declared even when none of the subsystems here is
    // allowed to touch the game.
    LoadDepositRegistry();
    ValidateDeposits();

    if (g_depositPatch) PatchDepositType();
    if (g_minimapPatch) InstallMinimapPatch();
    if (g_editorPatch)  InstallEditorPatch();

    H->provide(TSM_SERVICE_DEPOSITS, TSM_DEPOSITS_VERSION, &kDepositApi);

    // Stays loaded even with nothing declared and every switch off: the service
    // is published, and a consumer asking for an empty registry is a valid
    // answer. Unloading would take the service with it.
    return 0;
}

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) { return TRUE; }
