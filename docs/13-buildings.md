# Buildings — a new building from four lines of config

[06-building-mods.md](06-building-mods.md) is the manual. This is the machine.

A new building has never needed reverse engineering: the game's own Workshop
format describes one completely, and 1594 of the buildings installed on this
machine arrived that way. What it needed was *labour* — seven files per
building, five of them byte copies of a base-game asset, one of them the
donor's own `building.ini` with three lines changed, and one texture gotcha
that silently untextures the mesh.

The `buildings` plugin does that. One section, and the folder is written before
the game reads its first file:

```ini
[pharmacy]
id     = 9100000010
donor  = shop_clothes
object = Pharmacy
name   = Pharmacy
line   = $TYPE_SHOP
line   = $WORKERS_NEEDED 8
line   = $CITIZEN_ABLE_SERVE 6
line   = $STORAGE_SPECIAL RESOURCE_TRANSPORT_COVERED 8 medicine
```

## What it is not

**It patches nothing.** No hook, no import swap, no address, no prologue to
verify. It reads no game structure and it cannot be broken by a game update —
the only thing it can get wrong is a `building.ini`, and the game's own log says
so. It is the only plugin here with nothing in it that a version bump threatens.

**It is not a new engine building type.** `$TYPE_SHOP` and the other seventy-odd
`$TYPE_*` tokens are compiled into the parser; a genuinely new one is a code
patch, and the shape it would take is at the end of this document. Everything
below is about buildings the engine already knows how to be.

## The four phases

### 1. What the donor gives

```
media_soviet\buildings\<donor>.nmf          -> <item>\<object>\model.nmf
media_soviet\buildings_types\<donor>.bbox   -> <item>\<object>\building.bbox
media_soviet\buildings_types\<donor>.fire   -> <item>\<object>\building.fire
media_soviet\editor\tool_<donor>.png        -> <item>\<object>\imagegui.png
                                               <item>\previewimage.png
media_soviet\buildings\<donor>.mtl          -> <item>\material.mtl
media_soviet\buildings\<donor>_e.mtl        -> <item>\material_e.mtl
media_soviet\buildings_types\<donor>.ini    -> <item>\<object>\building.ini
```

The mesh is the only one that is required. A missing `.bbox` or `.fire` is a
line in the log; a missing `.nmf` is a building that is not written at all.

**`<donor>_e.mtl` decides one line of `renderconfig.ini`.** 159 of the 493 base
building materials ship one — the lit-window glow — and a mesh built for it
renders through a null node array and takes the process down in
`C3D_MESH::Render` on the first frame after the world loads. So
`MATERIALEMISSIVE ../material_e.mtl` is emitted when, and only when, the donor
has one. That cost a test cycle when it was done by hand; it is now a
`GetFileAttributes`.

### 2. The material, which is not a byte copy

130 of the 493 base building materials write `$TEXTURE_MTL`, whose paths resolve
**next to the `.mtl` file itself**. In a Workshop item that folder is the mod's
own, so the textures are not found and that part of the mesh renders untextured
— with no error anywhere. `shop_clothes.mtl` is one of the 130:

```ini
$SUBMATERIAL lambert1
$TEXTURE_MTL 0 shop_clothes.dds
```

Every such line is rewritten on the way out:

```ini
$TEXTURE 0 buildings/shop_clothes.dds
```

`$TEXTURE` resolves against `media_soviet\`, and a donor material lives in
`media_soviet\buildings\`, so prefixing `buildings/` is the whole of the
translation. Nothing else in the file is touched — submaterial names, colours
and specular power are the mesh's business.

### 3. `building.ini`, and the rule for it

**Start from the donor's own file and change only the economy.** Everything
geometric — `$CONNECTION_*`, `$COST_WORK*`, `$VEHICLE_STATION`, `$PARTICLE`,
`$TEXT_CAPTION`, `$ANIMATION_MESH` — is measured against the mesh that was just
copied and has to survive verbatim. `$COST_WORK_BUILDING_NODE` even names a mesh
*inside* the `.nmf`.

So the donor's file is copied line for line, the declared block goes in front of
it, and a donor line is dropped only when a declared line replaces it.

**In front, not behind.** A handful of base files end in a bare `end` and
nothing is established about whether the parser stops there. Order does not
otherwise matter to anything this touches — the one token where it would,
`$RESOURCE_VISUALIZATION`, is dropped whenever the storages are re-declared.

#### Which line replaces which

The first `$TOKEN` **anywhere in the line** is what counts, because that is what
the game's own parser matches. A `building.ini` has no comment syntax:
`//$WORKERS_NEEDED 13` in `co_look_out_cafe.ini` is a `$WORKERS_NEEDED`, and
`--$TYPE_PUB` in the same file is a `$TYPE_PUB`. Reading the first word instead
would leave both in.

Most tokens replace only themselves. Four families replace each other:

| Family | Why it is a family |
|---|---|
| `$NAME`, `$NAME_STR` | a name is a name |
| every `$TYPE_*` | exactly one may be in effect |
| every `$STORAGE*`, and `$RESOURCE_VISUALIZATION` | the visualisation takes a storage **index**, counting `$STORAGE_*` lines from zero — re-declaring the storages silently moves every pile |
| `$PRODUCTION`, `$CONSUMPTION`, `$CONSUMPTION_PER_SECOND` | a recipe is replaced whole or not at all |

Exactly those three recipe tokens, by name.
`$PRODUCTION_SEWAGE_POLLUTION` and `$CONSUMPTION_WATER_REQUIRED_QUALITY` are
settings rather than recipe lines, and a plant that keeps the donor's water
quality requirement while changing what it makes is the case that matters — the
pharmaceutical plant below is exactly that. A prefix match would have eaten
both.

`strip = $TOKEN` drops something without putting anything back.

### 4. The stamp

Every generated folder carries `tesmioloader.stamp`, holding an FNV-1a of the
section, the generator's own version and the donor `building.ini`'s size and
timestamp.

It answers two questions:

- **Do we rewrite?** Only when the hash differs, so a 300 KB mesh is not copied
  at every launch. `always = 1` forces it. A change to the donor's own file —
  a game patch — regenerates too.
- **Is this ours?** A folder that exists *without* a stamp is refused with a log
  line and left alone. That is what stops an id that collides with a real
  Workshop subscription from overwriting it.

## Where it goes, and the one thing on disk

`media_soviet\workshop_wip\<id>`, which is the folder the game scans for
unpublished Workshop items.

This is the only thing in the whole project that puts a file in the game folder,
and it is worth being clear about why it is not the VFS. The loader's VFS
redirects **opens**, not **listings**: the game finds Workshop items by walking
that directory with `FindFirstFileW`, and a folder that exists only under
`tesmioloader\vfs\` would never be enumerated. Serving the item's files out of
the VFS while the folder itself was real would need a second mechanism —
appending synthetic entries to `FindFirstFileA/W` and `FindNextFileA/W` through
the executable's import table, both of which it does import. That is a real
option and it is written down here rather than built, because a generated folder
on disk is inspectable, hand-editable and publishable to the Workshop, and those
are worth more than the purity.

Nothing the game shipped is modified. Steam's verification only checks the files
it knows about, so an added folder under `workshop_wip` leaves it happy — that
folder is where a modder's own work-in-progress items are meant to live.

**Timing is not luck.** The launcher creates the process suspended and the
loader is injected before any game code runs, so a plugin's `TsmPluginInit`
happens from `DllMain` — earlier than the game's first `fopen`. A folder written
there is a folder that was always there as far as the game is concerned.

## The pharmacy, end to end

Four files and one 48×48 icon.

**`plugins/resources.ini`** — the resource:

```ini
medicine = eletronics, Medicine
```

Cloned from `eletronics` for its transport class: `RESOURCE_TRANSPORT_COVERED`
is what the shop shelf declares, and a storage whose class does not match the
resource's own reports `0.00 of 0.00 t`. Like `eletronics`, `clothes` and
`food`, it has **no cargo geometry in the base game at all**, so the only asset
it needs is `vfs/media_soviet/resources/medicine.png`.

**`plugins/needs.ini`** — the want:

```ini
medicine = eletronics, 0.5, none, 0.30, 0.008
```

`none` is new, and it is the half of this that the pharmacy needed. The `needs`
plugin's storage rule is *put the goods wherever the donor already is*, which
for medicine is every department store in the republic — exactly wrong for
something meant to be sold in one building of its own. `none` turns that half
off and leaves the other half alone: the citizen still carries the demand, and
the shop tick still sells it. See [11-needs.md](11-needs.md).

The donor stays `eletronics` because the donor is what the **demand** is cloned
from — the routing that sends a citizen shopping — and a chemist's is a
department-store-shaped errand rather than a grocery one.

**`plugins/buildings.ini`** — the shop and the plant, above.

**`vfs/media_soviet/resources/medicine.png`** — 48×48 RGBA.

### Why `$STORAGE_SPECIAL` is enough

The pharmacy declares one storage holding one resource:

```ini
$STORAGE_SPECIAL RESOURCE_TRANSPORT_COVERED 8 medicine
```

and nothing else has to know. The sale at `0x171DA0` was read for this and it
is unconditional over storages:

```c
for (customer in building+0xBD8)
  for (d in customer+0x118, stride 0x80)
    if ((unsigned)(d.kind - 1) < 2)                    // kind 1 or 2
      for (s = 0; s < (building+0x978 - building+0x970) / 0xE0; s++)
        for (slot in storage s, stride 0x10)
          if (slot.res == d.resource && slot.content > 0 && d.total*dt < d.amount)
              move it across
```

There is no test on which `$STORAGE_*` token built the storage, no test on its
transport class, and no test on the resource. **A `$TYPE_SHOP` building sells
anything that is in any of its storages**, which is the same mechanism the pub
uses for its `$STORAGE_SPECIAL ... alcohol`.

Supply is the gas station's problem and has the gas station's answer: a
distribution office told to bring medicine here, or a truck.

### What has not been established

**Whether a citizen will walk to a shop that stocks only a modded good.** The
demand is a byte-for-byte clone of the electronics demand, so its two `Target`
sub-structures at `+0x18` and `+0x4C` carry `9` — the place-kind the base game
uses for a department-store errand. What decides which *building* answers a
place-kind has not been read. Two possibilities, and they differ:

- the search asks "which building near me has this resource in a storage" — the
  pharmacy is found, and everything works;
- the search asks "which building near me is a department store" — the pharmacy
  is not found, because it stocks no electronics.

The furniture case proves nothing either way: furniture goes into shops that
stock electronics as well.

The fallback is two lines and is in `plugins/buildings.ini` already, commented
out — make the pharmacy a department store that also sells medicine
(`$STORAGE_DEMAND_ADVANCED RESOURCE_TRANSPORT_COVERED 12`) and set medicine's
category in `needs.ini` to `advanced`. That is the shape furniture already
works in.

## Testing one

`build\tesmioloader.log`, at startup, before anything else:

```
building generating into A:\...\SovietRepublic\media_soviet\workshop_wip
building "pharmacy" -> 9100000010\Pharmacy  from "shop_clothes": 5 line(s) in,
         4 donor line(s) out, 3 texture path(s) rewritten, emissive material
building "pharmaceutical_plant" -> 9100000011\PharmaceuticalPlant  from
         "fabric_factory": 8 line(s) in, 8 donor line(s) out, ...
building 2 section(s) processed
```

`up to date` on the second launch is the stamp doing its job.

Then read the generated `building.ini` — it is a plain file and the fastest
check there is that the right lines went and the right lines stayed. Then the
game's own log, mirrored into `tesmioloader.log`, for
`Failed to open ...building.ini`, which means `$OBJECT_BUILDING` names a folder
that is not there.

Three failures and what each looks like, from
[06-building-mods.md](06-building-mods.md):

| Symptom | Cause |
|---|---|
| `ResourceGet - not found <an English word>` then a crash at `SOVIET64.exe + 0x117B91` | a `$TOKEN` written inside a comment — including one written into a `line =` here |
| crash on the first frame in `C3DDLL64.dll + 0xAC544` | a mesh with nothing loaded — a missing `MATERIALEMISSIVE`, or a `.nmf` that did not resolve |
| the building loads and its storage reads `0.00 of 0.00 t` | the storage's class does not match the resource's |

## A genuinely new `$TYPE_*`

Not done, and written down because it is the question this plugin does *not*
answer.

`building+0x318` is the building's **type descriptor**, and two ints in it carry
what `$TYPE_*` decided:

| Type offset | Contents |
|---|---|
| `+0x360` | the `BUILDINGTYPE_*` number the engine keys on — 2 living, 3 shop, 6 factory, 7 mine, 17 powerplant… |
| `+0x364` | a second, finer number; living buildings are told apart by `+0x360 == 2 && +0x364 == 1` |

Both are initialised to `0x1E` at `0x10DFF9` and `0x10E003`, at the top of the
`building.ini` parser's own state reset.

The tokens themselves are a `strcmp` chain inside the parser at `0x10E200`,
each link eleven instructions long and all writing one field:

```asm
0010EBBD  lea  rdx,[rip+0x77a954]        ; "$TYPE_SHOP"
0010EBC4  lea  rcx,[rbp+0x49a0]
0010EBCB  call 0x84f340                  ; the compare
0010EBD0  test eax,eax
0010EBD2  jne  0x10ebe4                  ; next link
0010EBD4  mov  eax,3                     ; BUILDINGTYPE_SHOP
0010EBD9  mov  [rbp+0x1e10],eax
0010EBDF  jmp  0x118815                  ; done with this line
```

This is **the same shape as the deposit type**, which is the one thing in this
project that needed spliced code — see [05-deposits.md](05-deposits.md). A new
token would be another link in the chain, emitted from a config table rather
than written out, and it would give a building a type number the engine has
never dispatched. That last part is the real work: `0x139A80`, the building
dispatcher, decides what a building *does* from `+0x360`, and a number it does
not know is a building that ticks not at all. A new type is therefore only worth
it for behaviour that a plugin then supplies itself — which is what
`accumulator` does today without one, by being a type the engine already has.

`media_soviet/scripts/SOVIETInstructions.txt` lists every `BUILDINGTYPE_*` and
its number, and is the authority.

## Settings

`plugins/buildings.ini`, one file for the whole feature.

| Key | Default | What |
|---|---|---|
| `enabled` | 1 | 0 unloads the plugin. Folders already written stay, and the game keeps loading them |
| `out` | `media_soviet\workshop_wip` | relative to the game folder |
| `always` | 0 | 1 rewrites every folder on every launch |
| `verbose` | 0 | a line per file copied and per donor line dropped |

Per section:

| Key | What |
|---|---|
| `id` | the Workshop item id, and the folder name |
| `donor` | a name under `media_soviet\buildings_types\`. **Match the shape, not the looks** — a mine wants a mine, a processing plant wants one with conveyor inputs |
| `object` | the object subfolder and `$OBJECT_BUILDING`. Defaults to the section name |
| `name` | `$NAME_STR` and the build-menu name. A literal, so no language file. ASCII |
| `desc` | the Workshop description. Repeatable, one line each |
| `life` | `renderconfig` `LIFE`. Default 3000 |
| `enabled` | 0 skips the section |
| `line` | **a `building.ini` line, verbatim.** Repeatable, emitted in order |
| `strip` | a `$TOKEN` to take out of the donor with nothing put back. Repeatable |

Sixteen sections, forty-eight `line`s each — arbitrary limits, raised by two
constants at the top of `buildings.cpp`.
