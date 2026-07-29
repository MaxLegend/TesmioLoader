# Plugins

A plugin is an ordinary DLL in `build/plugins/`. The loader finds it at startup,
checks it was built against a compatible `src/tesmio_api.h`, and hands it a
table of the things the loader already knows how to do. The plugin patches the
game from there.

Removing a feature is deleting its DLL. Adding one is dropping a folder in
`plugins/` and rebuilding — nothing in `build.bat` or in the loader lists them
by name.

## What is a plugin and what is not

The loader keeps only what is **infrastructure** — what has to exist before any
plugin runs, and what more than one of them may want:

| In the loader | Why |
|---|---|
| the VFS and the file hooks | every plugin's assets come through it |
| the log | one file, one timestamp order |
| IAT and inline hooking, `AllocNear`, `ReadablePtr` | the techniques, not any use of them |
| the crash handler and the guard-page probe | process-wide, and diagnostic |
| the main-menu version line | one `lea` displacement; there is nothing to modularise |
| the plugin host itself | |

Everything that is a **feature** is a plugin, including the three this project
started with:

| Plugin | What | Doc |
|---|---|---|
| `resources` | resources the base game does not have. Hooks `ResourceGet`, publishes records into the engine's own vector | [04](04-adding-resources.md) |
| `deposits` | deposit types: the code patch, the minimap layer, the editor brush | [05](05-deposits.md) |
| `depletion` | deposits that run out | [08](08-depletion.md) |
| `accumulator` | batteries for the electric grid | [10](10-accumulator.md) |
| `needs` | resources the citizens buy in a shop | [11](11-needs.md) |
| `walking` | how far a citizen walks | [12](12-walking.md) |

The split is not about safety. A plugin is in the same address space and can
corrupt the process exactly as easily as the loader can — there is no sandbox
and there was never going to be one. It is about being able to write, rebuild
and remove a feature without touching a shared file, and about shipping one by
copying a DLL.

## Writing one

```
plugins/
  yourthing/
    yourthing.cpp        required, and the folder name decides the DLL name
    yourthing.ini        optional, copied next to the DLL
```

**One ini per plugin, and it holds everything that plugin needs** — settings in
a `[yourthing]` section, and whatever content it declares in sections of its
own. `resources` and `deposits` both do this: `[list]` and `[copper]` sit in the
same file as the switches that drive them. There is nothing in the loader's own
folder for a plugin to read, and deleting a DLL leaves no stray file behind.

`build.bat` compiles every `plugins/<name>/<name>.cpp` into
`build/plugins/<name>.dll` and copies `<name>.ini` beside it.

The whole of the minimum:

```cpp
#include "../../src/tesmio_plugin.h"

extern "C" __declspec(dllexport) unsigned TsmPluginApiVersion(void)
{
    return TSM_API_VERSION;
}

extern "C" __declspec(dllexport) int TsmPluginInit(const TsmHost* host, TsmPluginInfo* info)
{
    TsmBind(host);
    info->name    = "yourthing";
    info->version = "1.0";

    if (!H->configInt("plugins\\yourthing.ini", "yourthing", "enabled", 1))
        return 1;                       // non-zero: nothing hooked, unload me

    Logf("yourthing  installed");
    return 0;
}

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) { return TRUE; }
```

[`src/tesmio_plugin.h`](../src/tesmio_plugin.h) is the boilerplate every plugin
would otherwise repeat. It binds the host table to `H`, `g_exe`, `g_exeBase`,
`Logf`, `FindIatSlot`, `InstallInlineHook`, `ReadablePtr`, `FaultFilter` and a
few small helpers, with the same shapes the loader itself uses — so code moved
out of the loader compiles unchanged, and a new plugin reads like the loader
rather than like a foreign thing bolted to it. Header-only and everything in it
`static`, so each plugin gets its own copy; nothing but PODs crosses the
boundary.

### The two phases

```cpp
extern "C" __declspec(dllexport) int TsmPluginStart(void)   // optional
```

| Phase | Do | Do not |
|---|---|---|
| `TsmPluginInit` | read config, `provide` your interfaces | `consume`, hook anything |
| `TsmPluginStart` | `consume` what you need, install hooks | |

Every plugin's `Init` runs before any `Start`, which is what lets one plugin use
another **without either knowing the load order**. `depletion` and `deposits`
are exactly that case: alphabetically `depletion` loads first, but it consumes
the deposit registry in `Start`, by which time `deposits` has published it.

A plugin that depends on nothing can do everything in `Init` and omit `Start`.

Returning non-zero from either tells the loader the plugin declined — it is
freed and never called again. **Only return non-zero if nothing was hooked**,
because a plugin that installed an inline hook can never be unloaded: the game
would jump into freed memory. That is also why there is no uninstall in the API.

## The host table

`TsmHost`, in [src/tesmio_api.h](../src/tesmio_api.h). Everything crossing the
boundary is a POD or a C string; the loader and every plugin are built `/MT`, so
each has its own CRT and **nothing may be freed across the boundary**. Pointers
the host hands out stay valid for the life of the process.

| Field | What |
|---|---|
| `exeBase`, `exeSize`, `exeModule` | `SOVIET64.exe`. Add an RVA to `exeBase`; ASLR is on and nothing may be hard-coded absolute |
| `engineModule` | `C3DDLL64.dll` |
| `baseDir`, `pluginDir` | where the loader and the plugins live |
| `log` | one line into `tesmioloader.log`. Prefix it with the plugin's short name, as every subsystem does |
| `findIatSlot`, `patchIat` | import table. **First choice** for anything named — no fixed addresses, survives a game update |
| `installInlineHook` | relocates a prologue into a trampoline and writes a 14-byte absolute jump. Compares the site against `expect` first and refuses on a mismatch |
| `allocNear` | executable memory within ±2 GB, for a cave a `call rel32` can reach |
| `readablePtr` | is this range committed and readable |
| `faultFilter` | for the filter expression of a `__try` around anything reading game structures |
| `configInt`, `configString` | a key out of an ini resolved against `baseDir` |
| `provide`, `consume` | the service noticeboard, below |

A vtable swap needs no host help at all — the vtable is in the engine and the
plugin can write it directly. That is still the second-best technique after an
import swap; see [01-architecture.md](01-architecture.md).

## Services

How one plugin uses another. The host is only a noticeboard: it never looks
inside an interface and has no opinion on what any of them mean.

```cpp
// in the provider's Init
static const TsmDepositApi kApi = { svc_Count, svc_Get, svc_Setting };
H->provide(TSM_SERVICE_DEPOSITS, TSM_DEPOSITS_VERSION, &kApi);

// in the consumer's Start
D = (const TsmDepositApi*)H->consume(TSM_SERVICE_DEPOSITS, TSM_DEPOSITS_VERSION);
if (!D) Logf("yourthing  no deposits plugin");
```

The interface must outlive the process — a file-scope `static const` is the
obvious choice. `consume` returns null when nobody provided that name at that
version, and **a consumer has to cope with that**: it means the providing plugin
is simply not installed, which is the whole point of the architecture.

Two services exist, both declared in `tesmio_api.h` so a consumer needs one
include:

| Service | Provider | What |
|---|---|---|
| `TSM_SERVICE_DEPOSITS` | `deposits` | `deposits.ini`, parsed and validated |
| `TSM_SERVICE_RESOURCES` | `resources` | which engine index each mod resource ended up with |

### Per-deposit settings

`TsmDepositApi::setting(i, key)` returns any key of a `plugins\deposits.ini` section the
deposits plugin itself does not understand. It keeps up to eight per section
verbatim and has no opinion on what they mean, so another plugin carries its own
settings in the same file the deposit is declared in:

```ini
[copper]
token     = $TYPE_MINE_COPPER
type      = 10
...
deplete   = 4000        ; nothing in `deposits` reads this
```

```cpp
if (const char* s = D->setting(i, "deplete")) tonnes = (float)atof(s);
```

## Versioning

`TSM_API_VERSION` is bumped whenever anything in `tesmio_api.h` changes shape.
It is at **2**; version 1 had no `Start` phase and carried the deposit registry
in the host table.

The loader calls `TsmPluginApiVersion` **before** `TsmPluginInit` and does not
call `Init` at all on a mismatch. That ordering is the point: a plugin built
against an older header would be reading moved fields out of the host table,
which corrupts the process rather than misbehaving. `TsmPluginApiVersion` must
therefore do nothing but return the constant.

`TsmHost::structSize` is there so a plugin can tell whether a field it wants
exists in an older host, if the version ever grows compatibly. Nothing uses it
yet.

## Load order

Plugins load **last** in the loader's init, after every import swap and with the
VFS live and the crash handler armed, so a plugin sees a fully built loader.

Within the folder they load in the order `FindFirstFile` returns, which on NTFS
is alphabetical. **That order should not matter**: anything one plugin needs
from another goes through a service, and the Init/Start split makes both phases
independent of it. Ordering by name is a last resort, not a mechanism.

`plugins = 0` in `tesmioloader.ini` skips the folder entirely. It is the first
thing to try when the game will not start.

## Turning one off

A `[plugins]` section in `tesmioloader.ini`, one key per DLL named after the file
without its extension. **A key that is not there counts as 1**, so the section is
empty until something is turned off:

```ini
[plugins]
needs = 0
```

```
plugin   needs.dll        off in tesmioloader.ini [plugins]
plugin   5 loaded
```

The check happens in `LoadPlugins` before `LoadLibrary`, so a disabled plugin is
never mapped and its `DllMain` never runs.

This is what the launcher's checkboxes write, and the reason it is a config key
rather than a renamed or moved file: a plugin the user turned off is still on
disk, still listed in the window, and still one click from coming back. It also
means the state is readable by anything that can read an ini — the loader, the
launcher, and a person with a text editor — rather than being implied by which
files happen to be present.

`build\tesmioloader.ini` is therefore **live config, not a build output**.
`build.bat` copies the repo's copy over it only when it is not there yet;
delete it to take new defaults.

Three ways to switch a feature off, in order of how big the hammer is: its own
`enabled = 0` in its own ini, which still loads and initialises the DLL; the
`[plugins]` key, which does not load it at all; and `plugins = 0`, which skips
every plugin.

## What goes wrong, and what the log says

| Line | Meaning |
|---|---|
| `plugin   x.dll          off in tesmioloader.ini [plugins]` | its `[plugins]` key is 0. Never loaded, never initialised |
| `plugin   "x.dll" failed to load (126)` | a missing dependency, usually a non-`/MT` build |
| `plugin   "x.dll" is not a tesmioloader plugin` | one of the required exports is missing. Check `extern "C"` and `__declspec(dllexport)` |
| `plugin   "x.dll" wants API 1, this loader is 2` | rebuild it against the current header |
| `plugin   "x.dll" declined to install (1)` | `TsmPluginInit` returned non-zero, usually its own `enabled = 0` |
| `plugin   x started with 1 - inactive` | `TsmPluginStart` returned non-zero. It stays loaded, because by then it may already have hooked something |
| `plugin   service "s" v1 from x.dll` | a provider published |
| `FAULT    plugin init: ...` | it crashed while installing. The loader survives and carries on with the rest |

The loader wraps every call into a plugin in `__try`, so one that faults while
installing takes itself out rather than the process. Nothing wraps a plugin's
own hooks — that is the plugin's job, and `faultFilter` is in the table for it.

## A worked startup

```
plugin   depletion        1.0      from depletion.dll      <- Init: config only
deposits  "copper" type 10 "$TYPE_MINE_COPPER" -> ...
patch  deposit type 10 added
minimap  1 mod layer(s) hooked
editor   1 mod brush pair(s) hooked
plugin   service "deposits" v1 from deposits.dll
plugin   deposits         1.0      from deposits.dll
registry  "copper_ore" -> next free slot, template 18
hook ok      ResourceGet
plugin   service "resources" v1 from resources.dll
plugin   resources        1.0      from resources.dll
hook ok      mine tick                                     <- depletion's Start
deplete  copper     type 10  resourcemap2 component 3
plugin   3 loaded
```

Three Inits in alphabetical order, then the Starts. `depletion` hooks nothing
until its `Start`, which is after `deposits` published — which is why copper is
in its table at all.
