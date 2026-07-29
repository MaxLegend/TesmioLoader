# Walking distance — how far a citizen goes on foot

**A plugin**, not part of the loader: `plugins/walking/walking.cpp` builds to
`build/plugins/walking.dll`, and deleting that DLL restores the base game
exactly. See [09-plugins.md](09-plugins.md) for the mechanism.

It is the smallest feature in this project — one displacement and four bytes of
immediate — and it is worth writing down mostly for *why* it is that small.

## What walking is, in the engine

A building carries a list of the buildings reachable from it on foot. The script
API names it: `nWalkingBuildingNum`, with
`Building_WalkingBuilding_GetID`/`GetDistance` reading it.

| Building offset | Contents |
|---|---|
| `+0xCA8`…`+0xCB0` | `std::vector<WalkingConnection>`, stride `0xF0` |
| `+0xCC0` | the same for parking / personal cars |
| `+0xBC0`…`+0xBC8` | the building's citizens, unrelated but adjacent in every dump |

| Connection offset | Contents |
|---|---|
| `+0x08` | the building at the other end |
| `+0x98` | the path length that was found, world units |

**Everything the simulation does with walking reads that list.** There is no
"is this close enough" test in the job, shop or service code — if the pair is in
the list, the walk happens. So the whole feature is the one number that decides
what goes in it.

## Where the limit lives

The list is built by a path search, and the search carries the ceiling in the
query object it is handed. `query+0x3C` is the longest path it will accept.
Three places in the path expander read it, all with the same shape —
`0x5799B5`, `0x57A278`, `0x57ABFD`:

```c
limit = query[0x3C];
if (limit > 0.0f && node[0x24] > limit)   // node+0x24 = length accumulated so far
    return -1.0f;                          // this branch is too long, drop it
```

so **a non-positive limit means no limit at all**, and anything positive is a
distance in world units, which for this game are metres.

**Eight sites write that field, in three roles, and the walking distance is
spelled out separately in four different functions** — there is no shared
constant anywhere.

| Role | Function | Site | Value |
|---|---|---|---|
| builder, walking, batch | `0x12E1D0` | `0x12E2DD`, an immediate | **480** |
| builder, walking, queued | `0x12E6E0` | `0x12E7AF` → `0x90AF38` | **480** |
| builder, parking | `0x12F830` | `0x12F926` → `0x90B11C` | **2500** |
| overlay, walking | `0x43EF10` | `0x43F04A`, `0x43F835` → `0x90AF38` | **480** |
| overlay, parking | `0x43FEA0` | `0x43FFB3` → `0x90B11C` | **2500** |
| collector, walking | `0x12DE30` | `0x12DEA7` → `0x90AF70` | 530 |
| collector, parking | `0x12F480` | `0x12F502` → `0x90B120` | 2600 |

A **builder** clears a building's connection vector and fills it in again; its
limit *is* the walking distance. Walking has two of them, the same logic written
out twice: `0x12E1D0` takes a whole set of buildings and is called straight from
the save loader, while `0x12E6E0` takes one building and is drained twenty at a
time off the queue at `game+0x11F88` by `0x12EC50` — the path everything in a
running game takes. Parking has only the queued form, off `game+0x11FA0`.

A **collector** runs after a road changes, to work out which buildings need
rebuilding, and hands that set to a builder; its limit is a search radius,
deliberately a little wider.

The **overlay** is what the building window's walking-distance button
highlights. It is a third implementation: **it does not read the connection
vector at all**, it runs the search again and draws the path polylines and the
metre labels off the result. `0x441890` next to it does the same for a single
hovered connection, and for a building that is only a blueprint.

The plugin patches all eight: five limits from the ini, and the two radii held
at `max(limit, vanilla radius)` — a building 900 m from a road that changed has
to be in the rebuild set, or it never notices the road at all.

The in-game hint says "approx. 300m", which was true two constants ago.

**It took four versions to get this right**, and every wrong one logged a
perfectly clean patch: 1.0 patched only the collectors, 1.1 added the batch
builder but not the queued one, and 1.2 moved citizens 1000 m while the button
still drew the 480 m highlight. See [07-pitfalls.md](07-pitfalls.md).

## Why the instruction is repointed and not the constant

Seven of the eight sites read `.rdata`, and both `480.0f` at `0x90AF38` and
`530.0f` at `0x90AF70` are in the shared literal pool: dozens of instructions
across the executable read those same four bytes, panel widths and GUI
coordinates among them, so writing `1000.0` over either would move far more than
walking. The displacement of each `movss` is rewritten instead, to point at a
float the plugin owns in an `AllocNear` block — four floats, one per distinct
value, with several sites sharing each. The eighth site is an `imm32` inside
`mov dword ptr [rsp+0x7C],480.0`, with nothing to share, and is simply replaced.

This is the same technique as the loader's main-menu version line: a data
pointer swap, third on the preference list in
[01-architecture.md](01-architecture.md), and the cheapest thing that survives a
game update — a mismatch at the site makes the patch refuse.

Every site is verified before writing: the four opcode bytes, the address the
displacement resolves to, **and the value in that address**. All four checks
have to pass.

## Making an existing city notice

Connections are stored in the save, not derived at load, so a raised limit only
affects what is built afterwards — the town you already have keeps the
connections it was built with.

The base game has the same problem every time the developers raise the figure,
and it already ships the fix. The save loader at `0x430F20`:

```asm
0x438366   cmp  dword [0x9E9C3C],110    ; save format version - 124 in every save today
0x43836D   jge  <skip>
0x438373   test al,al                   ; al = byte [0x9E9C50]
0x438375   jnz  <skip>
0x43837B   lea  rdx,["Import - Regenerating walking and parking connections "]
```

and the block behind it walks every building, collects the road nodes off each
one's connection vector at `+0xA10`, and calls `0x12DE30` and `0x12F480` over
the lot.

`regen_on_load = 1` makes both conditions true:

- the compare's `imm8` becomes **127**, the largest one can hold, which is above
  today's format version of 124;
- `test al,al` becomes `xor al,al` — the same two bytes, and it always sets ZF.

Nothing is reimplemented. The game does its own full regeneration on every load,
with whatever limit the plugin installed. It costs a few seconds of loading
screen on a large city.

`0x9E9C50` is set at `0x431070` from the terrain name — it is one value for
`dlc2/terrains_new/terrain_siberia` and `terrain_jungle` and the other for
everything else. Rather than work out which way round that is, the guard is
neutralised.

## The settings

`plugins/walking.ini`, one section:

| Key | Default | What |
|---|---|---|
| `enabled` | 1 | 0 unloads the plugin without patching anything |
| `distance` | 1000 | metres of path a citizen will walk. Base game 480 |
| `car_distance` | 2500 | the same for a citizen with a car. Base game 2500 |
| `regen_on_load` | 1 | rebuild every connection in the world at load |
| `probe` | 0 | log all four sites and the values in them before writing |

The two rebuild radii have no setting. They follow the limits, because a
rebuild radius narrower than the distance it serves is never what anyone wants.

`distance` is a **path length along roads and footpaths**, not a straight line:
a building 400 m away across a river may be 1200 m of walking.

Zero is passed through — the engine reads it as no ceiling — and the search then
runs until it has exhausted the road graph, which on a large city makes every
placement take seconds. The plugin clamps anything above 20000 for the same
reason: the search is breadth-first over the road graph, so the work grows with
the square of the limit.

## What a save keeps

The connections are saved with the world, so removing the plugin or lowering
`distance` again restores the limit for everything built from then on, while a
save made while it was raised still holds the long connections until something
regenerates them — the next load if `regen_on_load` is still set, otherwise
whenever a road near the pair changes.

**The save format is unchanged**, so a save stays loadable with or without this
plugin. That is the difference from adding a resource, which shifts the format
and makes the save require the mod.

## What the log says

```
walking  walk build batch 480 -> 1000  (rva 0x12E2DD, immediate)
walking  walk build queued 480 -> 1000  (rva 0x12E7AF now reads 00007FF7CA920000)
walking  walk overlay     480 -> 1000  (rva 0x43F04A now reads 00007FF7CA920000)
walking  walk overlay 2   480 -> 1000  (rva 0x43F835 now reads 00007FF7CA920000)
walking  car build        2500 -> 2500  (rva 0x12F926 now reads 00007FF7CA920004)
walking  car overlay      2500 -> 2500  (rva 0x43FFB3 now reads 00007FF7CA920004)
walking  walk rebuild     530 -> 1000  (rva 0x12DEA7 now reads 00007FF7CA920008)
walking  car rebuild      2600 -> 2600  (rva 0x12F502 now reads 00007FF7CA92000C)
walking  regen on: every load rebuilds all walking and parking connections
```

Three sites reading the same address is the point, not a bug: `SLOT_WALK` is one
float and every walking site is pointed at it.

and the game's own log, when a save is loaded with `regen_on_load = 1`:

```
game.info  Loading string --- Import - Regenerating walking and parking connections
```

A refusal names the site and what was found there instead:

```
walking  walking: rva 0x12E2DD holds 300.0, expected 480.0 - refusing
```

If every *limit* refuses, the plugin returns non-zero from `Start` and is
inactive. A rebuild radius that refuses only costs the radius. Nothing is
hooked, so a refusing plugin stays loaded and does nothing.

## State

**Confirmed working in game as of 1.2**: citizens walk the configured distance,
shops and workplaces past the vanilla 480 m are used, and the save loader's
regeneration pass runs on every load.

**1.3 adds the overlay**, so the walking-distance button highlights the same
distance the simulation uses. That half is patched and logged but has not been
looked at in game yet.

Still worth watching: whether a longer limit makes placement noticeably slower
in a built-up city, which is the only real cost here.

## Where to go next

- **A separate limit per purpose.** The one number covers work, shops and
  services alike, because they all read the same list. Splitting them would mean
  a second list, not a second constant.
- **The 125 m sibling.** `0x441890` — the hovered-connection and blueprint
  version of the overlay — uses `0x90ABE8`, 125, for buildings of type `0x69`
  and for the not-yet-built case. It has not been worked out what that distance
  means; it is left alone.
- **Vehicle path limits.** `query+0x3C` is generic; the two values here are the
  only two that come from a literal, but the same field is filled in by every
  other search the game runs. A `probe` that logged the value at each caller
  would map them.
- **The in-game hint.** `sovietEnglish.btf` still says 300 m. Language files are
  editable through the VFS, and the id space above 580231 is free — see
  [07-pitfalls.md](07-pitfalls.md).
