# Pitfalls

Every entry here cost at least one debugging session. Most were the loader's
fault, not the game's. Read this before assuming a crash is the game misbehaving.

## Check the build stamp before you trust a single address

`easystart` 1.0 was reverse engineered, written, built and shipped against the
copy of `SOVIET64.exe` that was on disk at the time. Steam updated the game part
way through, and every code address in the plugin was then wrong: `+0x70` in the
`0x1Bxxxx` region, `+0xA0` around `0x488xxx`, `+0x1E0` at `0x83Axxx`, and `+0`
in `0x76xxxx` and `0x82xxxx`. The reflow is **not monotonic**, so no single
shift can be applied and nothing can be inferred from a neighbour.

What made it visible was a disassembly that disagreed with an earlier one of the
same address in the same session. What settles it in one line:

```
python -c "import struct;d=open(r'...\SOVIET64.exe','rb').read();print(hex(struct.unpack_from('<I',d,struct.unpack_from('<I',d,0x3c)[0]+8)[0]))"
```

`0x69C4098C` is v1.1.1.7, `0x6A3EB6AD` is v1.1.1.9. The launcher prints the same
thing with `--find`, and it is the first thing to run after any session that
spans a Steam update.

Three things limited the damage and are worth keeping:

- **`.data` does not move.** Both reason ledgers, the game object and the
  resource vector were at the same addresses in both builds, so every structure
  offset and every global survived.
- **`.rdata`'s float pool moved by a uniform `-0x18`**, so a constant is found
  by its *value*, never by its address.
- **Every site verifies its own bytes before writing**, so the wrong-build
  plugin would have refused rather than corrupted anything — which is the whole
  reason that rule exists.

Re-deriving is cheap when the original discovery was pattern-driven rather than
address-driven: the twelve rate sites were found again by scanning a window
around each old address for the same opcode *whose displacement resolves to a
constant holding the same value*, and the evaluator by the five-way
`cmp dword [rsi+8], 3/4/5/6/10` chain that no other function has.

## A need has four faces, and gating one gates one

`easystart` 1.0 removed a locked need from the citizen's demand list, which is
where happiness and health are decided, and the status bars in the building
window duly read 100 %. The window above them still said *56 Citizen(s)
currently without power*, *13 Children can't go to school* and *8 Workers can't
work because all kindergartens are inaccessible*.

Nothing was broken. A need in this game is expressed in four independent places
and the happiness ledger is only the first:

| Face | Where | Example |
|---|---|---|
| the citizen's demand | `person+0x118` | *Unable to enjoy culture* |
| a per-inhabitant rate | the building tick | *No electricity* |
| a counter the window prints | `building+0x1108`…`+0x1120` | *56 Citizen(s) without power* |
| a real mechanic | `game+0x5DC` | a parent held at home by a child |

The lesson generalises past this plugin: **the statistics ledger is the map of
what a need costs, not the map of what a need is.** Before declaring a feature
that suppresses something, open the one window that shows it and read every
line, because the panel reaches state the simulation keeps for its own reasons.

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

## A cached import is not the live import

`deposits` resolved `C3D_LANGUAGE::GetString` at init time — `FindIatSlot`, then
`*slot` — to build its minimap hover text. `resources` **swaps that same import**,
because that is how a mod resource's caption is answered: private ids from
1 000 000 up that the engine's own string table has never heard of.

Plugins load in directory order. `deposits` comes before `resources`. So the
cached pointer was the engine's own `GetString`, the mod id fell through to the
real table, and the tooltip read `Deposits: ` and stopped — with **no error
anywhere**, because an id with no string is not a failure, it is an empty string.

What made it confusing: the *editor's* tooltip for the same deposit was correct.
That one is built by `0x383BD0`, which calls `GetString` itself, through the
executable's IAT, at the moment it needs it — so it always gets whatever is in
the slot now. Two tooltips, one string table, one right and one wrong, and the
difference was entirely *when the pointer was read*.

- **Dereference the slot at call time when another plugin may own it.** That is
  the property an import swap is supposed to have, and caching throws it away.
  `FindIatSlot` returns the slot for exactly this reason; store that, not `*slot`.
- Caching is still right for an import nobody else touches — which is every other
  one in this file, and why this was the only site that had to change.
- **An empty string is a silent failure mode.** Anything that resolves a name to
  a number to a string can fail at each step and only the last one is visible.

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

## A guess written down as a finding

This section used to read:

> Setting `resource_capacity` moves the array and updates the vector at rva
> `0x9E11C0`. There is another structure sixteen bytes later holding the same
> base pointer, and it is not updated — index lookups start failing and the game
> dereferences −1.

**None of that was ever observed.** Relocation had never been run; a memory scan
had shown two things holding the same pointer, and the consequences were
imagined from there and written in the same voice as everything that had
actually been debugged. The entry then justified leaving the feature off,
capping mod resources at six, and it stood for months because it read like a
diagnosis.

What is really at `0x9E11D8` is `game+0xC2C8`, the first of 57 `Resource*` the
engine caches for itself right behind the vector. It holds `workers`, index 0,
so it equals `begin` **because index 0 starts where the array does** — a cached
record, not a container, and a memory scan cannot tell the two apart. Relocation
was already safe, because the cache is filled through `ResourceGet` and
`EnsureArmed` runs ahead of the original; it is now also carried across
explicitly. See [04-adding-resources.md](04-adding-resources.md).

Two lessons, and the second is the general one:

- **Two pointers with the same value are not two references to the same
  object.** Ask what each one *is* before concluding one of them will dangle.
  One `Xrefs.py` run on `0x9E11D8` would have shown a single writer and
  seventeen readers comparing records — nothing shaped like an array base.
- **Write down what was observed and what was inferred, differently.** Every
  other entry in this file cost a debugging session and is worth trusting on
  sight. This one cost a feature, and it looked identical.

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

## Dpi awareness has to be claimed before anything asks

`SetProcessDPIAware` in the launcher was the first line of the function that
builds the window, and it did nothing. The window came out laid out at 96 dpi and
bitmap-stretched by the OS on a 125 % display — legible, so easy to miss, and
`GetDeviceCaps(LOGPIXELSY)` reported the 96 that made the scaling a no-op rather
than reporting the truth and being ignored.

**The first dpi-sensitive call in a process fixes its awareness permanently, and
`SetProcessDPIAware` fails silently afterwards.** Something before the window —
console attach, CRT startup, one of the profile-API reads — had already asked.
Moving the call to the first line of `wWinMain`, before the config and before the
search, is what made it take: `--find` then reported `dpi 120`.

The check that shows which happened is one line of output. A window that is
wrong by exactly the display's scale factor is the symptom.

Two things confused the diagnosis and are worth knowing separately:

- **`GetWindowRect` is virtualised to the *calling* process.** PowerShell 5.1 is
  dpi-unaware, so it read the corrected 575×392 window back as 460×314 — the
  designed numbers, which looked like proof that nothing had changed. The
  geometry was right both times; only the reported units differed.
- **`PrintWindow` renders at physical size** regardless. Sizing the bitmap from
  a virtualised rect silently crops the bottom-right fifth, which reads exactly
  like a layout bug — a clipped Browse button and missing buttons along the
  bottom.

## `/MANIFEST` writes a file nobody copies

The launcher declares a `MANIFESTDEPENDENCY` on Common-Controls v6 in the source,
which is what gets themed checkboxes without an `.rc`. It worked, and the reason
was `build\tesmiolauncher.exe.manifest` — link.exe's default is to leave the
manifest in a file *beside* the exe rather than in it. Windows honours that file,
so nothing looks wrong until someone copies the exe on its own and the controls
turn Win95 grey.

`/MANIFEST:EMBED` puts it in the binary. Cheap, and the failure it prevents would
only ever be reported as "looks different on my machine".

## A 13-byte prologue and a 14-byte jump

Saving the game crashed, every time, with an access violation at
`SOVIET64.exe+0x7C2D` — writing to `FFFFFFFF98A41405`, from a call site at
`0x42CD13`. Nothing in the loader logged a failure: the hook on `0x7C20` had
reported

```
hook ok      world map save         target=00007FF639D57C20 tramp=... (13 bytes stolen)
```

**Thirteen.** `InstallInlineHook` writes a 14-byte absolute jump
(`FF 25 00000000` plus an 8-byte address) and copies `stolen` bytes into the
trampoline. With `stolen = 13` the two numbers disagree, and both halves of the
disagreement are fatal:

- the jump runs one byte past the prologue, overwriting the first byte of the
  instruction at `0x7C2D` with the top byte of the detour's address — which for
  a user-space pointer is `0x00`;
- the trampoline still returns to `target + 13`, which is now that corrupted
  byte rather than an instruction boundary.

`0x7C20`'s prologue really is 13 bytes, and what follows is the stack-cookie
load:

```asm
00007C20  48 89 5C 24 08           mov [rsp+8],rbx
00007C25  57                       push rdi
00007C26  48 81 EC 30 03 00 00     sub rsp,0x330
00007C2D  48 8B 05 14 A4 98 00     mov rax,[rip+0x98a414]     __security_cookie
```

`48 8B 05 14 A4 98 00` with its first byte zeroed decodes as
`add [rbx+0x98A414],cl` — a *write*, through a register holding whatever the
caller left there. The faulting address in the crash report carries `98A414`
literally, which is what identified the site in one reading.

Three things generalise:

- **The stolen count is bounded below by the jump, not by the prologue.** The
  API had said "at least 14" all along; nothing enforced it. The host now
  refuses and logs rather than writing a jump longer than the space it was
  given. A contract in a comment is a contract nobody checks.
- **`hook ok` means the bytes matched, and nothing more.** The prologue compare
  passed because the prologue was quoted correctly — it just was not long
  enough. Same failure mode as the walking distance: a clean log line about a
  patch that corrupts.
- **A rip-relative instruction cannot be stolen at all**, so extending the
  prologue to 20 bytes would have replaced one bug with a quieter one: copied to
  a trampoline, `mov rax,[rip+disp32]` reads `disp32` from its *new* address and
  loads a cookie from somewhere else entirely. Prologues that begin
  `mov [rsp+8],rbx / push / sub rsp,imm32` are 13 bytes followed by exactly this
  instruction in every `/GS` function the game has, so this is the common case,
  not a curiosity.

The fix is not a bigger prologue but a **call patch**: `0x7C20` has exactly one
caller, `E8` at `0x42CD0E`, and rewriting that rel32 needs five bytes, steals
nothing, relocates nothing, and calls the original by address. It is also higher
on the preference list in CLAUDE.md than spliced code. When an inline hook wants
an awkward prologue, count the callers first:

```bash
python tools/pe/xref.py SOVIET64.exe <rva>
```

## A heuristic find without an invariant is a write into a stranger's struct

`needs` finds a building type's storage vector by scanning the type descriptor
for a `{begin, end, cap}` triple whose span divides by the storage stride and
whose first entries "look like" storages — class in range, capacity positive.
That bar is three field reads, and a 0x900-byte descriptor is long enough that
a triple of pointers satisfies it **by accident**. The candidate that passed
had a slot vector whose `begin` did not name readable memory at all; the first
dereference faulted, and had the push path been reached instead it would have
reallocated somebody else's memory and written a slot into it.

What made it worse: the `__except` in the type-load hook logged
"type storages disabled after a fault" and then **did not disable anything** —
the flag it should have cleared did not exist. The same fault re-fired on every
later type load, one exception per call, forever. A log line that promises a
reaction is a claim the code has to keep; this one read like a guard and was a
greeting.

Two repairs, both cheap:

- **Validate against an invariant the lookalike cannot fake.** A real storage's
  slot vector is readable, a whole number of 16-byte slots, and every resource
  pointer in it lands inside the engine's resource vector at the vector's own
  stride. A found triple passes the first two by luck perhaps once; the third
  never. The scan now reads the resource vector once and demands all three of
  every candidate.
- **A `__except` that says "disabled" must disable.** The type pass has its
  own flag now, and the handler clears it. The other three handlers in the
  same file had done this from the start; the fourth was written later, in the
  same shape, minus the one line that mattered.

The general lesson: when a search can return a false positive, the check that
accepts it must be stronger than the check that finds it — and the strongest
available check is always "do the pointers point where real ones point", not
"do the fields hold plausible values".

## A name table sized for today reads out of bounds tomorrow

`depletion` logged each tracked deposit's map through
`kMapName[3] = { "resourcemap ", "resourcemap2", "terrain mask" }`. Then
`map = auto` deposits arrived — clay, gas — and their map numbers run 3 to 10.
Every log line for one of them read a pointer past the end of the array and
printed whatever string the next bytes of `.rdata` happened to be; one build
printed `(null)`.

A table indexed by a value another config controls is a pair that has to be
maintained in lockstep, and nothing makes that happen. The fix is the form
that cannot drift: a `switch` for the three named maps and `resourcemap%d`
for the rest, which is also how the filenames are actually numbered.

## A failed lookup once a frame is a frame-rate leak

Reported as: with the minimap open, frame rate falls off a cliff — 40 FPS down
to 8 over a few minutes, recovering the instant the minimap closes, and
resuming from the same low value when it reopens. CPU load doubles while it is
open. It never reproduced on a machine whose `plugins/resources.ini [list]`
declared every resource the deposits reference — and that difference *was* the
bug.

`DrawDepositButton` resolved its icon by calling `ResourceGet(self, icon)` —
fresh, every frame, twice for a hovered button. On a config where `sand`,
`clay` and `gas` had deposit sections but no resource declarations, every one
of those calls fell through to the game's own resolver, and the resolver logs
`game.ERROR ResourceGet - not found X` on every miss. The captured log of the
affected machine shows ~14 000 of those lines, in a per-frame cadence that
tracks the reported slowdown exactly: 25 ms a frame when the minimap opens,
33, 36, 54, 238 ms six minutes later. The per-line cost is constant; what
grows is the game's own log machinery behind it.

Two lessons:

- **A UI path must never call a resolver that can fail.** The question "does
  this resource exist" has a session-fixed answer; asking it 60 times a second
  is never right, and asking a resolver whose failure mode is a log line turns
  a missing ini entry into a performance bug rather than a missing icon. The
  hit is now cached too — re-validated against the resource vector's span,
  because a map load rebuilds that vector — and the miss is latched after one
  warning of our own naming the ini section to fix.
- **"Cannot reproduce" can mean the repro machine's config is wrong in a way
  yours is not.** The degrading frame rate was a *symptom of a missing ini
  line* the whole time. The log cadence — which lines repeat per frame, and
  how the interval between them grows — was the measurement that found it.

## A bracket you never added can look like a feature that works

The first report of this one was the opposite of a bug report: copper mining
worked. It crashed only *before* the deposit was painted — placing or even
hovering the mine on an unpainted map took the process down at
`C3DDLL64.dll+0x19096`, fault address a small number like `0x89C`.

The scan at `0x1DD190` brackets its texture per deposit type — types 0–2 open
`resourcemap`, 6–7 open `resourcemap2`, 3 opens the terrain mask — and the
dispatch chain this plugin splices new types into sits *inside* that bracket.
The chain gave copper (type 10) its texture and component; nobody gave it an
open/close. `TextureAccesGetTexel` reads the mapped-texel pointer at
`tex+0x158` with no null check, so on a never-painted map — where the texture
was never opened — it dereferenced zero plus the texel offset. `0x89C` *is*
`(x + rowpitch/4·y)·4`; the fault address decodes the coordinates.

And the reason it "worked after painting": `TextureAccessClose` unmaps and
releases the staging texture but **never clears `tex+0x158`**. One editor
paint of the channel leaves a dangling pointer that still happens to
dereference, into staging memory whose last contents the close had just copied
back from — so the unbracketed scan then reads stale-but-correct data. A
dangling pointer that returns the right answer is the most expensive kind of
defect: it converts a missing bracket into a heisenbug whose trigger is "the
player did not paint first".

Lessons:

- **A spliced case inherits the site's code, not its context.** The dispatch
  case was modelled on the type-6 block instruction-for-instruction and was
  correct as far as it went — but the block it imitates *relies on* a bracket
  twenty basic blocks upstream, and that dependency is invisible at the splice
  point. When a patch imitates one case of a chain, check what the chain's
  other cases get from outside the chain.
- **"Works after the user does X" is a state leak, not a feature.** The
  difference between painted and unpainted was one qword of texture state, and
  the correct fix was never to make the sampler tolerant — it was to give the
  mod types the same open/close the vanilla types get, at the same two sites,
  emitted per declared deposit exactly like the terrain-mask bracket already
  was.

## Slowing one clock in a game that keeps two

`daynight` 1.x made a calendar day last thirteen times longer by giving back
most of what the day tick had just added to `game+0x59C`. It works, in the sense
that the date really does advance thirteen times slower. It logged cleanly, and
the plugin's own probe agreed with it.

What it did not do is slow anything else, and **this game counts in two
different units.** Every rate is integrated per frame from
`C3D_TIMER::PowerTime`; a long list of other things fires once per calendar day
from the day tick and from nowhere else. Slowing only the second put the two a
factor of thirteen apart, and the damage was in places nobody would think to
look at after changing the sky:

- **Pollution.** The daily decay pass subtracts a flat `0.06` and `0.005` from
  every cell of the grid once per date. Emission is per frame. Thirteen times
  the emission between two subtractions, so it never settles.
- **Winter.** The same daily pass pins every home at full pollution exposure once
  the grid runs away, and local heating — which is what runs in winter — is one
  of the emitters. Citizens sicken and the population falls. The season
  boundaries are days of the year, so each winter also lasts thirteen times
  longer on the wall clock.
- **Loans.** The per-loan payment function decrements a term counted **in
  days** and pays `outstanding / days-left`, once per date, out of an income
  that did not slow.
- Used-vehicle offers, expiring notifications and the random-event roll: same
  shape, same cause.

None of these was found by reading the plugin. They were reported from a real
game as three unrelated-sounding bugs — "extra pollution", "the population
collapses in winter", "the loan starts over every day" — and only turned out to
be one thing after the pollution and loan daily passes were traced back to the
day tick.

Lessons:

- **Ask what unit a quantity advances in before scaling anything.** "Per second"
  and "per day" are both spelled as a number in a config file, and the only way
  to tell them apart is to find the code that increments them.
- **A rate change is not local.** Scaling one clock in a simulation that has two
  is a change to every ratio between them, and there is no list of those.
- **"Scale the whole clock or none of it" was the second answer, and it was also
  wrong — too broad rather than too narrow.** Scaling the simulation timer
  instead of the calendar (riding the same mechanism the game's own speed
  buttons use — `0x105A90` multiplies `timer[0]` by 0.35, 0.05 or 0.01) does
  make "everything slows in step" true by construction, and it is the correct
  answer if "everything" really is what should slow. It was not: walking,
  vehicle speed and construction slowing along with the calendar read as the
  whole world stuck rather than a longer day, and that was only found by
  watching it in game — see "Slowing a shared clock slows everything that
  reads it" below. The eventual fix scales neither the calendar's whole
  neighbourhood nor the whole simulation: it touches only the calendar field
  and separately drives the two or three functions that specifically need to
  stay in step with per-frame time, by calling them directly rather than by
  broadening what a shared clock reaches.
- **Scaling three functions and not the fourth is the same bug again, smaller.**
  The executable imports `Power`, `PowerTime` *and* `PowerKmh`; the last is
  vehicle speeds. Swapping the time and leaving the traffic at full speed would
  have been a fresh desynchronisation, so the plugin resolves all three slots
  before patching any and puts back whatever it patched if one refuses.

## A residue class is not a date

The same plugin invents a day of the year congruent to the phase it wants
modulo 13 and hands it to the weather machine. Version 1.0 took *the next one at
or after the real date*, which is up to twelve days ahead.

That is fine while the only thing reading it is `% 13`. It is not fine because
the weather tick also passes it to the snow-season test and compares it
against the winter boundaries per climate. As the phase advances through one
calendar day the invented date sweeps the whole thirteen-day window — and near a
winter boundary that window straddles it, so the weather flips in and out of
winter thirteen times a day.

The fix is to keep the congruence and add a second condition: walk the
representatives outward from the real date and take the first one the game's own
season test puts in the same season. Every season in that function is at least a
hundred days long and the representatives are thirteen apart, so one always
exists.

Lesson: **when you fake a value, enumerate every consumer of it, not just the
one you are aiming at.** A field's meaning is the union of what reads it, and
`% 13` was only the loudest reader.

## A matching PE timestamp does not mean an unchanged binary

While tracking down the daily-pass functions for `daynight` 2.1, a Ghidra
project imported earlier in the same investigation gave function boundaries
that made no sense — a 484-byte function reported as 1119 bytes, containing
what looked like three merged functions, and a `.pdata`-based scan of the raw
exe (`tools/pe`'s own function-boundary lookup) turning up gaps where a
well-formed x64 function should have unwind info.

The cause: the exe on disk had been rebuilt between the Ghidra project's import
and this check, **while keeping exactly the same PE `TimeDateStamp`**
(`0x6A3EB6AD`) the project table names as this build's own. Six addresses this
plugin already depended on — the weather tick, the light factor, the
snow-season test, the day-length constant, the cross-fade `mov` and the day
tick's own `PowerTime` call site — had already been correctly re-derived by
that point (a `+0xA0`/`+0xD0`/`−0x18`-style shift, consistent with content
inserted earlier in `.text`); the three daily-pass addresses this session was
adding had not, and were still pointing at whatever those bytes had become in
the new layout.

Two things saved this from becoming a silent wrong-function call, which would
have run arbitrary game code that happened to live at a plausible-looking
address:

- **Every address here is still checked against expected bytes before it is
  trusted**, per the project rule — the mismatch was caught by the prologue
  comparison, not discovered in game.
- **The re-derivation methodology (string/constant scan, cross-checked against
  a fresh, fully-analysed Ghidra project) does not care whether the timestamp
  moved.** Re-running it against the live file found the three functions again
  at their new addresses — `484`, `229` and `189` bytes respectively, identical
  to the old ones byte-for-byte in field layout, just relocated — and a
  from-scratch Ghidra import (`-import`, not `-noanalysis` against a stale
  project) confirmed the same boundaries independently.

Lesson: **treat a Ghidra project, or any address recorded in a previous
session, as a claim about a specific file, not about "the game."** The
`TimeDateStamp` this project already checks at launch is the right idea but is
not sufficient on its own to prove a project's cached addresses are current —
it is a field the build pipeline evidently does not always bump. When
addresses that used to check out stop matching, or a function's reported size
no longer makes sense, the fix is to re-derive against the file that is
actually on disk right now, not to adjust the tooling to tolerate the
mismatch.

## Slowing a shared clock slows everything that reads it, including the parts nobody meant to touch

`daynight` 2.0 scaled the one `C3D_TIMER` the whole simulation is integrated
from, specifically so that pollution, loans and every other per-day system
would stay in step with the per-frame systems they balance against — and it
worked, exactly as designed. What was not anticipated until it was watched in
game: **that same timer is what walking, vehicle speed, construction and every
animation are integrated from too**, because "the whole simulation" is not a
figure of speech - 337 sites in the executable read that one object. At the
default thirteen-times stretch this did not read as "the day is longer," it
read as the entire world running in slow motion, and vehicles - already the
slower of the two per raw unit in the base game - became slower than a walking
citizen.

The fix (version 2.1) is not a smaller scale factor - any nonzero factor
applied to the shared timer slows movement by exactly that factor, so there is
no value of `day_scale` that fixes this while still stretching the day
noticeably. It is scaling something *narrower*: touch only the calendar field,
and drive the handful of functions that specifically need to stay in step with
per-frame time by calling them directly, at their own natural cadence,
independent of the display. See "Slowing one clock in a game that keeps two"
above for why the calendar-alone approach was rejected the first time; this is
the same lesson from the other side - scale too broad a mechanism and
correctness is easy, scale too narrow a one and desync comes back, and the
answer is neither "which one" in isolation but "the widest mechanism that
excludes the things that must not move."

Lesson: **"the whole simulation" in a description like the one this plugin's
own source carried is not scoped by what you were thinking about when you
wrote the hook - it is scoped by what the hooked function actually is.**
Before scaling a shared resource, enumerate what else reads it, not just what
you intended to fix - `python scan.py ref <address>` finding 337 call sites for
one timer was exactly the information that should have prompted asking "what
are all of these," not just "does the economy come out right."

## A building type lives on its type descriptor, not on the building

`easystart`'s one-time catch-up sweep (`SweepExistingBuildings`, 1.1) read a
live building's type with `*(int*)(building + 0x360)` and compared it against
`BUILDINGTYPE_LIVING` (2) to decide whether to clear a pre-existing building's
power/water warning counters immediately rather than waiting for its next
tick. It never matched — the log said `sweep: 0 of 668 building(s) are living
quarters` on every load, on two different sessions, which is what made it
look like a real property of the map rather than a bug: 668 buildings and not
one of them a house is not plausible, but it reads exactly like a spurious
mismatch would.

`+0x360` is real, but it belongs to the building's *type descriptor*
(`*(building + 0x318)`), not to the building object itself — `depletion.cpp`
and `accumulator.cpp` already read it that way, both with a `typedesc ->
building type` comment, and the vanilla planner's own disassembly confirms it:
`mov rdx,[person+0x20]` (the building) / `mov r8,[rdx+0x318]` (its type
descriptor) / `mov esi,[r8+0x360]` (the type, tested there against
`BUILDINGTYPE_ORPHANAGE`/`BUILDINGTYPE_PRISON` for two special-cased blocks).
Reading `+0x360` straight off the building reads whatever unrelated field
happens to sit there in the *instance* layout — not zero, not garbage that
faults, just a number that is never 2 — so the check fails silently every
time, with no crash and no refusal to point at it.

Fixed by going through the type descriptor first, in a small `BuildingTypeOf`
helper both call sites now share. The practical effect is narrow — a
pre-existing building's power/water warning lines now clear at world load
instead of waiting for that building's own next `0x1BC210` tick, which
happens anyway shortly after — so this was never the cause of a citizen's
demand list itself being wrong, only of the sweep silently doing nothing.

Lesson: **when a count comes back exactly zero against a population that
obviously contains matches, suspect the field before the data** — especially
when a sibling plugin already reads the same-looking offset correctly one
indirection deeper, and the discrepancy was sitting in a `grep` away the whole
time.
