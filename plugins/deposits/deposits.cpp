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

#define MAX_DEPOSITS 32
#define DEPOSIT_EXTRAS 8    // plugin-owned keys kept per section

// Maps, by index: 0 and 1 are the engine's own, everything above is one this
// plugin creates, loads and saves itself. `resourcemap<index+1>.dds`.
//
// Eight channels was never a property of the deposit system - it is two
// textures times four components, and the only thing that made two textures a
// limit is that the world loader creates exactly two. Nothing downstream cares:
// the sampler at 0x8360 takes a texture pointer, the texel writer at 0x238B00
// takes one out of the game object, and both are reachable. So a ninth deposit
// gets a ninth channel out of a third map rather than an apology.
#define DEP_MAP_1     0     // resourcemap,  gameobj+0xF00
#define DEP_MAP_2     1     // resourcemap2, gameobj+0xF08
#define DEP_MAP_EXTRA 2     // resourcemap3 and up - ours

#define MAX_MAPS       10                    // resourcemap .. resourcemap10
#define MAX_EXTRA_MAPS (MAX_MAPS - DEP_MAP_EXTRA)
#define DEP_MAP_AUTO   (-2)                  // `map = auto`, resolved at validate time

// The terrain's own material mask at terrain+0x158 - the splat map the ground
// textures blend through, and the one place gravel lives. Numbered outside the
// resource maps because it is not one: it is painted by C3D_TERRAIN::EditMask
// rather than by the deposit brush, it has to be bracketed before it can be
// sampled, and a deposit on it visibly wears the ground away as it is mined.
#define DEP_MAP_TERRAIN 64

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

    // The minimap icon's resource record, resolved once rather than once per
    // frame. iconFailed latches the first miss: the resource set is fixed for
    // the session, so a name that found nothing once would find nothing on
    // every later frame too - and each of those misses costs a game.ERROR line
    // in the game's own log, which is what bled frame rate with the minimap open.
    BYTE* iconRecord;
    int   iconFailed;
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
            // "resourcemap", "resourcemap2", ... "resourcemapN", the bare
            // number, or "auto" to let the plugin pick a free channel. The
            // stored value is always the digit minus one, so `resourcemap` and
            // `1` are the same thing and the two engine maps keep the numbers
            // every existing deposits.ini already uses.
            const char* digits = val;
            if (_strnicmp(val, "resourcemap", 11) == 0) digits = val + 11;

            if (KeyIs(val, "terrain") || KeyIs(val, "mask")) d->map = DEP_MAP_TERRAIN;
            else if (KeyIs(val, "auto"))     d->map = DEP_MAP_AUTO;
            else if (!digits[0])             d->map = DEP_MAP_1;    // plain "resourcemap"
            else if (digits[0] >= '1' && digits[0] <= '9')
                                             d->map = (int)strtol(digits, NULL, 10) - 1;
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

// How many maps past the engine's two are actually needed. 0 means nothing here
// creates a texture and the plugin behaves exactly as it did before.
static int g_extraCount;

// How many deposits live in the terrain's material mask. 0 means the two extra
// patch sites that bracket that texture are never touched.
static int g_terrainCount;

// How many live on a resource map (the engine's two or an extra one). 0 means
// the scan's TextureAccess bracket needs no new cases either.
static int g_mapCount;

// "resourcemap", "resourcemap2", "resourcemap3"... for the log and for paths.
static const char* MapName(int map, char* buf, size_t n)
{
    if (map == DEP_MAP_TERRAIN) { strncpy_s(buf, n, "terrain mask", _TRUNCATE); return buf; }
    if (map == DEP_MAP_1)       { strncpy_s(buf, n, "resourcemap",  _TRUNCATE); return buf; }
    _snprintf_s(buf, n, _TRUNCATE, "resourcemap%d", map + 1);
    return buf;
}

// Whether any kept deposit already owns this channel. Used both by the
// duplicate check and by auto-allocation, so the two can never disagree.
static bool ChannelTaken(int upto, int map, int component)
{
    for (int j = 0; j < upto; j++)
        if (g_dep[j].map == map && g_dep[j].component == component) return true;
    return false;
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
        bool  autoChannel = (d->map == DEP_MAP_AUTO);

        if (!d->token[0])
            bad = "no token";
        // 0..9 belong to the game. The upper bound is the encoding's, not a
        // policy: every compare this patches is CMP r/m32,imm8, sign-extended.
        else if (d->type < 10 || d->type > 127)
            bad = "type must be 10..127 (0..9 are the game's own, and the compare takes an imm8)";
        else if (!autoChannel && d->map != DEP_MAP_TERRAIN && (d->map < 0 || d->map >= MAX_MAPS))
            bad = "map must be resourcemap..resourcemap10, terrain, or auto";
        else if (!autoChannel && (d->component < 0 || d->component > 3))
            bad = "component must be 0..3";
        else if (!d->radiusRva && d->radiusValue <= 0.0f)
            bad = "no usable radius";

        for (int j = 0; !bad && j < kept; j++)
        {
            if      (g_dep[j].type == d->type)             bad = "duplicate type";
            else if (strcmp(g_dep[j].token, d->token) == 0) bad = "duplicate token";
        }
        if (!bad && !autoChannel && ChannelTaken(kept, d->map, d->component))
            bad = "duplicate channel";

        if (bad)
        {
            Logf("deposits  \"%s\" rejected: %s", d->name, bad);
            continue;
        }

        if (i != kept) g_dep[kept] = *d;
        kept++;
    }
    g_depCount = kept;

    // Auto channels, once every explicit one is known - which is the whole
    // reason this is a second pass. Allocation starts at the first map this
    // plugin owns rather than at the two free channels in the engine's own
    // pair: `resourcemap` component 3 reads as 255 everywhere on half the
    // shipped maps, and handing a deposit a channel that says "maximum richness
    // on every texel" by default is not a sensible thing to do silently. Those
    // two remain available by naming them.
    for (int i = 0; i < g_depCount; i++)
    {
        DepositDef* d = &g_dep[i];
        if (d->map != DEP_MAP_AUTO) continue;

        d->map = -1;
        for (int m = DEP_MAP_EXTRA; m < MAX_MAPS && d->map < 0; m++)
            for (int c = 0; c < 4; c++)
                if (!ChannelTaken(g_depCount, m, c))
                {
                    d->map = m; d->component = c;
                    break;
                }

        if (d->map < 0)
        {
            Logf("deposits  \"%s\" rejected: no free channel left in %d maps", d->name, MAX_MAPS);
            for (int k = i; k + 1 < g_depCount; k++) g_dep[k] = g_dep[k + 1];
            g_depCount--; i--;
        }
    }

    for (int i = 0; i < g_depCount; i++)
    {
        DepositDef* d = &g_dep[i];

        // Sharing a channel with a base-game deposit is legitimate - a second
        // mine type reading iron's ore, say - but it is far more often a typo,
        // and the symptom is a mine that finds someone else's deposit.
        static const struct { int map; int comp; const char* who; } kTaken[] = {
            { DEP_MAP_1, 0, "oil"     }, { DEP_MAP_1, 1, "iron"    },
            { DEP_MAP_1, 2, "coal"    }, { DEP_MAP_2, 0, "uranium" },
            { DEP_MAP_2, 1, "bauxite" },
            // Component 2 of the mask is gravel's, and it is also the channel
            // the editor's own rock brush paints - so a deposit put there is
            // mined out of the rock the player painted, which may well be what
            // was wanted. Component 3 is the oasis brush's.
            { DEP_MAP_TERRAIN, 2, "gravel (and the rock brush)" },
            { DEP_MAP_TERRAIN, 3, "the oasis brush" },
        };
        for (size_t k = 0; k < sizeof(kTaken) / sizeof(kTaken[0]); k++)
            if (kTaken[k].map == d->map && kTaken[k].comp == d->component)
                Logf("deposits  \"%s\" WARN  shares a channel with %s - both read the same bytes",
                     d->name, kTaken[k].who);

        // The eight-value index the texel writer at 0x238B00 takes. It decodes
        // its argument as tex = (ch - 4) < 4 ? resourcemap2 : resourcemap and
        // component = (ch + 3) & 3, so this is that mapping inverted. It agrees
        // with all six channels the base game reaches.
        //
        // An extra map has no index of its own, because the writer picks its
        // texture out of the game object and knows only those two slots. It gets
        // resourcemap2's, and the brush hook swaps the pointer in that slot for
        // the length of the call - see h_ED_PaintTexels.
        //
        // A terrain-mask deposit is not painted by that writer at all. It goes
        // through C3D_TERRAIN::EditMask, whose channel argument turns out to use
        // the identical `(component + 1) & 3` encoding - rock is channel 3 and
        // component 2, oasis is channel 0 and component 3 - so the same field
        // carries both, without the map bit.
        d->editorChannel = d->map == DEP_MAP_TERRAIN
                         ? ((d->component + 1) & 3)
                         : ((d->map != DEP_MAP_1 ? 4 : 0) | ((d->component + 1) & 3));

        for (int c = 0; c < 4; c++) d->vector[c] = (c == d->component) ? 1.0f : 0.0f;

        if (d->map == DEP_MAP_TERRAIN) g_terrainCount++;
        else
        {
            g_mapCount++;
            if (d->map >= DEP_MAP_EXTRA && d->map - DEP_MAP_EXTRA + 1 > g_extraCount)
                g_extraCount = d->map - DEP_MAP_EXTRA + 1;
        }
    }

    // The minimap row and the Resources tab both carry five vanilla entries; the
    // Rocks tab is a grid of its own, and a mask deposit's pair is counted
    // separately because it is drawn by a different panel.
    int mmSlot = 5, edCol = 5, rockCol = 0;
    for (int i = 0; i < g_depCount; i++)
    {
        DepositDef* d = &g_dep[i];
        char mapName[32];
        d->minimapSlot  = d->wantMinimap ? mmSlot++ : -1;
        d->editorColumn = !d->editor[0] ? -1
                        : d->map == DEP_MAP_TERRAIN ? rockCol++ : edCol++;

        Logf("deposits  \"%s\" type %d \"%s\" -> %s component %d, radius %s, "
             "editor channel %d (slot %d, column %d)",
             d->name, d->type, d->token,
             MapName(d->map, mapName, sizeof(mapName)), d->component,
             d->radiusRva ? "from the game" : "fixed",
             d->editorChannel, d->minimapSlot, d->editorColumn);
    }

    if (g_extraCount)
        Logf("deposits  %d map(s) past the engine's two: resourcemap3..resourcemap%d",
             g_extraCount, DEP_MAP_EXTRA + g_extraCount);
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

// The terrain's own material mask, and the two sites that bracket it.
//
// Gravel's dispatch is not in the chain at 0x1DD773 at all. It is three places
// in the same function: an open before the scan, the sample inside the loop, and
// a close after it. Only the sample can be expressed as a case in the chain -
// its block writes the same [rsp+0x5C] and its position is the same value -
// which leaves the bracket, and **the bracket is not optional**: the sampler
// reads a CPU-side copy of the texture, and the mask is one the editor writes.
//
//   1DD499  83 BE 68 03 00 00 03   CMP [RSI+0x368],3
//   1DD4A0  75 14                  JNZ 1DD4B6
//   1DD4A2  48 8B 0D ..            MOV RCX,[gameobj]
//   1DD4A9  48 8B 89 D8 0E 00 00   MOV RCX,[RCX+0xED8]
//   1DD4B0  FF 15 ..               CALL [MaskTextureOpen]
//
// and 1DDE08 is the same five instructions with MaskTextureClose. Opening it
// per sample point instead would be a GPU Map/Unmap per point - the mistake
// docs/07-pitfalls.md already records - so both are patched, or neither.
#define P_MASK_OPEN_SITE   0x1DD499
#define P_MASK_OPEN_NEXT   0x1DD4B6   // where its JNZ lands
#define P_MASK_CLOSE_SITE  0x1DDE08
#define P_MASK_CLOSE_NEXT  0x1DDE25
#define P_MASK_OPEN_IAT    0x86CF38   // C3D_TERRAIN::MaskTextureOpen
#define P_MASK_CLOSE_IAT   0x86CF30   // C3D_TERRAIN::MaskTextureClose
#define P_TERRAIN_OFF      0xED8      // gameobj -> C3D_TERRAIN
#define P_TERRAIN_MASK     0x158      // C3D_TERRAIN -> its material mask texture

// The resource maps need the same bracket, and every mod type was missing it.
//
// The scan brackets the texture it is about to sample by deposit type: types
// 0-2 open resourcemap, types 6-7 open resourcemap2, type 3 opens the terrain
// mask (the sites above), and each is closed after the loop. A mod type -
// 10 and up - matches none of the checks, so the sampler's GetTexel read
// [tex+0x158], the mapped-texel pointer, while it was still zero: crash at a
// fault address of (row pitch/4 * y + x) * 4. What masked the defect for a
// painted map is TextureAccessClose never clearing that pointer: one editor
// paint of the channel leaves a stale, still-dereferenceable address behind,
// and the scan then reads last paint's staging copy and happens to be right.
//
//   1DD458  83 F8 02               CMP EAX,2            <- open, types 0-2
//   1DD45B  77 17                  JA  1DD474
//   1DD45D  48 8B 05 ..            MOV RAX,[gameobj]
//   1DD464  48 8B 88 00 0F 00 00   MOV RCX,[RAX+0xF00]
//   1DD46B  48 8B 01               MOV RAX,[RCX]
//   1DD46E  FF 90 80 00 00 00      CALL [RAX+0x80]     ; TextureAccessOpen
//
//   1DDE25  83 BE 68 03 00 00 02   CMP [RSI+0x368],2    <- close, types 0-2
//   1DDE2C  77 17                  JA  1DDE45
//   ...                            ... same, [RAX+0x90] ; TextureAccessClose
//
// Both sites are heads of their block and reached by fall-through only. The
// cave puts our types in front of the displaced check, exactly as the mask
// bracket does; the rejoin re-reads the type, so a clobbered EAX costs nothing.
#define P_MAP_OPEN_SITE    0x1DD458
#define P_MAP_OPEN_NEXT    0x1DD474   // the types 6-7 check, which re-reads EAX
#define P_MAP_CLOSE_SITE   0x1DDE25
#define P_MAP_CLOSE_NEXT   0x1DDE45   // the types 6-7 close check
#define P_TEX_OPEN         0x80       // TextureAccessOpen,  vtable slot 16
#define P_TEX_CLOSE        0x90       // TextureAccessClose, vtable slot 18

static const BYTE kParserOrig[]   = { 0x48, 0x8D, 0x15, 0xF1, 0xAA, 0x77, 0x00 };
static const BYTE kDispatchOrig[] = { 0x83, 0xBE, 0x68, 0x03, 0x00, 0x00, 0x06 };
static const BYTE kRadiusOrig[]   = { 0x83, 0xF9, 0x09, 0x75, 0x09 };
static const BYTE kMaskOrig[]     = { 0x83, 0xBE, 0x68, 0x03, 0x00, 0x00, 0x03 };  // both bracket sites
// Verified past the bytes the jump replaces, so the check pins the site, not
// just five common encodings: the MOV that follows carries the build's own
// rip-relative displacement to the game object.
static const BYTE kMapOpenOrig[]  = { 0x83, 0xF8, 0x02, 0x77, 0x17,
                                      0x48, 0x8B, 0x05, 0x8C, 0x6D, 0x7B, 0x00 };
static const BYTE kMapCloseOrig[] = { 0x83, 0xBE, 0x68, 0x03, 0x00, 0x00, 0x02, 0x77, 0x17,
                                      0x48, 0x8B, 0x05, 0xBB, 0x63, 0x7B, 0x00 };

// ---------------------------------------------------------------- maps past the engine's two
//
// The eight channels were never a property of the deposit system. They are two
// textures times four components, and the only thing that made two textures a
// limit is that the world loader creates exactly two of them. Everything
// downstream is already generic over which texture it is handed:
//
//   the sampler at 0x8360   takes a texture pointer as its fourth argument
//   the texel writer 0x238B00 reads one out of the game object, at a fixed offset
//   the overlay shader      binds whatever texture was put in stage 0
//
// So a third map needs no new engine machinery at all - it needs the three
// things the world loader does for the first two, done again:
//
//   140007B1A  CreateManagedTexture(middlepoint, "<folder>/resourcemap.dds")
//   140007B4E  tex->vtbl[2] (tex, path, 0, 0, 0, 0)      Load2DFromFile
//   140007B5C  tex->vtbl[19](tex)                        TextureAccessInitTempResource
//
// and the one thing the world saver does, at 0x7C20:
//
//   140007C6x  tex->vtbl[36](tex, "<folder>/resourcemap.dds")   SaveToDDS
//
// Neither needs a spliced prologue. The load side is an **import swap** on
// CreateManagedTexture: the world loader hands it "<folder>/resourcemap.dds",
// which is the one path that always carries the folder - `resourcemap2` falls
// back to the bare "resourcemap2default.dds" when a map ships without one - so
// seeing that path is both the signal that a world is loading and the name of
// the folder it is loading from.
//
// The save side redirects the **one call** that reaches 0x7C20, at 0x42CD0E. It
// was an inline hook on 0x7C20's prologue until that turned out to crash every
// save, and the call patch is both the fix and the better technique:
//
//   0042CD0B  49 8B D7           mov rdx,r15        the world folder
//   0042CD0E  E8 0D AF BD FF     call 0x7C20        <- the five bytes rewritten
//   0042CD13  48 8B 0D ...                          the return address
//
// 0x7C20's prologue is exactly 13 bytes, and byte 13 begins a **rip-relative**
// load of the stack cookie - `mov rax,[rip+0x98a414]`. So it can neither be
// stolen at 13 (a 14-byte jump overwrites that instruction's first byte, and
// the trampoline returns straight into the wreckage) nor at 20 (the copy would
// read the cookie through a displacement measured from the original address).
// A call site has neither problem: the argument registers are already loaded,
// the original is called by address with no trampoline at all, and five bytes
// of rel32 are the whole patch. See docs/07-pitfalls.md.
//
// The rel32 only reaches +-2GB, so it lands in a cave allocated next to the
// executable which holds one absolute jump to the detour.
//
// A map with no resourcemapN.dds of its own loads a **blank**, written once into
// the loader's own VFS. It cannot be the engine's `resourcemap2default.dds`:
// that file is not blank - components 0, 1 and 2 carry a stock uranium and
// bauxite layout, measured, and only its alpha is clear - and CreateManagedTexture
// caches by path, so two extra maps loading one file would be one texture.

#define P_WORLD_SAVE_RVA   0x7C20     // FUN_140007c20(self, folder) - SaveToDDS x4
#define P_WORLD_SAVE_CALL  0x42CD0E   // the only `call 0x7C20` in the executable

#define TEX_LOAD2D         2          // vtbl+0x10   Load2DFromFile(path,0,0,0,0)
#define TEX_INIT_TEMP      19         // vtbl+0x98   TextureAccessInitTempResource
#define TEX_SAVE_DDS       36         // vtbl+0x120  SaveToDDS(path)

#define MAP_EDGE           1024       // both engine maps, and therefore ours
#define MAP_BLANK_REL      "tesmio/resourcemap_blank.dds"

// The blank's header, byte for byte the one media_soviet/resourcemap2default.dds
// carries: 1024x1024, uncompressed 32-bit, R at byte 0, no mip chain. Embedded
// rather than copied from that file at runtime so nothing here depends on a game
// asset being where it is expected to be.
static const BYTE kBlankDdsHeader[128] = {
    0x44,0x44,0x53,0x20, 0x7C,0x00,0x00,0x00, 0x0F,0x10,0x02,0x00, 0x00,0x04,0x00,0x00,
    0x00,0x04,0x00,0x00, 0x00,0x10,0x00,0x00, 0x00,0x00,0x00,0x00, 0x01,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x20,0x00,0x00,0x00,
    0x41,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x20,0x00,0x00,0x00, 0xFF,0x00,0x00,0x00,
    0x00,0xFF,0x00,0x00, 0x00,0x00,0xFF,0x00, 0x00,0x00,0x00,0xFF, 0x00,0x10,0x00,0x00,
    0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
};

struct ExtraMap
{
    void*  tex;        // the live C3DAPI_TEXTURE, rebuilt with every world
    void** caveSlot;   // where the spliced dispatch reads it from; NULL if unpatched
};
static ExtraMap g_extra[MAX_EXTRA_MAPS];

static char g_extraFolder[MAX_PATH];   // the world folder the maps were loaded from
static bool g_inExtraLoad;             // guards the CreateManagedTexture re-entry

typedef void* (__fastcall* t_CreateManagedTexture)(void*, const char*);
typedef bool  (__cdecl*    t_FileExists)(const char*, bool, bool);
typedef void  (*t_WorldSaveMaps)(void*, const char*);

static t_CreateManagedTexture o_CreateManagedTexture;
static t_FileExists           o_FileExists;
static t_WorldSaveMaps        o_WorldSaveMaps;

// Rewrites the rel32 of the one call that reaches the world map saver so it
// lands in a cave holding an absolute jump to `detour`. Verified by decoding the
// call rather than by comparing its bytes: what has to be true is that the site
// is a direct call *to 0x7C20*, and that survives the function moving.
static bool PatchWorldSaveCall(void* detour)
{
    BYTE* site   = g_exeBase + P_WORLD_SAVE_CALL;
    BYTE* target = g_exeBase + P_WORLD_SAVE_RVA;

    if (!ReadablePtr(site, 5)) { Logf("maps     save call site unreadable"); return false; }

    int rel = 0;
    memcpy(&rel, site + 1, 4);
    if (site[0] != 0xE8 || site + 5 + rel != target)
    {
        Logf("maps     save call site is not `call %p` - wrong game build, refusing to patch",
             target);
        return false;
    }

    BYTE* cave = AllocNear(g_exeBase, 16);
    if (!cave) { Logf("maps     no cave within reach of the save call site"); return false; }

    cave[0] = 0xFF; cave[1] = 0x25;                  // jmp qword ptr [rip+0]
    memset(cave + 2, 0, 4);
    memcpy(cave + 6, &detour, sizeof(detour));

    int newRel = (int)(cave - (site + 5));
    if (cave - (site + 5) != (INT_PTR)newRel)
    {
        Logf("maps     cave at %p is out of rel32 range of the save call site", cave);
        return false;
    }

    DWORD prot = 0;
    if (!VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &prot))
    {
        Logf("maps     save call site not writable (%lu)", GetLastError());
        return false;
    }
    memcpy(site + 1, &newRel, 4);
    VirtualProtect(site, 5, prot, &prot);
    FlushInstructionCache(GetCurrentProcess(), site, 5);

    o_WorldSaveMaps = (t_WorldSaveMaps)target;       // called by address, no trampoline
    Logf("patch  world map save  call at %p -> cave %p -> detour %p (original %p)",
         site, cave, detour, target);
    return true;
}

static void* TexVCall(void* tex, int slot)
{
    if (!ReadablePtr(tex, sizeof(void*))) return NULL;
    void** vtbl = *(void***)tex;
    if (!ReadablePtr(vtbl, (size_t)(slot + 1) * sizeof(void*))) return NULL;
    return vtbl[slot];
}

// The texture a deposit's channel lives in, whichever kind of map that is. The
// engine's two are read out of the game object every time rather than cached,
// because they are replaced at every world load; ours are replaced at the same
// moment and by the same event, so they are treated the same way.
static void* DepositMapTexture(const DepositDef* d)
{
    if (d->map == DEP_MAP_TERRAIN)
    {
        BYTE* gameobj = *(BYTE**)(g_exeBase + P_GAMEOBJ);
        if (!ReadablePtr(gameobj, P_TERRAIN_OFF + sizeof(void*))) return NULL;
        BYTE* terrain = *(BYTE**)(gameobj + P_TERRAIN_OFF);
        if (!ReadablePtr(terrain, P_TERRAIN_MASK + sizeof(void*))) return NULL;
        return *(void**)(terrain + P_TERRAIN_MASK);
    }

    if (d->map >= DEP_MAP_EXTRA)
    {
        int k = d->map - DEP_MAP_EXTRA;
        if (k >= MAX_EXTRA_MAPS) return NULL;
        return ReadablePtr(g_extra[k].tex, sizeof(void*)) ? g_extra[k].tex : NULL;
    }

    BYTE* gameobj = *(BYTE**)(g_exeBase + P_GAMEOBJ);
    if (!ReadablePtr(gameobj, P_MAP2_OFF + sizeof(void*))) return NULL;
    return *(void**)(gameobj + (d->map == DEP_MAP_2 ? P_MAP2_OFF : P_MAP1_OFF));
}

// Writes the blank map into the loader's VFS if it is not already there, and
// answers with the media_soviet-relative path the engine will ask for. 4 MB,
// written once and shared by every extra map: CreateManagedTexture caches by the
// name it is *given*, and Load2DFromFile takes its own path argument, so one
// file can back any number of distinct textures.
static const char* BlankMapPath()
{
    static char rel[64];
    static int  state;                    // 0 unknown, 1 ready, -1 failed
    if (state) return state > 0 ? rel : NULL;

    strncpy_s(rel, sizeof(rel), MAP_BLANK_REL, _TRUNCATE);

    char dir[MAX_PATH], file[MAX_PATH];
    _snprintf_s(dir,  sizeof(dir),  _TRUNCATE, "%s\\vfs\\media_soviet", g_baseDir);
    CreateDirectoryA(dir, NULL);
    _snprintf_s(dir,  sizeof(dir),  _TRUNCATE, "%s\\vfs\\media_soviet\\tesmio", g_baseDir);
    CreateDirectoryA(dir, NULL);
    _snprintf_s(file, sizeof(file), _TRUNCATE, "%s\\resourcemap_blank.dds", dir);

    WIN32_FILE_ATTRIBUTE_DATA fad;
    const LONGLONG want = 128 + (LONGLONG)MAP_EDGE * MAP_EDGE * 4;
    if (GetFileAttributesExA(file, GetFileExInfoStandard, &fad) &&
        ((LONGLONG)fad.nFileSizeHigh << 32 | fad.nFileSizeLow) == want)
    {
        state = 1;
        return rel;
    }

    HANDLE h = CreateFileA(file, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
    {
        Logf("maps     could not write %s (%lu) - extra maps have no blank to fall back on",
             file, GetLastError());
        state = -1;
        return NULL;
    }

    DWORD wrote = 0;
    bool  ok = WriteFile(h, kBlankDdsHeader, sizeof(kBlankDdsHeader), &wrote, NULL) != 0;

    static BYTE zero[65536];              // .bss, so it costs nothing and is already zero
    for (LONGLONG left = (LONGLONG)MAP_EDGE * MAP_EDGE * 4; ok && left > 0; left -= sizeof(zero))
    {
        DWORD chunk = (DWORD)(left < (LONGLONG)sizeof(zero) ? left : (LONGLONG)sizeof(zero));
        ok = WriteFile(h, zero, chunk, &wrote, NULL) != 0 && wrote == chunk;
    }
    CloseHandle(h);

    if (!ok) { Logf("maps     failed writing %s", file); state = -1; return NULL; }
    Logf("maps     wrote the blank deposit map to %s", file);
    state = 1;
    return rel;
}

// Everything the world loader does for resourcemap and resourcemap2, once per
// extra map. Runs inside the CreateManagedTexture hook, on whatever thread the
// world loader is using - which is the same thread, and the same moment, the
// engine does its own two on.
// `mp` is the middlepoint the world loader is using, taken straight off the call
// this hook intercepted rather than from the static at P_MIDDLEPOINT. The two
// are almost certainly the same object - the loader passes `*(void**)gameobj` -
// but "almost certainly" would decide which cache the textures live in and
// therefore when they are released, and the right answer was already in the
// argument list.
static void LoadExtraMaps(void* mp, const char* folder)
{
    if (!o_CreateManagedTexture) return;

    strncpy_s(g_extraFolder, sizeof(g_extraFolder), folder, _TRUNCATE);

    for (int k = 0; k < g_extraCount; k++)
    {
        char own[MAX_PATH];
        _snprintf_s(own, sizeof(own), _TRUNCATE, "%s/resourcemap%d.dds",
                    folder, DEP_MAP_EXTRA + k + 1);

        // The map's own file when the world has one - a saved game always will,
        // because the save hook below writes it - and the blank otherwise, which
        // is what a terrain that has never seen this deposit type looks like.
        const char* src = own;
        if (o_FileExists && !o_FileExists(own, false, true))
        {
            src = BlankMapPath();
            if (!src) { g_extra[k].tex = NULL; continue; }
        }

        // Created under its own name whatever it is loaded from, so each map is
        // a distinct texture and each saves back to its own file.
        void* tex = o_CreateManagedTexture(mp, own);
        if (!ReadablePtr(tex, sizeof(void*)))
        {
            Logf("maps     resourcemap%d: CreateManagedTexture failed", DEP_MAP_EXTRA + k + 1);
            g_extra[k].tex = NULL;
            continue;
        }

        typedef void (*t_Load)(void*, const char*, int, int, int, int);
        typedef void (*t_Init)(void*);
        if (void* fn = TexVCall(tex, TEX_LOAD2D))    ((t_Load)fn)(tex, src, 0, 0, 0, 0);
        if (void* fn = TexVCall(tex, TEX_INIT_TEMP)) ((t_Init)fn)(tex);

        g_extra[k].tex = tex;
        if (g_extra[k].caveSlot) *g_extra[k].caveSlot = tex;

        Logf("maps     resourcemap%d = %p (%s)", DEP_MAP_EXTRA + k + 1, tex,
             src == own ? own : "blank");
    }
}

// The one path that always carries the world folder. resourcemap2 is built the
// same way but falls back to a bare "resourcemap2default.dds" on a map that
// ships without one, and the farmap and emissive map are created elsewhere.
static const char* WorldFolderOf(const char* path, char* out, size_t n)
{
    if (!path) return NULL;
    size_t len = strlen(path);
    static const char kTail[] = "/resourcemap.dds";
    const size_t tail = sizeof(kTail) - 1;
    if (len <= tail) return NULL;

    const char* at = path + len - tail;
    if (_stricmp(at, kTail) != 0 && _stricmp(at, "\\resourcemap.dds") != 0) return NULL;

    size_t keep = (size_t)(at - path);
    if (keep >= n) return NULL;
    memcpy(out, path, keep);
    out[keep] = 0;
    return out;
}

static void* __fastcall h_CreateManagedTexture(void* self, const char* path)
{
    void* r = o_CreateManagedTexture(self, path);

    // Re-entrant by construction - LoadExtraMaps calls straight back into this
    // function - so the guard is the whole of the recursion control.
    if (g_extraCount > 0 && !g_inExtraLoad)
    {
        char folder[MAX_PATH];
        if (WorldFolderOf(path, folder, sizeof(folder)))
        {
            g_inExtraLoad = true;
            for (int k = 0; k < MAX_EXTRA_MAPS; k++)
            {
                g_extra[k].tex = NULL;
                if (g_extra[k].caveSlot) *g_extra[k].caveSlot = NULL;
            }
            __try { LoadExtraMaps(self, folder); }
            __except (FaultFilter("extra deposit maps", GetExceptionInformation()))
            {
                Logf("maps     load faulted - extra deposit maps unavailable this world");
            }
            g_inExtraLoad = false;
        }
    }
    return r;
}

// The world saver writes farmap, the emissive map and both deposit maps, each
// through SaveToDDS with the folder it was handed. Ours go with them, so a
// painted or depleted extra map survives a reload exactly the way the engine's
// two already do.
static void h_WorldSaveMaps(void* self, const char* folder)
{
    o_WorldSaveMaps(self, folder);

    __try
    {
        typedef void (*t_Save)(void*, const char*);
        for (int k = 0; k < g_extraCount; k++)
        {
            if (!g_extra[k].tex) continue;
            void* fn = TexVCall(g_extra[k].tex, TEX_SAVE_DDS);
            if (!fn) continue;

            char path[MAX_PATH];
            _snprintf_s(path, sizeof(path), _TRUNCATE, "%s/resourcemap%d.dds",
                        folder, DEP_MAP_EXTRA + k + 1);
            ((t_Save)fn)(g_extra[k].tex, path);
        }
    }
    __except (FaultFilter("extra deposit map save", GetExceptionInformation()))
    {
        Logf("maps     save faulted - extra deposit maps were not written");
    }
}

static void InstallExtraMaps()
{
    if (g_extraCount <= 0) return;

    void** slot = FindIatSlot(g_exe, DLL_ENGINE, "?C3DHelp_CheckIfFileExist@@YA_NPEBD_N1@Z");
    if (slot) o_FileExists = (t_FileExists)*slot;
    else Logf("maps     WARN  no import slot for C3DHelp_CheckIfFileExist - every extra map "
              "will load its own file and fail silently if it is not there");

    if (!PatchIat(g_exe, DLL_ENGINE,
                  "?CreateManagedTexture@C3D_MIDDLEPOINT@@QEAAPEAVC3DAPI_TEXTURE@@PEBD@Z",
                  (void*)h_CreateManagedTexture, (void**)&o_CreateManagedTexture,
                  "CreateManagedTexture"))
    {
        Logf("maps     FAILED  cannot hook CreateManagedTexture - %d extra map(s) disabled",
             g_extraCount);
        g_extraCount = 0;
        return;
    }

    if (!PatchWorldSaveCall((void*)h_WorldSaveMaps))
        Logf("maps     WARN  the save hook did not install - extra maps load but are never "
             "written back, so painting and depletion will not survive a reload");

    BlankMapPath();      // written now rather than mid-load
    Logf("maps     %d map(s) past the engine's two are loaded and saved with the world",
         g_extraCount);
}

// Data below, code above. Both are bump-allocated and both are bounds-checked:
// with the number of cases coming out of a config file, running off the end is
// a thing a user can cause, and it must fail before a byte of the executable
// has been touched rather than halfway through.
// Sized for MAX_DEPOSITS: a case costs about 60 bytes in each of the parser and
// dispatch chains and 16 in the radius one, so 32 of them is roughly 4.5 KB of
// code, against tokens, radius floats and map pointers in the data half.
#define CAVE_SIZE          0x4000
#define CAVE_CODE          0x1000

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
    BYTE* je32()                      { b(0x0F); b(0x84); BYTE* at = p; d32(0); return at; }
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
// game's own type-6 block with two substitutions: which map pointer is loaded,
// and which float of the sampled colour is kept.
//
// The engine's two maps are read out of the game object exactly as its own cases
// do. A map this plugin owns has no home in the game object, so its case reads
// the pointer from a qword in the cave's own data instead - `mapSlot` - which
// LoadExtraMaps writes at every world load. Same one instruction's worth of
// work, one indirection shorter, and the sampler cannot tell the difference:
// its fourth argument is a texture pointer and nothing about it says where the
// pointer came from.
static void EmitDispatchCase(Emit& e, const DepositDef* d, BYTE* mapSlot)
{
    e.b(0x83); e.b(0xBE); e.d32(P_DEP_TYPE_FIELD); e.b((BYTE)d->type);      // cmp [rsi+0x368],type
    BYTE* next = e.jne32();

    e.b(0xF2); e.b(0x0F); e.b(0x10); e.b(0x44); e.b(0x24); e.b(0x40);       // movsd xmm0,[rsp+0x40]
    e.b(0xF2); e.b(0x0F); e.b(0x11); e.b(0x45); e.b(0x38);                  // movsd [rbp+0x38],xmm0
    e.b(0x8B); e.b(0x44); e.b(0x24); e.b(0x48);                             // mov eax,[rsp+0x48]
    e.b(0x89); e.b(0x45); e.b(0x40);                                        // mov [rbp+0x40],eax
    if (mapSlot)
    {
        e.b(0x4C); e.b(0x8B); e.b(0x0D); e.rel32(mapSlot);                  // mov r9,[our slot]
    }
    else if (d->map == DEP_MAP_TERRAIN)
    {
        e.b(0x4C); e.b(0x8B); e.b(0x0D); e.rel32(g_exeBase + P_GAMEOBJ);    // mov r9,[gameobj]
        e.b(0x4D); e.b(0x8B); e.b(0x89); e.d32(P_TERRAIN_OFF);              // mov r9,[r9+0xed8]
        e.b(0x4D); e.b(0x8B); e.b(0x89); e.d32(P_TERRAIN_MASK);             // mov r9,[r9+0x158]
    }
    else
    {
        e.b(0x4C); e.b(0x8B); e.b(0x0D); e.rel32(g_exeBase + P_GAMEOBJ);    // mov r9,[gameobj]
        e.b(0x4D); e.b(0x8B); e.b(0x89);
        e.d32(d->map == DEP_MAP_2 ? P_MAP2_OFF : P_MAP1_OFF);               // mov r9,[r9+map]
    }
    e.b(0x4C); e.b(0x8D); e.b(0x45); e.b(0x38);                             // lea r8,[rbp+0x38]
    e.b(0x48); e.b(0x8D); e.b(0x95); e.d32(0xB0);                           // lea rdx,[rbp+0xb0]
    e.b(0xE8); e.rel32(g_exeBase + P_SAMPLER);                              // call sampler
    e.b(0xF3); e.b(0x0F); e.b(0x10); e.b(0x40);
    e.b((BYTE)(d->component * 4));                                          // movss xmm0,[rax+c*4]
    e.b(0xF3); e.b(0x0F); e.b(0x11); e.b(0x44); e.b(0x24); e.b(0x5C);       // movss [rsp+0x5c],xmm0
    e.b(0xE9); e.rel32(g_exeBase + P_DISPATCH_TAIL);

    e.land(next);
}

// One half of the terrain-mask bracket: `if (type is one of ours, or 3)
// Mask{Open,Close}(terrain)`, in the shape the site it replaces already had.
// Emitted only when a deposit actually declares the mask, so a configuration
// without one leaves both sites untouched.
static void EmitMaskBracket(Emit& e, DWORD iatRva, BYTE* rejoin)
{
    BYTE* land[MAX_DEPOSITS];
    int   n = 0;

    for (int i = 0; i < g_depCount && n < MAX_DEPOSITS; i++)
    {
        if (g_dep[i].map != DEP_MAP_TERRAIN) continue;
        e.b(0x83); e.b(0xBE); e.d32(P_DEP_TYPE_FIELD); e.b((BYTE)g_dep[i].type);
        e.b(0x0F); e.b(0x84); land[n++] = e.p; e.d32(0);                    // jz open
    }

    e.b(0x83); e.b(0xBE); e.d32(P_DEP_TYPE_FIELD); e.b(0x03);               // cmp [rsi+0x368],3
    BYTE* skip = e.jne32();

    for (int i = 0; i < n; i++) e.land(land[i]);                            // open:
    e.b(0x48); e.b(0x8B); e.b(0x0D); e.rel32(g_exeBase + P_GAMEOBJ);        // mov rcx,[gameobj]
    e.b(0x48); e.b(0x8B); e.b(0x89); e.d32(P_TERRAIN_OFF);                  // mov rcx,[rcx+0xed8]
    e.b(0xFF); e.b(0x15); e.rel32(g_exeBase + iatRva);                      // call [MaskTexture*]

    e.land(skip);
    e.b(0xE9); e.rel32(rejoin);
}

// One half of the resource-map bracket: `if (type is one of ours) Open/Close
// (its map)`, chained in front of the displaced types 0-2 block, which is then
// reproduced verbatim. The texture is the game object's own for the engine's
// two maps - loaded exactly as the vanilla cases load it - or the cave qword
// LoadExtraMaps maintains for one of ours; a NULL there skips the call rather
// than dereferencing it, which leaves the later sample to fail the way an
// unloaded map already fails. After our call the flow rejoins *before* the
// vanilla checks, not after them: the rejoin re-reads the type field, and a
// mod type matches nothing downstream, so there is no double-open to fear and
// no need to preserve EAX across the call.
static void EmitMapBracket(Emit& e, DWORD vtblOff, BYTE* rejoin, bool openSite)
{
    for (int i = 0; i < g_depCount; i++)
    {
        const DepositDef* d = &g_dep[i];
        if (d->map == DEP_MAP_TERRAIN) continue;

        // Same bounded indexing the dispatch emitter uses - DEP_MAP_TERRAIN is
        // far outside the map numbers and is filtered above, but keep the shape.
        int k = (d->map >= DEP_MAP_EXTRA && d->map < MAX_MAPS)
              ? d->map - DEP_MAP_EXTRA : -1;

        e.b(0x83); e.b(0xBE); e.d32(P_DEP_TYPE_FIELD); e.b((BYTE)d->type);  // cmp [rsi+0x368],type
        BYTE* next = e.jne32();

        if (k >= 0)
        {
            e.b(0x48); e.b(0x8B); e.b(0x0D); e.rel32((BYTE*)g_extra[k].caveSlot); // mov rcx,[slot]
            e.b(0x48); e.b(0x85); e.b(0xC9);                                // test rcx,rcx
            BYTE* noTex = e.je32();                                         // no map loaded - skip
            e.b(0x48); e.b(0x8B); e.b(0x01);                                // mov rax,[rcx]
            e.b(0xFF); e.b(0x90); e.d32(vtblOff);                           // call [rax+slot]
            e.land(noTex);
        }
        else
        {
            e.b(0x48); e.b(0x8B); e.b(0x05); e.rel32(g_exeBase + P_GAMEOBJ);// mov rax,[gameobj]
            e.b(0x48); e.b(0x8B); e.b(0x88);
            e.d32(d->map == DEP_MAP_2 ? P_MAP2_OFF : P_MAP1_OFF);           // mov rcx,[rax+map]
            e.b(0x48); e.b(0x8B); e.b(0x01);                                // mov rax,[rcx]
            e.b(0xFF); e.b(0x90); e.d32(vtblOff);                           // call [rax+slot]
        }
        e.b(0xE9); e.rel32(rejoin);
        e.land(next);
    }

    // The displaced types 0-2 block, verbatim in everything but the JA, which
    // takes the near form because the target is behind us in the original.
    // The open site holds the type in EAX on entry (our cases never touch it);
    // the close site re-reads the field instead. Both close resourcemap, the
    // map of types 0-2.
    if (openSite)
    {
        e.b(0x83); e.b(0xF8); e.b(0x02);                                    // cmp eax,2
    }
    else
    {
        e.b(0x83); e.b(0xBE); e.d32(P_DEP_TYPE_FIELD); e.b(0x02);           // cmp [rsi+0x368],2
    }
    e.b(0x0F); e.b(0x87); e.rel32(rejoin);                                  // ja rejoin
    e.b(0x48); e.b(0x8B); e.b(0x05); e.rel32(g_exeBase + P_GAMEOBJ);        // mov rax,[gameobj]
    e.b(0x48); e.b(0x8B); e.b(0x88); e.d32(P_MAP1_OFF);                     // mov rcx,[rax+0xF00]
    e.b(0x48); e.b(0x8B); e.b(0x01);                                        // mov rax,[rcx]
    e.b(0xFF); e.b(0x90); e.d32(vtblOff);                                   // call [rax+slot]
    e.b(0xE9); e.rel32(rejoin);
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
    BYTE* maskOpenSite = g_exeBase + P_MASK_OPEN_SITE;
    BYTE* maskCloseSite= g_exeBase + P_MASK_CLOSE_SITE;
    BYTE* mapOpenSite  = g_exeBase + P_MAP_OPEN_SITE;
    BYTE* mapCloseSite = g_exeBase + P_MAP_CLOSE_SITE;

    if (memcmp(parserSite,   kParserOrig,   sizeof(kParserOrig))   != 0 ||
        memcmp(dispatchSite, kDispatchOrig, sizeof(kDispatchOrig)) != 0 ||
        memcmp(radiusSite,   kRadiusOrig,   sizeof(kRadiusOrig))   != 0)
    {
        Logf("patch  site bytes differ from build v1.1.1.7 - refusing to patch");
        return false;
    }

    // Same treatment as the terrain-mask bracket below: checked only when a
    // deposit on a resource map will use it, and a mismatch drops those
    // deposits rather than corrupting the scan. An unbracketed resource map is
    // worse than an unbracketed mask - the sampler dereferences a null mapped
    // pointer and takes the process down (see the comment at P_MAP_OPEN_SITE) -
    // so there is no "patch it anyway" option here.
    if (g_mapCount &&
        (memcmp(mapOpenSite,  kMapOpenOrig,  sizeof(kMapOpenOrig))  != 0 ||
         memcmp(mapCloseSite, kMapCloseOrig, sizeof(kMapCloseOrig)) != 0))
    {
        Logf("patch  resource-map bracket bytes differ from build v1.1.1.7 - "
             "%d map deposit(s) dropped", g_mapCount);
        int kept = 0;
        for (int i = 0; i < g_depCount; i++)
            if (g_dep[i].map == DEP_MAP_TERRAIN) g_dep[kept++] = g_dep[i];
        g_depCount  = kept;
        g_mapCount  = 0;
        g_extraCount = 0;
        if (!g_depCount) { Logf("patch  nothing left to splice"); return false; }
    }

    // Checked separately and only when they will be used, so a configuration
    // with no terrain-mask deposit is not held hostage to two sites it never
    // touches. Both or neither: an opened mask that is never closed leaves a
    // D3D11 resource mapped.
    if (g_terrainCount &&
        (memcmp(maskOpenSite,  kMaskOrig, sizeof(kMaskOrig)) != 0 ||
         memcmp(maskCloseSite, kMaskOrig, sizeof(kMaskOrig)) != 0))
    {
        Logf("patch  terrain-mask bracket bytes differ from build v1.1.1.7 - "
             "%d mask deposit(s) dropped", g_terrainCount);
        int kept = 0;
        for (int i = 0; i < g_depCount; i++)
            if (g_dep[i].map != DEP_MAP_TERRAIN) g_dep[kept++] = g_dep[i];
        g_depCount     = kept;
        g_terrainCount = 0;
        if (!g_depCount) { Logf("patch  nothing left to splice"); return false; }
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

    // One qword per map past the engine's two, for the dispatch to read the
    // texture out of. Kept in the cave rather than in this DLL's own data
    // because the cave is guaranteed to be within rel32 of the code that reads
    // it, and a plugin's globals are wherever Windows put the module.
    data = (BYTE*)(((size_t)data + 7) & ~(size_t)7);
    for (int k = 0; k < g_extraCount; k++)
    {
        if ((size_t)(dataEnd - data) < 8) { Logf("patch  cave data full at map %d", DEP_MAP_EXTRA + k + 1); return false; }
        *(void**)data     = g_extra[k].tex;                // NULL until a world loads
        g_extra[k].caveSlot = (void**)data;
        data += 8;
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
    for (int i = 0; i < g_depCount; i++)
    {
        // Bounded on both sides. DEP_MAP_TERRAIN is deliberately far outside the
        // map numbers, so a bare `map - DEP_MAP_EXTRA` would index the extra-map
        // table at 62 and hand the emitter whatever was there.
        int k = (g_dep[i].map >= DEP_MAP_EXTRA && g_dep[i].map < MAX_MAPS)
              ? g_dep[i].map - DEP_MAP_EXTRA : -1;
        EmitDispatchCase(e, &g_dep[i], k >= 0 ? (BYTE*)g_extra[k].caveSlot : NULL);
    }

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

    // The terrain-mask bracket, both halves, only when something asks for it.
    BYTE* maskOpenCave  = NULL;
    BYTE* maskCloseCave = NULL;
    if (g_terrainCount)
    {
        maskOpenCave = e.p;
        EmitMaskBracket(e, P_MASK_OPEN_IAT,  g_exeBase + P_MASK_OPEN_NEXT);
        maskCloseCave = e.p;
        EmitMaskBracket(e, P_MASK_CLOSE_IAT, g_exeBase + P_MASK_CLOSE_NEXT);
    }

    // The resource-map bracket, both halves, under the same rule. Without it
    // every mod type samples through a never-mapped texture - the crash this
    // is for - and the "it worked after painting" behaviour was a stale
    // pointer TextureAccessClose happens to leave behind, not a working scan.
    BYTE* mapOpenCave  = NULL;
    BYTE* mapCloseCave = NULL;
    if (g_mapCount)
    {
        mapOpenCave = e.p;
        EmitMapBracket(e, P_TEX_OPEN,  g_exeBase + P_MAP_OPEN_NEXT,  true);
        mapCloseCave = e.p;
        EmitMapBracket(e, P_TEX_CLOSE, g_exeBase + P_MAP_CLOSE_NEXT, false);
    }

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
    // len is how many bytes the jump and its NOP padding overwrite; the verify
    // constants above may run longer, to pin the site by the instruction that
    // follows. At the map close site only the CMP is replaced - its JA stays
    // behind as dead code the cave never rejoins into.
    struct { BYTE* site; BYTE* cave; size_t len; const char* what; } jumps[] = {
        { parserSite,    parserCave,    sizeof(kParserOrig),   "parser"     },
        { dispatchSite,  dispatchCave,  sizeof(kDispatchOrig), "dispatch"   },
        { radiusSite,    radiusCave,    sizeof(kRadiusOrig),   "radius"     },
        { maskOpenSite,  maskOpenCave,  sizeof(kMaskOrig),     "mask open"  },
        { maskCloseSite, maskCloseCave, sizeof(kMaskOrig),     "mask close" },
        { mapOpenSite,   mapOpenCave,   5,                     "map open"   },
        { mapCloseSite,  mapCloseCave,  7,                     "map close"  },
    };

    for (int i = 0; i < (int)(sizeof(jumps) / sizeof(jumps[0])); i++)
    {
        if (!jumps[i].cave) continue;         // the bracket, with nothing to bracket

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
    {
        char mapName[32];
        Logf("patch  deposit type %d added: \"%s\" in building.ini, %s component %d",
             g_dep[i].type, g_dep[i].token,
             MapName(g_dep[i].map, mapName, sizeof(mapName)), g_dep[i].component);
    }
    Logf("patch  cave at %p, %zu of %d bytes used (parser %p, dispatch %p, radius %p, mask %p/%p, map %p/%p)",
         g_cave, g_caveUsed, CAVE_SIZE, parserCave, dispatchCave, radiusCave,
         maskOpenCave, maskCloseCave, mapOpenCave, mapCloseCave);
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
#define G_RES_VECTOR      0x9E11C0   // the resource vector records live in: begin, end, capacity
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

// The minimap's hover text, and it needed no unexported formatter after all.
// Every vanilla layer does exactly this when its icon is hovered:
//
//   Resource* r = ResourceGet(&game, "coal");
//   FUN_140005290(&buffer, 0x800, L"%ls: %ls",
//                 GetString(&lang, 0x2F3), GetString(&lang, r[0x40]));
//
// - a swprintf into one global wide buffer the panel draws later. So a mod layer
// needs the same three things it already has: the resource its `icon` names, that
// record's caption id, and the same label.
#define G_LANG            0x997590   // C3D_LANGUAGE
#define G_TOOLTIP_BUF     0x9E24B0   // the wide buffer the hover text is built in
#define G_TOOLTIP_CHARS   0x800
#define P_FORMAT_WIDE     0x5290     // (wchar_t* buf, size_t chars, const wchar_t* fmt, ...)
#define TXT_DEPOSIT_LABEL 0x2F3      // the "<label>: <resource>" prefix every layer uses
#define RES_CAPTION_OFF   0x40       // resource record -> its caption's localisation id
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

// Not a hook, and deliberately **the slot rather than what is in it**.
//
// C3D_LANGUAGE::GetString is where a mod resource's caption comes from: the
// `resources` plugin swaps this same import and answers the private ids it mints
// from 1 000 000 up. But plugins load in directory order, `deposits` comes before
// `resources`, and reading the slot at init time therefore captured the engine's
// own GetString - which has nothing at 1 000 000 and returns an empty string.
// That is the whole of why a mod layer's tooltip read "Deposits: " and stopped.
//
// Dereferencing at call time instead makes the order irrelevant: whoever swapped
// the import last is who answers, which is exactly the property an import swap
// is supposed to have. Nothing else here caches an import that another plugin
// might own - this was the only one.
typedef wchar_t* (*t_MM_GetString)(void*, int);
static void** g_MM_GetStringSlot;

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

    // Whichever kind of map the section named. An extra one is not in the game
    // object at all, and the shader does not care: MM_TexBind puts whatever it
    // is given into stage 0.
    void* map = DepositMapTexture(d);
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
// The deposit's icon resource record, resolved once instead of once per frame.
// What this removes is not the lookup - a vector walk is cheap - but the miss:
// an `icon` naming a resource nobody declared falls through to the game's own
// resolver, which writes `game.ERROR ResourceGet - not found X` per call, and
// at one call per button per frame the game's log machinery becomes the
// hottest thing on the frame - the longer the minimap stays open, the slower
// every later frame gets. A miss is therefore latched: the resource set is
// fixed for the session, so a name that found nothing once would find nothing
// on every later frame too. The hit is cached as well, but re-validated
// against the resource vector's current span, because a map load rebuilds
// that vector and a record from the previous world is not safe to follow.
static BYTE* DepositIconRecord(DepositDef* d)
{
    if (!d->icon[0] || d->iconFailed) return NULL;

    if (d->iconRecord)
    {
        const BYTE* const* v = (const BYTE* const*)(g_exeBase + G_RES_VECTOR);
        if (v[0] && (const BYTE*)d->iconRecord >= v[0] && (const BYTE*)d->iconRecord < v[1])
            return d->iconRecord;
        d->iconRecord = NULL;          // the world it came from is gone - resolve again
    }

    t_ResourceGet resourceGet = (t_ResourceGet)(g_exeBase + P_RESOURCEGET);
    BYTE* record = (BYTE*)resourceGet(g_exeBase + G_RES_SELF, (void*)d->icon, NULL, NULL);
    if (!ReadablePtr(record, 0x50))
    {
        d->iconFailed = 1;
        Logf("minimap  \"%s\": icon resource \"%s\" not found - the button will draw "
             "without an icon (declare it in plugins/resources.ini [list])", d->name, d->icon);
        return NULL;
    }
    d->iconRecord = record;
    return record;
}

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

    // The hover text, in the same buffer and the same shape as every vanilla
    // layer's. Written while hovered and left alone otherwise, exactly as the
    // vanilla blocks do - whoever is hovered last owns the buffer, and the panel
    // draws it afterwards.
    if (hovered && g_MM_GetStringSlot && d->icon[0])
    {
        BYTE* record = DepositIconRecord(d);
        if (record && ReadablePtr(record, RES_CAPTION_OFF + sizeof(int)))
        {
            typedef void (*t_FormatWide)(void*, size_t, const wchar_t*, ...);
            t_MM_GetString getString = (t_MM_GetString)*g_MM_GetStringSlot;
            void* lang = g_exeBase + G_LANG;
            ((t_FormatWide)(g_exeBase + P_FORMAT_WIDE))(
                g_exeBase + G_TOOLTIP_BUF, G_TOOLTIP_CHARS, L"%ls: %ls",
                getString(lang, TXT_DEPOSIT_LABEL),
                getString(lang, *(int*)(record + RES_CAPTION_OFF)));
        }
    }

    // the resource's own icon, straight out of its record - no art asset of
    // our own needed. The record is cached: a miss here was the per-frame
    // ResourceGet storm that sank frame rate with the minimap open, and a hit
    // is one pointer comparison per frame.
    if (d->icon[0])
    {
        BYTE* record = DepositIconRecord(d);
        if (record)
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

    // The slot, not its contents - see the note on g_MM_GetStringSlot. Missing
    // it costs the hover text and nothing else, so it only warns.
    g_MM_GetStringSlot = FindIatSlot(g_exe, DLL_ENGINE, "?GetString@C3D_LANGUAGE@@QEAAPEA_WH@Z");
    if (!g_MM_GetStringSlot)
        Logf("minimap  WARN  no import slot for C3D_LANGUAGE::GetString - layers have no hover text");
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
#define TOOL_CAPTION      0x40       // localisation id of the hover text
#define TOOL_BUILDING     0x48       // a building type for a build tool, 0 for a terrain one
#define TOOL_ICON_TEX     0x58       // the texture the button binds
#define TOOL_ICON_PATH    0xB4       // empty on every built-in tool - see below

// The hover text, and why our buttons had none.
//
// The "accumulator" every vanilla button is handed is one qword, not a buffer:
// when a button is hovered, 0x3826C0 writes the tool's **descriptor** into it
// (and into editorSelf+0xC8C0) at rva 0x382A29. The panel then hands that qword
// to 0x383BD0, which reads two fields off the descriptor it names:
//
//   tool+0x48 == 0   ->  wcscpy(editorSelf+0xD5A0, GetString(lang, tool+0x40))
//   tool+0x48 != 0   ->  the rich building tooltip, drawn at the mouse
//
// Every terrain tool has +0x48 == 0, so a mod brush takes the simple path and
// needs nothing but a text id at +0x40.
//
// So there was never anything missing from the buttons - the panel calls
// 0x383BD0 before it returns, which is before an appended hook has drawn
// anything, and our accumulator went nowhere. Calling the consumer ourselves,
// once, after our own buttons, is the whole fix.
#define P_ED_TOOLTIP      0x383BD0   // FUN_140383bd0(editorSelf, hoveredTool)

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

// The Rocks tab, and the brush behind its rock pair.
//
// A deposit in the terrain's material mask is painted by a different primitive
// from one in a resource map: C3D_TERRAIN::EditMask rather than the deposit
// texel writer, and the tab that owns it is 0x22EE30 rather than 0x233110.
// Everything else is the same shape - clone the tool, borrow the vanilla call,
// rewrite the one argument that differs while it is in flight.
//
//   paint_rock / erase_rock    0x235300(self, mode)   EditMask channel 3
//   paint_oasis / erase_oasis  0x235510(self, mode)   EditMask channel 0
//
// Two functions, one constant apart, and the channel encoding is the deposit
// brush's: channel = (component + 1) & 3. So rock paints component 2, which is
// exactly the component gravel is mined from.
#define P_ED_ROCKS_PANEL  0x22EE30   // FUN_14022ee30 - draws the Rocks tab
#define P_ED_PAINT_ROCK   0x235300   // FUN_140235300 - one rock brush tick
#define ED_CH_ROCK        3          // the EditMask channel that call passes

// Rocks-tab layout, all read from the game for the same reason the Resources
// tab's are. x = DPI*(50 + 85), y = DPI*(250 + 80), one button 0.85 wide, and
// 85 apart across; the mod row sits one 90-step below the vanilla one.
#define G_ED_ROCK_X_A     0x90AA40   // 50
#define G_ED_ROCK_X_B     0x90AB2C   // 85 - and the step between buttons
#define G_ED_ROCK_Y_BASE  0x90ADD0   // 250
#define G_ED_ROCK_Y_CAP   0x90AB14   // 80

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
static const BYTE kEdRocksPrologue[] = {
    0x48, 0x8B, 0xC4,                                  // mov rax,rsp
    0x55,                                              // push rbp
    0x53,                                              // push rbx
    0x56,                                              // push rsi
    0x57,                                              // push rdi
    0x41, 0x56,                                        // push r14
    0x48, 0x8D, 0x68, 0xA1,                            // lea rbp,[rax-0x5f]
    0x48, 0x81, 0xEC, 0xF0, 0x00, 0x00, 0x00           // sub rsp,0xf0
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

// C3D_TERRAIN::EditMask(this, &pos, channel, innerR, outerR, delta, limit, on).
// The C3DVECTOR3 is declared by value and is twelve bytes, so MSVC passes it by
// address - which is what the call site at 0x2354xx does and what makes the
// channel the third argument rather than the fifth.
typedef void  (*t_ED_EditMask)(void* terrain, float* pos, int channel,
                               float innerR, float outerR, int delta,
                               int limit, char on);

static t_ED_Void1       o_ED_Panel;
static t_ED_Void1       o_ED_Rocks;
static t_ED_Void1       o_ED_Dispatch;
static t_ED_Void1       o_ED_Cursor;
static t_ED_PaintTexels o_ED_PaintTexels;
static t_ED_EditMask    o_ED_EditMask;
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
// The editor object, handed over by the per-frame cursor hook and consumed by
// the terrain overlay one. Non-null for exactly as long as the editor is the
// thing running - see h_ED_TerrainDraw.
static void* g_edSelf;

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

// The localisation id of the deposit's own name, taken from the record of the
// resource its `icon` names - the same record the minimap button already reads
// its texture out of, one field along. Nothing has to be minted: if that
// resource came from plugins/resources.ini its caption is already a private id
// the resources plugin answers, and if it is a base-game resource the id is the
// game's own. 0 leaves the donor's text, which is a wrong name but not a crash.
static int DepositCaptionId(const DepositDef* d)
{
    if (!d->icon[0]) return 0;
    t_ResourceGet get = (t_ResourceGet)(g_exeBase + P_RESOURCEGET);
    BYTE* rec = (BYTE*)get(g_exeBase + G_RES_SELF, (void*)d->icon, NULL, NULL);
    return ReadablePtr(rec, TOOL_CAPTION + sizeof(int)) ? *(int*)(rec + TOOL_CAPTION) : 0;
}

// Every clone is made from the matching bauxite tool, which is the same kind of
// tool in every respect that matters, and then differs in three fields: the
// name, the icon texture, and the hover text.
static bool BuildDepositTools(void* self)
{
    t_ED_ToolFind find = (t_ED_ToolFind)(g_exeBase + P_ED_TOOL_FIND);
    void* src[2] = { find(self, "paint_bauxite"), find(self, "erase_bauxite") };
    // The Rocks tab's own pair, for deposits in the terrain mask. Cloning
    // bauxite's would work as a descriptor, but rock's is the same kind of
    // brush in the same tab, and taking it keeps the two families honest.
    void* rock[2] = { find(self, "paint_rock"), find(self, "erase_rock") };
    if (!src[0] || !src[1])
    {
        Logf("editor   FAILED  bauxite tools not in the registry - mod brushes disabled");
        g_editorPatch = 0;
        return false;
    }
    if (g_terrainCount && (!rock[0] || !rock[1]))
        Logf("editor   WARN  rock tools not in the registry - terrain-mask brushes dropped");

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

        bool mask = (d->map == DEP_MAP_TERRAIN);
        if (mask && (!rock[0] || !rock[1])) { d->editorColumn = -1; continue; }

        for (int i = 0; i < 2; i++)
        {
            BYTE* tool = g_toolPool[k][i];
            memcpy(tool, mask ? rock[i] : src[i], TOOL_STRIDE);

            char name[64], icon[MAX_PATH];
            _snprintf_s(name, sizeof(name), _TRUNCATE, "%s_%s", kVerb[i], d->editor);
            _snprintf_s(icon, sizeof(icon), _TRUNCATE, "editor/tool_%s.png", name);

            // The name has to fit in the one it replaces, so the editor key is
            // capped at seven characters against bauxite's `paint_bauxite` and
            // at **four** against rock's `paint_rock`. Refusing one tool but
            // keeping the other would leave a brush that paints but cannot be
            // turned off, so this drops the whole pair.
            if (!ReplaceInlineString(tool, TOOL_NAME, name, "tool name"))
            {
                d->editorColumn = -1;
                break;
            }

            // The donor's caption would say "Bauxite" or "Rock" under a copper
            // brush. The record's caption id is the deposit's own name in
            // whatever language the game is running in, and 0 means the entry
            // named no resource - in which case the donor's text stands.
            if (int caption = DepositCaptionId(d)) *(int*)(tool + TOOL_CAPTION) = caption;

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

// Hands the hovered tool to the engine's own tooltip, which is what the panel
// does with its accumulator on the way out and what an appended hook is too late
// to be part of. Null when nothing is hovered, and the callee already treats
// that as "no tooltip", so this is called unconditionally.
static void ShowToolTip(void* self, void* hovered)
{
    typedef void (*t_ToolTip)(void*, void*);
    ((t_ToolTip)(g_exeBase + P_ED_TOOLTIP))(self, hovered);
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
    void* acc = NULL;                 // one for the whole row, as the vanilla has
    for (int k = 0; k < g_depCount; k++)
    {
        DepositDef* d = &g_dep[k];
        if (d->editorColumn < 0 || d->map == DEP_MAP_TERRAIN) continue;
        if (!d->toolPaint || !d->toolErase) continue;

        float x = x0 + (float)d->editorColumn * xStep;
        draw(self, d->toolPaint, &acc, x, yPaint, button, 0, 1, 1);
        draw(self, d->toolErase, &acc, x, yErase, button, 0, 1, 1);
    }
    ShowToolTip(self, acc);
}

// The same, for the Rocks tab and the deposits that live in the terrain's mask.
// Its vanilla pairs are laid out **across a single row** - paint then erase, one
// 85-step apart - rather than in two-row columns, so the mod pairs go on a
// second row underneath, from the tab's own constants.
static void DrawRockTools(BYTE* self)
{
    if (!BuildDepositTools(self)) return;

    float dpi    = MM_F(G_DPI);
    float button = MM_F(G_ED_BUTTON);
    float step   = dpi * MM_F(G_ED_ROCK_X_B) * button;
    float x0     = dpi * MM_F(G_ED_ROCK_X_A) + dpi * MM_F(G_ED_ROCK_X_B);
    float y      = dpi * MM_F(G_ED_ROCK_Y_BASE) + dpi * MM_F(G_ED_ROCK_Y_CAP)
                 + dpi * MM_F(G_ED_ROW_STEP) * button;

    t_ED_DrawButton draw = (t_ED_DrawButton)(g_exeBase + P_ED_DRAW_BUTTON);
    void* acc = NULL;
    for (int k = 0; k < g_depCount; k++)
    {
        DepositDef* d = &g_dep[k];
        if (d->editorColumn < 0 || d->map != DEP_MAP_TERRAIN) continue;
        if (!d->toolPaint || !d->toolErase) continue;

        float x = x0 + (float)(d->editorColumn * 2) * step;
        draw(self, d->toolPaint, &acc, x,        y, button, 0, 1, 1);
        draw(self, d->toolErase, &acc, x + step, y, button, 0, 1, 1);
    }
    ShowToolTip(self, acc);
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

static void h_ED_Rocks(void* self)
{
    o_ED_Rocks(self);
    if (!g_editorPatch) return;
    __try { DrawRockTools((BYTE*)self); }
    __except (FaultFilter("editor rock buttons", GetExceptionInformation()))
    {
        g_editorPatch = 0;
        Logf("editor   mod brushes disabled for this session");
    }
}

static void PaintDeposit(void* self, int dep, int mode)
{
    // A deposit in the terrain's mask has its own primitive and its own vanilla
    // brush to borrow. Nothing in the rock brush touches the "this map has X"
    // bytes, so there is nothing to save and restore on this path.
    if (g_dep[dep].map == DEP_MAP_TERRAIN)
    {
        typedef void (*t_RockBrush)(void*, char);
        g_brushDep = dep;
        ((t_RockBrush)(g_exeBase + P_ED_PAINT_ROCK))(self, (char)mode);
        g_brushDep = -1;
        return;
    }

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

    // Also the one place the editor object is handed over every frame *and only
    // while the editor is running*, which is what the terrain overlay below
    // needs: it hangs off a render function that runs in the game as well, and
    // a pointer that is merely non-null would leave a stale tool selected.
    g_edSelf = self;

    if (ActiveDepositTool(self, NULL) >= 0 && ReadablePtr((BYTE*)self + ED_BRUSH_CURSOR, 1))
        *((BYTE*)self + ED_BRUSH_CURSOR) = 1;
}

// ---------------------------------------------------------------- the red terrain overlay
//
// Painting a vanilla deposit turns the editor's terrain grid red where the
// channel is rich. That is a render pass of its own at rva 0xAEE0, and it is
// driven by **six bytes in the game object** - one per channel the base game
// can paint, each carrying that channel's unit vector:
//
//   +0x23 coal      resourcemap  component 2   (0,0,1,0)  0x90BDF0
//   +0x24 iron      resourcemap  component 1   (0,1,0,0)  0x90BDA0
//   +0x25 oil       resourcemap  component 0   (1,0,0,0)  0x90BD80
//   +0x26 uranium   resourcemap2 component 0   (1,0,0,0)
//   +0x27 bauxite   resourcemap2 component 1   (0,1,0,0)
//   +0x28 -         resourcemap2 component 2   (0,0,1,0)
//
// The brush at 0x2350D0 sets its own byte on the way past, which is why the
// overlay appears the moment you paint. **There is no byte for component 3 of
// either map**, so copper never had one to set - and this plugin's brush hook
// restores the bauxite byte it borrows on purpose, so it sets nothing either.
//
// The pass is otherwise entirely generic. It swaps the terrain's mask texture
// (terrain+0x158) for the resource map, binds the "Resources" technique, hands
// the shader one float4, renders, and puts both back. The pixel shader is four
// instructions:
//
//     float a = dot(SelectedResources, tex.Sample(uv));   // dp4
//     o = float4(1,1,1,1) + a * float4(1.5,-1,-1,0);      // white -> red
//
// A dp4, exactly like the minimap's, so component 3 was reachable all along and
// only the flag was missing.
//
// Which is why this hook reproduces nothing. It **brackets** the vanilla pass:
// set the flag whose vector is the component wanted, point the map that pass
// reads at whichever texture this deposit actually lives in, let the engine
// draw its own overlay, and put all of it back.

#define P_ED_TERRAIN_RVA  0xAEE0     // FUN_14000aee0 - contour lines and the resource overlay
#define G_OVL_FLAGS       0x23       // gameobj, six bytes, one per paintable channel
#define G_OVL_COUNT       6
#define G_OVL_VEC_C2      0x90BDF0   // the float4 the +0x23 branch passes: (0,0,1,0)

static const BYTE kEdTerrainPrologue[] = {
    0x48, 0x8B, 0xC4,                   // mov rax,rsp
    0x48, 0x89, 0x50, 0x10,             // mov [rax+0x10],rdx
    0x48, 0x89, 0x48, 0x08,             // mov [rax+0x08],rcx
    0x55,                               // push rbp
    0x53,                               // push rbx
    0x56,                               // push rsi
    0x57                                // push rdi
};

// Its one caller sets up no arguments at all - 0x482824 is a bare CALL after an
// unrelated one - and the two the prologue spills are never read. Taking none is
// therefore not an approximation.
typedef void (*t_ED_TerrainDraw)(void);
static t_ED_TerrainDraw o_ED_TerrainDraw;

// Which of the six flags carries the unit vector for a component. Component 3
// has none and borrows coal's, whose vector is rewritten below.
static int OverlayFlagFor(int component)
{
    return component == 0 ? 2 : component == 1 ? 1 : 0;
}

static void h_ED_TerrainDraw(void)
{
    // One editor frame, one overlay. The cursor hook runs only in the editor and
    // only once a frame, so consuming the pointer here is both the gate and the
    // guarantee that a tool left selected in a previous session cannot paint the
    // terrain red in the middle of a game.
    void* self = g_edSelf;
    g_edSelf = NULL;

    int dep = (self && g_editorPatch) ? ActiveDepositTool(self, NULL) : -1;
    if (dep < 0) { o_ED_TerrainDraw(); return; }

    const DepositDef* d       = &g_dep[dep];
    BYTE*             gameobj = *(BYTE**)(g_exeBase + G_GAMEOBJ);
    void*             tex     = DepositMapTexture(d);

    if (!tex || !ReadablePtr(gameobj, P_MAP1_OFF + sizeof(void*)))
    { o_ED_TerrainDraw(); return; }

    BYTE  savedFlags[G_OVL_COUNT];
    void* savedTex = *(void**)(gameobj + P_MAP1_OFF);
    memcpy(savedFlags, gameobj + G_OVL_FLAGS, G_OVL_COUNT);

    // Every flag cleared, then exactly ours: the pass takes the first one set,
    // so a map that really does have coal would otherwise draw coal over us.
    // The texture goes in resourcemap's slot whatever map the deposit is on,
    // which is what makes resourcemap2 and the plugin's own maps work through
    // the branch that only knows how to read the first.
    memset(gameobj + G_OVL_FLAGS, 0, G_OVL_COUNT);
    gameobj[G_OVL_FLAGS + OverlayFlagFor(d->component)] = 1;
    *(void**)(gameobj + P_MAP1_OFF) = tex;

    float  savedVec[4];
    float* vec     = (float*)(g_exeBase + G_OVL_VEC_C2);
    DWORD  prot    = 0;
    bool   patched = false;
    if (d->component == 3 && VirtualProtect(vec, sizeof(savedVec), PAGE_READWRITE, &prot))
    {
        // Sixteen bytes of .rdata, for the length of one call, restored before
        // anything else can read them. Its only other readers are the +0x28
        // branch of this same function - which we just cleared - and the minimap
        // overlay, which is a different function on the same thread.
        memcpy(savedVec, vec, sizeof(savedVec));
        vec[0] = vec[1] = vec[2] = 0.0f;
        vec[3] = 1.0f;
        patched = true;
    }

    o_ED_TerrainDraw();

    if (patched)
    {
        memcpy(vec, savedVec, sizeof(savedVec));
        VirtualProtect(vec, sizeof(savedVec), prot, &prot);
    }
    *(void**)(gameobj + P_MAP1_OFF) = savedTex;
    memcpy(gameobj + G_OVL_FLAGS, savedFlags, G_OVL_COUNT);
}

static void h_ED_PaintTexels(void* dead, float* pos, unsigned channel,
                             float innerR, float outerR, int delta,
                             unsigned limit, char bracket)
{
    // Guarded on one of our own calls being in flight, so every other brush in
    // the editor - including bauxite's, whose index we borrowed to get here -
    // passes through untouched.
    if (g_brushDep < 0 || g_brushDep >= g_depCount || channel != ED_CH_BAUXITE)
    {
        o_ED_PaintTexels(dead, pos, channel, innerR, outerR, delta, limit, bracket);
        return;
    }

    const DepositDef* d = &g_dep[g_brushDep];
    channel = (unsigned)d->editorChannel;

    // A map past the engine's two cannot be named by a channel index: the writer
    // decodes bit 2 of it into one of exactly two pointers in the game object.
    // So the deposit's own texture is put in resourcemap2's slot for the length
    // of the call and taken straight back out. Reimplementing the writer instead
    // would mean reimplementing its bracket, its bilinear footprint and its
    // clamping, for the sake of one pointer.
    void** slot  = NULL;
    void*  saved = NULL;

    if (d->map >= DEP_MAP_EXTRA)
    {
        void* tex     = DepositMapTexture(d);
        BYTE* gameobj = *(BYTE**)(g_exeBase + G_GAMEOBJ);
        if (!tex || !ReadablePtr(gameobj, P_MAP2_OFF + sizeof(void*)))
            return;     // its map never loaded; painting into bauxite's is worse

        slot  = (void**)(gameobj + P_MAP2_OFF);
        saved = *slot;
        *slot = tex;
    }

    o_ED_PaintTexels(dead, pos, channel, innerR, outerR, delta, limit, bracket);

    if (slot) *slot = saved;
}

// The same trick as the deposit texel hook, one primitive over: the rock brush
// is borrowed whole and the single argument that names the channel is rewritten
// while one of our calls is in flight. Every other caller of EditMask - the
// material brushes on the terrain tab included - passes through untouched.
static void h_ED_EditMask(void* terrain, float* pos, int channel,
                          float innerR, float outerR, int delta,
                          int limit, char on)
{
    if (g_brushDep >= 0 && g_brushDep < g_depCount &&
        g_dep[g_brushDep].map == DEP_MAP_TERRAIN && channel == ED_CH_ROCK)
        channel = g_dep[g_brushDep].editorChannel;
    o_ED_EditMask(terrain, pos, channel, innerR, outerR, delta, limit, on);
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

    // Separately, and not fatal: without it the brush paints correctly and the
    // terrain simply does not turn red under it, which is what every version
    // before this one did.
    if (!InstallInlineHook(g_exeBase + P_ED_TERRAIN_RVA, (void*)h_ED_TerrainDraw,
                           (void**)&o_ED_TerrainDraw, kEdTerrainPrologue,
                           sizeof(kEdTerrainPrologue), "editor terrain overlay"))
        Logf("editor   WARN  no terrain overlay - a mod brush will paint a channel "
             "nothing draws");

    // The Rocks tab and its primitive, and only when a deposit lives in the
    // terrain's mask. Half of this pair is worse than none - a button that
    // selects a tool which then paints rock - so both or neither.
    int maskBrushes = 0;
    for (int i = 0; i < g_depCount; i++)
        if (g_dep[i].map == DEP_MAP_TERRAIN && g_dep[i].editorColumn >= 0) maskBrushes++;

    if (maskBrushes)
    {
        bool ok = PatchIat(g_exe, DLL_ENGINE, "?EditMask@C3D_TERRAIN@@QEAAXVC3DVECTOR3@@HMMHH_N@Z",
                           (void*)h_ED_EditMask, (void**)&o_ED_EditMask, "C3D_TERRAIN::EditMask");
        if (ok)
            ok = InstallInlineHook(g_exeBase + P_ED_ROCKS_PANEL, (void*)h_ED_Rocks,
                                   (void**)&o_ED_Rocks, kEdRocksPrologue,
                                   sizeof(kEdRocksPrologue), "editor rocks panel");
        if (!ok)
        {
            Logf("editor   terrain-mask brushes disabled - %d pair(s) dropped", maskBrushes);
            for (int i = 0; i < g_depCount; i++)
                if (g_dep[i].map == DEP_MAP_TERRAIN) g_dep[i].editorColumn = -1;
            maskBrushes = 0;
        }
    }

    Logf("editor   %d mod brush pair(s) hooked, %d of them in the Rocks tab",
         brushes, maskBrushes);
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
    // The same numbering TSM_MAP_* documents: 0 and 1 are the engine's, 2 and up
    // are this plugin's, and the value is always the filename's digit minus one.
    out->map          = d->map == DEP_MAP_TERRAIN ? TSM_MAP_TERRAIN : d->map;
    out->component    = d->component;
    out->radius       = d->radiusRva ? *(float*)(g_exeBase + d->radiusRva) : d->radiusValue;
    out->icon         = d->icon;
    return 1;
}

// Saves a consumer from knowing which kind of map a deposit landed on. The two
// the engine owns come out of the game object, ours out of this plugin's table,
// and both are re-read on every call because a world load replaces them.
static void* svc_Texture(int i)
{
    if (i < 0 || i >= g_depCount) return NULL;
    return DepositMapTexture(&g_dep[i]);
}

static const char* svc_Setting(int i, const char* key)
{
    if (i < 0 || i >= g_depCount || !key) return NULL;
    const DepositDef* d = &g_dep[i];
    for (int k = 0; k < d->extraCount; k++)
        if (_stricmp(d->extraKey[k], key) == 0) return d->extraVal[k];
    return NULL;
}

static const TsmDepositApi kDepositApi = { svc_Count, svc_Get, svc_Setting, svc_Texture };

extern "C" __declspec(dllexport) unsigned TsmPluginApiVersion(void)
{
    return TSM_API_VERSION;
}

extern "C" __declspec(dllexport) int TsmPluginInit(const TsmHost* host, TsmPluginInfo* info)
{
    TsmBind(host);
    info->name    = "deposits";
    info->version = "1.1";

    const char* ini = "plugins\\deposits.ini";
    g_depositPatch = H->configInt(ini, "deposits", "code_patch", g_depositPatch);
    g_minimapPatch = H->configInt(ini, "deposits", "minimap",    g_minimapPatch);
    g_editorPatch  = H->configInt(ini, "deposits", "editor",     g_editorPatch);

    // The registry is read whatever the three switches say: another plugin may
    // want to know what was declared even when none of the subsystems here is
    // allowed to touch the game.
    LoadDepositRegistry();
    ValidateDeposits();

    // Before the code patch, because the patch reserves the cave slots the
    // dispatch reads its textures from and this decides how many are needed.
    // Does nothing at all unless a section named a map past the engine's two.
    InstallExtraMaps();

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
