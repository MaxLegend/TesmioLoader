# Pitfalls

Every entry here cost at least one debugging session. Most were the loader's
fault, not the game's. Read this before assuming a crash is the game misbehaving.

## The BOM that disabled everything

PowerShell 5.1's `-Encoding UTF8` writes a byte-order mark. Prepend one to
`tesmioloader.ini` and the first line stops being `[tesmioloader]`, so
`GetPrivateProfileInt` matches no section and **every setting silently falls
back to its default** — including `resourcehook`, which drops from 2 to 1 and
stops creating resources entirely.

The crash that followed looked exactly like a game bug: a building referencing
`copper_ore`, a lookup returning −1, a null dereference. The tell was in the log
— no `registry` lines at all.

Write config files with `[System.IO.File]::WriteAllText` and
`UTF8Encoding($false)`. Check the first bytes if a setting seems ignored.

## One import table is not enough

`C3DDLL64.dll` has its own imports. Hooking only the executable's meant every
file the engine opened for itself was invisible — no trace entries, no VFS
redirection. Resource icons never loaded and the log showed nothing at all,
which reads like "the game does not want icons" rather than "we cannot see it".

The same lesson arrived twice more: `CreateFileW` on the executable and
`CreateFile2` on the engine were both unhooked for a while. Assume the list is
still incomplete.

## Virtual calls are invisible to import hooks

The deposit maps are read through `TextureAccesGetTexel`, a virtual method. No
import hook can see it, and a guard page on the buffer `fread` filled never
fired for game code because the CPU reads a different copy inside the texture
object.

Several sessions went into probing before Ghidra showed the loader calling
`CreateManagedTexture`. When runtime observation returns nothing at all,
consider that the call may not be going through any table you can reach.

## Deriving an array base from the wrong name

`waste_mixed` is index 57 in the script VM's `Resources` struct but is not in
the engine's vector — it is a standalone object. Computing the array base as
`returnedPointer − index × stride` from that name gives a bogus address.

Once copper took slot 57, every `waste_mixed` lookup produced a fake base, the
loader concluded the array had been rebuilt, reset itself, and tried to re-arm —
forever. The log filled with alternating "array rebuilt" and "slot 57 is not the
next free one".

Only indices 0–56 are safe for that calculation. Better still: read `begin` from
the vector, which is what the loader does now.

## An auto-assigned slot must wait for the whole base game

Claiming "the next free slot" means claiming `live`, and `live` climbs while the
engine is still pushing its own records — the first `.ini` files are parsed
before the resource vector is full. Claiming at that moment writes a mod record
over a base-game one *and* moves `end` backwards, truncating the engine's array
to whatever it had reached.

The guard is to wait for `live >= 57`, the count of base-game resources, before
an auto slot resolves. A pinned slot number is safe for the same reason by
accident: 57 is only reachable once the other 57 have landed.

## The array is rebuilt on every map load

The first version armed once. Loading a second save left the resource in the old
allocation, lookups returned −1, and the game crashed on the next hover. Watch
`begin` and re-arm.

## Relocating the vector breaks a second reference

Setting `resource_capacity` moves the array and updates the vector at rva
`0x9E11C0`. There is another structure sixteen bytes later holding the same base
pointer, and it is not updated — index lookups start failing and the game
dereferences −1.

The evidence had been sitting in a memory scan from hours earlier, listing two
containers pointing at the same base. It was read and not acted on. Relocation
is off by default for this reason.

## Only the icon is found by name

For a long time this project believed a mod resource's assets were all looked up
by name, because the icons were. They are not: the icon is formatted through
`"resources/%s.png"`, and every mesh path is a **literal** in the resource table
at rva `0x2A1D60`.

So a cloned record inherits the template's *mesh objects*. `raw_copper` shipped
correctly named `.nmf` and `.mtl` files, the VFS was ready to serve them, and
nothing ever asked for them — the game drew steel, because the pointer in the
record was steel's mesh. The files sat in the tree looking like evidence that it
worked.

**The tell was in the trace log and went unread**: the only resource assets ever
opened were `.png`. A mesh that is never opened is not a mesh that failed to
load.

The loader now makes the same three engine calls the table makes and writes its
own pointers into `+0x318`…`+0x338`. The general lesson: when a clone behaves
like its donor in one respect only, look for a pointer that was copied rather
than a name that was resolved.

## Armed is a claim, not a fact

`EnsureArmed` latched each entry once and only reconsidered when the vector's
`begin` pointer changed. Enter a world, leave to the main menu, enter again, and
the allocator hands back **the same block**: `begin` is unchanged, so nothing was
re-armed, while the engine had reset `end` to 57 and overwritten our records with
its own init.

Everything downstream then failed in a way that pointed nowhere near the cause.
Buildings naming a mod resource resolved to −1. The icon at `+0x48` still held a
texture released with the previous world, so hovering the build menu crashed
inside the engine's texture code.

Latched state about a structure the game owns has to be **re-verified against
that structure**, not against the event that last changed it. The check is now
"is our name still at our index, within the current end" — which costs one
`strncmp` and is true by construction whenever nothing has happened.

Same shape as the editor brushes going stale: there, the trigger was the
descriptor clones outliving the editor that produced them. Anything the loader
caches across a world load is a candidate.

## `.mtl` has no comment syntax

A `#`, `;` or `//` line in a `.mtl` is not ignored — the parser scans for its
keywords wherever they appear. A comment written to explain *why* `$TEXTURE` was
used instead of `$TEXTURE_MTL` therefore contained the token `$TEXTURE_MTL`, the
parser acted on it before any `$SUBMATERIAL` had been declared, and the game
died selecting the building in the build menu:

```
=== CRASH: ACCESS_VIOLATION at C3DDLL64.dll + 0x9D0AE ===   writing 0x1C40
```

`0x9D0AE` is inside `C3D_MATERIAL::Load` and reads

```asm
mov rax, qword ptr [rdi+0x18]        ; the submaterial array - still NULL
mov qword ptr [rax+rcx*8+0x48], rbx  ; rcx = 0x37F -> 0x37F*8+0x48 = 0x1C40
```

The faulting address is the whole diagnosis: it is an offset, not a pointer, so
the base was null and the index was uninitialised garbage. **A near-null write
address decomposes into `field + index*stride` — solve it and you have the
statement.**

Not one stock `.mtl` contains a comment. Neither should any written here. The
explanation belongs in these documents, which is where this paragraph is.

Note that `building.ini` is a **different parser** and does accept `//`
comments — the copper concentrator has used them since it was written. Do not
generalise from one to the other.

## The private localisation id base has to clear the whole game

`TML_TEXT_ID_BASE` was 60000, on the reasoning that it was "far above anything
the base game uses". It is not: 793 entries in `sovietEnglish.btf` sit at or
above 60000 and the highest id in the game is **580231**.

Because the `GetString` hook answers *everything* at or above the base and never
falls through, four mod resource captions replaced four real labels — the
settings panel rendered "Copper Ore", "Copper Ore Concentrate", "Raw Copper" and
"Copper" where its own strings belonged. Nothing crashed and nothing was logged;
the only symptom was text in the wrong place, several menus away from anything
resource-shaped.

The base is now 1 000 000. **Measure, do not assume.** A `.btf` is trivially
readable — big-endian `{count:u32, file size:u32, ...}` header, then `count`
records of `{id:u32, offset:u32, length:u16}`, then UTF-16 payload — so the id
range of all twenty-one language files comes out in about ten lines of Python.
Re-measure after a game update.

The general form of the mistake: a hook that claims a **range** of an identifier
space rather than a known set of values must own that range for certain. Ours
did not, and a hook that answers too much is indistinguishable from the game
being wrong.

## One `VirtualQuery` is not a range check

`ReadablePtr` asked `VirtualQuery` once and compared the range against
`BaseAddress + RegionSize`. A region is a run of pages sharing **state and
protection**, not an allocation, so that answer is only as long as the current
protection run.

The engine's big globals live in `.data`, which the image loader maps
`PAGE_WRITECOPY`. The first write to a page makes it a private
`PAGE_READWRITE` one and **splits the region there**. The reported region around
a long-lived object therefore shrinks as the game runs, and a check over a large
structure that passed at startup starts failing partway through a session with
every byte of it still perfectly readable.

That is what silently disabled the terrain-editor brushes. `ActiveDepositTool`
asked whether all `0xD430` bytes of the editor object were readable before
reading **one pointer** out of it. The panel hook checks nothing, so the buttons
kept drawing, hovering and selecting; the cursor and dispatch hooks both go
through `ActiveDepositTool`, so the round brush cursor and the painting both
stopped — and nothing was logged, because nothing had failed.

Two fixes, both applied:

- `ReadablePtr` walks regions until it has covered the whole range.
- Ask about what is actually dereferenced. `self + 0xD428, 8 bytes` is the
  honest question; `self, 54 KB` never was.

The symptom to recognise: a feature that works right after launch and stops
later in the same session, with no crash and no log line. Anything gated on a
large `ReadablePtr` span is the first suspect.

## A guard page must be page-aligned

`VirtualProtect` rounds to page boundaries, so guarding a range that is not
aligned protects bytes outside the buffer too. A guard fault whose address the
handler does not recognise gets `EXCEPTION_CONTINUE_SEARCH`, nothing else
handles it, and the process dies.

Guard only whole pages strictly inside the buffer, and remember the marker used
to find the buffer lives in the alpha byte — three bytes past the start of its
pixel when the data is interleaved.

## A crash handler that crashes

The stack walk read a fixed 768 qwords from `RSP`, ran off the end of the stack,
faulted inside the handler, and re-entered it — burying the original report.

Bound the walk with `VirtualQuery` on `RSP`, guard against re-entry, and skip
exceptions raised inside the loader itself: the memory scanner walks off region
ends routinely and its `__except` handles that.

## The scanner trips its own trap

The marker scan read pages the probe had already guarded, which both produced
noise and *disarmed* the guard — leaving a window in which a real read went
unrecorded. Skip regions already under guard.

## Scanning is too slow to catch load-time work

A byte-at-a-time search over a multi-gigabyte process takes about 40 seconds; by
then the map had been converted and its staging buffer abandoned. `memchr` is
vectorised and turns that into roughly a second, and hooking `fread` to take the
destination pointer from the read itself is instant.

## One `FILE*` cache is not enough

The deposit map is opened twice in the same millisecond. Remembering a single
stream meant the second open overwrote the first and the wrong buffer was
captured. The CRT also recycles `FILE` structures, so a stale pointer matches a
completely unrelated file — three 7.4 MB buffers were captured that way. Track
several streams, check the payload size, and release the stream once its data
has arrived.

## Wrong terrain

`config.ini` names `terrain3`, but a new game loaded `campaign1`. The painted
deposit map sat at a path the game never asked for. `trace_filter = resourcemap`
shows what is actually opened.

I misdiagnosed this as a missing `CreateFile2` hook. The wide-character openers
were genuinely unhooked and worth fixing, but this file went through plain
`fopen` with a relative path all along.

## Missing cargo models crash a worker thread

A resource with an icon but no `<name>1..4.nmf` crashed on the asset loading
thread with a null dereference — `C3DDLL64.dll + 0xAC544`, far from anything
resource-shaped. While the icon was *also* missing the resource never got that
far, so the crash appeared only after the icon was fixed, which made it look
like the icon caused it.

## PowerShell interpolates `$` in double quotes

`"$TYPE_MINE_COAL"` becomes an empty string, and a search for it matches
everywhere — 399 414 hits in one attempt. Single quotes.

## Ghidra headless and quoting

Passing the command inline to `cmd /c` fails on the redirect; a relative path to
the `.bat` is not found. Write a `.bat` with absolute paths and launch it with
`Start-Process -RedirectStandardOutput`.

Import once, then `-process <binary> -noanalysis` — re-analysing a 10 MB
executable for every script wastes minutes.

## Reading logs while the game runs

The game holds them open. `Get-Content` reports an empty file; the directory
listing shows a stale size. Open with `FileShare.ReadWrite`.

## Rebuilding with the game running

`LNK1104: cannot open build\tesmioloader.dll`. Close the game first.

## Saves and resource count

The resource count is part of the save format. A save written with two mod
resources will not load without them, and a stock save will not load with them.
Expect to start a new game after changing `resources.ini`.

## The decompiler reuses one variable for two constants

The minimap overlay at `0x4BDDE0` decompiles to

```c
fVar21 = C3D_TERRAIN::GetTerrainHeight(...);
SetFloat(tech, handle("TerrainHeight"), fVar21);
...
fVar21 = DAT_140909df4;                       // 0.5f — a different value
fVar22 = *(float *)(param_1 + 0x58) * DAT_140909df4;
```

Read as a single variable, that says the overlay quad is inset by the terrain
height. It is not; it is inset by half. Building the copper layer on the first
reading multiplied the rect by a world height, put the quad far off the panel,
and drew nothing at all — while the button next to it, which does not use that
constant, worked perfectly. Days of "the hook must not be running".

Ghidra names registers, not values. Whenever a decompiled float feeds geometry
or an address, check it against the disassembly: here two adjacent `movss`
instructions from `0x909DF4` and `0x909F70` made it obvious in seconds.

## `C3D_PANEL2D::Draw` does not draw

It appends a quad to the `mArrayPosition` / `mArrayCoord` / `mArrayColor`
constant buffers the vertex shader indexes with `BLENDINDICES`, and flushes the
batch — through `C3DDLL64.dll` rva `0xBB720`, called from both `Draw` and
`EndDraw` under the pending flag at `this+0x30D` — only when the bound state
forces it, or when `EndDraw` does. **The flush is what commits the shader
constants**, which is how the vanilla overlay gets two quads with different
`MapType` values out of one bracket: rebinding the texture between them forces
the first to flush.

Three consequences for anything appended to this UI:

- Set a constant *before* the `Draw` that needs it, and restore it *after* the
  flush, never between the two. Resetting `MapType` between `Draw` and
  `EndDraw` silently redirects the pending quad to a different shader branch.
- A pass opens with `EndDraw` *then* `BeginDraw(technique)`. That is literally
  how `0x4BDDE0` begins.
- `0x4BDDE0` returns with a bracket still open — its tail is `EndDraw`,
  `PrintAllTexts`, `BeginDraw(NULL)`. A hook that calls `BeginDraw` without
  closing that one nests. The symptom the first time was the whole button row
  rendering as tiny copies of the minimap.

## Compiled shaders are readable without fxc

`media_soviet/shaders_d3d11/*.inix` are effect files: a technique name followed
by its vertex and pixel `DXBC` blobs, each blob's length at its own `+0x18`.
`D3DDisassemble` in `d3dcompiler_47.dll` — already on every Windows box — turns
one into annotated assembly plus a full constant-buffer and resource-binding
reflection, from about a dozen lines of ctypes. No SDK, no fxc hunt.

It is worth doing before assuming a shader needs patching. The deposit overlay
selects its colour channel with a `dp4` against a float4, so the "free" alpha
channel was reachable from the start and no patch was ever needed.

## The crash handler does not see faults in our own code

`CrashHandler` returns `EXCEPTION_CONTINUE_SEARCH` without logging when the
faulting address is inside `tesmioloader.dll`, because the guard-page scanner
walks off the end of regions constantly and its own `__except` deals with it.

That exemption also covers every bug in the injected UI code. A null
dereference in a draw hook kills the process and leaves **nothing** in the log
— not even a line saying it crashed. The absence of a `=== CRASH` entry is
therefore evidence in itself: the fault was on our side of the boundary, since
anything in `SOVIET64.exe` would have been logged.

Injected code that runs inside the game's draw and input paths now runs under
`__try`, with a filter that logs the exception code, the faulting address, the
module, and the read/write target, then disables that feature for the session.
A missing button beats a dead process, and the log names the offset.

## State that outlives the world

`DepositDef::minimapState` — which mod layer is selected — lives in the
loader's own registry, so it survives a map load, a return to the menu, and the
switch into the terrain editor. The game's own six flags live in a structure
that is rebuilt; ours do not.

That is how a mod layer stayed selected into the terrain editor, where
`gameobj+0xED8` and the deposit maps need not exist, and the overlay followed a
null pointer. Anything the loader keeps outside a game structure has to assume
the world underneath it can vanish: check the pointers, do not trust that a
flag set in one context is still meaningful in another.

## `TextureAccessOpen` is a D3D11 Map, not a lock

The first version of the depletion plugin resampled each mine from the mine
tick by calling the game's own deposit scan at `0x1DD190`, on the reasoning that
the water branch of that same tick function already does exactly that.

**Building a coal mine crashed the NVIDIA driver**, at the first tick after the
mine appeared:

```
deplete  coal mine at (-2302, 3486): 1369 sample points, ~429 texels, quality 0.086
=== CRASH: ACCESS_VIOLATION at 00007FFEA5F4776B ===
    nvwgf2umx.dll + 0x2C776B
    reading address 0000000000000010
    --- return addresses on the stack ---
```

Two things in that report point the same way. The fault is inside the graphics
driver, not the game — and the stack walk found **no return address into
`SOVIET64.exe` at all**, so the thread that died was not the one running our
code.

`C3DAPI_D3D11_TEXTURE::TextureAccessOpen` reads like a lock and is not one.
Disassembled straight out of `C3DDLL64.dll` at export rva `0x18D40`:

```asm
mov  rcx,[rip+...]         ; the ID3D11Device
call qword ptr [rax+0x28]  ; vtbl slot 5  - CreateTexture2D, USAGE_STAGING, CPU rw
call qword ptr [rax+0x178] ; slot 47      - CopyResource, GPU texture -> staging
call qword ptr [r10+0x70]  ; slot 14      - ID3D11DeviceContext::Map
```

and `TextureAccessClose` is `Unmap` plus the `CopyResource` back. That is the
**immediate context**: not thread-safe, and it moves the whole 4 MB map across
the bus in both directions on every open/close pair.

The base game never does this from a building tick. Every caller of `0x1DD190`
that maps a texture is a main-thread path — building placement at `0x2BAD70`
and `0x7ABBD0`, the terrain editor at `0x30D100`. The one tick-side caller is
the water branch, and water is deposit type 8 or 9, which is **excluded from
both of that function's texture guards** (`type < 3` and `type - 6 < 2`). Water
is precisely the one deposit type that touches no texture at all, which is why
it is safe to resample from a tick — and taking it as a template copied the
shape while missing the only thing that made it work.

Three lessons, in order of how much they generalise:

- **An engine function named like a lock can be a GPU call.** Read the export
  before calling it from anywhere the game does not. `tools/pe/exports.py`
  disassembles one by name in a second; it does not need Ghidra.
- **When copying a code path as a template, work out what makes that path
  legal**, not just what it does. Here the answer was a type number two
  comparisons away.
- **An empty stack walk in a crash report is information.** It means the
  faulting thread has no game frames, which for a fault inside a driver module
  means the damage was done somewhere else and earlier.

The fix was to split the plugin in two: the tick does arithmetic only, and
everything that maps the deposit map runs from an import hook on
`C3D_TERRAIN::Render` — unambiguously the render thread — rate-limited so one
Map/Unmap pair covers every mine at once. See
[08-depletion.md](08-depletion.md).
