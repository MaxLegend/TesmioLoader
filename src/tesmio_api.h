// tesmio_api.h - the contract between tesmioloader and a plugin.
//
// A plugin is an ordinary DLL in tesmioloader\plugins\. The loader finds it,
// checks that it was built against a compatible version of this header, and
// hands it a table of the things the loader already knows how to do: where the
// executable is, how to patch an import, how to splice a hook, how to log, and
// what deposits.ini declared.
//
// Everything crossing the boundary is a POD or a C string. No CRT object, no
// allocation ownership, no C++ type. The loader and every plugin are built
// /MT, so each has its own CRT and nothing may be freed across the boundary.
//
//   #include "../../src/tesmio_api.h"
//
//   static const TsmHost* g_host;
//
//   extern "C" __declspec(dllexport) unsigned TsmPluginApiVersion(void)
//   {
//       return TSM_API_VERSION;
//   }
//
//   extern "C" __declspec(dllexport) int TsmPluginInit(const TsmHost* host, TsmPluginInfo* info)
//   {
//       g_host = host;
//       info->name    = "example";
//       info->version = "1.0";
//       g_host->log("example  hello from a plugin");
//       return 0;                       // non-zero means "do not keep me loaded"
//   }
//
// Pointers handed out by the host stay valid for the life of the process.
//
// INIT AND START ARE TWO PHASES, and the split is what makes one plugin able to
// use another without either knowing the load order:
//
//   TsmPluginInit    read your config, and `provide` whatever interfaces you
//                    publish. Do not `consume` and do not hook anything.
//   TsmPluginStart   `consume` what you need and install your hooks. Every
//                    plugin's Init has run by the time any Start does.
//
// Start is optional. A plugin that depends on nothing can do everything in
// Init - depletion consumes the deposit registry, so it does not.

#ifndef TESMIO_API_H
#define TESMIO_API_H

#include <stddef.h>

// Bumped whenever anything below changes shape. The host refuses a plugin that
// reports a different number, because a stale plugin reading a moved field
// would corrupt the process rather than misbehave.
#define TSM_API_VERSION 3u

#ifdef __cplusplus
extern "C" {
#endif

// deposits.ini `map`, as an index: 0 is `resourcemap`, 1 is `resourcemap2`, and
// **anything above that is a map the deposits plugin creates itself** -
// `resourcemap3.dds`, `resourcemap4.dds` and so on, one file per index, loaded
// beside the game's own pair and written back with them on save. So the value
// is always `<the digit in the filename> - 1`.
//
// The first two live in the game object, at +0xF00 and +0xF08. The rest live
// nowhere the engine knows about, which is why anything that needs the texture
// asks TsmDepositApi::texture for it rather than reading an offset.
#define TSM_MAP_RESOURCEMAP   0     // gameobj + 0xF00
#define TSM_MAP_RESOURCEMAP2  1     // gameobj + 0xF08
#define TSM_MAP_EXTRA_FIRST   2     // resourcemap3 and up - the loader's own

// Not a resource map at all: the terrain's own material mask, at
// terrain+0x158 - the splat map the ground textures blend through, and where
// gravel lives. Numbered outside the maps because it behaves differently: it is
// painted by C3D_TERRAIN::EditMask, it has to be bracketed before it can be
// sampled, and mining it wears the ground away.
#define TSM_MAP_TERRAIN       64

// A read-only view of one deposits.ini section. `radius` is already resolved
// from a named constant to the float the game itself uses.
typedef struct TsmDeposit
{
    const char* name;
    const char* token;          // the building.ini token, $TYPE_MINE_...
    int         type;           // the deposit type number the engine keys on
    int         buildingType;   // 7 = mine
    int         map;            // TSM_MAP_*, and see TSM_MAP_EXTRA_FIRST
    int         component;      // 0..3
    float       radius;
    const char* icon;
} TsmDeposit;

typedef struct TsmHost
{
    unsigned    apiVersion;     // TSM_API_VERSION the host was built with
    unsigned    structSize;     // sizeof(TsmHost)

    // ---- the process ----
    void*          exeModule;   // HMODULE of SOVIET64.exe
    unsigned char* exeBase;     // add an RVA to this; ASLR is on
    size_t         exeSize;
    void*          engineModule;// HMODULE of C3DDLL64.dll
    const char*    baseDir;     // the folder tesmioloader.dll lives in
    const char*    pluginDir;   // baseDir\plugins

    // ---- logging ----
    // Goes to tesmioloader.log with a timestamp. Prefix the message with the
    // plugin's own short name; every subsystem in the loader does.
    void (*log)(const char* fmt, ...);

    // ---- patching ----
    //
    // In order of preference, and the earlier ones survive a game update far
    // better: an import slot, then a vtable slot (which needs no host help),
    // then a data pointer, and only then spliced code.

    // Walks a module's import directory. Returns the slot, or null.
    void** (*findIatSlot)(void* module, const char* dll, const char* fn);

    // Swaps one import and hands back what was there. Non-zero on success.
    int (*patchIat)(void* module, const char* dll, const char* fn,
                    void* detour, void** original, const char* label);

    // Relocates `stolen` bytes of prologue into a trampoline and writes a
    // 14-byte absolute jump over them. `expect` must match the site exactly or
    // nothing is written and this returns 0 - a game update has to make a hook
    // refuse, not corrupt the process. `stolen` must be at least 14 and must
    // end on an instruction boundary.
    int (*installInlineHook)(void* target, void* detour, void** trampoline,
                             const unsigned char* expect, size_t stolen,
                             const char* label);

    // Executable memory within +-2GB of `anchor`, for a code cave a `call
    // rel32` can reach. Never freed.
    unsigned char* (*allocNear)(unsigned char* anchor, size_t size);

    // True when the whole range is committed and readable. Walks the regions,
    // rather than trusting one VirtualQuery - see the comment on the host's
    // copy for why that matters.
    int (*readablePtr)(const void* p, size_t n);

    // For the filter expression of a __try around anything reading game
    // structures. Logs the faulting address as module + rva and returns
    // EXCEPTION_EXECUTE_HANDLER. Pass GetExceptionInformation().
    long (*faultFilter)(const char* what, void* exceptionPointers);

    // ---- configuration ----
    //
    // `iniName` is a bare file name, resolved against baseDir. Keep values
    // ASCII: these go through the ANSI profile API, and the ini files are
    // UTF-8. Never write one back with PowerShell's -Encoding UTF8 - the BOM
    // stops the section header matching and every setting silently falls back.
    int (*configInt)(const char* iniName, const char* section,
                     const char* key, int fallback);
    int (*configString)(const char* iniName, const char* section,
                        const char* key, char* out, int outSize,
                        const char* fallback);

    // ---- services ----
    //
    // How one plugin uses another without either knowing the load order. The
    // host is only a noticeboard: it never looks inside an interface and has
    // no opinion on what any of them mean.
    //
    // `iface` must outlive the process - a file-scope static is the obvious
    // choice. Provide from Init, consume from Start.
    int         (*provide)(const char* service, unsigned version, const void* iface);
    const void* (*consume)(const char* service, unsigned version);

} TsmHost;

// ---- well-known services -------------------------------------------------
//
// Declared here rather than in each provider's own header so a consumer needs
// one include. Nothing in the host implements these; the named plugin does, and
// consume() returns null when that plugin is not installed.

// Provided by `deposits`. deposits.ini, parsed and validated: a section that
// would have produced a broken patch has already been dropped.
#define TSM_SERVICE_DEPOSITS  "deposits"
#define TSM_DEPOSITS_VERSION  2u

typedef struct TsmDepositApi
{
    int  (*count)(void);
    int  (*get)(int index, TsmDeposit* out);         // non-zero on success

    // Any key of that section the deposits plugin itself does not understand,
    // so another plugin can carry its own per-deposit settings in the same
    // file. Null when the key is absent.
    const char* (*setting)(int index, const char* key);

    // The live C3DAPI_TEXTURE this deposit's channel lives in, whichever map
    // that is - the game object's own two included, so a caller never has to
    // know which kind it got. NULL before a world is loaded, and a fresh
    // pointer after every load: the maps are rebuilt with the world, so this
    // must be asked for each time rather than cached.
    void* (*texture)(int index);
} TsmDepositApi;

// Provided by `resources`. The mod resources published into the engine's own
// resource vector, by the index the engine ended up giving each.
#define TSM_SERVICE_RESOURCES "resources"
#define TSM_RESOURCES_VERSION 1u

typedef struct TsmResourceApi
{
    int         (*count)(void);
    const char* (*name)(int i);
    int         (*index)(int i);                     // engine index, -1 if unpublished
    int         (*indexOf)(const char* name);        // -1 when not a mod resource
} TsmResourceApi;

// Filled in by the plugin. The strings must outlive the call - a literal is
// the obvious choice.
typedef struct TsmPluginInfo
{
    const char* name;
    const char* version;
} TsmPluginInfo;

// ApiVersion and Init are required. ApiVersion is called first and must do
// nothing else: it is what decides whether Init may be called at all.
//
// Start is optional and runs after every plugin's Init, which is the only point
// at which consume() can see everything. Returning non-zero from either means
// "nothing hooked, unload me" - and it must be true, because a plugin that
// installed an inline hook can never be unloaded.
typedef unsigned (*TsmPluginApiVersionFn)(void);
typedef int      (*TsmPluginInitFn)(const TsmHost* host, TsmPluginInfo* info);
typedef int      (*TsmPluginStartFn)(void);

#define TSM_EXPORT_APIVERSION "TsmPluginApiVersion"
#define TSM_EXPORT_INIT       "TsmPluginInit"
#define TSM_EXPORT_START      "TsmPluginStart"

#ifdef __cplusplus
}
#endif

#endif // TESMIO_API_H
