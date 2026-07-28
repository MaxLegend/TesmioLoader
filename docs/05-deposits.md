# Deposits

Deposit types are **not** resources. They are a separate, much smaller enum, and
adding one is the only thing in this project that required patching code.

Nothing about any individual deposit is compiled into the loader. `deposits.ini`
declares them and one registry drives all three subsystems — the code patch, the
minimap layer and the editor brush. **Adding a deposit is adding a section**;
see [Adding one](#adding-one) below.

## How the game stores deposits

Two textures per map, `resourcemap.dds` and `resourcemap2.dds`, 1024×1024 BGRA.
One colour channel per deposit type; the byte value is richness at that point.
Every map ships its own pair — `terrain3`, `campaign1`, each `save/<name>`
folder, each Workshop terrain.

They are **only textures**. The world loader calls `CreateManagedTexture` and
`TextureAccessInitTempResource`; nothing parses the pixels into another
structure. The game reads individual texels through the texture's vtable when it
needs them, and writes the maps back with `SaveToDDS` on save — which is how
depletion survives a reload.

This cost several sessions to establish. A guard page on the buffer that `fread`
filled never fired for game code: the only reader was the GPU driver. Virtual
calls do not appear in an import table, so nothing the loader hooked could see
the texel reads either. Ghidra settled it.

## Which channels are free

Eight channels exist and the base game reaches six. Measured over **every**
shipped map — `campaign1`, `campaign2`, `terrain3`–`terrain6`, `terrain_svk`,
`terrainblank` and all eighteen `tutorial_map*` folders — by counting non-zero
bytes per channel:

| Map | Component | Deposit | State |
|---|---|---|---|
| `resourcemap` | 0 | oil | taken |
| `resourcemap` | 1 | iron | taken |
| `resourcemap` | 2 | coal | taken |
| `resourcemap` | 3 | — | **free, with a caveat** |
| `resourcemap2` | 0 | uranium | taken |
| `resourcemap2` | 1 | bauxite | taken |
| `resourcemap2` | 2 | — | data on about half the maps; avoid |
| `resourcemap2` | 3 | copper | taken by this project |

`resourcemap` component 3 holds **no spatial data on any shipped map**: it is
either all 255 or all 0, never anything between, which is what an unused alpha
channel looks like rather than a deposit. The caveat is that "all 255" is the
common case — `terrain3`, `terrain4`, `terrain5`, `terrainblank` and every
tutorial map — and a deposit assigned to it there reads as maximum richness
across the whole map until something clears it. Paint over it in the editor or
ship a replacement map through the VFS.

`resourcemap2` component 2 is the layer the minimap has a sixth flag for at
`+0x18` and no button; it carries real data on `terrain3`, `terrain4`,
`terrain6` and the tutorials, and none on `campaign1`, `campaign2` or
`terrain_svk`. Whatever it is, it is not spare.

Past those two there is nothing left, and a third map would have to be created
and saved by the loader itself.

The measurement is a dozen lines of Python over the DDS payload — 128-byte
header, then `payload[c::4]` is component `c` of every pixel. Re-run it after
any game update before assuming a channel is still free.

## What the game does with a type

Deposit type selects a (texture, colour component) pair, decided by a chain of
comparisons compiled into a 3734-byte function at rva `0x1DD190`:

| Type | Texture | Component |
|---|---|---|
| 0 | local | 0 |
| 1 `IRON` | local | 1 |
| 2 `COAL` | local | 2 |
| 3 `GRAVEL` | terrain | 2 |
| 6 `URANIUM` | `resourcemap2` | 0 |
| 7 `BAUXITE` | `resourcemap2` | 1 |
| 8, 9 water | separate path | — |
| **10 `COPPER`** | `resourcemap2` | **3** — this project |

There is no table to redirect. A new type means new code — which is why this is
the one subsystem that emits instructions rather than swapping a pointer.

## Adding one

1. **Register the resource** it produces in `resources.ini`, if it needs one —
   see [04-adding-resources.md](04-adding-resources.md). The minimap button
   takes its icon straight out of that record.

2. **Add a section to `deposits.ini`.** Every key is documented in the file
   itself; the shape is

   ```ini
   [tin]
   token         = $TYPE_MINE_TIN
   type          = 11
   map           = resourcemap
   component     = 3
   radius        = ore
   icon          = tin_ore
   minimap       = 1
   editor        = tin
   ```

   `type` must be 10 or above — 0–9 are the game's — and 127 or below, because
   every comparison the patch splices in takes a sign-extended `imm8`.
   `editor` is capped at **seven characters**: the tool name is written over a
   clone of `paint_bauxite`'s and a longer string would not fit.

3. **Drop two 96×96 PNGs** in `vfs/media_soviet/editor/` named
   `tool_paint_<editor>.png` and `tool_erase_<editor>.png`, if the deposit
   declares a brush. Without them the buttons fall back to bauxite's icon and
   still work.

4. **Use the token** in a mine's `building.ini`, exactly where a stock mine
   writes `$TYPE_MINE_BAUXITE`.

No source change, no rebuild. `tesmioloader.log` reports what it made of the
section:

```
deposits  "tin" type 11 "$TYPE_MINE_TIN" -> resourcemap component 3, radius from the game, editor channel 3 (slot 6, column 6)
patch     deposit type 11 added: "$TYPE_MINE_TIN" in building.ini, resourcemap component 3
minimap   2 mod layer(s) hooked
editor    2 mod brush pair(s) hooked
```

A section that would produce a broken patch is **dropped, not repaired** — a
bad type number here becomes spliced code, so the cost of guessing is a
corrupted process rather than a wrong colour. Rejections say why:

| Message | Meaning |
|---|---|
| `type must be 10..127` | collides with the game's own types, or will not encode |
| `component must be 0..3` | there are four channels per map |
| `duplicate type` / `duplicate token` / `duplicate channel` | two sections claim the same thing |
| `no usable radius` | `radius` names neither a known constant nor a number |
| `WARN shares a channel with iron` | legal, and almost always a typo |

## The patch

`PatchDepositType` in `src/tesmioloader.cpp`, enabled by `deposit_patch = 1`.
Three sites, one cave, and the cave is emitted in a loop over the registry —
one case per declared deposit, chained by `rel32` forward branches, then the
displaced vanilla check reproduced at the end of each chain.

**Parser**, rva `0x10EAC8`. Each token check is `lea rdx,[string]` /
`lea rcx,[rbp+0x49A0]` / `call 0x84F340` / `test` / `jnz next` / set building
type and the deposit type / `jmp 0x118815`. The cave emits one of those per
declared token, then reproduces the `$TYPE_MINE_BAUXITE` check it displaced.
Token strings live in the cave's own data region, so nothing has to be found in
`.rdata`.

**Dispatch**, rva `0x1DD773`. Same shape: each case tests its type, samples
`[gameobj+0xF00]` or `[gameobj+0xF08]` — whichever map the section names —
through the sampler at rva `0x8360`, takes its own component, and stores the
richness where every other branch does. After the last case it reproduces the
type-6 check and rejoins the chain.

**Radius**, rva `0x1DCACD` — see [below](#third-site-the-search-radius).

Original bytes are compared before anything is written and a mismatch aborts all
three; so does a cave that would not fit, which is checked before the first byte
of the executable is touched. Every displacement is computed from the runtime
module base, so only the RVAs are hard-coded.

The cave is allocated within ±2 GB of the module — `call rel32` cannot reach
further — by walking allocation granularity outward from the module base. It is
8 KB, the low 2 KB data and the rest code; copper alone uses about 2.3 KB of it.

**Verify the emitted bytes, don't read them.** The generator is small enough to
re-implement in twenty lines of Python and disassemble with capstone, which
catches a wrong ModRM or an out-of-range branch in seconds. Reading hand-written
opcode arrays does not. Do this whenever the emitter changes — the failure mode
is a corrupted process, not a wrong number on screen.

## Painting deposits

Until a cheat-menu brush exists, deposits are painted by editing the map through
the VFS. Copy the terrain's `resourcemap2.dds` to
`tesmioloader/vfs/media_soviet/<terrain>/resourcemap2.dds`, write richness into
the alpha byte of each pixel (`128 + (y*1024 + x)*4 + 3`), leave the other three
channels alone.

Copying an existing channel with a spatial offset produces plausible deposit
shapes in places the terrain already supports mining.

**Use the terrain the game actually loads.** `config.ini` names `terrain3`, but
a new game may load `campaign1`; an early attempt painted the wrong map and
nothing happened. The trace log with `trace_filter = resourcemap` shows the real
path.

### Third site: the search radius

The dispatch chain alone was not enough. With only those two patches the mine
built, but the window showed **quality of source −2147483648 %**, production
stayed at zero, and no placement radius was drawn.

The cause was a second, much smaller table at rva `0x1DCA70` mapping deposit
type to *search radius*, called at the very top of the same function that owns
the dispatch chain. An unknown type falls through to `XORPS XMM0,XMM0` — radius
zero, so the mine searches nothing, the average over an empty set is NaN, and
the NaN cast to int is exactly `0x80000000`.

The patch replaces the tail of that function — the type-9 branch at `0x1DCACD`,
five bytes of `CMP ECX,9 / JNZ` — with a jump to a cave that reproduces the
type-9 check, adds one case per declared deposit, and keeps the zero fallback.
Each radius is **copied out of `.rdata` at patch time**, not referenced, so a
section that says `radius = ore` tracks whatever the game's own constant is.

**Lesson for the next deposit type:** the type-to-channel mapping is not the
only per-type data. Scan `.text` for `cmp dword ptr [reg+0x368], imm8` —
`83 [B8-BF] 68 03 00 00 imm8` — to enumerate every branch on deposit type. There
are 18 such functions; two more (`0x2BAD70`, `0x2A9902`) have not been examined
and may hold further per-type behaviour. Anything found there becomes another
loop over the registry, not another special case.

## Current state

Copper works end to end. The type parses from `building.ini`, the mine finds the
deposit, quality and radius display correctly, and the ore feeds the
concentrator through a conveyor. It is declared entirely in `deposits.ini` —
there is no copper-specific line of code left.

## The minimap layer

Done, and it needed no code patch. Two functions, both reached by decompiling
around `gui_minimap_bauxit`:

| RVA | What |
|---|---|
| `0x4BFEA0` | draws the five-icon button row and handles clicks |
| `0x4BDDE0` | draws the coloured deposit overlay for whichever icon is on |

Both are hooked additively — the original runs first through a trampoline, then
one button per mod layer and, for whichever is selected, its overlay pass are
appended. Buttons take row slots 5, 6, 7… in registry order.
`InstallMinimapPatch` in `src/tesmioloader.cpp`, enabled by `minimap_patch = 1`;
it does not hook at all if no section declares a layer.

### The state struct

The row and the overlay share one object. Its layer flags are tri-state ints —
0 idle, 1 hovered, 2 selected:

| Offset | Layer | ResourceVector | Component | Texture |
|---|---|---|---|---|
| `+0x04` | coal | (0,0,1,0) | 2 | `resourcemap` |
| `+0x08` | iron | (0,1,0,0) | 1 | `resourcemap` |
| `+0x0c` | oil | (1,0,0,0) | 0 | `resourcemap` |
| `+0x10` | uranium | (1,0,0,0) | 0 | `resourcemap2` |
| `+0x14` | bauxite | (0,1,0,0) | 1 | `resourcemap2` |
| `+0x18` | **none** | (0,0,1,0) | 2 | `resourcemap2` |

**There are six flags but only five buttons.** The overlay tests `+0x18` and
would happily draw resourcemap2 component 2, but the row never draws an icon
for it, so that layer is unreachable in the stock UI. Anything enforcing mutual
exclusion has to clear it too — which is why the loader clears all six, not
five, and keeps mod layer states in their own `DepositDef` rather than in this
struct. The base game's object is only ever read for exclusion and written with
zero, exactly as every vanilla click handler already does to its neighbours.

Geometry constants: row origin `0x90A9A0`/`0x90AB30`, step `0x90AA5C`, icon
scale `0x909E6C`, hit box `0x90A6C0`, badge offset `0x909CF0`. The overlay quad
is inset by `param[0x58] * 0.5f`, the 0.5 living at `0x909DF4` — the same
constant the badge size reads.

Each button's icon comes from `ResourceGet(self, "<name>")` and the texture
pointer at record `+0x48`, so a mod resource's button needs no new art.

### What the shader actually does

`media_soviet/shaders_d3d11/default_panel2d.inix` holds the `MinimapColors` and
`MinimapDesertColors` techniques as compiled DXBC. `D3DDisassemble` from
`d3dcompiler_47.dll` reads them out — a dozen lines of ctypes, no fxc needed.
The deposit branch of the pixel shader is:

```hlsl
if (MapType == 2) {
    float4 t = Texture2DStage0.Sample(SamplerStage0, uv);
    o.a   = dot(t, ResourceVector);     // dp4 — all four components
    o.rgb = vertexColour;
}
```

Three things follow, and all three matter:

- `dp4`, not `dot(t.rgb, ...)`. Component 3 was reachable all along; the base
  game simply never passes a vector that selects it.
- The colour is **not** in the shader. `o.rgb` is the vertex colour, which is
  the panel tint at `0x9BE30C`. Every vanilla layer sets it to the (1,0,0,1) at
  `0x90C2F0` — that red is where the overlay's colour comes from.
- Only stage 0 is sampled. The overlay always binds `resourcemap` to stage 0 and
  then, for the `+0x10`/`+0x14`/`+0x18` three, binds `resourcemap2` over the top
  of it. Same stage. Copper does the second bind only.

`TerrainHeight` and `TerrainPos` are set by the vanilla function but the shader
reflection marks them used only by the terrain-colour branch, so a deposit pass
does not need them.

### Two traps this cost a session to

**The decompiler reuses one variable for two constants.** In `0x4BDDE0` Ghidra
emits `fVar21 = GetTerrainHeight(...)`, hands it to the `TerrainHeight` shader
constant, and then reassigns `fVar21 = DAT_140909df4` — the 0.5 — before
computing the overlay rect. Reading that as one value and multiplying the quad
by a world height puts it hundreds of units off the panel and nothing draws at
all. The button worked, the overlay did not, and the difference was one
misread constant. Check the disassembly whenever a decompiled float feeds
geometry.

**`C3D_PANEL2D::Draw` does not draw.** It appends a quad to arrays the vertex
shader indexes with `BLENDINDICES` and flushes only when the bound state forces
it or when `EndDraw` does — and the flush is what commits the shader constants.
So a pass must open with `EndDraw` then `BeginDraw(technique)`, which is how
`0x4BDDE0` itself starts, and must restore any constant it changed *after*
`EndDraw`, never between `Draw` and `EndDraw`. `0x4BDDE0` also *returns* with a
bracket open, because its tail is `EndDraw` / `PrintAllTexts` /
`BeginDraw(NULL)`. Full detail in [07-pitfalls.md](07-pitfalls.md).

### Known cosmetic difference

Appending after the trampoline means a mod quad is drawn after the vanilla tail
has already drawn the minimap frame and the region outlines, so it sits on top
of them where the base game's layers sit underneath. Fixing it would mean
splicing into the middle of `0x4BDDE0` instead — a code patch, for a cosmetic
gain.

## The terrain editor

Done, and like the minimap it needed **no code patch** — the machinery was
already generic and three of its eight channels simply had no caller.
`InstallEditorPatch` in `src/tesmioloader.cpp`, enabled by `editor_patch = 1`.
Four additive inline hooks and two PNGs per brush in the VFS; it does not hook
at all if no section declares one.

The editor's tools are identified by **name strings**, not an enum. The current
tool is a `char*` at `editorSelf + 0xD428`, compared with `strcmp` wherever it
matters. It is really the tool's *descriptor* pointer — the name is the
descriptor's first field, which is why the same pointer is both `strcmp`ed and
indexed at `+0x2B5`.

### The four sites

| RVA | What it does | How to extend it |
|---|---|---|
| `0x233110` | draws the resource tab: five paint/erase pairs | post-hook, append a pair per mod deposit |
| `0x03AAA0` | tool lookup by name, the single choke point | used to clone bauxite's descriptors |
| `0x30D100` | tool dispatcher, a long `strcmp` chain | post-hook, act if one of ours is active |
| `0x2F0E70` | decides which tools use the round brush cursor | post-hook, set `self+0x10F0 = 1` |

**`0x233110` — the resource tab.** 1799 bytes, sets `self+0xADA8 = 6` (the tab
id) and then, per resource, calls `FUN_14003aaa0(self, "paint_coal")` and hands
the result to the button drawer at `0x3826C0`. Pairs are laid out in columns;
the drawer returns the next Y in `XMM0`, which is how the erase button lands
under its paint button. Column x advances by `DPI * [0x90AB9C] * [0x909EEC]`.

**`0x03AAA0` — the tool registry.** A linear search over a
`std::vector<Tool>` at `self+0xD280`..`+0xD288`, **stride 0x2D0**, matching the
name inlined at the descriptor's `+0x00`. Returns the descriptor or NULL. It is
called once per mod brush to fetch `paint_bauxite` / `erase_bauxite`, whose
descriptors are cloned; because the active tool *is* the descriptor pointer,
comparing against those clones is an identity test and nothing has to be
registered anywhere.

Descriptor fields the drawer uses: `+0x00` name, `+0x58` icon texture, `+0xB4`
icon path, `+0x2B4` and `+0x2B5` behaviour flags.

**The name has to fit.** `ReplaceInlineString` refuses to write a string longer
than the one it replaces — the space around the name holds real fields, so
anything that pads or fills to a buffer length destroys them, which also rules
out `strcpy_s`. `paint_bauxite` is 13 characters, so a `deposits.ini` `editor`
key gets seven. A pair that does not fit is dropped whole: half a brush would
either paint with no way to erase, or erase into bauxite's channel.

**The icon is a texture, not a path.** The drawer has a lazy branch — if
`strlen(tool+0xB4)` is non-zero *and* `+0x58` is null, it calls
`CreateManagedTexture` and `Load2DFromFile` itself. That branch is for tools
that come out of `building.ini`, which is the only thing that ever writes
`+0xB4` (through the format string `editor/tool_%s.png` at `0x88C580`, whose
single xref is inside the parser at `0x10E200`). On every **built-in** terrain
tool `+0xB4` is an empty string and `+0x58` is already loaded.

So clearing `+0x58` on a clone and expecting the drawer to refill it produces a
null bind, and writing a path into `+0xB4` means guessing the size of a buffer
nothing observable constrains. Load the texture directly instead — both calls
take the path as an ordinary argument, so the string can be the loader's own:

```
CreateManagedTexture(0x9EACD0, path)         // IAT import, rcx=middlepoint rdx=path
tex->vtbl[0x10](tex, path, 0, 0, 0, 0)       // slot 2, Load2DFromFile
tool[0x58] = tex
```

`+0xB4` is left as the empty string the clone inherited, which is exactly what
keeps the drawer's lazy branch from running.

**`0x30D100` — the dispatcher.** 42 838 bytes of `strcmp` chain. The resource
brushes all funnel into one function:

```
paint_coal    -> FUN_1402350d0(self, 1, 2)      erase_coal    -> (self, 0, 2)
paint_iron    -> FUN_1402350d0(self, 1, 1)      erase_iron    -> (self, 0, 1)
paint_oil     -> FUN_1402350d0(self, 1, 0)      erase_oil     -> (self, 0, 0)
paint_uranium -> FUN_1402350d0(self, 1, 3)      erase_uranium -> (self, 0, 3)
paint_bauxite -> FUN_1402350d0(self, 1, 4)      erase_bauxite -> (self, 0, 4)
```

A name it does not know falls through the whole chain doing nothing, so the
hook can let the original run in full and then act if the active tool turned
out to be one of ours.

### The brush is already generic

`FUN_1402350d0` at `0x2350D0` sets a "this map has resource X" byte in the game
object (`+0x23` coal, `+0x24` iron, `+0x25` oil, `+0x26` uranium, `+0x27`
bauxite), applies brush radius and rate, and calls the texel writer at
`0x238B00`. In between it does this:

```c
if (2 < idx) idx = idx + 1;                 // 3 -> 4, 4 -> 5
FUN_140238b00(z, &pos, idx + 1, sx, sy, delta, limit, 1);
```

so the channel index actually reaching the writer is 1, 2, 3, 5, 6 — **0, 4 and
7 are unreachable from this caller.** And in the writer:

```c
tex = (unsigned)(ch - 4) < 4 ? resourcemap2 : resourcemap;   // map = bit 2
switch (ch & 3) { 0: alpha  1: byte2  2: byte1  3: byte0 }   // component
```

Which decodes as `channel = map*4 + component'`, giving:

| ch | Texture | Component | Deposit |
|---|---|---|---|
| 1 | `resourcemap` | 0 | oil |
| 2 | `resourcemap` | 1 | iron |
| 3 | `resourcemap` | 2 | coal |
| **0** | **`resourcemap`** | **3** | — free |
| 5 | `resourcemap2` | 0 | uranium |
| 6 | `resourcemap2` | 1 | bauxite |
| 7 | `resourcemap2` | 2 | the flag-`+0x18` layer |
| **4** | **`resourcemap2`** | **3** | **copper** |

which is exactly `(map == resourcemap2 ? 4 : 0) | ((component + 1) & 3)`, and
that is what `DepositDef::editorChannel` computes. The formula was checked
against all six channels the base game reaches before it was trusted.

**The writer needed nothing.** It reads each texel through vtable slot 20,
modifies one byte, writes it back through slot 23, and brackets the whole loop
in slots 16 and 18 — exactly the calls 02-findings.md lists. `SaveToDDS` then
persists it, so painted deposits survive a reload for free.

### How the brush is reached

`0x2350D0`'s arithmetic cannot emit 0, 4 or 7 for any input, so calling it with
an index is out. Reimplementing it is the obvious alternative and the wrong
one: brush radius, strength, limit, the rate timer and the guards that stop the
brush painting through the open panel all live in those forty lines, and every
one of them would have to be copied correctly.

Instead the dispatch hook calls `0x2350D0` with **bauxite's** index and the
hook on `0x238B00` rewrites the single argument that differs, guarded by a flag
that is only set while that one call is in flight:

```c
if (g_brushDep >= 0 && channel == 6) channel = g_dep[g_brushDep].editorChannel;
```

Every other brush in the editor, bauxite's included, passes through untouched.
The one side effect of borrowing bauxite's index is the `gameobj+0x27` byte it
sets on the way past, so the hook saves and restores it and a map without
bauxite does not quietly acquire it.

### Two details that are easy to get wrong

**The clones need alignment.** The engine reads pointers and floats out of a
descriptor, so a `BYTE[0x2D0]` — alignment 1 — is not a valid place to put one.
`__declspec(align(16))`.

**The clones do not outlive the editor.** Leaving to the main menu and coming
back runs the tool builder at `0x2E9420` again, which re-creates every
descriptor *and its icon texture*. A clone taken in a previous session then
holds a texture that has been released and the buttons stop drawing. The
descriptors are useless as a staleness key — the vector is frequently rebuilt
into the same block, with the same names at the same addresses — so the clones
are keyed on **bauxite's own icon pointer**, which tracks exactly that teardown
and costs one comparison per panel draw.

**Do not use `strcpy_s` on the descriptor.** The space between the name at
`+0x00` and the icon path at `+0xB4` is not slack, it holds real fields
(`+0x48`, `+0x58`), and a function that pads or fills to a buffer length
destroys them. Copy exactly `strlen + 1` bytes, and only ever a string no
longer than the one being replaced.

### Buttons and icons

The panel hook appends one pair per mod brush at columns 5, 6, 7… on the same
two rows as the vanilla five, computing each position from the same `.rdata`
constants the vanilla grid uses — `x = DPI*(50 + 85) + column * DPI * 105 *
0.85`, paint row at `DPI*(250 + 80)`, erase row `DPI * 90 * 0.85` below it.
Reading the constants rather than hard-coding the pixels means a patch that
moves the grid moves the mod buttons with it.

The buttons have no hover tooltip: the vanilla accumulator is one stack local
passed through all ten calls and handed to `0x383BD0` before the function
returns, which has already happened by the time an appended hook runs. Icon,
hover tint, click and selection badge all work.

Icons live at `vfs/media_soviet/editor/tool_paint_<editor>.png` and
`tool_erase_<editor>.png`, 96×96 RGBA, loaded explicitly as above; a missing
file leaves the button showing bauxite's icon and the brush still works.

Copper's pair were made from the bauxite pair by remapping the ore through a
luminance ramp sampled from the mod's own `copper_ore.png`, so the button
matches the resource everywhere else in the UI. The ore mask is the set of
pixels that differ from **all four** other paint icons — the brush and frame
sit in roughly the same place in all five and match at least one neighbour, the
ore matches none. Diffing against a single icon is not enough: the brush is not
pixel-identical between any two of them, so a one-icon diff recolours the brush
handle as well.
