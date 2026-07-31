[English](05-needs.md) | [Русский](05-needs_RU.md)

# Plugin: needs — giving citizens a new thing to shop for

New to this? Read [00-getting-started](00-getting-started.md) first. This
plugin works closely with [01-resources](01-resources.md) (to create the
good itself) and, for a good sold in its own dedicated shop, with
[07-buildings](07-buildings.md) (to create that shop). Read this guide
first, it explains the concepts the other two build on.

## What this plugin does

The base game has four things citizens shop for: food, clothes, electronics
and (in some buildings) alcohol/medicine-like specials. This plugin adds a
**fifth** — citizens will start wanting a resource of your choice, shops
will stock it, and trucks/trains will need to keep it supplied, exactly like
the base four.

Two ready-made examples ship with this project:

- **Furniture** — a department-store good, sold alongside electronics in
  every big shop that already sells electronics.
- **Medicine** — sold only in a dedicated **Pharmacy** building (see
  [07-buildings](07-buildings.md)), nowhere else.

## The one concept you need: the "donor"

Every new need is defined as a copy of an **existing** citizen demand — a
**donor**. You don't invent shopping behaviour from scratch; you clone an
existing one (say, "wants electronics") and just change what resource it
asks for, how much, how often, and how unhappy going without it makes
someone. The donor also decides *which shops* end up selling your new good:
whatever shops already sell the donor's resource are where a slot gets added
for yours, automatically — unless you say `category = none`, which means
"nowhere automatically, I'll build a dedicated shop myself."

## How to add a new department-store-style good, step by step

Use this path if you want your good sold alongside existing categories, like
furniture next to electronics.

1. Close the game.
2. Declare the resource itself in `resources.ini` first — see
   [01-resources](01-resources.md). Clone it from `eletronics` if you don't
   care about a truck-bed appearance, or from a bulkier template if you do.
3. Open `tesmioloader\build\plugins\needs.ini` and add a line under
   `[list]`:

   ```ini
   my_good = eletronics, 1.0, advanced, 0.35, 0.010
   ```

   Reading it left to right:
   - `my_good` — the resource name from step 2.
   - `eletronics` — the **donor**. Use `food` or `meat` for a grocery-shaped
     good, `clothes` or `eletronics` for a department-store good.
   - `1.0` — how much of it citizens want, relative to how much of the
     donor they want. `1.0` means "as much as electronics."
   - `advanced` — which kind of shop stocks it. `auto` (leave this column
     out) puts it wherever the donor's own resource already goes; naming
     `advanced` narrows it to big department stores only, `medium` to
     smaller ones, and so on. See the comments in the file for the full
     list.
   - `0.35` — the **chance**, 0 to 1, that any given citizen picks this need
     up on a given planning cycle. This is what decouples "wants furniture"
     from "wants electronics today" — without it, a citizen only wants your
     good exactly when they also want the donor. `0.35` means roughly a
     third of citizens take it up per cycle.
   - `0.010` — how much unhappiness one cycle of going without it costs,
     0 to 1. `0` (or leaving it out) means it's tracked but doesn't affect
     mood. Compare: the base game itself takes about `0.035` off for
     clothes shortages; start small (`0.005`–`0.02`) for a comfort good.
4. Provide a 48×48 PNG icon at
   `tesmioloader\vfs\media_soviet\resources\my_good.png` if the resource
   doesn't already have cargo geometry to fall back on (anything cloned
   from `eletronics`, `clothes` or `food` needs this).
5. Save (UTF-8, no BOM) and start the game with `needs` and `resources`
   both ticked.

## How to add a good sold only in its own dedicated shop

Use this path for something like medicine, sold in a pharmacy and nowhere
else.

1. Do steps 1–2 and 4–5 above, but in step 3 set the category to `none`:

   ```ini
   medicine = eletronics, 0.5, none, 0.30, 0.008
   ```

   `none` means "citizens want this, but don't add it to any existing
   shop's shelf — I'm building it a shop of its own."
2. Build that dedicated shop through [07-buildings](07-buildings.md) — it
   needs a `$STORAGE_SPECIAL` line naming your resource. The pharmacy
   example in `buildings.ini` shows exactly this.

## ⚠️ Save compatibility

Adding a need adds a storage slot to every shop that stocks its donor, and
that slot list is part of the save. **Test on a copy of a save**, the same
caution as [01-resources](01-resources.md) — see
[00-getting-started, step 7](00-getting-started.md#7-before-you-experiment-back-up-your-save).

## Settings reference

The `[list]` line format, in full:

```
resource = donor[, factor[, category[, chance[, unhappiness]]]]
```

Every field after `donor` is optional and has a sensible default (see the
comments in the `.ini` for exact defaults).

| `[needs]` setting | What it does |
|---|---|
| `enabled` | `0` turns the whole plugin off. |
| `demand` | `1` makes citizens actually want the goods. Turn off to only stock shops without changing citizen behaviour. |
| `storage` | `1` adds shelf space to shops. Turn off if you're stocking shelves some other way. |
| `max_demands` | How many things a citizen can want at once — capped at `7` by the game itself, can't be raised. |
| `when_full` | `skip` (default) — a citizen who already has 7 demands waits for room; `replace` — swaps in your need for the donor's for one cycle. |
| `probe` | Diagnostics — dumps citizens' demand lists and shop storage details to the log. Leave on until you've confirmed shops are stocked correctly, then it's safe to turn off. |
| `log_seconds` | How often a summary line prints. `0` to silence. |

## Watching it work

1. Set `probe = 1` (default).
2. Load a save and check `tesmioloader.log` for lines about your resource —
   whether it was found in citizens' demand lists and in the shop storages
   it should have landed in.
3. In-game, check a department store (for a `category` other than `none`)
   or your dedicated shop (for `none`) actually has a shelf for the good,
   that a truck brings it, and that the shelf slowly empties as customers
   arrive.

## Troubleshooting

- **Citizens never seem to want it** — check `demand = 1`, and remember
  `chance` limits how many citizens take it up per cycle; a low chance
  looks like "nobody wants it" over a short session.
- **The good never appears on a shelf** — check `storage = 1`, and that the
  category you picked actually matches shops that sell the donor (the log
  warns if you named a category the donor's shops don't use).
- **Citizens want it but it's never in the store, and the store never
  seems to restock** — the store may simply not be *known* to sell it from
  the citizens' point of view (a documented rough edge — see the project's
  own notes on this). Try switching the shop to a wider `$STORAGE_DEMAND_*`
  category as described in the pharmacy example's comments in
  `buildings.ini`.
