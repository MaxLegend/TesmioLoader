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

`renderconfig.ini`:

```ini
$TYPE_WORKSHOP
 MODEL model.nmf
 MATERIAL ../material.mtl
 LIFE 3800.000000
 DERBIS_MESH buildings/buildingwreck1.nmf buildings/buildingwreck.mtl
 END
```

## Naming without language files

`$NAME_STR "Copper Mine"` takes a literal. Stock buildings use `$NAME 6160`, an
id into the `.btf` language files, but a Workshop building does not have to.

## Reusing base-game assets

Base building assets sit loose in `media_soviet/`:

- meshes and materials in `buildings/` — `iron_mine.nmf`,
  `bauxite_processing.nmf`, `eletronic_components_factory.mtl`
- collision and fire data in `buildings_types/` — `iron_mine.bbox`,
  `iron_mine.fire`
- build-menu icons in `editor/` — `tool_iron_mine.png`
- animated parts in `buildings_types/` — `iron_mine_anim.nmf` with its `.naf`

Copy the model, bbox and fire into the mod folder; write your own `.mtl`
pointing at the stock textures, since paths inside a `.mtl` are relative to
`media_soviet/` and need no duplication. Animation meshes can be referenced in
place:

```ini
$ANIMATION_MESH buildings_types/iron_mine_anim.nmf
buildings_types/iron_mine_anim.naf
```

Materials are matched by submaterial name inside the `.nmf`, so a replacement
`.mtl` must declare the same names — `iron_mine.nmf` wants `budovky_small_mat`
and `budovky_big_mat`, `bauxite_processing.nmf` wants `storage_aluminium_mat`,
`alumina_plant.nmf` wants `storage_aluminium_mat` and `voda_mat`, and
`aluminium_plant.nmf` wants five: `Material1`, `big_heating_plan_mat1`,
`warehouse_open_combined:_crane_119`, `temp:____Default1` and `temp:_T_TOWER1`.

**Copy the texture lines as `$TEXTURE`, not `$TEXTURE_MTL`.** Some stock
building materials — `alumina_plant.mtl` among them — use `$TEXTURE_MTL`, whose
paths resolve next to the `.mtl` itself. In a Workshop item that is the mod's
own folder, so the textures would not be found. `$TEXTURE` paths are relative to
`media_soviet/` and work unchanged.

Borrowed geometry is fine locally and must be replaced before anything is
published.

## building.ini essentials

```ini
$NAME_STR "Copper Mine"
$TYPE_MINE_COPPER              ; mine; or $TYPE_FACTORY
$WORKERS_NEEDED 250
$PRODUCTION copper_ore 4
$STORAGE_EXPORT RESOURCE_TRANSPORT_GRAVEL 20
```

Factories declare both sides and may have conveyor connections:

```ini
$TYPE_FACTORY
$CONSUMPTION copper_ore 5.00
$PRODUCTION copper_concentrate 3.00
$STORAGE_IMPORT RESOURCE_TRANSPORT_GRAVEL 50.00
$STORAGE_EXPORT RESOURCE_TRANSPORT_GRAVEL 50.00
$CONNECTION_CONVEYOR_INPUT
-34.5016 6.8000 20.0000
-22.5016 6.8000 20.0000
```

The storage transport class must match the resource's own or capacity is zero.

**`$RESOURCE_VISUALIZATION <n>` takes a storage index**, counting `$STORAGE_*`
lines from zero in the order they are written. That makes the storage order
load-bearing whenever a donor's visualisation blocks are kept: reorder the
storages and the piles move to the wrong yard. `alumina_plant` visualises 0 and
1, its two gravel heaps; `aluminium_plant` visualises 2 twice, the open export
yard, with `numstepx 4.8 10` / `numstept 2.5 8` — a 10×8 grid of whole cargo
units, which is how an open-class resource is displayed. Bulk resources use
`0.0 1` for both and get a single heap mesh instead.

Node names in `$COST_WORK_BUILDING_NODE` and
`$COST_WORK_VEHICLE_STATION_ACCORDING_NODE` refer to meshes inside the `.nmf`.
Copy them from the donor building's `.ini` along with the model, or construction
will not work.

## Picking a donor

Match the shape of what you need, not the looks:

- a mine wants `iron_mine` — animation, conveyor output, the right footprint
- a processing plant wants `bauxite_processing` — conveyor **inputs** as well as
  outputs, gravel-class storages on both sides, and `$RESOURCE_VISUALIZATION`
  piles
- `eletronic_components_factory` has no conveyor input, which made an early
  copper smelter impossible to feed by belt

## Checking it loaded

`tesmioloader.reads.log` with `trace_reads = 2` shows the game opening
`media_soviet/workshop_wip/<id>/<object>/building.ini`. The game's own log,
mirrored into `tesmioloader.log`, reports `Failed to open ...building.ini` when
`$OBJECT_BUILDING` names a folder that is not there.
