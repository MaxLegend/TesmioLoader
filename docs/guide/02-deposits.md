[English](02-deposits.md) | [Русский](02-deposits_RU.md)

# Plugin: deposits — new kinds of mineable deposits

New to this? Read [00-getting-started](00-getting-started.md) first.

## What this plugin does

It teaches the game a new kind of ore/mineral deposit — something a mine can
search for, find, and produce from — with its own icon on the minimap and
its own paintbrush in the terrain editor. Copper ore, sand, clay and gas all
work this way in this project.

A deposit type needs three things to be usable in-game, and this plugin
gives you all three from one `.ini` section:

1. A `$TYPE_MINE_...` token you can put in a mine's `building.ini` (only
   relevant if you're also building a new mine via
   [07-buildings](07-buildings.md) — the base game's own mines already use
   the base game's own tokens).
2. A place on the map where the deposit's richness is actually stored — a
   hidden "resource map" image under the terrain.
3. A minimap button/overlay and an editor brush, so you (or anyone playing
   the map) can see and paint where the deposit is.

## How to add a deposit, step by step

1. Close the game.
2. Open `tesmioloader\build\plugins\deposits.ini`.
3. Add a new section, one per deposit:

   ```ini
   [my_ore]
   token         = $TYPE_MINE_MYORE
   type          = 14
   map           = auto
   radius        = ore
   icon          = my_ore
   minimap       = 1
   editor        = myore
   ```

   - `token` — pick a name, always starting `$TYPE_MINE_`. This is what a
     mine's `building.ini` will say to search for this deposit.
   - `type` — any unused number from 10 to 127. Look at the other sections
     in the file and pick one nobody else is using.
   - `map` — leave this as `auto` unless you have a specific reason not to.
     It picks an unused spot for you automatically, in a fresh map the
     plugin creates just for your new deposits — you don't have to think
     about channels or textures at all. (`terrain` is a special option for a
     deposit that should visibly scar the ground when mined, like a gravel
     or sand pit — see the note in the `.ini` file's comments if you want
     that look.)
   - `radius` — how far a mine searches to find this deposit. Use one of
     the words `ore`, `oil`, `bauxite`, `gravel`, `wood`, `water`,
     `watersurface` to copy a vanilla mine's search distance (`ore` is a
     sensible default, shared by iron/coal/uranium). **Don't skip this** —
     without it, mines report a garbage quality number.
   - `icon` — the name of a resource (from `resources.ini`, see
     [01-resources](01-resources.md)) whose icon to show on the minimap
     button. Leave it out for no icon.
   - `minimap` — `1` to get a minimap button, `0` for none.
   - `editor` — a short name (7 characters max) for the paint/erase brush in
     the map editor's Resources tab. Leave it out if you don't want to be
     able to paint this deposit by hand.

4. If you want a mine that actually uses this deposit, either edit an
   existing mine's `building.ini` to add your `$TYPE_MINE_...` token, or
   declare a whole new mine building — see
   [07-buildings](07-buildings.md).
5. Save the file (UTF-8, no BOM) and start the game with `deposits` ticked.

## You have to paint it before a mine can find anything

**A new deposit map starts completely empty.** Unlike iron, coal or bauxite
— which every official map already has painted somewhere — your new deposit
exists nowhere on the terrain until you put it there yourself.

1. In-game, open the **terrain editor**.
2. Go to the tab your brush appears in — the **Resources** tab for a normal
   deposit, or the **Rocks** tab if you used `map = terrain`.
3. Find your deposit's paint tool (its icon, or a placeholder if you didn't
   add art for it) and paint an area on the map, the same way you'd paint
   iron or bauxite.
4. Build a mine on the painted area with the right `$TYPE_MINE_...` token in
   its `building.ini`. Check its info window — it should show a real
   "quality of source" number, not a huge negative number.
5. Save and reload the map, and check the paint is still there. This is the
   one part of the feature that only shows up after a real save/reload, not
   just while playing — the deposit's texture is written to disk on save,
   the same way the game's own resource maps are.

## Settings reference

| Field | Meaning |
|---|---|
| `token` | The `$TYPE_MINE_...` word used in a mine's `building.ini`. |
| `type` | A unique number ≥10 identifying this deposit to the engine. |
| `map` | Where the richness data lives. `auto` is almost always right. |
| `component` | Only needed if you picked a specific `map` by hand instead of `auto`. |
| `building_type` | `7` = mine (default, almost always what you want); `92` = water well. |
| `radius` | Search distance — use `ore`, `oil`, `bauxite`, `gravel`, `wood`, `water`, `watersurface`, or a number of your own. |
| `icon` | Which resource's icon to borrow for the minimap button. |
| `minimap` | `1`/`0` — minimap button and overlay layer. |
| `editor` | Short name for the paintbrush; leave out for no brush. |
| `deplete` | Advanced — see [03-depletion](03-depletion.md); leave alone unless you're also using that plugin and want this deposit to behave differently from the rest. |

Plugin-wide switches at the top of the file (`code_patch`, `minimap`,
`editor` under `[deposits]`) turn the whole feature on/off; leave them at `1`
unless you're troubleshooting.

## Troubleshooting

- **Mine shows quality of source as a huge negative number** — you forgot
  `radius`, or misspelled the word. Fix it and rebuild/reload.
- **Nothing to paint, or the paintbrush isn't there** — check `editor = `
  is set and is 7 characters or fewer; longer names are silently dropped
  (check the log for a line saying so).
- **Deposit disappeared after reloading a save** — this generally means the
  save was made *before* the deposit type existed in your `.ini`, or the
  plugin was off when you saved. Deposits declared here need the plugin
  active every time you play that save.
- **A commented-out `[nickel]` example** already sits in the file if you
  want to see a second deposit working side by side with copper — just
  remove the leading `;` from each of its lines.
