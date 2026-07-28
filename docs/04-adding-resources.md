# Adding a resource

A resource that does not exist in the base game, usable in `$PRODUCTION`,
`$CONSUMPTION` and `$STORAGE` lines exactly like a stock one.

**A plugin**: `plugins/resources/resources.cpp` builds to
`build/plugins/resources.dll`, and deleting that DLL removes mod resources
entirely. What exists is still declared in `resources.ini` in the loader's own
folder — that is content; `plugins/resources.ini` holds the wiring (the hook
mode and the three RVAs). See [09-plugins.md](09-plugins.md).

## How it works

The engine keeps its resources in one `std::vector` of 832-byte records, 57 of
them, with room for 63. the plugin claims the slot after the last live
record: it copies an existing record over the empty one, overwrites the name and
the caption id, and moves the vector's `end` pointer forward by one. From that
moment the game's own resolver finds the new resource by itself — nothing else
has to be intercepted.

Three things make it work in practice:

**Cloning, not zeroing.** A freshly claimed record is all zeroes and the
resource is inert. Copying a template gives it a transport class, densities and
display data that the engine already knows how to handle.

**Cloning, then correcting.** What a clone must *not* keep is the template's
assets. The five mesh pointers in the record's tail are replaced with meshes
loaded from this resource's own files, and the icon at `+0x48` is refilled by
the engine's own by-name pass on the next world load. The template still decides
the *shape* — whether there are four pile stages or one open-cargo mesh — which
is read straight off the clone rather than declared anywhere.

**Arming is retried, not done once.** The engine is still filling the vector
while building types are being parsed. The loader watches for the slot to become
the next free one and claims it then.

**Re-arming on map load.** The array is rebuilt every time a map loads —
sometimes at a new address, and sometimes **in the same block**, which is why
`begin` changing is not the test. Before skipping an armed entry the loader
checks that its name is still at its index inside the current `end`; anything
else and it re-arms. See [07-pitfalls.md](07-pitfalls.md) for what the latched
version looked like from the outside.

## Steps

### 1. Register it

`tesmioloader/resources.ini`, UTF-8, no BOM:

```ini
[resources]
copper_ore         = rawiron, Copper Ore
copper_concentrate = bauxite, Copper Ore Concentrate
```

`<name> = [<slot>,] <template>[, <caption>]`

- **slot** — optional, and better left out. Omitted, the loader waits until
  every base-game record has landed and then claims the slots after them, in
  the order the entries appear. Written as a number it is pinned, and refused
  unless it is exactly the next free one — which is what makes a hard-coded 57
  break the moment anything else claims it first. A leading field that is not a
  number is read as the template, so both forms parse unambiguously.
- **template** — an existing resource to copy. **Match the transport class to
  what you intend.** A storage whose class differs from the resource's reports
  zero capacity, which shows up as `0.00 of 0.00 t` in the building window. Ores
  want `rawiron`, `rawcoal`, `rawbauxite`; processed ores want `bauxite`,
  `alumina`; liquids want `oil`, `alcohol`. Open cargo — anything that travels
  the way steel and boards do — wants `steel` or `aluminium`.
- **caption** — the display name. The loader mints a private localisation id
  from **1 000 000** up, writes it into the record, and answers
  `C3D_LANGUAGE::GetString` for that id itself. No language file is touched.
  Omit it and the template's caption is inherited — which is why an early
  attempt showed "iron ore" everywhere.

  That base has to stay clear of every id the game uses, because the hook
  answers **everything** at or above it and never falls through. The game's
  highest is 580231; see [07-pitfalls.md](07-pitfalls.md) for how that was
  measured and what it looked like when the base was too low.

### 2. Provide the assets

All named after the resource, all served through the VFS from
`tesmioloader/vfs/media_soviet/resources/`:

**The engine only looks the icon up by name.** Every mesh path in its own
resource table is a literal in `.rdata`, so a cloned record would otherwise be
drawn with the template's geometry however the files are named. `tesmioloader`
closes that gap: after cloning, it makes the same three calls the table makes —
`CreateManagedMesh`, `LoadFromFile`, `LoadMaterial` — against this resource's own
files and writes the results into the record's five mesh slots. The files below
are therefore genuinely used, and a missing one leaves that slot showing the
template's mesh.

**Which files depends on the transport class**, and the split is visible in the
stock folder. Bulk resources — `rawiron`, `coal`, `gravel`, `bauxite`,
`uranium`, `asphalt` — ship four pile stages plus a vehicle load:

| File | Notes |
|---|---|
| `<name>.png` | 48×48 RGBA, matching the stock icons |
| `<name>1.nmf` … `<name>4.nmf` | cargo pile, four fill stages |
| `<name>_vehicle.nmf` | load carried on a vehicle |
| `<name>.mtl` | material; its `$TEXTURE 0` may point at a stock `.dds` |

**Open-transport resources ship one mesh instead.** `steel`, `aluminium`,
`boards`, `bricks`, `prefabpanels` and `wood` have exactly `<name>.nmf`,
`<name>.mtl`, `<name>.dds` and `<name>.png` — no numbered stages, no `_vehicle`.
The single mesh is one unit of cargo and the game lays it out on a grid, which
is what `numstepx` / `numstept` in a `$RESOURCE_VISUALIZATION` block control. A
record cloned from `steel` or `aluminium` takes that path, so mirror the donor:
give it the four files and nothing else.

Copy a donor set and rename. The `.mtl` may reference the donor's texture
directly — paths inside `.mtl` are relative to `media_soviet/`, so
`resources/rawiron.dds` works without duplicating a 350 KB file.

**Check the submaterial name in the `.nmf` you copied.** It is the first
identifier-looking string in the file. `aluminium.nmf` calls its material
`lambert1`, which is what `aluminium.mtl` declares — but `steel.nmf` calls its
`____Default1` while `steel.mtl` still says `lambert1`, so the engine clearly
falls back when the name does not match. Declaring both names in the `.mtl` is
free and removes the question.

### Recolouring a donor texture

A copper version of `steel.dds` needs no image editor. DXT1 stores each 4×4
block as two RGB565 endpoints and sixteen 2-bit indices, so **recolouring is
rewriting the endpoints and leaving every index alone** — about thirty lines of
Python over the whole payload, mip chain included, since every block has the
same shape wherever it sits.

The one invariant to preserve is `color0 > color1`, which is what selects
4-colour opaque mode over 3-colour-plus-transparent. A tint that collapses two
near-identical endpoints past each other turns opaque texels transparent; nudge
one endpoint instead of swapping them, because swapping would invert all
sixteen indices.

Mapping each endpoint through its own luminance onto a target hue keeps the
brushed-metal detail intact — `raw_copper.dds` and `copper.dds` are `steel.dds`
and `aluminium.dds` put through exactly that.

**Do not skip the cargo models.** A resource whose icon loads but whose meshes
are missing crashed the asset worker thread with a null dereference. While the
icon was also missing the resource was never processed that far, which made the
crash look unrelated to the icon.

### 3. Use it

In any `building.ini`:

```ini
$CONSUMPTION copper_ore 5.00
$PRODUCTION copper_concentrate 3.00
$STORAGE_IMPORT RESOURCE_TRANSPORT_GRAVEL 50.00
$STORAGE_EXPORT RESOURCE_TRANSPORT_GRAVEL 50.00
```

The storage class must match the resource's own.

## Limits

**Six mod resources** fit in the engine's allocation — slots 57 to 62. Beyond
that the vector has to be relocated: set `resource_capacity` in
`tesmioloader.ini` to the number of records you want and the loader will move
the array, copying the existing records and repointing the vector.

Relocation works but is **incomplete and off by default**. At least two
structures hold the array base — one at rva `0x9E11C0`, another sixteen bytes
later at `0x9E11D8` — and only the first is updated. The second keeps pointing
at the old buffer, index lookups start returning −1, and the game dereferences
that without checking. Before raising the capacity, enumerate every reference to
the base and update all of them.

**Saves are not interchangeable.** The resource count is part of the save
format. A save written with two mod resources will not load without them.

**The vector is not the only table indexed by resource number.** The icon set is
another. Growing capacity lets more resources exist; it does not make those
other tables any larger, and each one is its own ceiling.

## Diagnosing

Everything lands in `tesmioloader.log`:

```
registry  "copper_ore" -> next free slot, template 18, text id 1000000
resource  array now at 000001C02B72E020
resource  name field at +0x0, 57 live records, room for 63
resource  "copper_ore" published as index 57 (template 18, caption 1000000), vector now 58
```

Symptoms and causes:

| Symptom | Cause |
|---|---|
| no `registry` lines at all | `resources.ini` not found, or `resourcehook` is not 2 — check the .ini has no BOM |
| `slot N unusable (M live…)` | wrong slot number in `resources.ini` |
| storage shows `0.00 of 0.00 t` | transport class mismatch between storage and template |
| caption is the template's | no caption given, or `GetString` hook failed to install |
| icon is a random image | icon file missing, or the VFS did not serve it — check for `vfs fopen` in the log |
| crash on the asset worker thread | cargo models missing |
| cargo is drawn as the template | the mesh slots were not replaced — look for the `cargo meshes: N of …` line, and check the `.nmf`/`.mtl` names against the table above |
| icons vanish and hovering crashes after re-entering a world | the entry stayed latched through a rebuild; the log should show `no longer at index N … re-arming` |
