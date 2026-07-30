// buildings - new buildings declared in a config file, as a tesmioloader plugin.
//
// A new building needs no reverse engineering at all. The game's own Workshop
// format already describes one completely, and 1594 of the buildings installed
// on this machine arrived that way. What it does not have is a way to say "the
// clothes shop, but selling medicine" - every Workshop item is a folder of
// seven files, five of which are byte copies of a base-game asset and one of
// which is the donor's own building.ini with three lines changed.
//
// So this plugin does not patch the game at all. It installs no hook, swaps no
// import and reads no game structure. It **writes the folder**, from one
// section of plugins\buildings.ini, before the game has run a single
// instruction:
//
//   [pharmacy]
//   donor  = shop_clothes
//   line   = $STORAGE_SPECIAL RESOURCE_TRANSPORT_COVERED 8 medicine
//
// The loader is injected into a suspended process and plugins are initialised
// from DllMain, so Init runs before the game opens its first file - which is
// what makes generating a folder at startup equivalent to having shipped it.
//
// WHAT IS COPIED AND WHAT IS REWRITTEN
//
//   media_soviet\buildings\<donor>.nmf          -> <item>\<object>\model.nmf
//   media_soviet\buildings_types\<donor>.bbox   -> <item>\<object>\building.bbox
//   media_soviet\buildings_types\<donor>.fire   -> <item>\<object>\building.fire
//   media_soviet\editor\tool_<donor>.png        -> <item>\<object>\imagegui.png
//                                                  and <item>\previewimage.png
//   media_soviet\buildings\<donor>.mtl          -> <item>\material.mtl
//   media_soviet\buildings\<donor>_e.mtl        -> <item>\material_e.mtl
//   media_soviet\buildings_types\<donor>.ini    -> <item>\<object>\building.ini
//
// The two `.mtl` copies are not byte copies. 130 of the 493 base building
// materials write `$TEXTURE_MTL`, whose paths resolve next to the `.mtl` file
// itself; in a Workshop item that folder is the mod's own and the textures are
// simply not found. Each one is rewritten to `$TEXTURE buildings/<path>`, which
// resolves against media_soviet\ and therefore works wherever the file sits.
// shop_clothes.mtl is one of the 130, which is why this is not optional.
//
// `building.ini` is not a byte copy either, and the rule for it is the one
// 06-building-mods.md states by hand: **start from the donor's own file and
// change only the economy.** Everything geometric - $CONNECTION_*, $COST_WORK*,
// $VEHICLE_STATION, $PARTICLE, $TEXT_CAPTION - is measured against the mesh
// that was just copied and has to survive verbatim. So the donor's file is
// copied line by line, and a line is dropped only when the section declares a
// line of its own that replaces it.
//
// WHICH LINES A DECLARED LINE REPLACES
//
// A building.ini has no comment syntax: the parser matches its keywords
// wherever they occur, so `//$WORKERS_NEEDED 13` is a $WORKERS_NEEDED. The
// filter therefore reads the **first $TOKEN anywhere in the line** rather than
// the first word, which is the same thing the parser does.
//
// Most tokens replace only themselves. Four families replace each other,
// because replacing one member and leaving the rest produces a building that
// is half the donor:
//
//   $NAME / $NAME_STR                     a name is a name
//   $TYPE_*                               exactly one may be in effect
//   $STORAGE* + $RESOURCE_VISUALIZATION   the visualisation takes a storage
//                                         *index*, so re-declaring the storages
//                                         silently moves every pile
//   $PRODUCTION, $CONSUMPTION,            a recipe is replaced whole or not at
//   $CONSUMPTION_PER_SECOND               all
//
// Exactly those three, by name - $PRODUCTION_SEWAGE_POLLUTION and
// $CONSUMPTION_WATER_REQUIRED_QUALITY are settings rather than recipe lines and
// survive. `strip = $TOKEN` in a section drops anything else.
//
// WHERE THE FOLDER GOES, AND WHY IT IS ON DISK
//
// media_soviet\workshop_wip\<id>, which is the folder the game scans for
// unpublished Workshop items. It is not served out of the loader's VFS, and the
// reason is enumeration rather than reading: the game finds items with
// FindFirstFileW over that directory, and the VFS redirects opens, not
// listings. A virtual folder would be invisible.
//
// This is the one thing in the project that puts a file in the game folder. It
// never modifies one: it only creates folders under workshop_wip, it stamps
// each with tesmioloader.stamp, and it refuses to touch a folder that exists
// without that stamp - which is what keeps a real Workshop item safe from an id
// collision. Steam's verification only checks the files it shipped, so an added
// folder leaves it happy.
//
// Nothing here is addresses: this plugin does not read the executable at all,
// which is why it has no prologue to verify and cannot be broken by a game
// update. See docs/13-buildings.md.

#include "../../src/tesmio_plugin.h"

// ---------------------------------------------------------------- the registry

#define MAX_BUILDINGS   16
#define MAX_LINES       48
#define LINE_LEN        200
#define TOKEN_LEN       48

// Bumped whenever the generator's output changes shape. It goes into the stamp,
// so a plugin change regenerates every folder even when no ini did.
#define GENERATOR_VERSION 1

struct Decl
{
    char section[40];
    char id[24];                       // the workshop item id, and the folder name
    char object[64];                   // the object subfolder, named by $OBJECT_BUILDING
    char donor[64];                    // a base-game buildings_types name, no extension
    char name[128];                    // $NAME_STR, and the workshop item name
    char desc[1024];
    int  life;                         // renderconfig LIFE
    int  enabled;

    char line[MAX_LINES][LINE_LEN];    // building.ini lines to put in
    int  lines;
    char strip[MAX_LINES][TOKEN_LEN];  // extra tokens to take out
    int  strips;
};

static Decl g_decl[MAX_BUILDINGS];
static int  g_declCount;

static int  g_enabled    = 1;
static int  g_always     = 0;          // regenerate even when the stamp matches
static int  g_verbose    = 0;
static char g_outDir[MAX_PATH]         = "media_soviet\\workshop_wip";
static char g_gameDir[MAX_PATH];
static char g_media[MAX_PATH];

// ---------------------------------------------------------------- small helpers

static bool FileExists(const char* p)
{
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static bool DirExists(const char* p)
{
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

// `out` is relative to the game folder, which is what it should be for the one
// place it ever points at - but an absolute path is taken as it stands, so the
// generator can be aimed somewhere harmless while a section is being written.
static void OutRoot(char* dst, size_t n)
{
    if ((g_outDir[0] && g_outDir[1] == ':') || g_outDir[0] == '\\' || g_outDir[0] == '/')
        strncpy_s(dst, n, g_outDir, _TRUNCATE);
    else
        _snprintf_s(dst, n, _TRUNCATE, "%s\\%s", g_gameDir, g_outDir);
}

// Reads a whole text file into a heap buffer, NUL-terminated. Every file this
// touches is an ini or an mtl of a few kilobytes; the cap is there so a wrong
// path cannot ask for a gigabyte.
static char* ReadTextFile(const char* path, DWORD* sizeOut)
{
    if (sizeOut) *sizeOut = 0;

    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;

    DWORD size = GetFileSize(h, NULL);
    if (size == INVALID_FILE_SIZE || size > 4u * 1024 * 1024) { CloseHandle(h); return NULL; }

    char* buf = (char*)malloc((size_t)size + 1);
    if (!buf) { CloseHandle(h); return NULL; }

    DWORD got = 0;
    ReadFile(h, buf, size, &got, NULL);
    CloseHandle(h);
    buf[got] = 0;
    if (sizeOut) *sizeOut = got;
    return buf;
}

static bool WriteTextFile(const char* path, const char* text)
{
    HANDLE h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;

    DWORD wrote = 0;
    BOOL  ok = WriteFile(h, text, (DWORD)strlen(text), &wrote, NULL);
    CloseHandle(h);
    return ok != 0;
}

// One asset. `required` decides whether a miss is a failure or a line in the
// log: a mesh that is not there produces a building that crashes on the first
// frame, a missing .fire produces one that does not burn interestingly.
static bool CopyAsset(const char* src, const char* dst, bool required, const char* what)
{
    if (!FileExists(src))
    {
        if (required) Logf("building ERROR %s not found: %s", what, src);
        else if (g_verbose) Logf("building   no %s (%s) - skipped", what, src);
        return !required;
    }
    if (!CopyFileA(src, dst, FALSE))
    {
        Logf("building ERROR could not copy %s -> %s (%lu)", src, dst, GetLastError());
        return false;
    }
    if (g_verbose) Logf("building   %s <- %s", dst, src);
    return true;
}

// FNV-1a over everything that decides the output, so a folder is rebuilt when
// its declaration changed and left alone when it did not. A plugin startup that
// copies a 300 KB mesh per building on every launch is not worth the two lines
// this saves.
static unsigned long long HashInit(void) { return 14695981039346656037ULL; }

static unsigned long long HashBytes(unsigned long long h, const void* p, size_t n)
{
    const unsigned char* b = (const unsigned char*)p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ULL; }
    return h;
}

static unsigned long long HashStr(unsigned long long h, const char* s)
{
    return HashBytes(h, s, strlen(s) + 1);
}

// ---------------------------------------------------------------- ini tokens

// The first $TOKEN anywhere in the line, which is what the game's own parser
// matches - a `//` or a `--` in front of one does not make it a comment. Only
// the upper-case-and-underscore run is taken, so `$COST_WORK_BUILDING_KEYWORD
// $brick` yields the keyword and not the argument.
static bool FirstToken(const char* line, char* out, size_t n)
{
    out[0] = 0;
    const char* p = strchr(line, '$');
    if (!p) return false;

    size_t i = 0;
    out[i++] = '$';
    for (const char* q = p + 1; *q && i < n - 1; q++)
    {
        char c = *q;
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_') out[i++] = c;
        else break;
    }
    out[i] = 0;
    return i > 1;
}

static bool StartsWith(const char* s, const char* prefix)
{
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

static bool IsRecipeToken(const char* t)
{
    return strcmp(t, "$PRODUCTION") == 0
        || strcmp(t, "$CONSUMPTION") == 0
        || strcmp(t, "$CONSUMPTION_PER_SECOND") == 0;
}

// Does a line the section declares, opening with `mine`, replace a donor line
// opening with `theirs`? Equality, plus the four families in the header
// comment. Everything else survives, which is the half of this that matters:
// a clone that quietly loses a $CONNECTION_ROAD or a $COST_WORK phase is a
// building that cannot be delivered to or built.
static bool Replaces(const char* mine, const char* theirs)
{
    if (strcmp(mine, theirs) == 0) return true;

    if (StartsWith(mine, "$TYPE_")   && StartsWith(theirs, "$TYPE_"))   return true;
    if (StartsWith(mine, "$NAME")    && StartsWith(theirs, "$NAME"))    return true;

    // The visualisation counts $STORAGE_* lines from zero, so it belongs to
    // whatever set of storages is in force.
    if (StartsWith(mine, "$STORAGE") &&
        (StartsWith(theirs, "$STORAGE") || strcmp(theirs, "$RESOURCE_VISUALIZATION") == 0))
        return true;

    if (IsRecipeToken(mine) && IsRecipeToken(theirs)) return true;
    return false;
}

static bool DonorLineIsReplaced(const Decl* d, const char* token)
{
    // `name =` is emitted as a $NAME_STR without being a `line =`, so it has to
    // be in the strip set explicitly. Leaving it out left the donor's own
    // `$NAME 6260` in the file below ours, which is a building whose name comes
    // out of a language file rather than out of the config.
    if (d->name[0] && Replaces("$NAME_STR", token)) return true;

    for (int i = 0; i < d->lines; i++)
    {
        char mine[TOKEN_LEN];
        if (!FirstToken(d->line[i], mine, sizeof(mine))) continue;
        if (Replaces(mine, token)) return true;
    }
    for (int i = 0; i < d->strips; i++)
        if (Replaces(d->strip[i], token)) return true;
    return false;
}

// ---------------------------------------------------------------- generation

// A growable output buffer, because building.ini runs to a couple of hundred
// lines and the pieces are appended from four places.
struct Out
{
    char*  buf;
    size_t len;
    size_t cap;
};

static bool OutInit(Out* o, size_t cap)
{
    o->buf = (char*)malloc(cap);
    o->len = 0;
    o->cap = cap;
    if (o->buf) o->buf[0] = 0;
    return o->buf != NULL;
}

static void OutFree(Out* o) { free(o->buf); o->buf = NULL; }

static void OutAdd(Out* o, const char* s)
{
    if (!o->buf) return;
    size_t n = strlen(s);
    if (o->len + n + 1 > o->cap)
    {
        size_t want = (o->cap * 2 > o->len + n + 1) ? o->cap * 2 : o->len + n + 1024;
        char*  p = (char*)realloc(o->buf, want);
        if (!p) return;
        o->buf = p;
        o->cap = want;
    }
    memcpy(o->buf + o->len, s, n + 1);
    o->len += n;
}

static void OutAddF(Out* o, const char* fmt, ...)
{
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(line, sizeof(line), _TRUNCATE, fmt, ap);
    va_end(ap);
    OutAdd(o, line);
}

// $TEXTURE_MTL resolves next to the .mtl file. A donor material sits in
// media_soviet\buildings\, so every one of its relative paths is
// buildings/<path> once the file has moved into a Workshop item - and
// $TEXTURE, unlike $TEXTURE_MTL, resolves against media_soviet\ wherever the
// file is. Nothing else in the file is touched.
static bool RewriteMaterial(const char* src, const char* dst, int* rewritten)
{
    *rewritten = 0;

    char* text = ReadTextFile(src, NULL);
    if (!text) { Logf("building ERROR could not read material %s", src); return false; }

    Out o;
    if (!OutInit(&o, 8192)) { free(text); return false; }

    char* ctx = NULL;
    for (char* line = strtok_s(text, "\n", &ctx); line; line = strtok_s(NULL, "\n", &ctx))
    {
        // strtok leaves the \r of a CRLF file on the end of the line.
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\r')) line[--n] = 0;

        char* at = strstr(line, "$TEXTURE_MTL");
        if (!at) { OutAdd(&o, line); OutAdd(&o, "\r\n"); continue; }

        // "$TEXTURE_MTL <slot> <path>" -> "$TEXTURE <slot> buildings/<path>"
        char* rest = at + strlen("$TEXTURE_MTL");
        while (*rest == ' ' || *rest == '\t') rest++;

        char* slotEnd = rest;
        while (*slotEnd && *slotEnd != ' ' && *slotEnd != '\t') slotEnd++;
        char slot[16] = "0";
        size_t sl = (size_t)(slotEnd - rest);
        if (sl && sl < sizeof(slot)) { memcpy(slot, rest, sl); slot[sl] = 0; }

        char* path = slotEnd;
        while (*path == ' ' || *path == '\t') path++;
        Trim(path);

        *at = 0;                                   // whatever preceded the token
        OutAddF(&o, "%s$TEXTURE %s buildings/%s\r\n", line, slot, path);
        (*rewritten)++;
    }

    bool ok = WriteTextFile(dst, o.buf ? o.buf : "");
    OutFree(&o);
    free(text);
    if (!ok) Logf("building ERROR could not write material %s", dst);
    return ok;
}

// The donor's building.ini with the declared block in front of it and every
// line the block replaces taken out. In front rather than behind, because a
// handful of base files end in a bare `end` and nothing is known about whether
// the parser stops there - and because order does not otherwise matter to
// anything this touches: the one token where it would, $RESOURCE_VISUALIZATION,
// is dropped whenever the storages are re-declared.
static bool WriteBuildingIni(const Decl* d, const char* donorIni, const char* dst, int* dropped)
{
    *dropped = 0;

    char* text = ReadTextFile(donorIni, NULL);
    if (!text) { Logf("building ERROR could not read donor %s", donorIni); return false; }

    Out o;
    if (!OutInit(&o, 32768)) { free(text); return false; }

    // No dollar sign anywhere in these three lines: the parser would read it as
    // a keyword, comment or not.
    OutAddF(&o, "; generated by tesmioloader plugins\\buildings.dll - section [%s]\r\n", d->section);
    OutAddF(&o, "; donor: media_soviet\\buildings_types\\%s.ini\r\n", d->donor);
    OutAdd(&o, "; edits here are overwritten on the next launch - change buildings.ini instead\r\n\r\n");

    if (d->name[0]) OutAddF(&o, "$NAME_STR \"%s\"\r\n", d->name);
    for (int i = 0; i < d->lines; i++) OutAddF(&o, "%s\r\n", d->line[i]);
    OutAdd(&o, "\r\n");

    char* ctx = NULL;
    for (char* line = strtok_s(text, "\n", &ctx); line; line = strtok_s(NULL, "\n", &ctx))
    {
        size_t n = strlen(line);
        while (n && line[n - 1] == '\r') line[--n] = 0;

        char token[TOKEN_LEN];
        if (FirstToken(line, token, sizeof(token)) && DonorLineIsReplaced(d, token))
        {
            (*dropped)++;
            if (g_verbose) Logf("building   dropped donor line: %s", line);
            continue;
        }
        OutAdd(&o, line);
        OutAdd(&o, "\r\n");
    }

    bool ok = WriteTextFile(dst, o.buf ? o.buf : "");
    OutFree(&o);
    free(text);
    if (!ok) Logf("building ERROR could not write %s", dst);
    return ok;
}

static bool WriteWorkshopConfig(const Decl* d, const char* dst)
{
    Out o;
    if (!OutInit(&o, 4096)) return false;

    OutAddF(&o, "$ITEM_ID %s\r\n\r\n", d->id);
    OutAdd(&o, "$OWNER_ID 0\r\n\r\n");
    OutAdd(&o, "$ITEM_TYPE WORKSHOP_ITEMTYPE_BUILDING\r\n\r\n");
    OutAdd(&o, "$VISIBILITY 0\r\n");
    OutAddF(&o, "$OBJECT_BUILDING %s\r\n\r\n", d->object);
    OutAddF(&o, "$ITEM_NAME \"%s\"\r\n\r\n", d->name[0] ? d->name : d->object);
    OutAddF(&o, "$ITEM_DESC \"%s\"\r\n\r\n", d->desc[0] ? d->desc : "Generated by tesmioloader.");
    OutAdd(&o, "$END\r\n");

    bool ok = WriteTextFile(dst, o.buf ? o.buf : "");
    OutFree(&o);
    return ok;
}

// MATERIALEMISSIVE is not optional when the donor has an _e.mtl. 159 of the 493
// base building materials ship one - the lit-window glow - and a mesh built for
// it renders with a null node array and takes the process down in
// C3D_MESH::Render on the first frame. That is the whole reason this is a
// parameter rather than a constant.
static bool WriteRenderConfig(const Decl* d, const char* dst, bool emissive)
{
    Out o;
    if (!OutInit(&o, 2048)) return false;

    OutAdd(&o, "$TYPE_WORKSHOP\r\n");
    OutAdd(&o, " MODEL model.nmf\r\n");
    OutAdd(&o, " MATERIAL ../material.mtl\r\n");
    if (emissive) OutAdd(&o, " MATERIALEMISSIVE ../material_e.mtl\r\n");
    OutAddF(&o, " LIFE %d.000000\r\n", d->life);
    OutAdd(&o, " EXPLOSION_GROUP 0\r\n");
    OutAdd(&o, " DERBIS_FALLING_FX buildingfall1 1.000000\r\n");
    OutAdd(&o, " DERBIS_FALLED_FX buildingfall2 1.400000\r\n");
    OutAdd(&o, " DERBIS_FALLED_SFX collapse\r\n");
    OutAdd(&o, " DERBIS_NUM 20\r\n");
    OutAdd(&o, " DERBIS_FALLING_FX_MAXTIME 3.000000\r\n");
    OutAdd(&o, " DERBIS_SCALE 1.000000\r\n");
    OutAdd(&o, " DERBIS_MESH buildings/buildingwreck1.nmf buildings/buildingwreck.mtl\r\n");
    OutAdd(&o, " DERBIS_MESH buildings/buildingwreck2.nmf buildings/buildingwreck.mtl\r\n");
    OutAdd(&o, " END\r\n");

    bool ok = WriteTextFile(dst, o.buf ? o.buf : "");
    OutFree(&o);
    return ok;
}

static unsigned long long DeclHash(const Decl* d, const char* donorIni)
{
    unsigned long long h = HashInit();
    int ver = GENERATOR_VERSION;
    h = HashBytes(h, &ver, sizeof(ver));
    h = HashStr(h, d->id);
    h = HashStr(h, d->object);
    h = HashStr(h, d->donor);
    h = HashStr(h, d->name);
    h = HashStr(h, d->desc);
    h = HashBytes(h, &d->life, sizeof(d->life));
    for (int i = 0; i < d->lines; i++)  h = HashStr(h, d->line[i]);
    for (int i = 0; i < d->strips; i++) h = HashStr(h, d->strip[i]);

    // The donor's own file, so a game patch that changes it regenerates too.
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (GetFileAttributesExA(donorIni, GetFileExInfoStandard, &fa))
    {
        h = HashBytes(h, &fa.nFileSizeLow, sizeof(fa.nFileSizeLow));
        h = HashBytes(h, &fa.ftLastWriteTime, sizeof(fa.ftLastWriteTime));
    }
    return h;
}

#define STAMP_NAME "tesmioloader.stamp"

static bool StampMatches(const char* item, unsigned long long want)
{
    char path[MAX_PATH];
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\" STAMP_NAME, item);

    char* text = ReadTextFile(path, NULL);
    if (!text) return false;

    unsigned long long have = 0;
    const char* at = strstr(text, "hash=");
    if (at) have = _strtoui64(at + 5, NULL, 16);
    free(text);
    return have == want;
}

static void Generate(Decl* d)
{
    char donorIni[MAX_PATH], item[MAX_PATH], obj[MAX_PATH], src[MAX_PATH], dst[MAX_PATH];

    _snprintf_s(donorIni, sizeof(donorIni), _TRUNCATE,
                "%s\\buildings_types\\%s.ini", g_media, d->donor);
    if (!FileExists(donorIni))
    {
        Logf("building \"%s\": donor \"%s\" has no %s - nothing generated",
             d->section, d->donor, donorIni);
        return;
    }

    char outRoot[MAX_PATH];
    OutRoot(outRoot, sizeof(outRoot));
    _snprintf_s(item, sizeof(item), _TRUNCATE, "%s\\%s", outRoot, d->id);
    _snprintf_s(obj,  sizeof(obj),  _TRUNCATE, "%s\\%s", item, d->object);

    unsigned long long want = DeclHash(d, donorIni);

    // An id collision with a real Workshop item would otherwise overwrite
    // somebody's subscription. A folder we did not write has no stamp.
    if (DirExists(item))
    {
        char stamp[MAX_PATH];
        _snprintf_s(stamp, sizeof(stamp), _TRUNCATE, "%s\\" STAMP_NAME, item);
        if (!FileExists(stamp))
        {
            Logf("building \"%s\": %s exists and was not written by this plugin "
                 "(no " STAMP_NAME ") - refusing to touch it. Give the section another id",
                 d->section, item);
            return;
        }
        if (!g_always && StampMatches(item, want))
        {
            Logf("building \"%s\" -> %s  up to date", d->section, d->id);
            return;
        }
    }

    // media_soviet\workshop_wip may itself be absent on a clean install.
    CreateDirectoryA(outRoot, NULL);
    if (!CreateDirectoryA(item, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
    {
        Logf("building ERROR could not create %s (%lu)", item, GetLastError());
        return;
    }
    if (!CreateDirectoryA(obj, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
    {
        Logf("building ERROR could not create %s (%lu)", obj, GetLastError());
        return;
    }

    bool ok = true;

    _snprintf_s(src, sizeof(src), _TRUNCATE, "%s\\buildings\\%s.nmf", g_media, d->donor);
    _snprintf_s(dst, sizeof(dst), _TRUNCATE, "%s\\model.nmf", obj);
    ok &= CopyAsset(src, dst, true, "mesh");

    _snprintf_s(src, sizeof(src), _TRUNCATE, "%s\\buildings_types\\%s.bbox", g_media, d->donor);
    _snprintf_s(dst, sizeof(dst), _TRUNCATE, "%s\\building.bbox", obj);
    ok &= CopyAsset(src, dst, false, "collision box");

    _snprintf_s(src, sizeof(src), _TRUNCATE, "%s\\buildings_types\\%s.fire", g_media, d->donor);
    _snprintf_s(dst, sizeof(dst), _TRUNCATE, "%s\\building.fire", obj);
    ok &= CopyAsset(src, dst, false, "fire points");

    _snprintf_s(src, sizeof(src), _TRUNCATE, "%s\\editor\\tool_%s.png", g_media, d->donor);
    _snprintf_s(dst, sizeof(dst), _TRUNCATE, "%s\\imagegui.png", obj);
    ok &= CopyAsset(src, dst, false, "build-menu icon");
    _snprintf_s(dst, sizeof(dst), _TRUNCATE, "%s\\previewimage.png", item);
    CopyAsset(src, dst, false, "preview image");

    int rewrote = 0, rewroteE = 0;
    _snprintf_s(src, sizeof(src), _TRUNCATE, "%s\\buildings\\%s.mtl", g_media, d->donor);
    _snprintf_s(dst, sizeof(dst), _TRUNCATE, "%s\\material.mtl", item);
    if (FileExists(src)) ok &= RewriteMaterial(src, dst, &rewrote);
    else { Logf("building ERROR no material %s", src); ok = false; }

    bool emissive = false;
    _snprintf_s(src, sizeof(src), _TRUNCATE, "%s\\buildings\\%s_e.mtl", g_media, d->donor);
    if (FileExists(src))
    {
        _snprintf_s(dst, sizeof(dst), _TRUNCATE, "%s\\material_e.mtl", item);
        emissive = RewriteMaterial(src, dst, &rewroteE);
        ok &= emissive;
    }

    _snprintf_s(dst, sizeof(dst), _TRUNCATE, "%s\\workshopconfig.ini", item);
    ok &= WriteWorkshopConfig(d, dst);

    _snprintf_s(dst, sizeof(dst), _TRUNCATE, "%s\\renderconfig.ini", obj);
    ok &= WriteRenderConfig(d, dst, emissive);

    int dropped = 0;
    _snprintf_s(dst, sizeof(dst), _TRUNCATE, "%s\\building.ini", obj);
    ok &= WriteBuildingIni(d, donorIni, dst, &dropped);

    if (!ok)
    {
        Logf("building \"%s\" -> %s  INCOMPLETE - the game may refuse it or crash on it",
             d->section, d->id);
        return;
    }

    char stampText[512];
    _snprintf_s(stampText, sizeof(stampText), _TRUNCATE,
                "tesmioloader plugins\\buildings.dll generated this folder.\r\n"
                "section=%s donor=%s object=%s\r\n"
                "hash=%016llX\r\n"
                "Delete this file to make the plugin leave the folder alone;\r\n"
                "delete the folder to have it written again.\r\n",
                d->section, d->donor, d->object, want);
    _snprintf_s(dst, sizeof(dst), _TRUNCATE, "%s\\" STAMP_NAME, item);
    WriteTextFile(dst, stampText);

    Logf("building \"%s\" -> %s\\%s  from \"%s\": %d line(s) in, %d donor line(s) out, "
         "%d texture path(s) rewritten%s",
         d->section, d->id, d->object, d->donor,
         d->lines + (d->name[0] ? 1 : 0), dropped, rewrote + rewroteE,
         emissive ? ", emissive material" : "");
}

// ---------------------------------------------------------------- the config

// [buildings] is the switches and every other section is a building, the way
// deposits.ini already reads. Parsed by hand rather than through the profile
// API for two reasons: the API cannot enumerate a section's keys, and `line`
// and `desc` are deliberately repeatable - a building.ini block is a list, and
// one key per line is the only shape in which it stays readable.
static void AppendMulti(char* dst, size_t cap, const char* text)
{
    size_t have = strlen(dst);
    if (have && have + 2 < cap) { dst[have++] = '\n'; dst[have] = 0; }
    strncat_s(dst, cap, text, _TRUNCATE);
}

static void LoadRegistry(void)
{
    char path[MAX_PATH];
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\plugins\\buildings.ini", g_baseDir);

    char* text = ReadTextFile(path, NULL);
    if (!text)
    {
        Logf("building no plugins\\buildings.ini - nothing declared");
        return;
    }

    Decl* d = NULL;
    char* ctx = NULL;
    for (char* line = strtok_s(text, "\n", &ctx); line; line = strtok_s(NULL, "\n", &ctx))
    {
        Trim(line);
        if (!line[0] || line[0] == ';' || line[0] == '#') continue;

        if (line[0] == '[')
        {
            d = NULL;
            char* end = strchr(line, ']');
            if (!end) continue;
            *end = 0;

            const char* name = line + 1;
            if (_stricmp(name, "buildings") == 0) continue;      // the switches
            if (g_declCount >= MAX_BUILDINGS)
            {
                Logf("building at most %d section(s) - \"%s\" ignored", MAX_BUILDINGS, name);
                continue;
            }

            d = &g_decl[g_declCount++];
            memset(d, 0, sizeof(*d));
            strncpy_s(d->section, sizeof(d->section), name, _TRUNCATE);
            strncpy_s(d->object,  sizeof(d->object),  name, _TRUNCATE);
            d->life    = 3000;
            d->enabled = 1;
            continue;
        }

        if (!d) continue;

        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char* key = line;
        char* val = eq + 1;
        Trim(key);
        Trim(val);

        if      (_stricmp(key, "id")      == 0) strncpy_s(d->id,     sizeof(d->id),     val, _TRUNCATE);
        else if (_stricmp(key, "object")  == 0) strncpy_s(d->object, sizeof(d->object), val, _TRUNCATE);
        else if (_stricmp(key, "donor")   == 0) strncpy_s(d->donor,  sizeof(d->donor),  val, _TRUNCATE);
        else if (_stricmp(key, "name")    == 0) strncpy_s(d->name,   sizeof(d->name),   val, _TRUNCATE);
        else if (_stricmp(key, "desc")    == 0) AppendMulti(d->desc, sizeof(d->desc), val);
        else if (_stricmp(key, "life")    == 0) d->life    = atoi(val);
        else if (_stricmp(key, "enabled") == 0) d->enabled = atoi(val);
        else if (_stricmp(key, "line")    == 0)
        {
            if (d->lines < MAX_LINES && val[0])
                strncpy_s(d->line[d->lines++], LINE_LEN, val, _TRUNCATE);
        }
        else if (_stricmp(key, "strip")   == 0)
        {
            if (d->strips < MAX_LINES && val[0])
                strncpy_s(d->strip[d->strips++], TOKEN_LEN, val, _TRUNCATE);
        }
        else
            Logf("building \"%s\": unknown key \"%s\" - ignored", d->section, key);
    }

    free(text);
}

static void ReadSettings(void)
{
    const char* ini = "plugins\\buildings.ini";
    char v[MAX_PATH];

    g_enabled = H->configInt(ini, "buildings", "enabled",  g_enabled);
    g_always  = H->configInt(ini, "buildings", "always",   g_always);
    g_verbose = H->configInt(ini, "buildings", "verbose",  g_verbose);

    if (H->configString(ini, "buildings", "out", v, sizeof(v), "") && v[0])
        strncpy_s(g_outDir, sizeof(g_outDir), v, _TRUNCATE);
}

// The game's own folder, from the executable rather than from the working
// directory: the launcher does set the cwd, but a plugin that depends on that
// breaks the moment anything in the game calls SetCurrentDirectory - and the
// world editor does.
static bool ResolveGameDir(void)
{
    char exe[MAX_PATH];
    if (!GetModuleFileNameA((HMODULE)H->exeModule, exe, sizeof(exe))) return false;

    char* slash = strrchr(exe, '\\');
    if (!slash) return false;
    *slash = 0;

    strncpy_s(g_gameDir, sizeof(g_gameDir), exe, _TRUNCATE);
    _snprintf_s(g_media, sizeof(g_media), _TRUNCATE, "%s\\media_soviet", g_gameDir);
    return DirExists(g_media);
}

extern "C" __declspec(dllexport) unsigned TsmPluginApiVersion(void)
{
    return TSM_API_VERSION;
}

// Everything happens here, and it has to. Plugins are initialised from
// DllMain in a process whose main thread has not run yet, so a folder written
// now is a folder the game finds when it scans workshop_wip. There is nothing
// to do in Start and nothing to hook, so this returns 0 rather than declining:
// a plugin that logged what it generated should stay in the list.
extern "C" __declspec(dllexport) int TsmPluginInit(const TsmHost* host, TsmPluginInfo* info)
{
    TsmBind(host);
    info->name    = "buildings";
    info->version = "1.0";

    ReadSettings();
    if (!g_enabled)
    {
        Logf("building enabled = 0 - no buildings generated");
        return 1;
    }

    if (!ResolveGameDir())
    {
        Logf("building could not find media_soviet beside SOVIET64.exe - nothing generated");
        return 1;
    }

    LoadRegistry();
    if (g_declCount == 0)
    {
        Logf("building nothing declared in plugins\\buildings.ini");
        return 1;
    }

    char outRoot[MAX_PATH];
    OutRoot(outRoot, sizeof(outRoot));
    Logf("building generating into %s", outRoot);

    int made = 0;
    for (int i = 0; i < g_declCount; i++)
    {
        Decl* d = &g_decl[i];

        if (!d->enabled) { Logf("building \"%s\" enabled = 0 - skipped", d->section); continue; }
        if (!d->id[0])   { Logf("building \"%s\" has no id - skipped", d->section);   continue; }
        if (!d->donor[0]){ Logf("building \"%s\" has no donor - skipped", d->section); continue; }

        __try { Generate(d); made++; }
        __except (FaultFilter("buildings generate", GetExceptionInformation()))
        {
            Logf("building \"%s\" faulted while generating - skipped", d->section);
        }
    }

    Logf("building %d section(s) processed", made);
    return 0;
}

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) { return TRUE; }
