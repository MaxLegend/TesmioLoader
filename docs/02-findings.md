# Findings — game internals

Everything here is for **SOVIET64.exe v1.1.1.7, 64-bit DX11.1**, PE timestamp
2026-03-25 16:13 UTC, 10 308 096 bytes. Addresses are **RVAs**; add the runtime
module base. ASLR is on, so nothing may be hard-coded as an absolute address.

Where something is inferred rather than observed, it says so.

## The binaries

`SOVIET64.exe` — game logic. Plain MSVC build, `.text` uncompressed, imports
readable. `DllCharacteristics = 0x8160`: high-entropy VA, dynamic base,
NX. **No Control Flow Guard**, no forced integrity. Leftover PDB path
`D:\GitLab\soviet-republic\SOVIET64.pdb`.

`C3DDLL64.dll` — the "C3D" engine. **2218 exported symbols with MSVC mangling
intact**, so full signatures come for free. The executable imports 733 of them.

`steam_api64.dll` is used for 10 functions: stats, achievements, Workshop
callbacks. No anti-tamper, no anti-cheat, no packer anywhere.

## Resources

### The vector

A global `std::vector` of fixed-size records in `.data`.

| | |
|---|---|
| vector object | rva `0x9E11C0` — `{ begin, end, capacity }`, three pointers |
| record stride | 832 bytes (`0x340`) |
| live records | 57 |
| capacity | 63 |

The array is heap-allocated and **rebuilt at every map load** at a fresh address.
Anything caching a record pointer must notice `begin` changing.

### Record layout

| Offset | Meaning |
|---|---|
| `+0x00` | name, inline, NUL-terminated, up to 32 bytes |
| `+0x40` | localisation id of the display caption |
| `+0x48` | the resource's icon texture, `media_soviet/resources/<name>.png` |

`+0x48` is what the minimap button row binds, so any UI that wants a resource's
icon can take it straight from the record instead of loading its own asset.

Found by diffing full records of `workers`, `coal`, `rawiron` and `alcohol`:
`+0x40` was the only field that differed between them and stayed stable across
runs — 518, 508, 524, 512 respectively. The rest of the 832 bytes is not
mapped; cloning an existing record is how a new one is made viable.

### Index order

Indices 0–56, exactly the field order of the `Resources` struct in
`media_soviet/scripts/SOVIETInstructions.txt`:

```
workers eletric vehicles trains heat gravel rawgravel plants steel aluminium
prefabpanels bricks wood oil chemicals coal rawcoal iron rawiron bauxite
rawbauxite bitumen boards uranium yellowcake uf6 nuclearfuel nuclearfuelburned
fuel fabric alcohol cement alumina food clothes meat livestock asphalt concrete
ecomponents mcomponents plastics eletronics explosives water usagewater
fertiliser_liquid waste_gravel waste_steel waste_aluminium waste_plastic
waste_bio fertiliser waste_burnable waste_toxic waste_other waste_ash
```

**`waste_mixed` (57) and `service_material` (58) appear in that struct but are
not in the vector** — they are standalone objects elsewhere. Using either to
derive the array base yields a bogus address; this caused an infinite re-arm
loop once. The reserved slots the struct shows after them are padding in the
script VM's view only, not spare capacity in the engine.

### ResourceGet

```
rva 0x2AA7C0, 735 bytes
Resource* ResourceGet(void* self, const char* name, ...)
```

Returns a pointer to the record, `NULL` when unknown, and logs
`"ResourceGet - not found %s"` (format string at rva `0x8A6B00`, exactly one
code reference, at `0x2AAA5C`).

It is called speculatively on every token the .ini parser meets, so failures are
routine — the stock game asks for `waste` 19 times per load and never finds it.
That makes it a single choke point through which every resource name resolution
passes.

### Per-resource assets

**Only the icon is looked up by name.** This was wrong here for a long time and
the mistake is visible in the game: a mod resource with correctly named meshes
was still drawn as its template.

| Path | Purpose | How the engine finds it |
|---|---|---|
| `media_soviet/resources/<name>.png` | 48×48 RGBA icon | `"resources/%s.png"` at rva `0x899C48`, formatted from the record's name |
| `media_soviet/resources/<name>1..4.nmf` | cargo pile, four fill stages | **literal path in `.rdata`** |
| `media_soviet/resources/<name>_vehicle.nmf` | load on a vehicle | **literal path in `.rdata`** |
| `media_soviet/resources/<name>.mtl` | material for the above | **literal path in `.rdata`** |

The icon is loaded by the UI texture pass at rva `0x2960DE`, which walks the
whole vector and writes each texture to record `+0x48`. It runs **per world
load**, after `Initializing resources` and at `Initializing GUI textures`, so a
record published during the former gets its icon in the latter.

The meshes come from the resource table at rva `0x2A1D60` — 30 KB of
straight-line code with one block per resource and every path spelled out:

```asm
mesh = middlepoint->CreateManagedMesh("resources/steel.nmf");
mesh->LoadFromFile("resources/steel.nmf", middlepoint, true);
mesh->LoadMaterial("resources/steel.mtl", 0);
record[+0x318] = mesh;
```

so a record cloned from a template inherits the template's *mesh objects* and no
amount of correct file naming changes what is drawn. `tesmioloader` therefore
makes the same three calls itself — see
[04-adding-resources.md](04-adding-resources.md).

### The mesh slots

The record's last five qwords, and they end exactly at the record boundary:
`0x338 + 8 == 0x340`.

| Offset | Contents |
|---|---|
| `+0x318` | vehicle load — and the **only** mesh an open-transport resource has |
| `+0x320` | pile stage 1 |
| `+0x328` | pile stage 2 |
| `+0x330` | pile stage 3 |
| `+0x338` | pile stage 4 |

Read off `0x2A1D60`, which builds each record in one stack buffer at `rsp+0x40`
and pushes it. Its prologue is `lea rbp,[rsp-0x290]` before `sub rsp,0x390`, so
**`rbp == record + 0xC0`**, and every block writes its meshes to
`rbp+0x258`…`+0x278`. The caption id lands at `rbp-0x80`, which is `+0x40` and
corroborates the offset independently.

A resource with no cargo geometry — `workers`, `eletric` — has all five null,
which is what makes "does the template have stage 1" a usable test for bulk
versus open.

The engine calls, all imported by name:

| Symbol | Signature |
|---|---|
| `?CreateManagedMesh@C3D_MIDDLEPOINT@@…` | `C3D_MESH* (C3D_MIDDLEPOINT*, const char* path)` |
| `?LoadFromFile@C3D_MESH@@…` | `int (C3D_MESH*, const char* path, C3D_MIDDLEPOINT*, bool)` |
| `?LoadMaterial@C3D_MESH@@…` | `int (C3D_MESH*, const char* mtl, int)` |

`CreateManagedMesh` caches by path, so asking for the same file twice is free
and re-arming after every map load does not leak.

### Transport class

Each record carries one. A building storage declared with a different class
reports **zero capacity** — the symptom is a storage line showing
`0.00 of 0.00 t`. Ores cloned from `rawiron` or `bauxite` are
`RESOURCE_TRANSPORT_GRAVEL`.

## Deposits

### Storage

Two textures per map, `resourcemap.dds` and `resourcemap2.dds`, both
1024×1024 BGRA — 128-byte DDS header plus 4 194 304 bytes, no mipmaps. Each
colour channel is one deposit type; the byte value is richness at that point.

Channel usage measured over **every** shipped map — `campaign1`, `campaign2`,
`terrain3`–`terrain6`, `terrain_svk`, `terrainblank` and all eighteen
`tutorial_map*` folders — by counting non-zero bytes per channel:

- **`resourcemap2` component 3 is empty on every map.** Copper's channel.
- **`resourcemap` component 3 holds no spatial data on any map either**: it is
  all 255 or all 0, never anything between — an unused alpha channel, not a
  deposit. Usable as a second free channel, but on the maps where it is 255
  (`terrain3`–`terrain5`, `terrainblank`, every tutorial) a deposit assigned to
  it reads as maximum richness everywhere until something clears it.
- `resourcemap2` component 2 carries real data on `terrain3`, `terrain4`,
  `terrain6` and the tutorials, none on `campaign1`, `campaign2` or
  `terrain_svk`. It is the layer the minimap has a flag for at `+0x18` and no
  button. Not spare.

The other five are oil, iron, coal, uranium and bauxite. So of eight channels,
two are free and six are spoken for. Re-measure after a game update before
assuming that still holds.

The maps are **only ever textures**. The loader calls
`C3D_MIDDLEPOINT::CreateManagedTexture`, then `TextureAccessInitTempResource` to
get a CPU-reachable copy; nothing parses the pixels into another structure. On
save they are written back with `SaveToDDS`, which is how depletion persists.

### Type numbers

From the .ini parser and the type-to-name function:

| Value | Token |
|---|---|
| 1 | `$TYPE_MINE_IRON` |
| 2 | `$TYPE_MINE_COAL` |
| 3 | `$TYPE_MINE_GRAVEL` |
| 4 | `$TYPE_MINE_WOOD` |
| 6 | `$TYPE_MINE_URANIUM` |
| 7 | `$TYPE_MINE_BAUXITE` |
| 8 | `$TYPE_MINE_WATER` |
| 9 | `$TYPE_MINE_WATER_SURFACE` |
| 10 | `$TYPE_MINE_COPPER` — added by this project |

`$TYPE_MINE_OIL` is assigned from a register the decompiler did not resolve; it
is 0 or 5. The dispatch chain handles a type 0, so 0 is likely OIL.

Building type 7 means "mine"; water wells are building type 92 (`0x5C`).

### Type to (texture, component)

Compiled as a chain of comparisons inside a 3734-byte function at rva
`0x1DD190`, chain starting `0x1DD773`:

| Type | Texture | Colour component |
|---|---|---|
| 0 | `resourcemap` — the sampler's default, reached by passing `R9 = 0` | 0 |
| 1 | `resourcemap`, same way | 1 |
| 2 | `resourcemap`, same way | 2 |
| 3 | **terrain mask**, `[terrain+0x158]` — the material splat map `C3D_TERRAIN::EditMask` paints, bracketed by `MaskTextureOpen`/`Close` | 2 |
| 6 | `resourcemap2` | 0 |
| 7 | `resourcemap2` | 1 |
| 8, 9 | water — separate path via `FUN_1404D5B60` | — |
| 10 | `resourcemap2` | 3 — added by this project |

Types 6 and 7 load the texture explicitly from `[gameobj+0xF08]`. Types 0–2 pass
`R9 = 0` and let the sampler fall back to `[gameobj+0xF00]` — an earlier reading
of the decompiler's "local" here was wrong; the disassembly at `0x1DD6E1` is a
plain `XOR R9D,R9D`.

### Sampler

```
rva 0x8360
C3DFCOLOR* Sample(void*, C3DFCOLOR* out, float* worldPos, Texture* tex)
```

Defaults `tex` to `[gameobj+0xF00]` when null. Converts world position through
`C3D_TERRAIN::GetOffset` and `GetTerrainSize` into texel coordinates, reads a
2×2 block through the texture vtable and bilinearly interpolates. Returns
`out`. Out-of-range positions return a zero colour.

### Related functions

| RVA | What |
|---|---|
| `0x5920`–`0x7C1B` | world loader; creates both deposit map textures |
| `0x7C20`–`0x7D1A` | writes `farmap`, `emissivemap` and both deposit maps via `SaveToDDS` |
| `0xBFFF4` | deposit type → token name, knows exactly the seven texture-backed types |
| `0x10E200` | `building.ini` parser, 62 979 bytes; mine-type branches from `0x10EAC8` |
| `0x1DCA70` | deposit type → **search radius**, 111 bytes, returns in `XMM0` |
| `0x1DD190` | the big one, 3734 bytes: finds deposits under a building. **Fifteen** comparisons against the type field, not just the dispatch chain |
| `0x1B3690` | **one mine, one tick** — building type 7. Reached from the dispatcher at `0x139A80` |
| `0x1B1220` | the same for building type 92, the water well, via `0x188FC0` |
| `0x139A80` | the building dispatcher, 20 964 bytes: one `if (typeDesc[0x360] == n)` per building type |

### What a mine keeps

`0x1DD190` writes a `std::vector` of **28-byte** sample points into
`building+0xE90` (`end` at `+0xE98`):

| Offset | Field |
|---|---|
| `+0x00` `+0x04` `+0x08` | world x, y, z |
| `+0x0C` | richness — the sampled colour component |
| `+0x10` | weight, `1 - distance / radius` |
| `+0x14` | per-point progress; wood only, zero for everything else |
| `+0x18` | distance from the building |

Quality of source is `sum(w*r)/sum(w)` over it, kept in `building+0xDF8`, and
production is `building+0xDDC = workers × quality × max`. **`+0xDF8` is written
once at construction and never again for an ore mine** — which is exactly why
deposits are infinite in the base game, and the whole opening for
[08-depletion.md](08-depletion.md).

`0x1DD190` opens with `vec.clear()`, so calling it a second time is a resample.
The water branch of both tick functions already does that every 50 seconds.

### Search radius

A separate, much smaller table, and the one that is easy to miss:

```c
float GetDepositRadius(int type)      // rva 0x1DCA70
{
    if (type == 0)                           return [0x90ABFC];
    if (type == 1 || type == 2 || type == 6) return [0x90AD50];   // iron, coal, uranium
    if (type == 7)                           return [0x90AA40];   // bauxite
    if (type == 3)                           return [0x90A9B8];   // gravel
    if (type == 4)                           return [0x90ADD0];   // wood
    if (type == 8)                           return [0x90AC9C];   // water
    if (type == 9)                           return [0x90AC38];   // water surface
    return 0;                                                     // anything else
}
```

Called at the very top of `0x1DD190`:

```c
radius = GetDepositRadius(building[0x368]) * <scale>;
```

A type this table does not know gets radius zero, so the mine searches nothing,
the average over an empty set is NaN, and the building window prints the NaN
cast to int: **−2147483648 %**. No placement radius is drawn either. A new
deposit type needs an entry here as well as in the dispatch chain.

## Finding the rest of the type-dependent code

Scanning `.text` for `cmp dword ptr [reg+0x368], imm8` — bytes
`83 [B8-BF] 68 03 00 00 imm8` — finds every place that branches on deposit type.
There are 18 such functions. Most compare against 4 (wood) and are
woodcutting-specific. The ones that matter so far:

| Function | Sites | Note |
|---|---|---|
| `0x1DD190` | 15 | radius call, dispatch chain, and more |
| `0x2BAD70` | 7 | six comparisons against 0, one against 9 — not yet examined |
| `0x2A9902` | 3 | compares against 5 — not yet examined |
| `0x786AC0` | 4 | mixed 3 and 4 |

This scan is the fastest way to check whether a new deposit type is missing an
entry somewhere: add the type, then look for symptoms in whichever of these has
not been taught about it.

### Painting deposits — the engine's own brush

The terrain editor already contains a fully generic deposit brush, and one of
its channels is unused.

```
rva 0x238B00, 1101 bytes
void PaintDeposit(float z, C3DVECTOR3* pos, uint channel,
                  float innerR, float outerR, int delta, uint limit, bool bracket)
```

It picks the texture and colour byte out of `channel` alone:

```c
tex = (unsigned)(channel - 4) < 4 ? resourcemap2 : resourcemap;
switch (channel & 3) { 0: alpha  1: byte2  2: byte1  3: byte0 }
```

so `channel = map*4 + component'`, with the falloff between `innerR` and
`outerR`, `delta` added per tick (negative to erase), and `limit` clamping.
Texels go through vtable slots 20 and 23, bracketed by 16 and 18.

| channel | Texture | Component | Deposit |
|---|---|---|---|
| 1 | `resourcemap` | 0 | oil |
| 2 | `resourcemap` | 1 | iron |
| 3 | `resourcemap` | 2 | coal |
| **0** | **`resourcemap`** | **3** | **free** |
| 5 | `resourcemap2` | 0 | uranium |
| 6 | `resourcemap2` | 1 | bauxite |
| 7 | `resourcemap2` | 2 | no editor tool, no minimap button |
| **4** | **`resourcemap2`** | **3** | **copper** |

Inverted, `channel = (map == resourcemap2 ? 4 : 0) | ((component + 1) & 3)`,
which agrees with all six vanilla rows and is what the loader computes.

Its only caller, `0x2350D0`, maps the editor's five paint/erase pairs onto it
with `if (2 < idx) idx++` then `idx + 1`, which can emit 1, 2, 3, 5, 6 and
never 0, 4 or 7. Those three are not missing a capability, they are missing a
caller. See [05-deposits.md](05-deposits.md) for the editor side.

## Building information panels

The window that opens on a building is built by a family of functions in the
`0x78xxxx`–`0x7Axxxx` range, one per kind of panel. They share a shape worth
knowing, because it makes any of them extensible from a post-hook.

| RVA | Panel |
|---|---|
| `0x786AC0` | the mine: **Quality of source**, **Current production per workday** |
| `0x789E10` | transport: **Amount of material transported per second** |
| `0x79DF00` | a third, not examined |

`FUN_140786ac0(game, window, ...)`:

| Window offset | Contents |
|---|---|
| `+0x01` | non-zero while the panel is not drawn; the builder returns immediately |
| `+0x04`, `+0x08` | panel position |
| `+0x28`, `+0x2C` | panel offset |
| `+0x240` | the building the window is about |
| `+0x250` | **the running Y, written at the very end — the panel's bottom edge** |

Layout is a fixed X and a running Y, both ordinary locals:

```c
x = DPI*[0x90ABB0] + (window[0x04] - DPI*[0x90A8E4] + window[0x28] + DPI*[0x90A9E4]);
y = DPI*[0x90AA08] + window[0x08] + window[0x2C];    // first row
y += DPI*[0x90A9E4];                                 // 35, one row
```

with `DPI` at `0x992088`. A row is
`C3D_LANGUAGE::GetString(&[0x997590], id)` for the label and
`C3D_FONTMANAGER::PrintLeftUnicode(&[0x996FB0], [0x995220], x, y, 0xFF990000,
L"%ls: ", text)` to draw it; the value is printed again at `x + labelWidth`,
measured through the font's own vtable slot 5.

**`+0x250` is what makes a row appendable**: read it in a post-hook, draw there,
write back `y + DPI*35`, and the window grows to fit. Used by the depletion
plugin — see [08-depletion.md](08-depletion.md).

Label ids resolve through `tools/assets/btf.py`: `0xC82` "Quality of source",
`0xC83` "Current production per workday", `0x223` "Water quality".

## The terrain editor

Tools are identified by **name string**, not by an enum. The active tool is a
`char*` at `editorSelf + 0xD428`.

| RVA | What |
|---|---|
| `0x233110` | draws the resource tab — five paint/erase pairs |
| `0x03AAA0` | tool lookup by name over the tool vector |
| `0x30D100` | tool dispatcher, 42 838 bytes of `strcmp` chain |
| `0x2F0E70` | decides which tools get the round terrain cursor (`self+0x10F0`) |
| `0x3826C0` | draws one tool button, returns the next Y in `XMM0` |

The tool vector lives at `editorSelf + 0xD280`..`+0xD288`, **stride 0x2D0**.
Descriptor fields in use: `+0x00` name, `+0x58` cached icon texture, `+0xB4`
icon path, `+0x2B4`/`+0x2B5` behaviour flags. The icon path is built from the
name through `editor/tool_%s.png` at `0x88C580`, so a tool's art is chosen by
what it is called.

`0x03AAA0` is to tools what `ResourceGet` is to resources: a single choke point
every by-name lookup passes through, and the obvious place to inject.

## Minimap

| RVA | What |
|---|---|
| `0x4BFEA0` | five-icon button row, hover and click |
| `0x4BDDE0` | the coloured deposit overlay |

Both share a state object whose six tri-state layer flags sit at `+0x04`
through `+0x18`; only five have buttons. Full table, the shader semantics, and
the constants in [05-deposits.md](05-deposits.md).

The overlay's colour channel is chosen by the `ResourceVector` float4 through a
**`dp4`** in the pixel shader of `default_panel2d.inix`, so all four components
are selectable and nothing in the shader had to change for copper. The overlay
colour is not in the shader at all — it is the `C3D_PANEL2D` vertex tint at
`0x9BE30C`, which every vanilla layer sets to the (1,0,0,1) at `0x90C2F0`.

Compiled shader blobs read out with `D3DDisassemble` from
`d3dcompiler_47.dll` via ctypes; the `.inix` files are a name followed by its
vertex and pixel `DXBC` blobs, whose lengths are at blob `+0x18`.

## Globals

`DAT_1409941F0` — rva `0x9941F0` — the main game object.

| Offset | Contents |
|---|---|
| `+0x23`–`+0x27` | "this map has coal / iron / oil / uranium / bauxite", set by the editor brush |
| `+0xED8` | `C3D_TERRAIN` |
| `+0xF00` | `resourcemap` texture |
| `+0xF08` | `resourcemap2` texture |
| `+0xF10` | `farmap` texture |
| `+0xF18` | `emissivemap` texture |

Building object: mine type at `+0x368`.

## Engine texture vtable

`C3DAPI_D3D11_TEXTURE`, `C3DDLL64.dll` rva `0x187BF0`. Every entry is an
exported symbol, so the whole table reads out by name.

| Slot | Offset | Method |
|---|---|---|
| 2 | `+0x10` | `Load2DFromFile` |
| 16 | `+0x80` | `TextureAccessOpen` |
| 17 | `+0x88` | `TextureAccessOpen2` |
| 18 | `+0x90` | `TextureAccessClose` |
| 19 | `+0x98` | `TextureAccessInitTempResource` |
| 20 | `+0xA0` | `TextureAccesGetTexel(x, y) -> colour` |
| 21 | `+0xA8` | `TextureAccesGetTexelFloat` |
| 23 | `+0xB8` | `TextureAccesSetTexel(x, y, colour)` |
| 36 | `+0x120` | `SaveToDDS(path)` |

`TextureAccesSetTexel` is the obvious route to painting deposits at runtime.

**`TextureAccessOpen` is a D3D11 Map, not a lock.** Disassembled at
`C3DDLL64.dll` export rva `0x18D40`:

```asm
call [rax+0x28]    ; ID3D11Device::CreateTexture2D   USAGE_STAGING, CPU read|write
call [rax+0x178]   ; ID3D11DeviceContext::CopyResource   GPU texture -> staging
call [r10+0x70]    ; ID3D11DeviceContext::Map
```

and `TextureAccessClose` (`0x18FC0`) is `Unmap` plus the `CopyResource` back.
So an open/close pair moves the whole 4 MB map across the bus twice and uses the
**immediate context**, which is not thread-safe. Nothing may bracket a texel
read or write from anywhere but the render thread, and it should be done once
for as much work as possible. The staging texture is created once and cached at
`tex+0x168`, guarded by the byte at `tex+0x170`.

This cost a driver crash — see [07-pitfalls.md](07-pitfalls.md).

## The built-in script VM

The game embeds **Pyrois** (Michal Kuchárik, 2017), a bytecode VM. Its opcode
and structure definitions ship in `media_soviet/scripts/SOVIETInstructions.txt`
— 261 opcodes, about 230 of them game-specific, 20 structs, 69 struct methods.
Scripts are distributable as `WORKSHOP_ITEMTYPE_SCRIPT` and their state is saved
(`runningScripts.bin`).

It is a scenario and campaign API: read buildings, vehicles, people, resources;
set money and prices; start fires, epidemics, earthquakes; drive objectives and
windows. It cannot change simulation rules, add resources, build UI, or hook
events. That ceiling is why this loader exists.

Its value to reverse engineering is high anyway: the `Resources` struct in that
file is effectively a header for the engine's resource order, and the
`Building` struct names 72 fields.

## Language files

`media_soviet/soviet<Language>.btf`, twenty-one of them plus
`sovietComments.btf`. Big-endian throughout:

```
u32 count
u32 file size
u32 (offset of something after the payload)
count * { u32 id, u32 offset into the payload, u16 length in bytes }
UTF-16LE payload
```

`sovietEnglish.btf` has 7906 entries. **Ids run 300 to 580231**, sparsely, and
twenty of the twenty-one full translations end at exactly 580231. Workshop
buildings that use `$NAME <id>` draw from the same space and stay under 8000.

Anything the loader mints for itself has to sit above 580231 — see
[07-pitfalls.md](07-pitfalls.md) for what happened when it did not.

## Materials

`C3D_MATERIAL::Load` is `C3DDLL64.dll` rva `0x9CE80`. Two things worth knowing:

- **There is no comment syntax.** The parser matches its keywords wherever they
  occur in the file, so a `$TEXTURE` inside a `;`-prefixed line is a `$TEXTURE`.
- `$TEXTURE` paths are relative to `media_soviet/`; `$TEXTURE_MTL` paths are
  relative to the `.mtl`'s own folder. Stock building materials use both, which
  matters when one is copied into a Workshop item.

A `$TEXTURE` before any `$SUBMATERIAL` writes through a null submaterial array
at `material+0x18` and faults at rva `0x9D0AE`.

## Main menu version line

The line along the bottom of the main menu is drawn by a single call at the end
of the menu builder.

| RVA | What |
|---|---|
| `0x28AEF0`–`0x28B5B2` | builds the main menu: `GetString` for each entry, `GAMEPAD_CONFIRM`, `PostQuitMessage`, and the version line last |
| `0x28B55C` | `lea rax,[rip+0x60A26D]` — computes the format string |
| `0x8957D0` | `L"v%d.%d.%d.%d (64 bit DX11.1 - GPU: %ls)"` — one code reference, that `lea` |
| `0x8961E0` | `"v%d.%d.%d.%d (64 bit DX11.1)"`, the narrow variant — **no reference in `.text`** |

The call itself:

```
mov  [rsp+0x48],7 / [rsp+0x40],1 / [rsp+0x38],1 / [rsp+0x30],1   ; 1.1.1.7
mov  [rsp+0x28],rax                    ; the format string
mov  [rsp+0x20],0xFFAA0000             ; colour
call C3D_FONTMANAGER::PrintLeftUnicode ; IAT slot 0x86C880
```

so the four version numbers are immediates, not a lookup, and the GPU name is
the wide string returned by the virtual call on the object at `0x9EACD0` a few
instructions earlier.

`?PrintLeftUnicode@C3D_FONTMANAGER@@QEAAXPEAVC3D_FONT@@MMKPEB_WZZ` demangles to
`void PrintLeftUnicode(C3D_FONT*, float x, float y, unsigned long colour,
const wchar_t* fmt, ...)`. `x` and `y` are passed in **both** `xmm2`/`xmm3` and
`r8d`/`r9d`, which is what MSVC does for a variadic callee.

## The terrain editor object

`SOVIET64 + 0x9D5F10`, a global rather than a heap object. The per-frame update
at `0x105E39` passes it to both the cursor decision (`0x2F0E70`) and the tool
dispatcher (`0x30D100`) as `rcx`, and the resource tab (`0x233110`) gets the
same pointer.

| Offset | Contents |
|---|---|
| `+0xD280`..`+0xD288` | the tool vector, stride `0x2D0` |
| `+0xD428` | active tool — the descriptor pointer, which is also its name |
| `+0x10F0` | round terrain cursor, set per frame |
| `+0x11DB0` | cleared alongside a tool selection |

Selection is a **toggle**, at `0x38302F` inside the button drawer:

```asm
cmp   qword ptr [rdi+0xD428], r14   ; r14 = the descriptor being drawn
mov   ecx, 0
mov   rax, r14
cmove rax, rcx                      ; clicking the active tool clears it
mov   qword ptr [rdi+0xD428], rax
```

`+0x10F0` has exactly two writers, both inside `0x2F0E70` — cleared at
`0x2F12D0`, then set to 1 at `0x2F14D5` if the active tool's name matched
anything in the chain — and one reader, at `0x3DB749`.

## Cheat menu

Present in the release build. GUI layer names `cheatsystem`, `cheatgeneral`,
`cheataction`; actions include `BuyAllResources`, `building_sellallvehicles`,
`building_callvehicleshome`, `close_all_windows`. It is reachable in game
already, so its activation path was never traced.

The engine also ships a full frame profiler (`FrameProfiler_StartFrame` /
`EndFrame` / `StartSection`) and debug drawing (`DebugDrawBox`, `DebugDrawLine`,
`DebugDrawTerrainCollisionNodes`) — and the game calls into them, so those code
paths exist in the shipped build.

## Constants worth having by value

Read with `tools/pe/rdata.py`; every one of these shows up as a bare `DAT_` in
the decompiler.

| RVA | Value | What |
|---|---|---|
| `0x909B14` | 0.001 | the argument every `PowerTime` call in the simulation passes |
| `0x90AA90` | 60 | what a production rate is divided by per tick |
| `0x909F70` | 1.0 | the one-second rollover, and the general-purpose 1 |
| `0x90A840` | 10 | deposit sample grid step, world units |
| `0x90AD50` | 210 | search radius for iron, coal and uranium |
| `0x90AA40` | 50 | bauxite radius — and the water re-scan period |
| `0x90A9B8` / `0x90ABFC` / `0x90ADD0` | 30 / 130 / 250 | gravel, oil, wood radius |
| `0x90AC9C` / `0x90AC38` | 170 / 25 | water, water surface radius |
| `0x909E6C` | 0.7 | how close another mine has to be to claim a sample point |
| `0x9D4EE0` | — | the `C3D_TIMER` the whole simulation steps on, inline in `.data` |

## State

Copper works end to end: the type parses, the mine finds the deposit, quality
and radius display correctly, the ore feeds the concentrator, a sixth minimap
button shows the deposits, and the terrain editor has a paint/erase pair for
them.

Only the deposit type itself needed spliced code. The minimap layer and the
editor brush are both pure hook-and-append — see [05-deposits.md](05-deposits.md).

None of it is copper-specific any more: `deposits.ini` declares the deposit and
one registry drives the code patch, the minimap and the editor alike. What is
scarce is the channel, not the machinery.
