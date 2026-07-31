[English](06-walking.md) | [Русский](06-walking_RU.md)

# Plugin: walking — how far citizens will walk or drive

New to this? Read [00-getting-started](00-getting-started.md) first.

## What this plugin does

By default, a citizen will only walk up to 480 metres (measured along
roads/footpaths, not a straight line) to a shop, a job, or a service, and
will only drive up to 2500 metres between home, work and parking. Beyond
that distance, the game simply doesn't consider a building "reachable" —
it's not a difficulty setting, it's a hard cutoff. This plugin lets you
raise (or lower) both numbers.

This is the simplest plugin to use: four numbers, no hooks into the
economy, no save-format changes.

## How to use it

1. Close the game.
2. Open `tesmioloader\build\plugins\walking.ini`.
3. Change `distance` (walking, metres) and/or `car_distance` (driving,
   metres) to what you want. The base game's own values are `1000` and
   `2500` respectively in this file already (raised from vanilla's 480/2500
   — lower `distance` back to `480` if you want the original limit).
4. Leave `regen_on_load = 1` if you're applying this to a **city you
   already have built** — see below for why.
5. Save (UTF-8, no BOM) and start the game with `walking` ticked.

## Why `regen_on_load` matters

Which buildings a citizen can walk to isn't recalculated on the fly — it's
worked out once and stored with each building, as part of your save. That
means:

- **New construction** always uses whatever `distance` is set to when
  it's built — no extra step needed.
- **A city you already had before changing the setting** keeps its *old*
  connections until something recalculates them. `regen_on_load = 1`
  makes that recalculation happen automatically every time you load that
  save — it costs a few extra seconds on the loading screen for a big
  city, once per load, and after that citizens in your existing town use
  the new distance too.

If you'd rather have fast loads and don't mind that the new distance only
affects things you build from now on, set `regen_on_load = 0`.

## Settings reference

| Setting | What it does |
|---|---|
| `enabled` | `0` turns the plugin off entirely and restores vanilla behaviour. |
| `distance` | Max walking distance, in metres. `0` removes the limit entirely (works, but can make placing buildings slow in a large city, since the game then has to search the whole road network); the plugin refuses anything above `20000`. |
| `car_distance` | Max driving distance for citizens with a car, same units. Raise it alongside `distance` if you want cars to stay meaningfully longer-range — at equal values a car buys nothing over walking. |
| `regen_on_load` | `1` rebuilds every walking/parking connection when a save loads, so an existing city picks up the new limit immediately. `0` = only new construction gets it. |
| `probe` | Diagnostics only — logs what the patch found before writing anything. Leave at `0` unless troubleshooting after a game update. |

## What's confirmed, and one known rough edge

The actual walking/driving behaviour — which buildings a citizen is willing
to use — is confirmed working: citizens do use shops and workplaces past the
old 480 m limit, and `regen_on_load` does rebuild connections on an existing
city.

**One thing is not yet fixed**: the in-game overlay button that highlights a
building's walking distance on the map (for visually checking coverage)
draws its own, separate calculation and has historically shown the old
distance even when citizens were already using the new one. If the overlay
looks wrong but citizens are clearly reaching farther buildings in practice,
trust the citizens, not the overlay — this is a display quirk, not a sign
the setting didn't apply.

## Troubleshooting

- **Nothing seems to have changed on an existing city** — check
  `regen_on_load = 1` and that you actually reloaded the save after
  changing the setting (not just kept playing in the same session).
- **Placing buildings got noticeably slower** — you likely set `distance`
  very high or to `0` on a large, built-up city; the search cost grows with
  the limit. Lower it back down.
- **The walking-distance overlay button still shows the old radius** — see
  the note above; it's a known display-only issue, separate from actual
  citizen behaviour.
