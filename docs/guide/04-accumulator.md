[English](04-accumulator.md) | [Русский](04-accumulator_RU.md)

# Plugin: accumulator — battery storage for the electric grid

New to this? Read [00-getting-started](00-getting-started.md) first.

## What this plugin does

It adds a working **battery building** to the game — a "Stationary
Accumulator" that soaks up spare electricity while the grid has plenty, and
feeds it back out during a shortage, the way a real grid-scale battery
would. Nothing like this exists in the base game.

The building itself is already included:
`media_soviet\workshop_wip\9100000005`. You don't need to write anything to
try it — build it in-game like any other building, connect it to your power
grid, and the plugin does the rest automatically while the game runs.

## How to use it

1. Make sure `accumulator` is ticked in the launcher (or `accumulator=1` in
   `tesmioloader.ini`).
2. Start the game, load or start a save with an electric grid.
3. Find and build **Stationary Accumulator** the same way you'd build any
   other electrical building, and connect it to your grid.
4. That's it. While your grid has spare generation, the battery charges;
   its info window shows a **Charge: X / 5000 (Y %)** row so you can watch
   it fill.
5. During a real shortage (not enough power plants for demand), the battery
   discharges back into the grid automatically, up to its discharge limit.

No `.ini` editing is required for basic use — the settings below are for
tuning how aggressively it charges/discharges, not for turning the feature
on.

## Tuning how it behaves

Open `tesmioloader\build\plugins\accumulator.ini` (close the game first, as
always):

- **`charge_rate`** — how fast the battery fills, in storage units/second.
  Default `60`, roughly "one coal power plant's worth of spare generation."
  This exists because an empty battery would otherwise out-bid an entire
  town for electricity the moment it's connected — raise it to fill faster,
  lower it to be gentler on the rest of your grid while it tops up.
- **`discharge_rate`** — a **ceiling**, not a guaranteed drain rate: the
  battery only ever discharges into an actual shortage. Raise it to let the
  battery cover a bigger outage; lower it to make a full battery's charge
  last longer during a long shortage.
- **`min_capacity`** — how big a building's electric storage has to be
  before the plugin treats it as "a battery" rather than an ordinary grid
  node's incidental buffer. Leave this alone unless you're building your
  own custom battery building and it isn't being recognised.
- **`panel`** and **`panel_caption`** — the "Charge: X / Y" row on the
  building's window. Turn `panel` to `0` if you don't want it.
- **`gauges`** — fixes the two power dials on the battery's own window,
  which otherwise misreport it as having "no power supply" even while it's
  charging fine (a battery is a dead end on the wire, so the game's normal
  way of measuring current doesn't see it). Leave at `1`.

## What's confirmed working, and what isn't yet

Being upfront about this because it matters for expectations: **charging is
confirmed working in a real game.** Discharging into a real shortage, and
surviving a save/reload while charged, are implemented the same way but have
not yet been watched happening in an actual playthrough. If you try it and
something looks wrong during a real power shortage, that's useful
information — see the diagnostics below.

## Watching it work / diagnosing problems

Two things in the `.ini` exist purely to help you see what's happening:

- **`log_seconds = 30`** (default) prints one line per battery to
  `tesmioloader.log` every 30 real seconds:

  ```
  battery    4210.5 / 5000 (84.2 %)  in +0.417  out +0.000  this tick
             (59.87 / 0.00 per second, dt 0.00697)
  ```

  `in` / `out` tell you whether it's currently charging, discharging, or
  sitting idle — this is the fastest way to confirm the battery is actually
  doing something.

- **`trace = 1`** (default) prints a more detailed trace of what's
  happening to the buildings wired to the battery, useful if a substation
  fed by the battery isn't lighting up the way you'd expect. If you're not
  chasing a problem, you can set this to `0` to keep the log shorter.

Set both to `0` once you've confirmed things work the way you want and
don't need the extra log lines any more.

## Troubleshooting

- **Battery never seems to charge** — check your grid actually has spare
  generation beyond what's currently being consumed; the battery only takes
  what would otherwise go unused, throttled by `charge_rate`.
- **A substation fed only by a battery says "no power" even though the
  battery is full** — this is a known rough edge in how electricity
  quality/voltage propagates from a battery specifically (not from a power
  plant); it's actively being worked on. Check `tesmioloader.log` for
  `battery` and `trace` lines when this happens, since that trace exists
  specifically to diagnose it.
- **Want to remove the feature** — untick `accumulator` in the launcher, or
  delete `accumulator.dll`. The building you already placed stays standing
  and keeps whatever charge it had (it's an ordinary storage saved with the
  world), it simply stops charging or discharging automatically.
