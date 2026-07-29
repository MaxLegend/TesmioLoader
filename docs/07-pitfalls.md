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

**This came back, and the second time it cost a whole debugging session.** The
mechanism is worth having in full, because everything about how it presents is
misleading.

A `[list]` entry with **no files at all** — `copper2 = chemicals`, added as an
experiment and never given assets — produces two crashes twenty seconds apart:

```
[22:36:15.070] === CRASH: ACCESS_VIOLATION at C3DDLL64.dll + 0xB0D04 ===
[22:36:15.071] resource  "copper2": cargo mesh load faulted - keeping the template's
   ... a whole world loads ...
[22:36:37.206] === CRASH: ACCESS_VIOLATION at C3DDLL64.dll + 0xAC544 ===
```

`0xB0D04` is inside `C3D_MIDDLEPOINT::CreateManagedMaterial` and `0xAC544` is
`mov rcx,[rdi+0x58] / cmp dword [rcx+rsi],0` inside `C3D_MESH::Render` — the
mesh object is valid, its **node array is null and its node count is not**.

The order is: `CreateManagedMesh` registers an empty mesh in the middlepoint's
cache under a path that does not exist, `LoadFromFile` fails and leaves it
empty, `LoadMaterial` on an empty mesh faults inside `CreateManagedMaterial`.
The plugin's own `__try` catches that fault and carries on — **and that is the
trap**. The fault is caught, the engine is not repaired, and the damage surfaces
on the first frame rendered after the world has loaded, in a different function,
on a different thread, with a stack that names nothing recognisable.

Three lessons:

- **Catching a fault inside the engine is not the same as surviving it.** A
  `__try` around a call that half-built an engine-managed object buys nothing
  but a delay.
- **Ask before you build.** `LoadResourceMesh` now calls
  `C3DHelp_CheckIfFileExist` first, and refuses to touch `CreateManagedMesh`
  at all when the `.nmf` is not there. The import slot already carries the
  loader's VFS hook, so a file that exists only under `tesmioloader\vfs`
  answers yes. Paths are relative to `media_soviet`, the same form
  `CreateManagedMesh` takes; the argument pair every caller in the game passes
  is `(path, false, true)`.
- **A crash 22 seconds and one world load after the cause is still that
  cause.** The first line to look for in `tesmioloader.log` is not the last
  one — it is the earliest `=== CRASH` or `faulted`, however harmless the log
  makes it sound.

A resource with no cargo geometry is legal now and draws as its template, with
one `WARN` line naming the files it wanted.

## A cloned building needs the donor's emissive material

Second crash at `C3DDLL64.dll + 0xAC544` in this project, same instruction —
`mov rcx,[rdi+0x58] / cmp dword [rcx+rsi],0`, a valid `C3D_MESH` whose node
array is null — and a completely different cause from the
[first one](#missing-cargo-models-crash-a-worker-thread).

The furniture factory copied `clothing_factory.nmf` and `clothing_factory.mtl`
and declared only `MATERIAL ../material.mtl` in `renderconfig.ini`. But
`clothing_factory_e.mtl` exists too — the lit-window pass — and a mesh built
for one expects it. **159 of the 493 base building materials have an `_e.mtl`,
and 352 of 400 subscribed Workshop buildings declare `MATERIALEMISSIVE`.**

The stationary accumulator had been fine without it for the same reason a coin
lands heads: its donor, `eletric_substation`, has no `_e.mtl`. That made the
missing keyword look optional.

The check is one `ls`:

```
ls media_soviet/buildings/<donor>_e.mtl
```

If it is there, copy it as `material_e.mtl` beside `material.mtl` and add

```ini
 MATERIALEMISSIVE ../material_e.mtl
```

Both crashes at `0xAC544` say the same thing in the end: **a mesh object exists
and its geometry does not.** The causes differ — a missing `.nmf`, a missing
emissive material — but the first frame after a world load is always where it
surfaces, twenty seconds and a whole load away from anything that looks
related.

## A mod resource wakes up dead code

The first citizen to buy furniture crashed the game at
`SOVIET64.exe + 0x198868`, reading address 8:

```asm
140198854  test rax,rax                  ; element count
140198857  jz   done                     ; zero - nothing to do
140198859  mov  r8,[r14]                 ; r8 = begin
140198868  mulss xmm0,[r8+rcx*8+8]       ; <- r8 was null
```

`FUN_140198670(game, dst, resource, amount)` folds a purchase into a running
total, and picks which total by comparing the resource against four cached
records, falling through to a fifth bucket for anything else:

```
game+0xC300 -> game+0x12720      game+0xC318 -> game+0x12750
game+0xC310 -> game+0x12738      game+0xC320 -> game+0x12768
anything else                 -> game+0x12780
```

The four are the goods the base game sells. **The fifth branch is unreachable
in a stock game**, so its vector was never constructed — `begin` null while
`end` is not — and it sat there for years as code nobody could run. A modded
good is the first thing that ever takes that branch.

The lesson generalises past this one bucket: **adding a resource does not only
add a resource, it makes reachable every "else" that the base game's fixed set
of resources kept dead.** Those branches have never executed and are therefore
the least tested code in the executable. When something crashes the first time
a modded resource is *used* rather than declared, look for a comparison chain
against a handful of cached records with a fallthrough at the end.

The repair is small: a vector with a null `begin` and a non-null `end` is not a
state any live vector can be in, so normalising it to properly empty is safe,
and the loop guarded by `(end - begin) >> 4` then does nothing. Checking it
every shop tick costs three reads, and a world load rebuilds the game object,
so once at startup would not be enough.

## `building.ini` has no comment syntax either

The `.mtl` parser matching its keywords anywhere in the file is
[already written down](02-findings.md). **`building.ini` is the same**, and it
is worse there, because a mistake is not a wrong texture but a dead process:

```ini
// ... a shared one could only hold whichever class it declared.
// $STORAGE_IMPORT_SPECIAL takes the resource as its third argument ...
```

The parser read that as a real declaration, took `takes` for the transport
class and `the` for the resource, and `ResourceGet` came back null. The crash
lands at `SOVIET64.exe + 0x117B91`, inside the 63 KB `building.ini` parser at
`0x10E200`, reading `[0] + 0x30C`. The tell is one line above it in the log:

```
game.ERROR ResourceGet - not found the
=== CRASH: ACCESS_VIOLATION at SOVIET64.exe + 0x117B91 ===
```

**A `ResourceGet - not found <ordinary English word>` is always a token in a
comment.** The stock game produces hundreds of `not found waste` and
`not found $PARTICLE` lines, which is why the noise is easy to look past — but
those are keywords and resource names, not prose.

So: `//` is fine, indentation is fine, the base game's own `-$VEHICLE_STATION`
and `-------` are fine. **A `$` anywhere in a comment is not.** Write the
keyword without its dollar when a comment has to name one. The stationary
accumulator's `building.ini` had been doing that by luck since it was written;
now both do it on purpose.

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
Expect to start a new game after changing the `[list]` section of
`plugins\resources.ini`.

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

## The constant that looks like the limit can be the search radius

The walking distance turned up in two functions that regenerate walking and
parking connections, `0x12DE30` and `0x12F480`, each writing one literal — 530
and 2600 — into the same field of a path query, `+0x3C`, which the path expander
reads as "drop any branch longer than this". Two functions, two plausible
numbers, one field, and the shape of the check confirmed in disassembly. The
first version of the `walking` plugin repointed both.

Everything then looked right and nothing changed. The plugin logged its patch,
the game's own regeneration pass ran on load — visible in the log as
`Import - Regenerating walking and parking connections` — and citizens still
refused to walk past 500 m.

Those two functions are **collectors**: after a road changes they search outward
from it to decide *which buildings* need their connections rebuilt, then hand
that set to a builder. The builders are `0x12E1D0` and `0x12F830`, and their
limits are the walking distance: **480** as a bare immediate at `0x12E2DD`, and
2500 from `0x90B11C`. The collectors' radii are deliberately a little wider than
the limits they serve, which is exactly why they look like the answer.

- **A function that regenerates something is not necessarily the one that
  computes it.** Follow the write to the structure being filled in. `0x12E1D0`
  is the only function that clears `building+0xCA8` and pushes into it; that,
  not the name-shaped reasoning, is what identifies it.
- **Search for the value in every form, not just the one you found first.** The
  first search was over `.rdata` and RIP-relative reads. 480 is an `imm32` in a
  `mov dword ptr [rsp+0x7C]` and appears in no constant pool at all — a scan for
  the four bytes `00 00 F0 43` in `.text` found it, and found it exactly once.
- **A correct log line is not a working patch.** Every check the plugin makes
  passed, because everything it was told to verify was true. The claim it could
  not check was the one that mattered.

And then the same mistake a second time, one level down. Version 1.1 patched the
builder at `0x12E1D0` — correctly — and still nothing moved, because **the
walking limit is written out twice**. `0x12E1D0` rebuilds a whole set of
buildings and is called from the save loader; `0x12E6E0` rebuilds one building
and is drained twenty at a time off the queue at `game+0x11F88` by `0x12EC50`,
which is what a running game actually uses. Same logic, same field, its own copy
of 480 — an immediate in one and `0x90AF38` in the other, which is why a scan
for either form finds only one of them.

- **Scan for the value in every form before concluding there is one site.** Both
  scans were run and both were believed; neither was cross-checked against the
  other's result.
- **A second implementation of the same thing is common in this codebase.** The
  `building.ini` parser carries its own copy of the storage logic
  (`0x117B91`); walking carries its own copy of the connection builder. When one
  patch site is verified and the effect is still absent, look for the twin
  before looking for a new mechanism.
- **After two clean logs and no effect, stop guessing and hook.** An inline hook
  on the builder logging its arguments and results answers in one game load what
  a day of disassembly argues about.

And a third time, for the half that is drawn rather than simulated. With both
builders patched, citizens walked the new distance — and the building window's
walking-distance button still highlighted the vanilla 480 m. **The overlay does
not read the connection vector.** `0x43EF10` runs the whole search again, with
its own copies of the constant at `0x43F04A` and `0x43F835`, and draws the path
polylines and metre labels from its own result; `0x43FEA0` does the same for
cars. Four functions, one distance, no shared constant.

- **A displayed value can have its own code path.** When the simulation obeys a
  patch and the UI does not, the UI is not reading the simulation's data — look
  for its own copy before doubting the patch.
- **Count the readers of the constant, not the sites you have patched.**
  `0x90AF38` has 31 code references; three of them are this distance. The other
  28 are `mulss` against GUI coordinates, and skimming past them is how the
  overlay pair got missed twice.

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
