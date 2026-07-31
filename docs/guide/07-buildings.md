[English](07-buildings.md) | [Русский](07-buildings_RU.md)

# Plugin: buildings — new buildings from a config file

New to this? Read [00-getting-started](00-getting-started.md) first. This
plugin often pairs with [01-resources](01-resources.md) (to define what the
building makes or sells) and [05-needs](05-needs.md) (to make citizens want
what it sells).

## What this plugin does

It lets you add a whole new building — with its own name, description, and
economy — **without any 3D modelling or programming.** You pick an existing
building as a starting point (a "donor"), write a handful of lines saying
what's different about yours, and the plugin assembles a complete, working
Workshop item out of it automatically, every time the game starts.

Two examples ship with this project: a **Pharmacy** (a clothes shop, reused
as a shop that sells only medicine) and a **Pharmaceutical Plant** (a fabric
factory, reused with a different recipe). Both are in
`tesmioloader\build\plugins\buildings.ini` already, fully working, as
templates to copy from.

This is also the safest plugin in the whole project to experiment with: it
doesn't patch the game's code or memory at all, and it doesn't touch any
file the game shipped with. It only writes new folders under
`media_soviet\workshop_wip\`, the same place your own unpublished Workshop
items already live.

## The one rule: match the donor to the shape you want

Pick a donor whose **physical shape and behaviour** already matches what you
want — its mesh, its conveyor/vehicle points, its animations are all reused
as-is. A mine wants another mine as a donor, a shop wants another shop, a
factory wants another factory. You're not allowed to reshape the building,
only to relabel it and rewrite its economy (what it produces, consumes,
stores, costs, and is called). Donor buildings live under
`media_soviet\buildings_types\` — browse that folder to see what's
available and what each one looks like in-game.

## How to add a building, step by step

1. Close the game.
2. Open `tesmioloader\build\plugins\buildings.ini`.
3. Add a new section. Here's a minimal one, changing only the recipe of a
   donor shop:

   ```ini
   [my_shop]
   id     = 9100000020
   donor  = shop_clothes
   object = MyShop
   name   = My Little Shop
   desc   = Sells my_good and nothing else.

   line   = $TYPE_SHOP
   line   = $STORAGE_SPECIAL RESOURCE_TRANSPORT_COVERED 8 my_good
   ```

   - `id` — a unique number. Stick to the `91000000xx` range this project
     already uses, to stay clear of real Steam Workshop item IDs.
   - `donor` — the folder name under `media_soviet\buildings_types\` to
     clone. Look at the pharmacy (`donor = shop_clothes`) and pharmaceutical
     plant (`donor = fabric_factory`) examples already in the file for two
     working starting points.
   - `object` — a name for the item's own subfolder. Defaults to the
     section name if you leave it out.
   - `name` — what shows up in the build menu.
   - `desc` — the Workshop description. Repeat this line as many times as
     you want, one line of text each.
   - `line` — **the important part.** Each one is a real line straight out
     of a `building.ini` file, copied in verbatim. Everything the donor
     already declares stays as-is *unless* one of your `line` entries
     replaces it (see below).

4. Save (UTF-8, no BOM) and start the game with `buildings` ticked. The
   plugin writes the new item's folder before the game even starts reading
   files, so it's there from the very first load.
5. Find your building in the in-game build menu, in the same construction
   category as its donor.

## What a `line` replaces, and what it leaves alone

The donor's whole `building.ini` is copied over first, line by line.
Writing a `line =` only removes the donor's version of that **same**
setting — everything else in the donor stays untouched (its connections,
construction cost, fire points, water/sewage requirements, and so on).

Four groups of tokens replace *each other as a group*, not just
line-for-line:

| Group | What it means in practice |
|---|---|
| `$NAME` / `$NAME_STR` | Declaring either replaces both — you can't keep the donor's name and only change its internal id. |
| Any `$TYPE_*` | Only one type can be in effect — declaring a new one fully replaces the donor's (e.g. turning a shop into a factory). |
| Any `$STORAGE*` plus `$RESOURCE_VISUALIZATION` | Storages are numbered from zero, so redeclaring any of them re-numbers all of them — don't declare just one and expect the others to shift correctly. |
| `$PRODUCTION`, `$CONSUMPTION`, `$CONSUMPTION_PER_SECOND` | A recipe goes in as a whole set — declare all the lines for your new recipe together, as the pharmaceutical plant example does. |

Two settings, `$PRODUCTION_SEWAGE_POLLUTION` and
`$CONSUMPTION_WATER_REQUIRED_QUALITY`, are **not** part of the recipe group
and survive a new recipe untouched — you don't need to restate them.

`strip = $SOME_TOKEN` removes a donor line without putting anything back, if
you just want something gone rather than replaced.

## Two ways to build something new

**A. Same shape, different goods** (like the pharmacy) — pick a shop/plant
donor, keep its `$TYPE_*`, and only replace the `$STORAGE*` or recipe lines.
This is the simplest and safest kind of change.

**B. Same shape, different purpose** (like turning a shop into a factory,
or vice versa) — replace the `$TYPE_*` line too, plus whatever storage and
recipe lines that new type needs. Look closely at a real donor of the type
you're switching *to* for what it normally declares, and copy the shape of
those lines rather than guessing.

## Settings reference

| `[buildings]` setting | What it does |
|---|---|
| `enabled` | `0` unloads the plugin — folders already written stay on disk and the game keeps loading them; delete the folders by hand to remove the buildings entirely. |
| `out` | Where generated items go. Leave this alone. |
| `always` | `0` (default) only rewrites a building's folder when you've actually changed its section, the generator, or the donor. `1` rewrites everything on every launch — slower, only useful while actively iterating on assets. |
| `verbose` | `1` logs every single file copied and every donor line dropped — useful for understanding exactly why a building came out the way it did. |

## Troubleshooting

- **My changes to a section don't seem to show up in-game** — the plugin
  only rewrites a folder when something changed; if you edited a file the
  generator reads *indirectly* rather than the section itself, set
  `always = 1` once to force a full rewrite, then set it back to `0`.
- **A building.ini line I expected to survive is gone** — check whether it
  belongs to one of the four "replaces as a group" families above; if so,
  restate the parts you want to keep alongside your new line.
- **The game logs a building.ini error** — this plugin writes nothing but
  a text file per building, so the error is almost always a `line =` that
  isn't valid `building.ini` syntax, or a `$TYPE_*`/`$STORAGE*` combination
  the donor's mesh doesn't actually support (e.g. a storage index beyond
  what the donor has yards for). Compare against a real donor's own file
  under `media_soviet\buildings_types\<donor>\building.ini`.
- **An id collides with something already in `workshop_wip`** — the plugin
  refuses to touch any folder that doesn't already carry its own
  `tesmioloader.stamp` marker, specifically so it never overwrites a real
  Workshop subscription or hand-made item. Pick a different `id`.
