# Deposits

Deposit types are **not** resources. They are a separate, much smaller enum, and
adding one is the only thing in this project that required patching code.

**A plugin**: `plugins/deposits/deposits.cpp` builds to
`build/plugins/deposits.dll`, and deleting that DLL removes mod deposits — and
with them the registry other plugins read. See [09-plugins.md](09-plugins.md).

Nothing about any individual deposit is compiled in. `plugins/deposits.ini`
declares them — every section of it except `[deposits]` is a deposit — and one
registry drives all three subsystems: the code patch, the minimap layer and the
editor brush. **Adding a deposit is adding a section**; see
[Adding one](#adding-one) below. `[deposits]` itself
holds only the three switches that say which of those subsystems may touch the
game.

That registry is also published as a service, `TSM_SERVICE_DEPOSITS`, so another
plugin can read what was declared and carry its own per-deposit keys in the same
file. `depletion` does both.

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

Eight channels exist **in the maps the game ships**, and it reaches six of them.
Past that the plugin makes its own — see
[Maps past the engine's two](#maps-past-the-engines-two) — so what follows is
about the two that already exist, not about a ceiling.

Measured over **every** shipped map — `campaign1`, `campaign2`,
`terrain3`–`terrain6`, `terrain_svk`, `terrainblank` and all eighteen
`tutorial_map*` folders — by counting non-zero bytes per channel:

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

The measurement is a dozen lines of Python over the DDS payload — 128-byte
header, then `payload[c::4]` is component `c` of every pixel. Re-run it after
any game update before assuming a channel is still free.

## Maps past the engine's two

**Eight channels was never a property of the deposit system.** It is two
textures times four components, and the only thing that made two textures a
limit is that the world loader creates exactly two. Everything downstream is
already generic over which texture it is handed:

| | |
|---|---|
| the sampler, rva `0x8360` | takes the texture as its fourth argument |
| the texel writer, rva `0x238B00` | reads one out of the game object |
| the overlay shader | samples whatever is bound to stage 0 |

So `map = resourcemap3` in a section, or `map = auto`, and the plugin does for a
third map exactly what the world loader does for the first two.

### Loading

An **import swap on `CreateManagedTexture`**, not a code patch. The loader hands
it `"<folder>/resourcemap.dds"`, and that one path is both the signal that a
world is loading and the name of the folder it is loading from —
`resourcemap2`'s is not, because a terrain without one falls back to the bare
`resourcemap2default.dds` and the folder is gone. Seeing it, the plugin repeats
the loader's own three calls per extra map:

```
CreateManagedTexture(middlepoint, "<folder>/resourcemap3.dds")   // 0x7B1A
tex->vtbl[2] (tex, path, 0, 0, 0, 0)      Load2DFromFile         // 0x7B4E
tex->vtbl[19](tex)                        InitTempResource       // 0x7B5C
```

The hook re-enters itself — `LoadExtraMaps` calls straight back into
`CreateManagedTexture` — so a flag guards the recursion, and that flag is the
whole of the control.

**A terrain with no `resourcemapN.dds` gets a blank**, written once into
`vfs/media_soviet/tesmio/resourcemap_blank.dds`: 1024×1024, the same 128-byte
header `resourcemap2default.dds` carries, payload zeroed. It cannot *be*
`resourcemap2default.dds`, for two reasons — that file is not blank (components
0, 1 and 2 hold a stock uranium and bauxite layout; only its alpha is clear),
and `CreateManagedTexture` caches by path, so two maps loading one file would be
one texture. Each map is therefore **created under its own name and loaded from
whichever file exists**, which is why one blank can back any number of them.

### Saving

`0x7C20` is the function that writes `farmap`, `emissivemap` and both deposit
maps. It takes the folder in `rdx`, so an extra map is one more `SaveToDDS` with
the same argument. Painting and depletion survive a reload for the same reason
the vanilla channels do.

It is reached by **redirecting the one call to it**, at `0x42CD0E`, rather than
by splicing its prologue:

```asm
0042CD0B  49 8B D7           mov rdx,r15      the world folder
0042CD0E  E8 0D AF BD FF     call 0x7C20      <- the five bytes rewritten
0042CD13  ...                                 the return address
```

The rel32 points at a cave next to the executable holding one
`jmp qword ptr [rip+0]` to the detour, which calls `0x7C20` by address — there is
no trampoline and no stolen prologue at all. The call site is verified by
decoding it (`E8`, and the computed target is `0x7C20`), not by a byte compare,
so the check still holds if the function moves.

This started as an inline hook and **crashed every save**: `0x7C20`'s prologue is
13 bytes and byte 13 begins `mov rax,[rip+0x98a414]`, the stack-cookie load. The
14-byte jump cut that instruction in half. Written up in
[07-pitfalls.md](07-pitfalls.md).

### Sampling

The dispatch chain gets one substituted instruction. A vanilla map is

```asm
mov r9,[gameobj]
mov r9,[r9+0xF08]
```

and an extra one is

```asm
mov r9,[rip+slot]        ; a qword in the cave's own data
```

— one indirection shorter. The slot lives in the **cave**, not in the plugin's
globals, because the cave is guaranteed within `rel32` of the code reading it
while a DLL's data is wherever Windows put the module. `LoadExtraMaps` writes it
at every world load, and clears it when one starts.

### The scan's bracket

Handing the sampler a texture is not enough: the sampler reads a CPU-side copy,
and that copy exists only between `TextureAccessOpen` and `TextureAccessClose`.
`0x1DD190` brackets per deposit type, and the chain this plugin splices into
sits *inside* the bracket:

| Site | Check | Texture |
|---|---|---|
| `0x1DD458` | type ≤ 2 | open `resourcemap` |
| `0x1DD474` | type 6–7 | open `resourcemap2` |
| `0x1DD499` | type 3 | `MaskTextureOpen(terrain)` |
| `0x1DDE08` | type 3 | `MaskTextureClose(terrain)` |
| `0x1DDE25` | type ≤ 2 | close `resourcemap` |
| `0x1DDE45` | type 6–7 | close `resourcemap2` |

A mod type (10 and up) matches none of them, and for the first year of this
project that meant copper was sampled through a texture that was never mapped.
What hid it is that `TextureAccessClose` never clears the mapped-texel pointer
at `tex+0x158`: one paint of the channel in the editor leaves a stale, still
dereferenceable address behind, and the scan then reads last paint's staging
copy — which the close had just copied back from, so it is even correct. On a
map where the channel was never painted the pointer is zero and
`TextureAccesGetTexel` dereferences it: crash, fault address
`(x + rowpitch/4 · y) · 4`. That is the "place or hover the mine before the
deposit is painted" crash, and it is [07-pitfalls.md](07-pitfalls.md) now.

So `0x1DD458` and `0x1DDE25` are patch sites like the mask's: a cave checks
our types first — opening the game object's own map for the engine's two, or
the cave qword for an extra one, with a null test so an unloaded map skips the
call — then reproduces the displaced type ≤ 2 block and rejoins at
`0x1DD474`/`0x1DDE45`. The rejoin re-reads the type field, so the open cave
does not have to preserve `EAX` across the call, and a mod type matches
nothing downstream — no double-open. Verified against 11/15 bytes (the
displaced instruction plus the build's own rip-relative `MOV` after it) and
patched only when a deposit lives on a resource map, both or neither.

### Painting

The texel writer decodes bit 2 of its channel index into one of exactly two
pointers in the game object, so an extra map cannot be named by an index. The
brush hook therefore **puts the deposit's texture in `resourcemap2`'s slot for
the length of the call** and takes it straight back out. Reimplementing the
writer instead would mean reimplementing its bracket, its bilinear footprint and
its clamping for the sake of one pointer — and the same reasoning already
decided not to reimplement `0x2350D0`.

### What `auto` does not do

`map = auto` allocates only in maps past the engine's two, never the two free
vanilla channels. `resourcemap` component 3 reads **255 on every texel** of
`terrain3`, `terrain4`, `terrain5`, `terrainblank` and every tutorial map, so a
deposit handed it silently reports maximum richness everywhere. That is a fine
thing to opt into by naming it and a bad thing to be given. `resourcemap2`
component 2 is not free at all.

## The terrain's material mask

`map = terrain` puts a deposit where **gravel** already lives: component 2 of the
splat map at `terrain+0x158`, the one the ground textures blend through. Mining
it wears the surface away, which is what a gravel pit or a sand pit looks like.

Three things differ from a resource map, and each is one substitution.

**Sampling.** Gravel's dispatch is not in the chain at `0x1DD773` — it is a
separate branch at `0x1DD907`. But that branch writes the same `[rsp+0x5C]` and
takes the position from `xmm7`/`ebx`, which hold what `[rsp+0x40]` holds, so a
mask deposit is an ordinary case in the chain with the texture load replaced by
`mov r9,[gameobj] / mov r9,[r9+0xED8] / mov r9,[r9+0x158]`.

**The bracket.** What it cannot inherit is `MaskTextureOpen`. The sampler reads a
CPU-side copy and the mask is a texture the editor writes, so it must be
bracketed — and opening it per sample point would be a GPU `Map`/`Unmap` per
point, the mistake [07-pitfalls.md](07-pitfalls.md) already records. So there are
**two more patch sites**, `0x1DD499` and `0x1DDE08`, each five instructions of
exactly the same shape. They are verified and patched **only when a section asks
for the mask**, and both or neither: an opened mask that is never closed leaves a
D3D11 resource mapped.

**Painting.** The mask is painted by `C3D_TERRAIN::EditMask`, not by the deposit
texel writer, and its tab is the editor's **Rocks** tab rather than Resources.
Same shape as before: clone `paint_rock`/`erase_rock`, borrow the rock brush at
`0x235300`, and rewrite the channel argument in an import hook on `EditMask`
while one of our calls is in flight. `EditMask`'s channel encoding turns out to
be the deposit brush's — `(component + 1) & 3`, so rock is channel 3 and
component 2 — which is why one field carries both.

| Mask component | What is there |
|---|---|
| 0 | a ground layer, no deposit |
| 1 | a ground layer, no deposit |
| 2 | a ground layer — gravel's, and what the rock brush paints |
| 3 | a ground layer — what the oasis brush paints |

**None of the four is a free channel**, and calling them that was wrong. Every
one is a ground layer the terrain already blends, so a deposit here is mined
wherever that layer is painted — which is exactly how gravel works and the
reason to put a deposit in the mask at all. What the layer *looks* like is a
per-terrain asset question:

```
media_soviet/<terrain>/material.mtl
  $TEXTURE  5 tiles_normal/grass2.dds     +  6 grass2bump
  $TEXTURE  7 tiles_normal/field1.dds     +  8 field1bump
  $TEXTURE  9 tiles_normal/burma_dirt2.dds+ 10 grass1bump
  $TEXTURE 11 tiles_normal/rockburma.dds  + 12 rockburma_nm
```

Four layers, four mask components, and the shader has exactly four sets of
`LayerNTileSize` / `LayerNDisplacement` constants. **Giving a deposit a texture
of its own is editing that file** — a slot number and a `.dds`, served through
the VFS like any other asset, no code at all. Adding a *fifth* layer is not
possible without changing the shader.

Which `$TEXTURE` slot belongs to which mask component is **not established
here**; the cheap way to settle it is to paint one component in the editor and
look at the ground.

The tool name is cloned over `paint_rock`, ten characters, so `editor` is capped
at **four** for a mask deposit rather than seven.

### Limits

Ten maps (`resourcemap` … `resourcemap10`), forty channels, and 32 sections.
Each extra map is 4 MB of texture per world and 4 MB in every save that has one.

## What the game does with a type

Deposit type selects a (texture, colour component) pair, decided by a chain of
comparisons compiled into a 3734-byte function at rva `0x1DD190`:

| Type | Texture | Component |
|---|---|---|
| 0 `OIL` | `resourcemap`, by passing the sampler a null texture | 0 |
| 1 `IRON` | `resourcemap`, same way | 1 |
| 2 `COAL` | `resourcemap`, same way | 2 |
| 3 `GRAVEL` | the terrain's material mask at `[terrain+0x158]` | 2 |
| 6 `URANIUM` | `resourcemap2` | 0 |
| 7 `BAUXITE` | `resourcemap2` | 1 |
| 8, 9 water | separate path | — |
| **10 `COPPER`** | `resourcemap2` | **3** — this project |

There is no table to redirect. A new type means new code — which is why this is
the one subsystem that emits instructions rather than swapping a pointer.

## Adding one

1. **Register the resource** it produces in `plugins/resources.ini`, if it needs
   one — see [04-adding-resources.md](04-adding-resources.md). The minimap
   button takes its icon straight out of that record.

2. **Add a section to `plugins/deposits.ini`.** Every key is documented in the file
   itself; the shape is

   ```ini
   [tin]
   token         = $TYPE_MINE_TIN
   type          = 11
   map           = auto
   radius        = ore
   icon          = tin_ore
   minimap       = 1
   editor        = tin
   ```

   `type` must be 10 or above — 0–9 are the game's — and 127 or below, because
   every comparison the patch splices in takes a sign-extended `imm8`.
   `editor` is capped at **seven characters**: the tool name is written over a
   clone of `paint_bauxite`'s and a longer string would not fit.

   `map = auto` takes the next free channel, always in a map the plugin makes
   itself, and `component` is then not needed. Name a `map` and a `component`
   instead to put the deposit on a specific channel — including one of the two
   free vanilla ones, which `auto` will not hand out.

3. **Drop two 96×96 PNGs** in `vfs/media_soviet/editor/` named
   `tool_paint_<editor>.png` and `tool_erase_<editor>.png`, if the deposit
   declares a brush. Without them the buttons fall back to bauxite's icon and
   still work.

4. **Use the token** in a mine's `building.ini`, exactly where a stock mine
   writes `$TYPE_MINE_BAUXITE`.

No source change, no rebuild. `tesmioloader.log` reports what it made of the
section:

```
deposits  "tin" type 11 "$TYPE_MINE_TIN" -> resourcemap3 component 0, radius from the game, editor channel 5 (slot 6, column 6)
deposits  1 map(s) past the engine's two: resourcemap3..resourcemap3
maps      wrote the blank deposit map to ...\vfs\media_soviet\tesmio\resourcemap_blank.dds
maps      1 map(s) past the engine's two are loaded and saved with the world
patch     deposit type 11 added: "$TYPE_MINE_TIN" in building.ini, resourcemap3 component 0
minimap   2 mod layer(s) hooked
editor    2 mod brush pair(s) hooked
```

and, once a world loads:

```
maps      resourcemap3 = 000001C0... (blank)
```

or the terrain's own file where it has one.

A section that would produce a broken patch is **dropped, not repaired** — a
bad type number here becomes spliced code, so the cost of guessing is a
corrupted process rather than a wrong colour. Rejections say why:

| Message | Meaning |
|---|---|
| `type must be 10..127` | collides with the game's own types, or will not encode |
| `component must be 0..3` | there are four channels per map |
| `map must be resourcemap..resourcemap10, or auto` | ten maps is the plugin's own bound |
| `duplicate type` / `duplicate token` / `duplicate channel` | two sections claim the same thing |
| `no free channel left` | `auto` with all forty channels spoken for |
| `no usable radius` | `radius` names neither a known constant nor a number |
| `WARN shares a channel with iron` | legal, and almost always a typo |

## The patch

`PatchDepositType` in `plugins/deposits/deposits.cpp`, enabled by `code_patch = 1`.
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
`InstallMinimapPatch` in `plugins/deposits/deposits.cpp`, enabled by `minimap = 1`;
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

### Hover text

Also no unexported formatter, which is what this document used to claim. Every
vanilla layer builds its own with a `swprintf`:

```c
Resource* r = ResourceGet(&game, "coal");
FUN_140005290(&buffer /* 0x9E24B0 */, 0x800, L"%ls: %ls",
              GetString(&lang, 0x2F3), GetString(&lang, r[0x40]));
```

One global wide buffer the panel draws afterwards, and whoever was hovered last
owns it. A mod layer writes the same thing from the same three pieces it already
has — the resource its `icon` names, that record's caption id one field along
from the texture, and the same `0x2F3` label.

**`GetString` is dereferenced from its import slot at call time, not cached.**
`resources` swaps that import to answer the private caption ids it mints, and
plugins load in directory order with `deposits` first — so a pointer read at init
is the engine's own, the mod id finds nothing, and the tooltip reads
`Deposits: ` with the name missing. See [07-pitfalls.md](07-pitfalls.md).

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

### The red terrain overlay

Painting a vanilla deposit turns the editor's terrain grid red under the brush.
That is a render pass at rva `0xAEE0`, driven by six bytes in the game object —
one per channel the base game can paint, each carrying that channel's unit
vector. **There is no byte for component 3 of either map**, which is why copper
painted a channel nothing drew: not a missing capability, a missing flag.

The hook reproduces none of it. It **brackets** the vanilla pass: sets the flag
whose vector is the component wanted, points the map that pass reads at
whichever texture the deposit lives in — so `resourcemap2` and the plugin's own
maps go through the branch that only knows how to read the first — lets the
engine draw, and puts all of it back. Component 3 borrows coal's flag and has
the sixteen bytes of its vector rewritten for the length of that one call.

The gate is the editor object, handed over by the per-frame cursor hook and
consumed here. The render function runs in the game too, and a tool left
selected in a previous session would otherwise paint the terrain red in the
middle of a city. One editor frame, one overlay.

Full table of the six flags and the shader in
[02-findings.md](02-findings.md).

### Known cosmetic difference

Appending after the trampoline means a mod quad is drawn after the vanilla tail
has already drawn the minimap frame and the region outlines, so it sits on top
of them where the base game's layers sit underneath. Fixing it would mean
splicing into the middle of `0x4BDDE0` instead — a code patch, for a cosmetic
gain.

## The terrain editor

Done, and like the minimap it needed **no code patch** — the machinery was
already generic and three of its eight channels simply had no caller.
`InstallEditorPatch` in `plugins/deposits/deposits.cpp`, enabled by `editor = 1`.
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

**The hover text works, and the accumulator was never the obstacle.** It is one
qword, not a buffer: `0x3826C0` writes the hovered tool's **descriptor** into it
at `0x382A29`, and the panel hands that to `0x383BD0`, which reads two fields off
the descriptor it names —

```c
tool+0x48 == 0  ->  wcscpy(editorSelf+0xD5A0, GetString(lang, tool+0x40))
tool+0x48 != 0  ->  the rich building tooltip, drawn at the mouse
```

Every terrain tool has `+0x48 == 0`, so a mod brush takes the simple path and
needs nothing but a text id at `+0x40`. The buttons were silent only because the
panel calls `0x383BD0` before it returns, which is before an appended hook has
drawn anything, and our accumulator went nowhere. **Calling the consumer
ourselves, once, after our own buttons, is the whole fix.**

The text is the deposit's own name, not the donor's: `+0x40` on each clone is set
to the caption id of the resource its `icon` names, read from that record one
field along from the texture the button already takes. Nothing is minted — a
resource from `plugins/resources.ini` already carries a private id the
`resources` plugin answers, and a base-game one carries the game's.

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
