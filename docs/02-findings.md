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

A `std::vector` of fixed-size records, and **a field of the game object rather
than a free-standing global**: `ResourceGet` reads it out of `self + 0xC2B0`,
and the game object is the static at rva `0x9D4F10`, so
`0x9D4F10 + 0xC2B0 == 0x9E11C0`. That is why no instruction in `.text` computes
`0x9E11C0` RIP-relatively from inside `ResourceGet` — every access is through
`rcx`.

| | |
|---|---|
| game object | rva `0x9D4F10` — not `0x9941F0`, which is a different object |
| vector object | rva `0x9E11C0` = `game+0xC2B0` — `{ begin, end, capacity }` |
| record stride | 832 bytes (`0x340`) |
| live records | 57 |
| capacity | 63 — MSVC's growth sequence 1,2,3,4,6,9,13,19,28,42,**63** after 57 `push_back`s |

The array is heap-allocated. **A map load clears it rather than destroying it**
— `end = begin` at rva `0x25EC7A` — so `begin` and the capacity survive, the 57
records are pushed back into the same block, and anything that raised the
capacity once keeps it for the rest of the session. `begin` does change when the
allocation is replaced, so it is still the thing to watch; it is not the thing
that changes on every load.

The three pointers are written by exactly one function outside the vector's own
growth: `0x25D4B0`, which zeroes them at `0x25D79E`/`0x25D7A6` and clears at
`0x25EC73`.

### The record cache behind the vector

`game+0xC2C8`…`+0xC488` — **57 qwords of `Resource*`, filled one `ResourceGet`
call at a time** by a straight run of code at rva `0x2A82B0`, immediately after
the array is built. These are the records the engine wants by hand rather than
by lookup.

Fifty-three point into the array. The first, `game+0xC2C8` = rva `0x9E11D8`, is
`workers` — index 0 — so it is **bit-for-bit equal to `begin`**, which is what
made an earlier memory scan report "two containers holding the array base". It
is not a container; it is a cached record. The other four —
`+0xC3B8`, `+0xC3C0`, `+0xC418`, `+0xC420` — are the standalone records
`ResourceGet` compares against before scanning anything, and never point into
the array.

Anything that moves the array has to carry this block with it. `resources`
does, in `RebaseResourceCache`.

### Record layout

| Offset | Meaning |
|---|---|
| `+0x00` | name, inline, NUL-terminated, up to 32 bytes |
| `+0x40` | localisation id of the display caption |
| `+0x44` | price kind — see [Money](#money) |
| `+0x48` | the resource's icon texture, `media_soviet/resources/<name>.png` |
| `+0x58` / `+0x5C` | **price**, RUB and USD |
| `+0x70` / `+0x74` | the previous price, mirrored by the pass for `eletric` only |
| `+0x78` / `+0x7C` | **base price**, RUB and USD — `$Economy_Base*` |
| `+0x80` / `+0x84` | the previous base, mirrored by the drift pass |
| `+0x88` / `+0x8C` | sell and buy multipliers, `0.95` and `1.05` on every resource |

`+0x48` is what the minimap button row binds, so any UI that wants a resource's
icon can take it straight from the record instead of loading its own asset.

Found by diffing full records of `workers`, `coal`, `rawiron` and `alcohol`:
`+0x40` was the only field that differed between them and stayed stable across
runs — 518, 508, 524, 512 respectively. The rest of the 832 bytes is not
mapped; cloning an existing record is how a new one is made viable.

### The resource table

`0x2A1D60`, 30 KB of straight-line code with one block per resource, each built
in a stack buffer at `rsp+0x40` and pushed with `add qword ptr [rbx+8], 0x340`.
Its prologue is `lea rbp,[rsp-0x290]` before `sub rsp,0x390`, so **`rbp` is
`record + 0xC0`** and every constant in it reads off directly:
`mov [rbp-0x80], 0x207` is caption id 519.

**It only covers the first 35 records** — `workers` through `clothes`. There are
exactly 35 pushes in the whole executable at that stride and the last one is at
`0x2A618A`; the remaining 22 records are built somewhere that does not use the
same idiom. Their constants are still in the same code range and still written
through `rbp`, which is how `explosives` at `0x2A6D0B` and `water` at `0x2A6E52`
were read.

Everything the table writes is a *starting* value. The price pass below
overwrites `+0x58`/`+0x5C` for all but one kind before the first frame.

### Money

Two float pairs and one integer, and between them they are everything the trade
window quotes. Confirmed field by field against the `$Economy_*` blocks a save
writes into `media_soviet/save/<name>/stats.ini`, which is the only place the
engine's own names for them are visible.

**`+0x44` is the price kind**, and the first thing the pass branches on:

| Value | Resources |
|---|---|
| `-1` | `workers` — skipped by the pass entirely, so its constants stand |
| `-2` | `eletric` |
| `-3` | `vehicles` |
| `-4` | `trains` |
| `-5` | `heat` — pinned to `1.0` in both currencies |
| `0` | raw: everything mined, pumped, grown or scrapped |
| `1` | manufactured |
| `2` | `food` and `clothes` |

**`+0x58`/`+0x5C` is the price.** The trade window's buy figure is this times
`+0x8C` (`1.05`) and its sell figure this times `+0x88` (`0.95`) — one number
per currency behind both, which checks out to five figures against any save:
coal `16.940750` to buy and `15.327346` to sell are `16.133 × 1.05` and
`16.133 × 0.95`.

**`+0x78`/`+0x7C` is the base price**, `$Economy_BaseRUB` and
`$Economy_BaseUSD`. It is an *input* to the solver, not a result. Only fifteen
base-game resources have one and they are the ones that come out of the ground
or cannot be produced at all: `rawgravel` 1.2, `plants` 1.8, `wood` 7.5, `oil`
40, `rawcoal` 5.3, `rawiron` 4.5, `rawbauxite` 4.5, `uranium` 5.2/4.2,
`explosives` 15/13, `water` 0.3/0.2, `usagewater` 40, `waste_toxic` 115/100 and
four other wastes.

**Which float is which currency was settled on the three that differ** —
`uranium`, `explosives`, `water` — because every other base pair is the same
number twice. `+0x78` is RUB in all three. One thing does not fit and is
recorded here rather than explained away: world init at `0x28ED59` writes
`workers+0x5C = 9.0` and `+0x58 = 12.0`, while the save calls the workday cost
`RUB 9.0` and `USD 12.0`. Either the record's worker price is not the workday
cost, or one of the two identifications is backwards; the base-price evidence is
three independent resources against that one coincidence.

### The price pass

```
rva 0x2A92D0   void RecomputeResourcePrices(GameObject*)
rva 0x2A9470   float ResourcePrice(GameObject*, Resource*, bool rub, bool usd, ...)
```

`0x2A92D0` walks the whole vector and, per record, reads `+0x44` and then either
skips it (kind `-1`), pins it to `1.0` (kind `-5`), or calls `0x2A9470` twice and
stores the results in `+0x5C` and `+0x58`. Called from world init at `0x28ED78`
and from the economy module at `0x2FBB95`, each time followed by `0x2A9F40`.

`0x2A9470` is where the base price is consumed. Its shape is
`workers_price × k + resource_base`, once per currency, with the two flags
selecting `(+0x58, +0x78)` or `(+0x5C, +0x7C)` — the pairing that makes both
fields the same currency. A handful of resources are special-cased by name
(`plants`) or against a cached record; everything else falls into two loops over
the building types at `game+0x11B20`, stride `0xBE8`, looking for one whose
output list contains this resource and summing what its inputs cost.

**A resource no building type produces prices at zero**, whatever its base is:
both loops fall through to `0x2A9F0D`, which returns the register zeroed on the
way in. Nothing consults `+0x78` on that path. This is the whole explanation for
a modded resource showing `0` in the trade table while another one shows a real
figure — copper is mined, concentrated, smelted and refined, so all four price
themselves, and a name declared in `[list]` and used by no `$PRODUCTION` line
cannot.

A third pass at `0x2FB390` drifts the base prices: for every record whose base is
non-zero it multiplies `+0x7C` and `+0x78` by two independent random walks, then
mirrors them into `+0x84` and `+0x80`. That is why a base written once does not
stay put, and why `resources` writes it again before every recompute.

### The economy vectors

`$Economy_*` in a save is six `std::vector<{Resource*, float value, float mul}>`
on the economy object, 16 bytes an entry:

| Offset | Section |
|---|---|
| `+0x198` | `$Economy_PurchaseCostUSD` |
| `+0x1B0` | `$Economy_PurchaseCostRUB` |
| `+0x1C8` | `$Economy_SellCostUSD` |
| `+0x1E0` | `$Economy_SellCostRUB` |
| `+0x1F8` | `$Economy_BaseRUB` |
| `+0x210` | `$Economy_BaseUSD` |

The parser is one strcmp chain at `0x2FFD60` handing each section to `0x301C10`,
which clears the vector and pushes one entry per line: `ResourceGet` on the name,
then two floats. **Keyed by `Resource*`, not by index** — so a mod resource
round-trips through a save without anything being taught about it, and a
resource the save does not mention simply has no entry.

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
Resource* ResourceGet(GameObject* self, const char* name, ...)
```

Two passes, in this order:

1. Four **standalone** records — `self+0xC3B8`, `+0xC3C0`, `+0xC418`, `+0xC420`
   — gathered into a throwaway vector and compared by name. These are the
   objects that are in the script VM's `Resources` struct but not in the array:
   `waste_mixed`, `service_material` and two more.
2. The array at `self+0xC2B0`, walked with `strcmp` against each record's inline
   name, stride `0x340`.

So the resolver is a **linear name scan, not a map** — which is exactly why
appending a record and moving `end` is enough to make a new resource resolvable,
and why nothing else has to be intercepted.

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

### Creating and saving them

Four calls, and there is nothing else to a resource map. The world loader at
`0x5920` takes the folder in **rdx** and does, per map:

| RVA | Call |
|---|---|
| `0x7B1A` | `CreateManagedTexture(*(void**)gameobj, "<folder>/resourcemap.dds")` |
| `0x7B4E` | `tex->vtbl[2](tex, path, 0, 0, 0, 0)` — `Load2DFromFile` |
| `0x7B5C` | `tex->vtbl[19](tex)` — `TextureAccessInitTempResource` |

then `[gameobj+0xF00]` or `[gameobj+0xF08]` takes the result. `resourcemap2`
alone has a fallback: `C3DHelp_CheckIfFileExist` on its path, and the bare
`resourcemap2default.dds` when it is missing — **so `resourcemap`'s is the only
one of the two paths that always carries the folder name.**

The saver at `0x7C20` takes the same folder in `rdx` and is four
`tex->vtbl[36](tex, "<folder>/....dds")` calls — `SaveToDDS` — for `farmap`, the
emissive map and both deposit maps.

That is the whole contract, which is why a **third** map needs no engine support
at all: repeat those four calls and hand the pointer to a sampler that already
takes one. `deposits` does exactly that for `resourcemap3` upward — see
[05-deposits.md](05-deposits.md).

`resourcemap2default.dds` is **not** a blank: 1024×1024 like the rest, and
components 0, 1 and 2 carry a stock uranium and bauxite layout. Only its alpha
is clear.

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
| `0x5920`–`0x7C1B` | world loader; creates both deposit map textures. Folder in `rdx` |
| `0x7C20`–`0x7D1A` | writes `farmap`, `emissivemap` and both deposit maps via `SaveToDDS`. Folder in `rdx` |
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

### The red terrain overlay

Painting a deposit in the editor turns the terrain grid red where the channel is
rich. It is a render pass of its own inside `FUN_14000aee0` (rva `0xAEE0`,
called from exactly one place, `0x482824`, with no arguments at all), and it is
driven by **six bytes in the game object**:

| Byte | Deposit | Map | Component | `SelectedResources` |
|---|---|---|---|---|
| `+0x23` | coal | `resourcemap` | 2 | (0,0,1,0) at `0x90BDF0` |
| `+0x24` | iron | `resourcemap` | 1 | (0,1,0,0) at `0x90BDA0` |
| `+0x25` | oil | `resourcemap` | 0 | (1,0,0,0) at `0x90BD80` |
| `+0x26` | uranium | `resourcemap2` | 0 | (1,0,0,0) |
| `+0x27` | bauxite | `resourcemap2` | 1 | (0,1,0,0) |
| `+0x28` | — | `resourcemap2` | 2 | (0,0,1,0) |

**There is no seventh byte**: component 3 of either map has no flag, which is
why a copper brush painted a channel nothing drew. The pass takes the *first*
flag set in each group, swaps the terrain's mask texture (`terrain+0x158`) for
the map, binds the `Resources` technique and renders. Its pixel shader is four
instructions:

```hlsl
float a = dot(SelectedResources, tex.Sample(uv));   // dp4
o = float4(1,1,1,1) + a * float4(1.5,-1,-1,0);      // white -> red
```

A `dp4` again, so component 3 was always reachable. `0x2350D0` sets its own byte
on the way past, which is what makes the overlay appear the moment you paint.

### The terrain material mask

Gravel is not in a resource map. It is component 2 of the **terrain's own mask**
at `terrain+0x158`, and the editor's Rocks tab paints the same mask:

| Tool | Call | `EditMask` channel | Component |
|---|---|---|---|
| `paint_rock` / `erase_rock` | `0x235300(self, mode)` | 3 | 2 — gravel's |
| `paint_oasis` / `erase_oasis` | `0x235510(self, mode)` | 0 | 3 |

```
C3D_TERRAIN::EditMask(terrain, &pos, channel, innerR, outerR, delta, limit, true)
```

Both functions are the same code with one constant changed, and the channel
encoding is the deposit brush's: `channel = (component + 1) & 3`. The Rocks tab
itself is `0x22EE30`; the dispatcher entries are at `0x312CE6` and `0x312D08`.

Gravel's dispatch is **not** in the type chain at `0x1DD773`. It is two separate
sites in the same function:

| RVA | What |
|---|---|
| `0x1DD499` | `if (type == 3) MaskTextureOpen(terrain)` — the bracket, once per scan |
| `0x1DD907` | the sample: `r9 = [terrain+0x158]`, sampler, `movss xmm0,[rax+8]` (component 2) |
| `0x1DDE08` | a third `cmp type,3` — the close |

Its sample block writes the same `[rsp+0x5C]` the type-6 block does and takes the
position from `xmm7`/`ebx` rather than `[rsp+0x40]`, but those hold the same
value — so a mask deposit **can** be emitted as an ordinary case in the
`0x1DD773` chain with one substitution, `mov r9,[gameobj]; mov r9,[r9+0xED8];
mov r9,[r9+0x158]`. What it cannot inherit is the bracket, and opening the mask
per sample point would be a GPU `Map`/`Unmap` per point — the mistake
[07-pitfalls.md](07-pitfalls.md) already records. A fourth patch site at
`0x1DD499` is the way in.

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

## Electricity, heat, and building storages

Electricity is **not** a special case in the engine. It is an ordinary resource
in an ordinary storage, moved between buildings by code that does not care what
kind of building either end is. The same is true of heat.

### A building's storages

`building+0x970`…`+0x978` is a `std::vector` of storages, stride `0xE0`.

| Offset | Contents |
|---|---|
| `+0x00` / `+0x08` | `std::vector<Slot>` begin and end, stride 16 — one slot per resource this storage can hold |
| `+0x8C` | capacity, the figure `$STORAGE` declared |
| `+0x90` | transport class, `RESOURCE_TRANSPORT_*` |
| `+0xA8` | what has been handed out through it this tick |

| Slot offset | Contents |
|---|---|
| `+0x00` | the resource record |
| `+0x08` | content |
| `+0x0C` | quality, 0..1 — a **source** with 0 hands out nothing. For electricity this *is* the node's voltage: `*(float**)(building+0x10E8)` points straight at it, confirmed by every line of a runtime dump agreeing |

**Quality is only ever raised inside a transfer**, and a transfer only happens
into a shortfall. A storage sitting at capacity with quality 0 therefore stays
that way — it holds energy and has no way to be told the power is back. That is
not a bug to route around casually: it is why a branch lights up the moment a
wire is connected, while the storages are still empty, and goes dark again once
they fill.

Wire connections are a vector at `building+0xA10`…`+0xA18`, stride `0x60`:
`+0x00` is the connection type (7 `ELETRIC_HIGH`, 8 `ELETRIC_LOW`), `+0x28` the
line object. The line's two ends are at `+0x20` and `+0x28`, and each end holds
a vector of buildings at `+0x70`…`+0x78` whose first entry is the building —
take whichever end does not lead back where you started, which is how both
`0x1B8EE0` and `0x1BB700` open.

Built by the `$STORAGE*` parser at rva `0xE40F0`, which writes `+0x8C` and
`+0x90` from its arguments and pushes one slot per resource whose per-class
factor at `resourceRecord + 0xCC + class*0x20` is non-zero. **That factor is why
a storage declared with the wrong transport class reports zero capacity.** A
companion byte at `resourceRecord + 0xE8 + class*0x20` excludes the pair
outright. `+0x88` records which `$STORAGE_*` token produced the storage: `-1`
plain, 0 basic, 1 medium, 2 advanced and mediumadvanced, 3 hotel, 10 carplant,
50 fuel — and for the `$STORAGE_DEMAND_*` family the four shop goods are named
in the code rather than looked up, which is the subject of
[11-needs.md](11-needs.md).

Classes that matter here: **9 `RESOURCE_TRANSPORT_ELETRIC`**, **14
`RESOURCE_TRANSPORT_HEATING`**. Both come from
`media_soviet/scripts/SOVIETInstructions.txt`, which also gives every
`BUILDINGTYPE_*` number: 17 powerplant, 18 substation, 19 transformator, 31/32
electricity export/import, 70 heating plant, 71 heating substation, 72 heating
switch.

### Moving it

| RVA | What |
|---|---|
| `0x139A80` | the building dispatcher. Collects power plants (17) and transformers (19) into vectors of their own, and heating plants (70) into another |
| `0x1B8EE0` | `(game, building)` — the power plant's: walk the wires, collect every reachable class-9 storage, hand energy over |
| `0x1BB700` | `(game, building, what, priority)` — the grid node's, and the one a transformer uses. `what` = 0 hands out **voltage** (slot quality) and no energy, 1 hands out energy; `priority` is matched against `building+0xE22+connIndex`, and only a filtered call reaches a neighbour flagged unpowered at `+0x1130` |
| `0x1BDD40` | the transfer, reached by both. Receiver's share is `(capacity − content)/shortfall`; the source may give at most `dt × its own capacity × scale`. **Its sixth argument gates the whole transfer**: non-zero runs only the tail, which propagates quality |
| `0x1B9640` | the substation's version, over the buildings in range at `building+0x10C8` rather than over wires |
| `0x1BD4C0` | one grid node's update — computes voltage as `2 × wattage/breaker` for types 17–19 and 35 |
| `0x1D1E10` | the production tick. Sets slot quality to 1 for a power plant's class-9 storage and a heating plant's class-14 one |

Connections are at `building+0xA10`…`+0xA18`, stride `0x60`, with the connection
type at `+0x00`: 7 is `ELETRIC_HIGH`, 8 is `ELETRIC_LOW`.

The three floats the script VM exposes as `fEletric_Wattage`, `fEletric_Voltage`
and `fEletric_CurcuitBreakerCapacity` are reached through **pointers** at
`building+0x10E0`, `+0x10E8` and `+0x10F8`; `+0x1100` is a plain copy of the
wattage and `+0x1128` is the voltage dial, eased towards its target by
`0x1BD37C` rather than written outright.

`building+0x1130` is **"the solver did not reach this node"**. One writer,
`0x13B02E` inside the dispatcher, which sets it on every grid node its walk out
from the power plants missed; two readers, that same solver on the next tick and
`0x1BB9BD` inside `0x1BB700`, which refuses to hand energy to a neighbour
carrying it. Anything that means to act as a source without a plant behind it
has to clear this for itself — see [10-accumulator.md](10-accumulator.md).

**Building type is checked in exactly one place in the transfer path**, and only
to keep power plants and electricity importers out of the receiver list
(`type != 17 && type != 31`). Everything else about the path is generic, which is
what makes a battery possible without new engine machinery — see
[10-accumulator.md](10-accumulator.md).

### The building vector

`game+0x11B08`…`+0x11B10`, an array of `Building*`. The dispatcher's own
argument is the game object, and it walks this several times per tick. A
building is finished when `+0x604 >= 1.0` and going away when `+0xEA8` is
non-zero — both are the dispatcher's own tests.

### The building type descriptor

`building+0x318` is the building's **type**, the thing `building.ini` parsed
into, and the whole game asks it what kind of building this is:

| Type offset | Contents |
|---|---|
| `+0x360` | the `BUILDINGTYPE_*` number — 2 living, 3 shop, 6 factory, 7 mine, 8 field, 17 powerplant, 18 substation, 19 transformer, 30 unknown… |
| `+0x364` | a second, finer number. Living buildings are told apart by `+0x360 == 2 && +0x364 == 1` |
| `+0x814` | divisor in the shop's tourist-spending term |

Both are initialised to `0x1E` — 30, `BUILDINGTYPE_UNKNOWN` — at `0x10DFF9`
and `0x10E003`, in the parser's own state reset, and set from the `$TYPE_*`
`strcmp` chain inside the parser at `0x10E200`, which writes `parser+0x1E10`.
`$TYPE_SHOP` is at `0x10EBBD` and each link is eleven instructions:
`lea rdx,<literal>` / `call 0x84F340` / `test` / `jne next` / `mov eax,<number>`
/ `mov [rbp+0x1E10],eax` / `jmp done`.

That is the same shape as the deposit-type chain, so a genuinely new `$TYPE_*`
is the same kind of splice — and the same caveat applies twice over, because
`0x139A80`, the building dispatcher, decides what a building *does* from
`+0x360`, and a number it does not know ticks not at all. See
[13-buildings.md](13-buildings.md).

`media_soviet/scripts/SOVIETInstructions.txt` lists every `BUILDINGTYPE_*` with
its number and is the authority.

## Citizens

### The person

`operator new(0x750)` inside the constructor at rva `0x823290`, which
randomises every status float in one run. Live people are a global array of
`Person*` at `0x9E75B8`…`0x9E75C0`, stride 8.

| Offset | Contents |
|---|---|
| `+0x20` | the building the person is in |
| `+0x70` | age; `0x8368B0` turns it into the eight-step factor every demand is scaled by |
| `+0xC8` | non-zero suppresses every service demand — a foreign worker or a tourist |
| `+0xD8` | **eleven status floats**, in exactly the order the script VM lists them: happiness, food, health, soviet, alcohol, culture, sport, religion, clothing, electronic, crime |
| `+0x110` | demand count |
| `+0x118` | **demand array**, stride `0x80`, capacity **7** |
| `+0x4F0` | unsatisfied-demand count, capped at 10, then entries at `+0x4F8` of `{ float amount, int kind, Resource* }` |
| `+0x71C` | 0 citizen, 1 soviet tourist, 2 western tourist |
| `+0x734` | money spent, in the currency `+0x71C` picks |

One demand, `0x80` bytes: `+0x00` amount still wanted, `+0x04` amount in total,
`+0x08` kind (`0xF` while being built, **1 and 2 are the two a shop serves**),
`+0x10` the `Resource*`, then two `0x34`-byte targets at `+0x18` and `+0x4C`.

The capacity is arithmetic rather than a declared bound: `(0x4F0 - 0x118) /
0x80 == 7`, and an eighth entry would run over the unsatisfied count. Nothing
in the game bounds-checks the append, and nothing has to — a food demand is
kind 1 and the planner clears every "wants a service" flag when it sees one, so
a hungry citizen carries four demands and a fed one at most seven.

### The daily plan

```
rva 0x836960   FUN_140836960(game, person)   ~15 KB
```

Reached from `0x830640` when a person is at home. Copies whatever the old list
had left over into `+0x4F0`, decays the statuses, resets `+0x110` to zero at
`0x836F8B`, and rebuilds through **nine** conditional append sites: food
`0x837757`, meat `0x837F1B`, clothes `0x838573`, electronics `0x838C71`, then
five resourceless service demands at `0x8392BD`, `0x8396A6`, `0x839A1A`,
`0x839D8A` and `0x83A1D3`. The four goods are `ResourceGet` calls on literals
at `0x8FEC70`…`0x8FEC88` — there is no table.

Too large for the decompiler, which dies on it; it was read as disassembly.

### Shopping

```
rva 0x171DA0   FUN_140171da0(game, building)   building type 3
```

From the dispatcher at `0x13DE28`. The second half is the sale, and it is
completely generic: for each customer in `building+0xBD8`…`+0xBE0`, for each of
their demands of kind 1 or 2, for each storage slot whose resource matches,
move `min(slot content, demand total × dt)` across and subtract it from both.
A tourist is additionally charged `resource+0x64` RUB or `resource+0x60` USD.

Read from the decompiler rather than inferred, because a whole building hangs
on it:

```c
for (customer in building+0xBD8)
  for (d in customer+0x118, stride 0x80)
    if ((unsigned)(d.kind - 1) < 2)                      // kind 1 or 2
      for (s = 0; s < (building+0x978 - building+0x970) / 0xE0; s++)
        for (slot in storage s, stride 0x10)
          if (slot.res == d.resource && slot.content > 0 && d.total*dt < d.amount)
              move it across
```

The storage loop is **unconditional**: no test on the `$STORAGE_*` token that
built the storage, none on its transport class, none on the resource. A
`$TYPE_SHOP` building therefore sells anything sitting in any of its storages,
including a `$STORAGE_SPECIAL` holding one named good — which is what the pub
does with alcohol and what makes a single-purpose shop possible with no patch
at all. See [13-buildings.md](13-buildings.md).

**Nothing in that path knows what food is**, which is what makes a fifth
citizen need reachable without a code patch — see
[11-needs.md](11-needs.md).

**What has not been read** is the other half: which building a citizen walks to
for a given demand. The choice lands in `*(person+0x688) + 0x4F0`, a
`Building*` that `0x830640` then looks up in the walking-connection list at
`+0xCA8` before handing it to `0x82D300`; who *fills* it is unknown. The
demand's two `0x34`-byte targets at `+0x18` and `+0x4C` open with a place-kind —
1 for food and meat, 9 for clothes and electronics, `0xE` for none — and whether
a building answers a place-kind by what it stocks or by what it is decides
whether a shop stocking only a modded good is ever visited.

## Walking and parking connections

A building carries the list of buildings reachable from it on foot, and a second
one for buildings reachable by personal car. The script API names the first:
`nWalkingBuildingNum`, with `Building_WalkingBuilding_GetID`/`GetDistance`.

| Building offset | Contents |
|---|---|
| `+0xCA8`…`+0xCB0` | `std::vector<WalkingConnection>`, stride `0xF0` |
| `+0xCC0` | the same vector for parking / personal cars |

| Connection offset | Contents |
|---|---|
| `+0x08` | the building at the other end |
| `+0x98` | the path length found, world units |

**Nothing downstream re-checks the distance.** The job, shop and service code
only asks whether the pair is in the list, so one number decides the whole of
walking.

| RVA | What |
|---|---|
| `0x12E1D0` | **builds** walking connections for a whole set of buildings: clears `+0xCA8` and fills it in again. Called from the save loader |
| `0x12E6E0` | the same for **one** building. `0x12EC50` drains twenty per call off the queue at `game+0x11F88` — the path a running game takes |
| `0x12F830` | the same for parking, `+0xCC0`. Drained from a queue at `game+0x11FA0` by `0x12FD90` |
| `0x43EF10` / `0x43FEA0` | the **overlay** behind the building window's walking-distance button: runs the search a third time and draws the path polylines and metre labels. Does not read `+0xCA8` at all |
| `0x441890` | the same for one hovered connection, and for a building that is still a blueprint. Uses 125 (`0x90ABE8`) for type `0x69` |
| `0x12DE30` / `0x12F480` | **collect**: after a road changes, gather the buildings that need rebuilding and hand them to the builders |
| `0x12DD00` / `0x12F350` / `0x12D9A0` | wrappers around the collectors, called when something is built |
| `0x430F20` | the save loader — regenerates both when the save predates the last raise |

Builders and collectors both fill in one path-query object and run it.
**`query+0x3C` is the longest path the search will accept**, read at
`0x5799B5`, `0x57A278` and `0x57ABFD`, always as

```c
if (query[0x3C] > 0.0f && node[0x24] > query[0x3C]) return -1.0f;
```

where `node+0x24` is the length accumulated so far — so a non-positive limit
means no limit.

**Which of the four fills it in is the difference between the walking distance
and a rebuild radius**, and it is easy to get wrong:

| Site | Value | Meaning |
|---|---|---|
| `0x12E2DD` | **480**, an immediate | the walking distance, batch builder |
| `0x12E7AF` → `0x90AF38` | **480** | the walking distance, queued builder |
| `0x43F04A`, `0x43F835` → `0x90AF38` | **480** | the walking distance, overlay |
| `0x12F926` → `0x90B11C` | **2500** | the personal-car distance |
| `0x43FFB3` → `0x90B11C` | **2500** | the same, overlay |
| `0x12DEA7` → `0x90AF70` | 530 | how far from a changed road walking connections are rebuilt |
| `0x12F502` → `0x90B120` | 2600 | the same for parking |

The collectors' radii are deliberately a little wider than the limits they
serve, and **the walking distance is written out separately in four functions**
— nothing shares a constant. Patching any strict subset changes either nothing
observable or only half of it: with the builders patched and the overlay not,
citizens walk the new distance and the button still draws the old one. See
[07-pitfalls.md](07-pitfalls.md).

`0x90AF70` has sixty-odd unrelated readers — it is the shared literal pool — so
that constant must be repointed at the instruction, never overwritten.

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

`0x9E9C3C` is the **save format version** of the world just loaded — 124 in
every save this build writes, and the value a dozen migration steps in
`0x430F20` compare against. `0x9E9C50` is a byte set at `0x431070` from the
terrain name: one value for `dlc2/terrains_new/terrain_siberia` and
`terrain_jungle`, the other for every other map.

`0x9E6A18`…`0x9E6A20` and `0x9E69F0`…`0x9E69F8` are two `vector<Building*>` the
loader's migration steps walk.

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
`sovietComments.btf`. Big-endian throughout, **the payload included**:

```
u32 count
u32 file size in bytes
u32 payload length in UTF-16 code units
count * { u32 id, u32 offset, u16 length }        <- 10 bytes on disk
payload: <payload length> UTF-16BE code units
```

`file size == 12 + count * 10 + payload length * 2`, exactly, for all
twenty-one files — there is no padding and no trailing anything.

**Offset and length are in code units, not bytes.** Entry 0 is `{off 0, len 6}`
and entry 1 is `{off 7, len 5}`: both fields have to be doubled to index the
payload, and the one unit between two strings is a NUL that `length` excludes.
Reading them as byte counts gives text that is shifted and truncated, which
looks like a different header layout and is not. Strings sit back to back in
entry order with **no de-duplication** — two entries with the same text each get
their own copy — which is why rebuilding a file from its entries reproduces it
byte for byte.

That is `C3D_LANGUAGE::Initialize(char *file, char *fallback)`, `C3DDLL64.dll`
rva `0x96A50`: three `fread(&field, 4, 1, f)`, then `count` iterations of
`fread(id, 4, 1)` / `fread(off, 4, 1)` / `fread(len, 2, 1)`, then one
`fread(payload, 2, payloadUnits, f)`. Every field is byte-swapped afterwards by
hand, the payload one code unit at a time — an open-coded `bswap` chain of
`sar`/`movzx`/`shl`, which is what makes the file big-endian on a little-endian
machine. The entry array is `malloc(count * 12)` — 12 in memory, 10 on disk —
and the payload `malloc(payloadUnits * 2)`, so **nothing caps the entry count**:
a rebuilt file may carry more entries than the original.

`C3D_LANGUAGE::GetString(int id)` is rva `0x97F10`, and three things in it
matter when writing a file rather than reading one:

- **The lookup is a linear scan** over the entry array, first match wins. Ids
  therefore need not be sorted — and in the shipped files they are not — but a
  duplicate id makes every copy after the first unreachable.
- **`length` is not used to fetch the text.** `GetString` takes the offset and
  scans forward to the NUL; `length` is what `GetStringLength` (`0x98080`)
  returns. A wrong `length` is therefore invisible in most of the UI, which is
  exactly the kind of thing that stays broken for a year.
- **An empty string counts as missing.** After the NUL scan `GetString` rejects
  a zero-length result and falls through to the fallback file — the second
  argument of `Initialize`, `sovietEnglish.btf` for every language, which is
  what the `.rdata` pairs at `0x8885C5` are. Blanking an entry does not blank
  the label, it un-translates it. A missing id ends at `lang+0x266E4`, an empty
  string, and that is the only way to get one.

Two tables are consulted before the file: a 16-byte-stride vector at
`lang+0x26740` that `ChangeStringsWithID` fills, and — for a **negative** id —
`~id` as an index into the runtime custom-string vector at `lang+0x26720`, which
is what `CustomStringAdd` returns a handle into. Negative ids are never in a
file.

`sovietEnglish.btf` has 7906 entries. **Ids run 300 to 580231**, sparsely, and
twenty of the twenty-one full translations end at exactly 580231. Workshop
buildings that use `$NAME <id>` draw from the same space and stay under 8000.

Anything the loader mints for itself has to sit above 580231 — see
[07-pitfalls.md](07-pitfalls.md) for what happened when it did not.

`tools/assets/btf.py` reads, unpacks, rebuilds and patches these files;
`selftest` round-trips all twenty-one byte for byte and is the check to re-run
after a game update. Because the loader patches `fopen` in **C3DDLL64.dll's own
import table** as well as the executable's, and `Initialize` opens the file with
`fopen`, a rebuilt language file ships as
`tesmioloader/vfs/media_soviet/soviet<Language>.btf` and no game file is
touched.

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
| `0x90AF38` | 480 | longest walking path, as the queued builder reads it — **31 unrelated readers** |
| `0x90B11C` | 2500 | longest personal-car path — the real limit |
| `0x90AF70` | 530 | radius walking connections are rebuilt in — **and sixty-odd unrelated readers** |
| `0x90B120` | 2600 | the same for parking; one reader |
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
