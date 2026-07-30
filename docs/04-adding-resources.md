# Adding a resource

> **How many fit: as many as `[list]` declares.** The engine allocates 63
> records and fills 57, so six fit in its own buffer; past that the plugin
> reallocates the vector the way `reserve()` would — a bigger block from the
> process heap, the records copied in, the vector's three pointers repointed,
> the old block deliberately left allocated (`RelocateResourceArray`). **The
> size is worked out from `[list]`**, not configured; `resource_capacity` is
> only a floor, and `-1` turns the growth off. See
> [Growing the array](#growing-the-array).
>
> What that does *not* lift is everything else sized against the base game's
> fixed set of resources. See the purchase bucket in
> [07-pitfalls.md](07-pitfalls.md), which a modded good walked into the first
> time one was ever bought.

A resource that does not exist in the base game, usable in `$PRODUCTION`,
`$CONSUMPTION` and `$STORAGE` lines exactly like a stock one.

**A plugin**: `plugins/resources/resources.cpp` builds to
`build/plugins/resources.dll`, and deleting that DLL removes mod resources
entirely. Everything it reads is in `build/plugins/resources.ini`: `[list]` is
what exists, `[resources]` is the wiring — the hook mode and the three RVAs. See
[09-plugins.md](09-plugins.md).

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

`plugins/resources/resources.ini`, section `[list]`, UTF-8, no BOM:

```ini
[list]
copper_ore         = rawiron, Copper Ore
copper_concentrate = bauxite, Copper Ore Concentrate
```

The plugin's own settings live in `[resources]`, further down the same file.

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

## What it costs

**The game does not store a price per resource. It computes one**, at world init
and again whenever the economy updates, by walking every building type looking
for one whose `$PRODUCTION` names the resource and adding up what its inputs
cost. `0x2A92D0` is that pass and `0x2A9470` is the solver behind it; both are
written up in [02-findings.md](02-findings.md).

Two consequences, and the first is the one that gets reported as a bug:

**A resource nothing produces prices at zero.** The solver's two loops over the
building types fall through and it returns the register it zeroed on the way in.
Nothing reads the base price on that path. So `copper_ore`, `copper_concentrate`,
`raw_copper`, `copper` and `furniture` all price themselves — each has a factory
or a mine — while a name declared in `[list]` and used by no `$PRODUCTION` line
anywhere is `0.00` however it is configured.

**A clone inherits the template's money.** `copper_ore` starts life with
`rawiron`'s base of 4.5 in both currencies because the whole 832-byte record is
copied, which is why the copper chain looked correctly priced without anyone
setting anything.

**Both of those are confirmed on a real save**, not read off a decompiler.
`media_soviet/save/15695 - coppertest2/stats.ini`, written by a game with all
ten mod resources live:

| Resource | Produced by | `$Economy_BaseUSD` | `$Economy_PurchaseCostUSD` |
|---|---|---|---|
| `copper` | electrolysis plant | 0 | 765.40 |
| `furniture` | furniture factory | 0 | 1408.23 |
| `medicine` | pharmacy plant | 0 | 1957.99 |
| `copper_ore` | copper mine | 4.5, from `rawiron` | 7.88 |
| `gas` | **nothing** | **40, from `oil`** | **0.00** |
| `sand`, `clay`, `glass` | nothing | 0 | 0.00 |

`gas` is the line that settles it: it carries `oil`'s base of 40 in both
currencies and still prices at exactly zero, because no building type produces
it. **A base price is not a floor.**

### The two sections

`plugins/resources.ini`, both taking `<resource> = <rubles>[, <dollars>]`, one
number setting both. Names may be mod resources or base-game ones — they are
looked up in the engine's vector by name, so retuning `rawiron` works exactly as
well as pricing `sand`.

```ini
[base_price]
copper_ore = 6.0, 5.0

[price]
sand = 12.0, 10.0
```

**`[base_price]` is `$Economy_BaseRUB` / `$Economy_BaseUSD`**, record `+0x78` and
`+0x7C` — the raw-material value the solver starts from. Fifteen base-game
resources have one and they are the ones dug out of the ground: `rawiron` 4.5,
`rawcoal` 5.3, `oil` 40, `uranium` 5.2/4.2, `explosives` 15/13. Raising it on an
ore makes everything downstream dearer, because the mine's output is priced from
it and the concentrator's from the mine's. It does **not** lift a resource off
zero on its own.

**`[price]` is the finished price**, record `+0x58` and `+0x5C` — what the trade
window quotes, times `1.05` to buy and `0.95` to sell. This is the half that
fixes a zero, and the only thing that gives a value to a resource no building
produces. A resource that *is* produced does not need it.

### Where they are written

One inline hook, on the pass itself, and the two halves are on opposite sides of
it:

- **base before**, because the solver reads `+0x78`/`+0x7C` on its way in;
- **price after**, because the pass overwrites `+0x58`/`+0x5C` on its way out.

Doing it anywhere else means racing whatever wrote last. A save carries
`$Economy_Base*` and puts the game's own numbers back into the record on load,
and a third pass at `0x2FB390` multiplies every non-zero base by a random walk
twice a period — so a value written once at arm time would be gone by the first
recompute. Written here it is re-applied every time and is the last word.

The cost of that is worth stating: **a declared base does not drift and a
declared price does not respond to its chain.** Both are pinned.

### Reading the table

`price_report = 1` in `[resources]` prints every declared resource after each
recompute:

```
price     copper_ore                 7.09 RUB       7.88 USD   base 4.50 / 4.50   kind 0
price     sand                       0.00 RUB       0.00 USD   base 0.00 / 0.00   kind 0
```

`0.00` with any base at all means no building type produces it. `kind` is the
record's `+0x44`: `0` raw, `1` manufactured, `2` consumer good, negative for the
five the pass special-cases.

## Growing the array

Six mod resources fit in the engine's own allocation — slots 57 to 62. **The
seventh and everything after it come from moving the array**, and the plugin
does that by itself: `NeededCapacity` is `57 + <entries in [list]>`, and if the
vector in front of it has less room than that, `RelocateResourceArray` runs.

`resource_capacity` in `plugins/resources.ini` is a **floor**, not the switch it
used to be:

| Value | Meaning |
|---|---|
| `0` | the default — size the array from `[list]` |
| `N` | the same, but never fewer than `N` records |
| `-1` | never move the array; the seventh mod resource is refused |

Three things make the move safe, and all three are worth knowing before touching
this code.

**It happens on the very first lookup of the session.** `EnsureArmed` runs
*before* the original `ResourceGet`, so the first name the engine ever resolves
already comes out of the new buffer. Nothing holds a `Resource*` at that moment:
the resource table at `0x2A1D60` builds each record in a stack buffer and
pushes it, and the building-type parser has not started.

**The engine's own record cache is carried across.** Immediately after building
the array, the engine resolves about forty names by hand and stores what it gets
in `game+0xC2C8`…`+0xC488`, directly behind the vector object — see
[02-findings.md](02-findings.md). The first entry is `workers`, index 0, so it
equals `begin` exactly; that is the "second structure holding the array base"
this document used to warn about. `RebaseResourceCache` walks the block and
moves any pointer that lands on a record boundary inside the old buffer.
Normally it moves nothing, because the cache is still zero at that point, and
the count it logs is a check on the ordering above rather than a repair.

**It happens once per process.** A map load *clears* the vector rather than
destroying it — `end = begin` — so the raised capacity survives every later
world, and the 57 base records are pushed straight back into the enlarged block.
The old buffer is leaked on purpose: tens of kilobytes, once, against any chance
of freeing memory the engine still believes it owns.

The plugin refuses to move an array that already holds one of its own records,
because by then the building-type parser has taken pointers into it. That cannot
happen in the normal order and the guard exists to keep it that way — if the log
ever shows `not moving the array now`, the ordering has changed and the reason
is worth finding before raising anything.

## Other limits

**`[list]` holds 256 names.** `RES_MAX_ENTRIES` in
`plugins/resources/resources.cpp`, and nothing in the engine chooses it — it is
the size of the plugin's own registry, about 220 bytes an entry. Past it the
extra lines are ignored and the log says so. The `.ini` is read whole, however
long it is.

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
resource  array moved 000001C02B72E020 -> 000001C02C0A0040, capacity 66 records (57 live)
resource  "copper_ore" published as index 57 (template 18, caption 1000000), vector now 58
```

The `array moved` line appears once per session and only when `[list]` needs
more than the engine's 63 records.

Symptoms and causes:

| Symptom | Cause |
|---|---|
| no `registry` lines at all | `plugins\resources.ini` not found, its `[list]` section is missing, or `hook` is not 2 — check the .ini has no BOM |
| `slot N is taken (M live)` | wrong slot number in `[list]` |
| `no room at index N` | `resource_capacity` is `-1`, or the reallocation failed — the line says which |
| `cached record pointer(s) rebased` | the array moved later than it should have. Nothing is broken, but the ordering in `EnsureArmed` has changed and is worth checking |
| storage shows `0.00 of 0.00 t` | transport class mismatch between storage and template |
| price is `0.00` in the trade table | nothing produces the resource — the solver never reaches the base price. Force it in `[price]` |
| `[base_price]` changes nothing | the resource is unproduced (see above), or `hook` is not 2, or `price_hook` is 0 |
| caption is the template's | no caption given, or `GetString` hook failed to install |
| icon is a random image | icon file missing, or the VFS did not serve it — check for `vfs fopen` in the log |
| crash on the asset worker thread | cargo models missing |
| cargo is drawn as the template | the mesh slots were not replaced — look for the `cargo meshes: N of …` line, and check the `.nmf`/`.mtl` names against the table above |
| icons vanish and hovering crashes after re-entering a world | the entry stayed latched through a rebuild; the log should show `no longer at index N … re-arming` |
