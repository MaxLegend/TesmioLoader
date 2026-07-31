[English](01-resources.md) | [Русский](01-resources_RU.md)

# Plugin: resources — adding new goods to the game

New to this? Read [00-getting-started](00-getting-started.md) first — it
explains where the files are and how to apply a change.

## What this plugin does

It lets you add brand-new resources — things like ores, materials or
products — that don't exist in the base game. Every mod resource here
(copper ore, copper concentrate, raw copper, copper, furniture, medicine,
gas, sand, clay, glass) was added exactly this way, by adding a few lines to
one file.

A new resource gets its own name, its own display caption, and its own
appearance when carried by trucks and trains — but it borrows all of that
appearance from an existing resource you pick as a **template**. You don't
model anything; you clone and rename.

## ⚠️ Before you touch this: save compatibility

**Adding or removing a resource from the list changes the save format.** A
save you made with 9 mod resources declared will not load if you remove one
and try to open it again, and a stock (unmodified) save will not load once
you've added any. If you want to experiment, do it on a **new game** or a
**copy of a save**, not your main city. See
[00-getting-started, step 7](00-getting-started.md#7-before-you-experiment-back-up-your-save).

## How to add a resource, step by step

1. Close the game.
2. Open `tesmioloader\build\plugins\resources.ini`.
3. Under the `[list]` section, add a line:

   ```ini
   my_resource = template_resource, Display Name
   ```

   - `my_resource` — the internal name. Lowercase, no spaces; this is also
     what you'll type in a building's recipe later if you ever add a
     building that uses it (see [07-buildings](07-buildings.md)).
   - `template_resource` — an **existing** resource whose look and transport
     type you're cloning. Good picks:
     - `rawiron`, `bauxite` — bulk/loose cargo (shows as a heap on the truck)
     - `steel`, `aluminium` — "open" cargo (shows as neat stacked units)
     - `oil` — liquid
     - `eletronics` — small covered cargo with **no truck-bed model at all**,
       the simplest option if you don't care how it looks on a truck, only
       that it exists and can be bought/sold/stored
   - `Display Name` — what shows up in menus and the trade window. Optional;
     leave it out to keep the template's own name.

   Example, a new "textiles" good that looks like electronics on a truck:

   ```ini
   textiles = eletronics, Textiles
   ```

4. If your resource needs an icon (most panels want one), drop a 48×48 PNG
   at `tesmioloader\vfs\media_soviet\resources\my_resource.png` — only
   resources cloned from `eletronics`/`clothes`/`food` need this, because
   those templates have no cargo model to fall back on for an icon either.
5. Save the file (UTF-8, no BOM — see the getting-started guide).
6. Start the game through `tesmiolauncher.exe` with `resources` ticked.
7. Open `tesmioloader.log` and search for your resource's name — you should
   see it get resolved/injected. If the file also grew past 63 total
   resources, you'll see a line like `resource array moved ... capacity NN
   records` the first time you load a world that session; that's expected,
   not an error.

## Giving it a price

**The game does not store a price per resource — it calculates one**, every
time the economy updates, by looking for a building whose recipe produces
that resource and adding up what the ingredients cost. This has one
consequence that surprises people: **a resource nothing produces is priced
at 0.00, no matter what you do**, until you either give it a producing
building or force a price by hand.

Two sections in the same file let you override this:

```ini
[base_price]
; my_ore = 6.0, 5.0        ; rouble, dollar — an INPUT to the price calculation
                            ; use this on a raw material (an ore, not a
                            ; finished good) — it makes everything made FROM
                            ; it more expensive too. On its own this does
                            ; NOT give the resource a non-zero price.

[price]
; my_finished_good = 260.0, 300.0   ; the FINAL price, forced after the
                                     ; game computes its own. This is what
                                     ; actually fixes a resource stuck at
                                     ; 0.00 — use it on anything nothing in
                                     ; the game produces.
```

Rule of thumb: if you also added a building (via
[07-buildings](07-buildings.md)) whose `$PRODUCTION` line makes this
resource, you don't need `[price]` at all — the chain prices it
automatically, the same way copper prices itself once it has a full
mine → smelter → refinery chain. If nothing produces it, uncomment a line in
`[price]`.

To watch this happen, set `price_report = 1` in the `[resources]` section
(it already is, by default) and load a save. `tesmioloader.log` prints a
line per resource per price recompute:

```
price     my_resource               0.00 RUB       0.00 USD   base 0.00 / 0.00   kind 0
```

`0.00` means "nothing in the game currently produces this" — not a bug, just
the fact the price sections above exist to fix.

## Settings you probably don't need to touch

| Setting | What it is |
|---|---|
| `hook` | Whether the plugin is watching resource lookups at all. Leave at `2`. |
| `resource_capacity` | How much room to reserve for resources. Leave at `0` — the plugin works this out from `[list]` automatically. |
| `price_hook` | Turns the whole pricing feature above on/off. Leave at `1` unless you want `[base_price]`/`[price]` to do nothing. |
| `price_report` | Prints the price table to the log. Turn to `0` once you've checked the numbers you care about — it runs on every price recompute. |

## Troubleshooting

- **Game won't start after adding resources** — double-check your `[list]`
  line has a valid template name (spelled exactly like an existing resource)
  and that the file saved without a BOM.
- **A resource shows 0.00 everywhere in the trade window** — see "Giving it
  a price" above; either give it a producing building or a `[price]` line.
- **A save from before you added resources won't load, or vice versa** —
  that's the save-format warning above, not a bug. Use a separate save for
  testing.
