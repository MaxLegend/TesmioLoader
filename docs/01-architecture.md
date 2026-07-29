# Architecture

`tesmioloader` is a DLL injected into `SOVIET64.exe` at startup by a launcher.
It never modifies a file on disk.

It is a **host, not a mod**. The loader itself contains only infrastructure: the
VFS, the log, the hooking techniques, the crash handler and the plugin host.
Every feature — resources, deposit types, depletion — is a DLL in `plugins/`
that the loader hands a versioned table of what it knows how to do. See
[09-plugins.md](09-plugins.md).

Everything either of them does to the game is one of four things, listed here in
the order you should reach for them — the earlier ones survive game updates far
better.

## The four techniques

### 1. Import table entry swap — first choice

The game imports 733 named functions from `C3DDLL64.dll` and a few dozen from
the CRT and KERNEL32. Every one of those is a pointer in a table that can be
rewritten at runtime. No trampolines, no disassembly, no fixed addresses.

`FindIatSlot(module, dll, name)` walks `IMAGE_DIRECTORY_ENTRY_IMPORT`, matches
the DLL name case-insensitively, then walks `OriginalFirstThunk` for names in
lockstep with `FirstThunk` for addresses and hands back the slot.

**Both modules have their own import tables.** Patching only the executable
leaves everything the engine does for itself invisible — this cost several
sessions when texture loads went unseen. Hook `SOVIET64.exe` *and*
`C3DDLL64.dll`.

### 2. Virtual table slot swap

Anything the game calls through a C++ interface never appears in an import
table. `C3DAPI_D3D11_TEXTURE` is the case that matters: its vtable is an
exported symbol, so the slot can be located without guessing.

### 3. Data pointer swap

The resource vector is three pointers in `.data`. Rewriting `end` publishes a
new record; rewriting all three relocates the array. No code is touched.

### 3½. Rewriting one instruction's operand

Between a data pointer swap and real spliced code. When the pointer to redirect
is not in a table but in a `lea`'s displacement, four bytes of operand is still
a data redirect in every way that matters: no cave, no trampoline, no change to
what the instruction *does*.

The main menu's version line is the case. It is one
`lea rax,[rip+disp32]` at rva `0x28B55C` computing
`L"v%d.%d.%d.%d (64 bit DX11.1 - GPU: %ls)"`, and repointing it at a longer copy
of the same format string is the whole feature — see
[02-findings.md](02-findings.md) for the site.

The alternative was hooking `C3D_FONTMANAGER::PrintLeftUnicode` through the
import table, which is normally the first choice and here is the wrong one: it
is a **variadic** that every label in the game goes through, and a `va_list`
cannot be forwarded to a variadic callee, so the hook would have to re-format
every string in the UI through its own CRT to pass anything on. Prefer the
operand when only one call site is meant to change.

The replacement string is placed by `AllocNear`, because a `rip`-relative
displacement is 32 bits and the loader's own image is not guaranteed to be
within ±2 GB of the executable.

The `walking` plugin is the second case, and it shows why the operand rather
than the constant: its rebuild radius is `530.0f` at `0x90AF70`, in the shared
literal pool with sixty-odd unrelated readers. Writing over it would move panel
widths as well; repointing the one `movss` that fills in the path query moves
exactly walking. The walking distance itself, in the same plugin, is a bare
`imm32` with nothing to share and is simply replaced — see
[12-walking.md](12-walking.md).

### 4. Spliced code — last resort

Only when a decision is compiled into a chain of comparisons and there is no
data to redirect. Used for exactly one thing in the whole project, the deposit
type — and even there the emitted code is generated from a config table rather
than written out per deposit. It lives in the `deposits` plugin; see
[05-deposits.md](05-deposits.md).

## Injection

`tesmiolauncher.exe` creates the game suspended, allocates the DLL path in the
target, and calls `LoadLibraryW` through `CreateRemoteThread`, then resumes.

The ordering matters and is not accidental. In a suspended process the loader
has not run, but a remote thread triggers `LdrInitializeThunk` first, so by the
time `LoadLibraryW` executes the executable's imports are fully resolved — while
no game code has run yet. Hooks are therefore installed from `DllMain` against a
complete import table, ahead of the first file the game opens.

`kernel32.dll` sits at the same base in every process of a boot session, so the
launcher's own `LoadLibraryW` address is valid in the target.

The working directory is set to the game folder: the game uses relative paths
like `media_soviet/...` throughout.

## What is hooked

### Engine, through the executable's import table

| Symbol | Why |
|---|---|
| `C3DHelp_ReadFileIntoBuffer` | universal asset read — VFS |
| `C3DHelp_CheckIfFileExist` | existence must agree with the VFS |
| `C3DLog_PrintInfo` / `Warning` / `Error` | mirrors the game's own log into ours |

`C3D_LANGUAGE::GetString(int)` is hooked too, for mod resource captions, but by
the `resources` plugin rather than by the loader.

The log functions are variadic. A `va_list` cannot be forwarded to a variadic
callee, so the hook formats the text itself and passes the result on as `"%s"`.

### File opening, through **both** import tables

`fopen`, `fopen_s`, `_wfopen`, `_wfopen_s`, `fread`, `CreateFileA`,
`CreateFileW`, `CreateFile2`.

That list grew one entry at a time, each after an asset was found slipping past.
Assume it is still incomplete: when something is not being redirected, the first
question is which opener it used.

### Inline hooks

The loader installs none. It provides the mechanism — `installInlineHook`
relocates the prologue into a trampoline, writes `jmp qword ptr [rip+0]` over
it, and compares the site against the caller's expected bytes first so a game
update makes a hook refuse rather than corrupt the process — and the plugins use
it: `ResourceGet` (1), the minimap (2), the terrain editor (4), the mine tick
(1), the building dispatcher and the production tick (2).

Nine of the ten are additive — the original runs through the trampoline and the
plugin's work is appended. The exception is `accumulator`'s hook on the
production tick, which exists to **suppress** the original for one building for
the length of one call; see [10-accumulator.md](10-accumulator.md).

### Virtual table

`C3DAPI_D3D11_TEXTURE` vtable at `C3DDLL64.dll` rva `0x187BF0`, slots 2
(`Load2DFromFile`) and 20 (`TextureAccesGetTexel`). Load identifies which
texture object is which file; GetTexel observes deposit sampling.

## Subsystems

### Virtual file system

Any read whose path resolves under `tesmioloader/vfs/<same relative path>` is
served from there instead. Absolute paths are refused deliberately — the game
uses them for Workshop content, and redirecting those has never been wanted.

This is how mod resources get icons and cargo models, and how a terrain's
deposit map is replaced without touching the shipped file.

### Plugins

`build/plugins/*.dll` are scanned at startup and handed `TsmHost`, a versioned
table of what the loader knows how to do: where the executable is, how to swap
an import, how to splice a hook, how to log, how to read a config key, and a
noticeboard for publishing interfaces to each other. The contract is
`src/tesmio_api.h`; the mechanism and the two-phase init are in
[09-plugins.md](09-plugins.md).

Not a sandbox. A plugin is in the same address space and can corrupt the process
exactly as easily as the loader can. What it buys is that a feature can be
written, rebuilt and removed without touching this file, and that shipping one
is copying a DLL.

Six ship with the project:

| Plugin | What | Doc |
|---|---|---|
| `resources` | resources the base game does not have | [04](04-adding-resources.md) |
| `deposits` | deposit types, the minimap layer, the editor brush | [05](05-deposits.md) |
| `depletion` | deposits that run out | [08](08-depletion.md) |
| `accumulator` | batteries for the electric grid | [10](10-accumulator.md) |
| `needs` | resources the citizens buy in a shop | [11](11-needs.md) |
| `walking` | how far a citizen walks | [12](12-walking.md) |

Plugins load **last**, after every import swap, so each sees a fully built
loader. `plugins = 0` skips the folder.

### Crash reporting

A vectored exception handler logs the faulting address as `module + rva`, the
accessed address, registers, and return addresses found on the stack.

Two details are load-bearing. The stack walk is bounded by `VirtualQuery` on
`RSP` — an earlier version read past the end and faulted inside the handler,
burying the original crash. And exceptions raised inside `tesmioloader.dll`
itself are skipped, because the memory scanner walks off region ends routinely
and its `__except` handles that.

### Guard-page probe

Optional. Finds a buffer in memory by a marker, turns its pages into guard
pages, and records who faults on them. Written to find the deposit sampler; it
proved the pixels are never read by game code, which is what sent the search to
Ghidra. Off by default — see [03-reverse-engineering.md](03-reverse-engineering.md).

## Configuration

`tesmioloader.ini`, read once at startup, next to the DLL. It is short now:
everything a feature needs lives in that plugin's own ini.

| Key | Meaning |
|---|---|
| `trace_reads` | 0 off, 1 paths containing `trace_filter`, 2 everything |
| `trace_filter` | substring filter for the trace |
| `log_game` | mirror the game's own log |
| `vfs` | enable file redirection |
| `probe_map` | guard-page probe for the deposit map |
| `probe_texel` | watch texel reads of the deposit maps |
| `plugins` | scan `plugins\` and load what is there |
| `menu_patch` | append `menu_tag` to the main menu's version line |
| `menu_tag` | what to append; ASCII, empty leaves the line alone |

**One file per plugin, beside its DLL, and it holds everything that plugin
needs** — both its wiring and whatever content it declares:

| File | Holds |
|---|---|
| `pluginsesources.ini` | the ResourceGet hook mode and the three RVAs it needs |
| `plugins\deposits.ini` | which of the three deposit subsystems may touch the game |
| `plugins\depletion.ini` | whether deposits run out, and how fast |
| `plugins\accumulator.ini` | what counts as a battery, and how fast it charges |
| `plugins\needs.ini` | what the citizens buy, and which shops stock it |
| `plugins\walking.ini` | how far a citizen walks, and whether a load rebuilds the connections |

The first two carry their content in the same file: `[list]` names the
resources, and every section of `plugins\deposits.ini` that is not `[deposits]`
is a deposit. Content and wiring used to live apart, in a `resources.ini` and a
`deposits.ini` in the loader's own folder, from before there were plugins at
all. They are merged now, because "where is this feature configured" should have
one answer per feature, and deleting a plugin should not leave a stray file
behind.

Those two content sections are parsed by their own plugin rather than through
the profile API, so display names and comments may hold anything UTF-8; every
settings section goes through `configString` and stays ASCII.

Every one of these is UTF-8 **without a BOM** and read through the ANSI profile
API. Writing one back with PowerShell's `-Encoding UTF8` adds a BOM, the section
header stops matching, and every setting in the file silently falls back to its
default. This has already cost one debugging session; see
[07-pitfalls.md](07-pitfalls.md).

## Logs

Written next to the DLL, in `build/`.

| File | Contents | Written by |
|---|---|---|
| `tesmioloader.log` | hooks, VFS hits, plugins, patches, the game's own log, crashes | the loader, and every plugin through `TsmHost::log` |
| `tesmioloader.reads.log` | file access trace | the loader |
| `tesmioloader.resources.log` | resource enum and record hex dumps | the `resources` plugin |

The game holds these open. Reading them while it runs needs
`FileShare.ReadWrite`; `Get-Content` alone will report an empty file.
