[English](README.md) | [Русский](README_RU.md)

# tesmioloader — user guides

Plain-language, step-by-step instructions for **players**, not modders. If
you want to know how the loader is *built* — addresses, hooks, engine
internals — that's the `docs/` folder one level up
([docs/01-architecture.md](../01-architecture.md) onward). This folder only
answers "what do I click, what do I type, what should I see."

Start here:

1. **[00-getting-started](00-getting-started.md)** — install, launch, turn
   plugins on/off, edit a setting safely, read the log, back up your save.
   Read this first, always.

Then pick whichever plugin you want to use:

| Guide | Adds |
|---|---|
| [01-resources](01-resources.md) | A brand-new tradeable resource (ore, material, product) |
| [02-deposits](02-deposits.md) | A new kind of mineable deposit, paintable on the map |
| [03-depletion](03-depletion.md) | Mines that actually run out of ore over time |
| [04-accumulator](04-accumulator.md) | A battery building that stores spare electricity |
| [05-needs](05-needs.md) | A new thing citizens shop for |
| [06-walking](06-walking.md) | Longer (or shorter) walking/driving distance for citizens |
| [07-buildings](07-buildings.md) | A whole new building, cloned from an existing one |

Each guide is self-contained but a few build on each other — a new shop good
(`needs`) usually starts with a new resource (`resources`), and a good sold
in its own dedicated shop needs a new building (`buildings`) too. Each guide
links to the others it depends on.

## The two things worth remembering before you start

- **Close the game before editing any `.ini` file**, and save it as
  UTF-8 **without a BOM** — see
  [00-getting-started](00-getting-started.md#4-changing-a-setting) for why
  this matters and how to do it in plain Notepad.
- **Back up your save before trying `resources`, `needs`, or `depletion`**
  — these three change things that live inside the save file itself, in
  ways that aren't always reversible on a city you already care about.
  `deposits`, `accumulator`, `walking` and `buildings` are all safe to
  toggle freely.
