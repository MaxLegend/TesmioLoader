[English](03-depletion.md) | [Русский](03-depletion_RU.md)

# Plugin: depletion — deposits that actually run out

New to this? Read [00-getting-started](00-getting-started.md) first.

## What this plugin does

In the base game, a mine checks how rich its deposit is *once*, the moment
it's built, and then produces at that same rate forever — the deposit itself
never gets smaller. This plugin changes that: a working mine now spends down
its deposit as it produces, its "quality of source" drops as the deposit
runs low, and — because the plugin is draining the same map the minimap
shows you — you can watch the coloured patch under a mine physically shrink
over time.

This applies to the base game's own deposits (oil, iron, coal, uranium,
bauxite, gravel) and to anything added through
[02-deposits](02-deposits.md), like copper.

## ⚠️ Read this before you turn it on

**This plugin writes into the terrain itself, and the terrain is part of
your save.** Once a mine has eaten into a deposit, that's saved permanently
— turning `enabled` back to `0` afterwards does not restore what was
already mined. It doesn't break your save (the file still loads fine
either way), it just means the depletion that already happened stays
happened. **Test this on a copy of a save**, not the city you've spent 40
hours on, until you're happy with your settings.

## How to turn it on

1. Close the game.
2. Open `tesmioloader\build\plugins\depletion.ini`.
3. Make sure `enabled = 1`.
4. Set `tonnes_per_texel` — this is the one number that decides how long
   deposits last. Bigger number = deposits last longer. Start with the
   default (`1200`) and adjust after watching a real mine for a while (see
   "Watching it work" below).
5. Check the `vanilla = ` line — it lists which of the base game's own
   deposits deplete: `oil,iron,coal,uranium,bauxite,gravel`. Remove a name
   to leave that one infinite, like the base game. Anything you added via
   [02-deposits](02-deposits.md) always depletes; give a deposit its own
   `deplete = 0` in `deposits.ini` if you want to exempt just that one.
6. Save (UTF-8, no BOM) and start the game with `depletion` ticked.

## Watching it work

Because deposits normally take a very long time to run down, the plugin can
print progress so you can actually see it happening instead of taking it on
faith:

1. Set `log_seconds = 60` (this is the default).
2. Play with a mine already built, or build a new one.
3. Open `tesmioloader.log` and search for `deplete`. You'll see a line per
   mine, every 60 real seconds, like:

   ```
   deplete  coal mine: 99.86% left, 258926 of 259289 t, quality 0.686
   ```

4. Watch that percentage over a few in-game days. If it barely moves and
   you want mines to run out faster, **lower** `tonnes_per_texel`; if it's
   dropping too fast, **raise** it.
5. The mine's own info window also gets a new row once you rebuild/reopen
   it: **"Deposit remaining: 251.4 kt / 259.3 kt (96.9 %)"** — that's the
   `panel = 1` setting doing its job, and you can change the label with
   `panel_caption`.

## Gravel is different from every other deposit

Every other deposit lives in an invisible data layer under the terrain.
**Gravel does not** — gravel richness is read straight from the terrain's
own ground texture, the same layer the map editor's rock brush paints. That
means depleting a gravel pit **visibly wears the ground texture away** under
it as it's mined — which is exactly what you'd expect from a real gravel
pit, but it's worth knowing it looks different from a coal mine slowly
fading on the minimap.

Because a gravel deposit's search area is much smaller than an ore mine's,
it needs a much bigger `tonnes_per_texel` to last a comparable time — that's
why the `vanilla = ` line gives gravel its own number after a colon
(`gravel:30000`) instead of sharing the general setting.

## Settings reference

| Setting | What it does |
|---|---|
| `enabled` | `0` turns the whole plugin off. |
| `tonnes_per_texel` | The main balance knob — how much ore one map "pixel" is worth. Bigger = deposits last longer. |
| `vanilla` | Which base-game deposits deplete: any of `oil,iron,coal,uranium,bauxite,gravel`, or `all`, or `none`. Add `:number` after a name for its own rate. |
| `flush_seconds` | How often the drained-down map is written back (visual/save only — production itself is always calculated live). Leave at default. |
| `log_seconds` | How often progress lines print to the log. `0` to silence them once you're done calibrating. |
| `panel` | `1` adds the "Deposit remaining" row to a mine's info window. |
| `panel_caption` | The text of that row's label. Keep it plain ASCII (no Cyrillic) — it's read through an API that mangles anything past basic English letters. |

## Troubleshooting

- **Nothing seems to be depleting** — check `enabled = 1`, and give it more
  real time; at the default rate a mine holds years of in-game output, so
  short test sessions won't show much movement. Turn `log_seconds` down to
  see it sooner.
- **A gravel pit visibly digs into the ground** — that's correct, not a
  bug; see the section above.
- **You changed your mind and want the old infinite deposits back** — set
  `enabled = 0`. Deposits already mined down stay mined down; new deposits
  and undepleted ones behave like vanilla again from that point on.
