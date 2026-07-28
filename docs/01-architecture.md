# Architecture

`tesmioloader` is a DLL injected into `SOVIET64.exe` at startup by a launcher.
It never modifies a file on disk. Everything it does is one of four things, and
they are listed here in the order you should reach for them — the earlier ones
survive game updates far better.

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

### 4. Spliced code — last resort

Only when a decision is compiled into a chain of comparisons and there is no
data to redirect. Used for exactly one thing, the deposit type — and even there
the emitted code is generated from a config table rather than written out per
deposit. See [05-deposits.md](05-deposits.md).

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
| `C3D_LANGUAGE::GetString(int)` | captions for mod resources |

The log functions are variadic. A `va_list` cannot be forwarded to a variadic
callee, so the hook formats the text itself and passes the result on as `"%s"`.

### File opening, through **both** import tables

`fopen`, `fopen_s`, `_wfopen`, `_wfopen_s`, `fread`, `CreateFileA`,
`CreateFileW`, `CreateFile2`.

That list grew one entry at a time, each after an asset was found slipping past.
Assume it is still incomplete: when something is not being redirected, the first
question is which opener it used.

### Inline hook

`ResourceGet` at rva `0x2AA7C0`. Twenty bytes of prologue are relocated into a
trampoline and replaced with `jmp qword ptr [rip+0]`. The prologue is compared
byte for byte against the known build first; a mismatch aborts the hook rather
than corrupting the process.

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

### Resource registry

`resources.ini` drives it. See [04-adding-resources.md](04-adding-resources.md).

### Deposit registry

`deposits.ini` drives it. One table feeds all three deposit subsystems — the
code patch, the minimap layer and the editor brush — so none of them contains
anything specific to a particular deposit, and adding one is adding a section.
See [05-deposits.md](05-deposits.md).

The code patch is the only place in the project that emits instructions, and it
emits them in a loop over that table. Two consequences worth keeping: the cave
is bounds-checked and refuses to patch anything if it would not fit, because
the number of cases now comes from a file a user edits; and the generator is
small enough to re-implement in Python and disassemble, which is how it should
be verified rather than by reading the opcode arrays.

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

`tesmioloader.ini`, read once at startup, next to the DLL.

| Key | Meaning |
|---|---|
| `trace_reads` | 0 off, 1 paths containing `trace_filter`, 2 everything |
| `trace_filter` | substring filter for the trace |
| `log_game` | mirror the game's own log |
| `vfs` | enable file redirection |
| `resourcehook` | 0 off, 1 observe, 2 observe and inject |
| `resource_rva` | entry of `ResourceGet` |
| `resource_vector_rva` | the resource vector |
| `resource_capacity` | records to reserve; 0 leaves the engine's allocation alone |
| `probe_map` | guard-page probe for the deposit map |
| `probe_texel` | watch texel reads of the deposit maps |
| `deposit_patch` | splice in the deposit types declared in `deposits.ini` |
| `minimap_patch` | a minimap button and layer per declared deposit |
| `editor_patch` | a terrain-editor brush pair per declared deposit |
| `menu_patch` | append `menu_tag` to the main menu's version line |
| `menu_tag` | what to append; ASCII, empty leaves the line alone |

## Logs

Written next to the DLL, in `build/`.

| File | Contents |
|---|---|
| `tesmioloader.log` | hooks, VFS hits, resource arming, patches, the game's own log, crashes |
| `tesmioloader.reads.log` | file access trace |
| `tesmioloader.resources.log` | resource enum and record hex dumps |

The game holds these open. Reading them while it runs needs
`FileShare.ReadWrite`; `Get-Content` alone will report an empty file.
