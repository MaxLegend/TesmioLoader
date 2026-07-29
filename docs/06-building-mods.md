# Building mods

New buildings need no reverse engineering at all. The game's own Workshop format
handles them, and 1594 of the buildings installed on this machine arrived that
way. `tesmioloader` is only needed for what those buildings *reference* — a
resource or deposit type that does not exist yet.

## Where they live

`media_soviet/workshop_wip/<id>/` is scanned at startup. Any numeric folder name
works; this project uses 9100000001–9100000004 to stay clear of real Workshop
ids.

| Id | Object | Donor | Chain step |
|---|---|---|---|
| 9100000002 | `CopperMine` | `iron_mine` | deposit → `copper_ore` |
| 9100000001 | `CopperConcentrator` | `bauxite_processing` | `copper_ore` → `copper_concentrate` |
| 9100000003 | `CopperSmelter` | `alumina_plant` | `copper_concentrate` → `raw_copper` |
| 9100000004 | `ElectrolysisPlant` | `aluminium_plant` | `raw_copper` → `copper` |
| 9100000005 | `StationaryAccumulator` | `eletric_substation` | stores electricity |
| 9100000006 | `FurnitureFactory` | `clothing_factory` | `boards` + `fabric` → `furniture` |

Subscribed items live in `A:\SteamLibrary\steamapps\workshop\content\784150\`
and are read from there directly.

## Layout

```
9100000002/
  workshopconfig.ini        item metadata, names the object folder
  previewimage.png
  material.mtl              shared by the object below
  CopperMine/
    building.ini            the type definition
    renderconfig.ini        model and destruction
    model.nmf
    building.bbox
    building.fire
    imagegui.png            icon in the build menu
```

`workshopconfig.ini`:

```ini
$ITEM_ID 9100000002
$OWNER_ID 0
$ITEM_TYPE WORKSHOP_ITEMTYPE_BUILDING
$VISIBILITY 0
$OBJECT_BUILDING CopperMine
$ITEM_NAME "Copper Mine"
$ITEM_DESC "..."
$END
```

`renderconfig.ini` — note that **its keywords carry no `$`** except the opening
`$TYPE_WORKSHOP`, and that the body is indented by one space:

```ini
$TYPE_WORKSHOP
 MODEL model.nmf
 MATERIAL ../material.mtl
 MATERIALEMISSIVE ../material_e.mtl
 LIFE 3000.000000
 EXPLOSION_GROUP 0
 DERBIS_FALLING_FX buildingfall1 1.000000
 DERBIS_FALLED_FX buildingfall2 1.400000
 DERBIS_FALLED_SFX collapse
 DERBIS_NUM 20
 DERBIS_FALLING_FX_MAXTIME 3.000000
 DERBIS_SCALE 1.000000
 DERBIS_MESH buildings/buildingwreck1.nmf buildings/buildingwreck.mtl
 DERBIS_MESH buildings/buildingwreck2.nmf buildings/buildingwreck.mtl
 END
```

Counted over 400 subscribed Workshop buildings, the keywords in use are
`MODEL` (400), `LIFE` (400), `END` (400), `MATERIAL` (429),
**`MATERIALEMISSIVE` (352)**, `EXPLOSION_GROUP`, the six `DERBIS_*`,
`LIGHT` (795), `PLANESHADOW`, `SMOKEPOINTCHANCE`, `MODEL_LOD2`,
`LIGHT_RGB`, `EXACTSPECULAR` and `REFLECTION`.

**`MATERIALEMISSIVE` is not optional when the donor has one.** 159 of the 493
base building materials ship an `<donor>_e.mtl` beside them — the lit-window
glow — and a mesh built for one expects it. Leaving it out of a clone of such a
donor leaves part of the mesh with nothing loaded, and the game dies on the
first frame in `C3D_MESH::Render` reading a null node array. That cost one test
cycle here: the clothing factory has `clothing_factory_e.mtl`, the electric
substation does not, which is why the accumulator had been fine without it.
Copy the `_e.mtl` next to `material.mtl` as `material_e.mtl` and declare it.

## Naming without language files

`$NAME_STR "Copper Mine"` takes a literal. Stock buildings use `$NAME 6160`, an
id into the `.btf` language files, but a Workshop building does not have to.

## Cloning a building, start to finish

Every building in this project was made this way. The worked example below is
the furniture factory, `9100000006`, cloned from the base game's clothing
factory; substitute any donor and the steps do not change.

### 1. Pick the donor and find its five files

Base building assets sit loose in `media_soviet/`, under the donor's own name:

| What | Where | Furniture factory took |
|---|---|---|
| mesh | `buildings/<donor>.nmf` | `clothing_factory.nmf` |
| material | `buildings/<donor>.mtl` | `clothing_factory.mtl` |
| collision box | `buildings_types/<donor>.bbox` | `clothing_factory.bbox` |
| fire points | `buildings_types/<donor>.fire` | `clothing_factory.fire` |
| build-menu icon | `editor/tool_<donor>.png` | `tool_clothing_factory.png` |
| the definition to start from | `buildings_types/<donor>.ini` | `clothing_factory.ini` |
| **emissive material, if any** | `buildings/<donor>_e.mtl` | `clothing_factory_e.mtl` |

**Check for the `_e.mtl` first.** 159 of the 493 base building materials have
one, and a mesh built for it will not render without it — see
`MATERIALEMISSIVE` above. `ls media_soviet/buildings/<donor>_e.mtl` is the
whole check.

Animated parts are the one thing **referenced in place rather than copied**,
because `$ANIMATION_MESH` paths are relative to `media_soviet/`:

```ini
$ANIMATION_MESH buildings_types/iron_mine_anim.nmf
buildings_types/iron_mine_anim.naf
```

**Match the shape of what you need, not the looks.** A mine wants `iron_mine`
(animation, conveyor output, the right footprint). A processing plant wants
`bauxite_processing` (conveyor *inputs* as well as outputs, gravel-class
storages both sides, `$RESOURCE_VISUALIZATION` piles).
`eletronic_components_factory` has no conveyor input, which made an early
copper smelter impossible to feed by belt.

### 2. Lay out the folder

```
media_soviet/workshop_wip/9100000006/
  workshopconfig.ini
  previewimage.png              copy of the donor's tool_*.png is fine
  material.mtl                  copy of buildings/<donor>.mtl
  material_e.mtl                copy of buildings/<donor>_e.mtl, when it exists
  FurnitureFactory/
    building.ini                rewritten from buildings_types/<donor>.ini
    renderconfig.ini
    model.nmf                   copy of buildings/<donor>.nmf
    building.bbox               copy of buildings_types/<donor>.bbox
    building.fire               copy of buildings_types/<donor>.fire
    imagegui.png                copy of editor/tool_<donor>.png
```

The three copies are renamed to fixed names — `model.nmf`, `building.bbox`,
`building.fire` — because `renderconfig.ini` and the loader look for those.
`material.mtl` sits one level up and is shared by every object in the item.

```powershell
$g = "A:\SteamLibrary\steamapps\common\SovietRepublic\media_soviet"
$d = "$g\workshop_wip\9100000006"
New-Item -ItemType Directory -Force "$d\FurnitureFactory" | Out-Null
Copy-Item "$g\buildings\clothing_factory.mtl"        "$d\material.mtl"
Copy-Item "$g\buildings\clothing_factory_e.mtl"      "$d\material_e.mtl"
Copy-Item "$g\buildings\clothing_factory.nmf"        "$d\FurnitureFactory\model.nmf"
Copy-Item "$g\buildings_types\clothing_factory.bbox" "$d\FurnitureFactory\building.bbox"
Copy-Item "$g\buildings_types\clothing_factory.fire" "$d\FurnitureFactory\building.fire"
Copy-Item "$g\editor\tool_clothing_factory.png"      "$d\FurnitureFactory\imagegui.png"
Copy-Item "$g\editor\tool_clothing_factory.png"      "$d\previewimage.png"
```

### 3. Write the three ini files

`workshopconfig.ini` and `renderconfig.ini` are the short ones, above.
`$OBJECT_BUILDING` must name the subfolder exactly.

`building.ini` is the whole of the work, and the rule for it is: **start from
the donor's own file and change only the economy.** Everything geometric —
`$CONNECTION_*`, `$COST_WORK*`, `$VEHICLE_STATION`, `$PARTICLE`,
`$TEXT_CAPTION` — is measured against the mesh you just copied and must be kept
verbatim. Field-by-field reference below.

### 4. Check the material

If the donor's `.mtl` was copied unchanged, verify it declares the submaterial
names the `.nmf` asks for and uses no `$TEXTURE_MTL`. Both are mechanical
checks — see the next section.

### 5. Load it

`tesmioloader.reads.log` with `trace_reads = 2` shows the game opening
`media_soviet/workshop_wip/<id>/<object>/building.ini`. The game's own log,
mirrored into `tesmioloader.log`, reports `Failed to open ...building.ini` when
`$OBJECT_BUILDING` names a folder that is not there.

Borrowed geometry is fine locally and must be replaced before anything is
published.

## Materials: the `.mtl` file

`C3D_MATERIAL::Load` is `C3DDLL64.dll` rva `0x9CE80`. A `.mtl` is a flat list of
submaterial blocks, and the base game ships 493 of them under `buildings/`.

```ini
$SUBMATERIAL _Material__517
$TEXTURE 0 buildings/factorycommon/3mtv5.dds
$TEXTURE 1 buildings/blankspecular.dds
$TEXTURE 2 buildings/blankbump.dds

$DIFFUSECOLOR 0.96 0.96 0.96 1.0
$SPECULARCOLOR 1.000000 1.000000 1.000000 1.000000
$AMBIENTCOLOR 0.96 0.96 0.96 1.000000

$SPECULARPOWER 2.000000

$SUBMATERIAL _Material__518
...

$END
```

Every token the base game's building materials use, with how often:

| Token | Uses | What |
|---|---|---|
| `$SUBMATERIAL <name>` | 1022 | opens a block. **The name must match one in the `.nmf`** |
| `$TEXTURE <slot> <path>` | 2776 | path **relative to `media_soviet/`** |
| `$TEXTURE_MTL <slot> <path>` | 274 | path relative to the `.mtl`'s own folder |
| `$TEXTURE_NOMIP <slot> <path>` | 52 | as `$TEXTURE`, without mipmaps |
| `$DIFFUSECOLOR r g b a` | 1107 | |
| `$SPECULARCOLOR r g b a` | 1034 | |
| `$AMBIENTCOLOR r g b a` | 959 | |
| `$SPECULARPOWER f` | 942 | |
| `$END` | 496 | |

Texture slots are positional: **0 diffuse, 1 specular, 2 bump**. A submaterial
that wants none of the last two still names them, and the base game's
placeholders are `buildings/blankspecular.dds` and `buildings/blankbump.dds`.

Three things to know before writing one:

**Submaterial names come out of the mesh, not out of imagination.** They are in
the `.nmf` header, `0x40` bytes each starting at `+0x14`, and
`tools/assets/nmf.py` prints them:

```
tesmioloader\tools\assets> python nmf.py <game>\media_soviet\buildings\clothing_factory.nmf --mtl
clothing_factory.nmf
  version 10, 3 submaterial(s), 2 object(s)
  header size 422352, file size 422352
  submaterials:
     0  _Material__517
     1  _Material__518
     2  _Material__530
```

`--mtl` prints a skeleton with one block per name, ready to have paths filled
in. A name that does not match is not applied and that part of the mesh renders
untextured — there is no error.

**Copy texture lines as `$TEXTURE`, never `$TEXTURE_MTL`.** 130 of the game's
493 building materials use `$TEXTURE_MTL`, whose paths resolve next to the
`.mtl` itself — `alumina_plant.mtl` among them. In a Workshop item that folder
is the mod's own, so the textures are simply not found. `$TEXTURE` paths are
relative to `media_soviet/` and work unchanged wherever the file sits.

**There is no comment syntax**, and the parser matches its keywords wherever
they occur, so a `$TEXTURE` inside a `;`-prefixed line is a `$TEXTURE`.
A `$TEXTURE` before any `$SUBMATERIAL` writes through a null submaterial array
at `material+0x18` and faults at rva `0x9D0AE`. Unknown tokens, on the other
hand, are ignored silently: the game itself ships nine materials that misspell
`$SPECULARPOWER` as `$SPECLARPOWER` and they load.

## Comments are not comments

`building.ini` has **no comment syntax**, exactly like `.mtl`. The parser
matches its keywords wherever they occur in the file, so a `$TOKEN` written
inside a `//` line is a `$TOKEN` and the words after it are its arguments.

```ini
// $STORAGE_IMPORT_SPECIAL takes the resource as its third argument
```

reads as a storage of class `takes` holding the resource `the`, and the null
that comes back kills the process inside the parser. Write keywords **without
the dollar** when a comment has to name one. `//`, indentation and the base
game's own `-$VEHICLE_STATION` and `-------` are all fine — it is the `$` that
is load-bearing. See [07-pitfalls.md](07-pitfalls.md).

## `building.ini`, field by field

488 of these ship with the game under `buildings_types/`, and they are the only
real specification there is. What follows is what they actually use, grouped by
what it decides. Counts are occurrences across all 488.

### Identity and type — the only mandatory part

```ini
$NAME_STR "Furniture Factory"
$TYPE_FACTORY
```

`$NAME` (488) takes an id into the `.btf` language files; **`$NAME_STR` takes a
literal** and is what a Workshop building should use, since it needs no
language file. Exactly one `$TYPE_*` line declares what the building is:
`$TYPE_FACTORY` (52), `$TYPE_LIVING` (54), `$TYPE_STORAGE` (27),
`$TYPE_CARGO_STATION` (43), `$TYPE_PASSANGER_STATION` (25), `$TYPE_SHOP`,
`$TYPE_ENGINE` (34, power plants), `$TYPE_TRANSFORMATOR`, `$TYPE_MINE_*` and so
on. `media_soviet/scripts/SOVIETInstructions.txt` lists every
`BUILDINGTYPE_*` number the engine keys on.

### Labour and the economy

```ini
$WORKERS_NEEDED 80              ; 156 uses
$PROFESORS_NEEDED 4             ; 34 - educated staff
$CITIZEN_ABLE_SERVE 9           ; 53 - customers a shop can handle at once
$PRODUCTION furniture 0.012     ; 89
$CONSUMPTION boards 0.020       ; 146
$CONSUMPTION fabric 0.008
$CONSUMPTION_PER_SECOND eletric 0.027   ; 38 - continuous draw, not per unit made
```

`$PRODUCTION` and `$CONSUMPTION` are per worker-day and are what the ratio
between inputs and output is expressed in; the clothing factory is
`clothes 0.015` from `fabric 0.03`, and the furniture factory follows the same
shape. **A name here goes straight through `ResourceGet`**, so a resource that
does not exist resolves to null — which is why `plugins/resources.ini` has to
declare `furniture` before this file can name it.

### Storages, and the class that has to match

```ini
$STORAGE RESOURCE_TRANSPORT_ELETRIC 5000                  ; 314
$STORAGE_IMPORT RESOURCE_TRANSPORT_COVERED 30             ; 75
$STORAGE_EXPORT RESOURCE_TRANSPORT_COVERED 20             ; 81
$STORAGE_IMPORT_SPECIAL RESOURCE_TRANSPORT_OPEN 40 boards ; 41 - one resource only
$STORAGE_FUEL RESOURCE_TRANSPORT_OIL 8                    ; 53
```

The transport class is the second argument, the capacity in tonnes the third,
and `_SPECIAL` takes the resource as a fourth. **A storage whose class does not
match the resource's own has zero capacity** — the symptom is a storage line
reading `0.00 of 0.00 t` and never filling. The engine decides that from a
per-class factor at `resourceRecord + 0xCC + class*0x20`; see
[02-findings.md](02-findings.md).

The classes, from `SOVIETInstructions.txt`:

| # | Name | # | Name |
|---|---|---|---|
| 0 | `RESOURCE_TRANSPORT_COVERED` | 9 | `RESOURCE_TRANSPORT_ELETRIC` |
| 1 | `RESOURCE_TRANSPORT_OPEN` | 10 | `RESOURCE_TRANSPORT_VEHICLES` |
| 2 | `RESOURCE_TRANSPORT_GRAVEL` | 11 | `RESOURCE_TRANSPORT_GENERAL` |
| 3 | `RESOURCE_TRANSPORT_OIL` | 12 | `RESOURCE_TRANSPORT_NUCLEAR1` |
| 4 | `RESOURCE_TRANSPORT_CEMENT` | 13 | `RESOURCE_TRANSPORT_NUCLEAR2` |
| 5 | `RESOURCE_TRANSPORT_COOLER` | 14 | `RESOURCE_TRANSPORT_HEATING` |
| 6 | `RESOURCE_TRANSPORT_LIVESTOCK` | 15 | `RESOURCE_TRANSPORT_WATER` |
| 7 | `RESOURCE_TRANSPORT_PASSANGER` | 16 | `RESOURCE_TRANSPORT_SEWAGE` |
| 8 | `RESOURCE_TRANSPORT_CONCRETE` | 17 | `RESOURCE_TRANSPORT_WASTE` |

A mod resource inherits its class from the template it was cloned from in
`plugins/resources.ini` — that is the easiest way to get it right: clone
`furniture` from `eletronics` and it is `COVERED`, like the department store
shelf it has to sit on.

**Two inputs of different classes need two storages.** `boards` is `OPEN` like
the sawmill that makes them, `fabric` is `COVERED`, and a shared import storage
could only have held whichever class it declared:

```ini
$STORAGE_IMPORT_SPECIAL RESOURCE_TRANSPORT_OPEN 40 boards
$STORAGE_IMPORT_SPECIAL RESOURCE_TRANSPORT_COVERED 20 fabric
$STORAGE_EXPORT RESOURCE_TRANSPORT_COVERED 20
```

The base game does the same in `eletronic_components_factory`: plastics, steel
and chemicals across three storages of two classes.

The `$STORAGE_DEMAND_*` family is the shops', and the resources in each are
named in the engine's code rather than in the file — see
[11-needs.md](11-needs.md).

### Electricity is not a consumption line

Power never rides a truck — `eletric` is transport class 9 and moves over
wires — so it does not belong in the `$CONSUMPTION` block beside the materials.
There are two ways to ask for it and they are alternatives, not a pair:

```ini
$ELETRIC_CONSUMPTION_LIVING_WORKER_FACTOR 1.5      ; draw computed from staffing
$ELETRIC_CONSUMPTION_LIGHTING_WORKER_FACTOR 1.25
```

```ini
$CONSUMPTION_PER_SECOND eletric 0.027              ; flat draw, whoever is in
```

The worker factors are what a staffed building uses — the clothing factory and
the electronics components factory both do. The flat line is for machinery that
runs regardless of staffing; the sawmill's `0.027` is one. Declaring both asks
twice.

**And a building runs without power unless you say otherwise.**

```ini
$ELETRIC_WITHOUT_WORKING_FACTOR 0.4      ; 40% output with no power at all
$ELETRIC_WITHOUT_LIGHTING_FACTOR 0.3
```

This catches everyone once: a cloned factory keeps producing off the grid and
it looks like the electricity declaration is broken, when in fact it is the
donor's `0.4` doing exactly what it says. Of the 66 base-game buildings that
declare it, 39 say `0.4`, 15 say `0.7`, and exactly one says `0.0` — the
fairground swing. So the base game's own factories all keep running at reduced
output when the lights go out, and `0.0` is what stops production dead.

### Where things are — copy these verbatim

Everything here is measured against the mesh, so a clone keeps the donor's
lines unchanged. This is most of the file by volume:

| Token | Uses | What |
|---|---|---|
| `$CONNECTION_ROAD_DEAD`, `_ROAD`, `_CONNECTION` | 1451 / 401 / 455 | road stubs and links, two points each |
| `$CONNECTION_ADVANCED_POINT` | 2171 | pathing hints |
| `$CONNECTION_PEDESTRIAN`, `_PEDESTRIAN_DEAD` | 895 / 134 | footpaths |
| `$CONNECTIONS_ROAD_DEAD_SQUARE` | 475 | a rectangle where roads may not be built |
| `$CONNECTION_CONVEYOR_INPUT` / `_OUTPUT` | 116 / 91 | belts |
| `$CONNECTION_BULK_INPUT` / `_OUTPUT` | 38 / 43 | bulk terminals |
| `$CONNECTION_PIPE_*`, `_WATERPIPE_*`, `_SEWAGE_OUTPUT` | | liquids |
| `$CONNECTION_ELETRIC_LOW_INPUT` / `_HIGH_OUTPUT` | 38 / 30 | wires |
| `$CONNECTION_RAIL`, `_TRAMROAD_DEAD`, `_AIRPORT_DEAD` | | rail, tram, air |
| `$VEHICLE_STATION` | 557 | loading bays, two points |
| `$VEHICLE_PARKING`, `_PERSONAL` | 359 / 175 | |
| `$TEXT_CAPTION` | 245 | where the building's name floats |
| `$PARTICLE <effect> x y z sx sy` | 342 | smoke and steam |
| `$WORKER_RENDERING_AREA` | 118 | where staff are drawn |

### Construction phases

```ini
-------
$COST_WORK SOVIET_CONSTRUCTION_GROUNDWORKS 0.0
$COST_WORK_BUILDING_NODE brickShape1
$COST_WORK_VEHICLE_STATION_ACCORDING_NODE brickShape1
$COST_RESOURCE_AUTO ground_asphalt 1.0

------------------
$COST_WORK SOVIET_CONSTRUCTION_SKELETON_CASTING 1.0
$COST_WORK_BUILDING_KEYWORD $brick
$COST_RESOURCE_AUTO wall_concrete 0.7
```

The `-------` lines are separators between phases and carry no meaning of their
own. `$COST_WORK` (1132) opens a phase, `$COST_RESOURCE_AUTO` (1386) says what
it consumes, and `$COST_WORK_BUILDING_NODE` (1751) names **a mesh inside the
`.nmf`** that appears during that phase. Those names are the reason a clone
must keep the donor's construction block: `tools/assets/nmf.py --nodes` lists
the candidates in the model, and the donor's own `.ini` is the authority on
which are used.

### Cosmetic and behavioural odds and ends

`$STYLE_FLAG` (144, the era filter in the build menu), `$AMBIENT_SFX` (171),
`$WORKING_SFX` (76), `$MENU_SFX` (147), `$POLLUTION_SMALL`/`_MEDIUM`,
`$HEATING_DISABLE` (101), `$ATTRACTIVE_SCORE` (23), `$QUALITY_OF_LIVING` (33),
`$ELETRIC_WITHOUT_WORKING_FACTOR` / `_LIGHTING_FACTOR` (66 / 70),
`$ELETRIC_CONSUMPTION_LIVING_WORKER_FACTOR`, `$CIVIL_BUILDING` (36),
`$MOVEABLE_DOOR` (36), `$UNDERGROUND_MESH` (44).

### `$RESOURCE_VISUALIZATION` — the one that is easy to break

**It takes a storage index**, counting `$STORAGE_*` lines from zero in the
order they are written. That makes storage order load-bearing whenever a
donor's visualisation blocks are kept: reorder the storages and the piles move
to the wrong yard. `alumina_plant` visualises 0 and 1, its two gravel heaps;
`aluminium_plant` visualises 2 twice, the open export yard, with
`numstepx 4.8 10` / `numstept 2.5 8` — a 10×8 grid of whole cargo units, which
is how an open-class resource is displayed. Bulk resources use `0.0 1` for both
and get a single heap mesh instead.

## Checking it loaded

`tesmioloader.reads.log` with `trace_reads = 2` shows the game opening
`media_soviet/workshop_wip/<id>/<object>/building.ini`. The game's own log,
mirrored into `tesmioloader.log`, reports `Failed to open ...building.ini` when
`$OBJECT_BUILDING` names a folder that is not there.

Three failures and what each looks like:

| Symptom | Cause |
|---|---|
| `ResourceGet - not found <an English word>` then a crash at `SOVIET64.exe + 0x117B91` | a `$TOKEN` inside a comment |
| crash on the first frame in `C3DDLL64.dll + 0xAC544` | a mesh with nothing loaded — a missing `MATERIALEMISSIVE`, or a `.nmf` path that does not resolve |
| the building loads and its storage reads `0.00 of 0.00 t` | storage class does not match the resource's |
