# Stationary accumulators — storing electricity

**A plugin**, not part of the loader: `plugins/accumulator/accumulator.cpp`
builds to `build/plugins/accumulator.dll`, and deleting that DLL removes the
feature. See [09-plugins.md](09-plugins.md) for the mechanism.

The base game cannot store electricity. It generates and consumes it in the same
tick, and a plant that trips takes its whole grid down with it. This adds a
building that charges when there is spare generation and feeds it back when
there is not.

**Nothing about it is new engine machinery.** Electricity is already an ordinary
resource in an ordinary storage, and the transfer code already works for any
building. What the plugin adds is a throttle and one extra call.

## What electricity actually is

Every building carries a vector of storages at `building+0x970`…`+0x978`, stride
`0xE0`. Each has a transport class, and **class 9 is
`RESOURCE_TRANSPORT_ELETRIC`** — the class of the `eletric` resource. A power
plant has one; so does every consumer.

| Storage offset | Contents |
|---|---|
| `+0x00` / `+0x08` | `std::vector<Slot>` begin and end, stride 16. Class 9 holds one resource, so there is exactly one slot |
| `+0x8C` | capacity, the number `$STORAGE` declared |
| `+0x90` | transport class |
| `+0xA8` | what has been handed out through it this tick |

| Slot offset | Contents |
|---|---|
| `+0x00` | the resource record |
| `+0x08` | how much is in it |
| `+0x0C` | quality, 0..1. **A source with 0 hands out nothing** |

Storage descriptors are built by `FUN_1400e40f0` at rva `0xE40F0`, the
`$STORAGE*` parser: it writes the capacity to `+0x8C`, the class to `+0x90`, and
pushes one slot per resource whose per-class factor at
`resourceRecord + 0xCC + class*0x20` is non-zero. That factor is what
[02-findings.md](02-findings.md) records as "a storage declared with the wrong
transport class reports zero capacity" — now with the field it comes from.

## How the grid moves it

| RVA | What |
|---|---|
| `0x139A80` | the building dispatcher, one simulation tick. Collects power plants (type 17) and transformers (19) into vectors of their own |
| `0x1B8EE0` | `(game, building)` — the **power plant's** distributor: walk the wires, collect every reachable class-9 storage, hand energy over |
| `0x1BB700` | `(game, building, what, priority)` — the **grid node's**. `what` picks voltage (0) or energy (1); this is what a transformer uses, and what a battery uses |
| `0x1BDD40` | the transfer itself, reached by both |
| `0x1B9640` | the same for a **substation** (18), over the buildings in its range at `+0x10C8` rather than over wires |
| `0x1D1E10` | the production tick. Among other things it sets slot quality to 1 for a power plant's class-9 storage, and for a heating plant's class-14 one |

`0x1B8EE0` walks the connection vector at `building+0xA10` (stride `0x60`,
`+0x00` is the connection type, 7 = `ELETRIC_HIGH`, 8 = `ELETRIC_LOW`), and
`0x1BDD40` then divides:

```c
shortfall  = sum over receivers of max(0, capacity - content)
available  = min(dt * sourceStorage.capacity * scale, sourceSlot.amount)
share      = (capacity - content) / shortfall * available     // per receiver
```

with a further ceiling from the line itself and `content` never passing
`capacity`. Everything moved is scaled by the **source's** slot quality.

**The transfer code does not care that the source is a power plant.** Building
type is read in one place, and only to keep plants and electricity importers out
of the *receiver* list (`type != 17 && type != 31`). That is the whole opening:

| | |
|---|---|
| charging | free. A building with a large class-9 storage is filled by the grid exactly like a consumer |
| discharging | four calls to `0x1BB700` for our own building after the dispatcher has run — the same ones it makes for a transformer, voltage first |
| saving | free. The charge is in the game's own storage, so it goes into the save and survives the plugin being removed |

## The problem, and the throttle

An empty battery declaring 5000 units has a shortfall of 5000. The town around
it has a shortfall of 200. The proportional split therefore hands the battery
96 % of everything generated and blacks the town out until it is full.

The one field the grid reads to decide how much a battery wants is its storage
capacity, so that is what the plugin rewrites, twice per tick:

```
before the dispatcher   capacity = min(declared, content + charge_rate * dt)
                        and, if it holds a charge, declare the node live
after it                capacity = discharge_rate, call 0x1BB700
then                    capacity = declared
```

The first line makes the shortfall the grid sees exactly one tick of charge
rate. The second makes `dt * capacity` — the distributor's own ceiling on a
source — exactly one tick of discharge rate. Between them, capacity is a rate
and not a size; outside the hook it is the declared size again, which is what
the building's window shows.

**There is no state between ticks.** The declared capacity is saved on the stack
for the length of one hook call, and the accumulators are re-found from the
game's building vector (`game+0x11B08`…`+0x11B10`) every tick. A cache keyed on
a building pointer would go stale on a world load, and every version of that bug
in this project has cost a session — see [07-pitfalls.md](07-pitfalls.md),
"Armed is a claim, not a fact".

There is no "is the grid short" test either, and none is needed: `0x1BDD40`
divides by the shortfall it finds and moves nothing when there is none. A
battery in a healthy grid hands out nothing without being asked to.

### Voltage is handed out separately from energy

This is the part that took a second session in game to find, and it is the
difference between a battery that works and one that looks broken.

A grid node hands out **two** things, and `0x1BB700`'s third argument picks
which. It ends in

```c
FUN_1401bdd40(game, b, storages, conns, buildings, what == 0, 1.0f);
```

and that sixth argument gates the transfer: **non-zero** runs the tail of
`0x1BDD40`, which propagates slot quality — "there is voltage here" — and moves
no energy at all; **zero** runs the body, which moves energy. The dispatcher
runs both for every transformer it considers live, voltage first:

```
0x1BB700(b, 0, -1)          voltage, unfiltered
0x1BB700(b, 1,  2 / 1 / 0)  energy, priority by priority
```

The first version of this plugin discharged through `0x1B8EE0`, the **power
plant's** distributor, which only ever moves energy. The result in game was a
battery holding 1489 of 5000 units next to a substation reading `0 KV` and
`0.000 MW`, having been filled exactly once. That is not a bug in the transfer —
it is the loop closing: without voltage nothing downstream draws, without draw
nothing is short of capacity, and `0x1BDD40` divides by the shortfall it finds.

So a battery discharges the way a transformer does. The fourth argument is a
per-connection priority matched against `building+0xE22+connectionIndex`; `-1`
skips the filter, but **only the filtered form reaches a neighbour the
dispatcher has flagged unpowered** at `+0x1130` — which is exactly the state a
substation hanging off a battery is in, so energy goes out priority by priority.

### A charged battery is a source, and has to say so first

Handing out voltage was still not enough on a branch with no power plant behind
it: a substation fed only by a battery went dark again.

`+0x1130` is the reason. The dispatcher's solver starts at the power plants,
walks outward, and sets that byte on **every grid node it did not reach** — the
one writer is `0x13B02E`, inside the dispatcher; the readers are its own solver
and `0x1BB9BD` inside `0x1BB700`. Next tick the flag keeps the node out of the
live set, and `0x1BB700` refuses to hand energy to a neighbour carrying it. The
battery was marked unpowered because nothing upstream reached it, and being
marked unpowered is what stopped it powering anything downstream.

So before the dispatcher runs, a battery holding a charge declares itself:

```c
slot.quality        = 1.0f;   // there is voltage at this node
*(voltage pointer)  = 1.0f;
building[0x1130]    = 0;      // and it is not unpowered
```

All three are what a live node looks like, and a charged battery is the one node
in the grid for which they need nothing upstream to be true. Doing it **before**
the tick rather than after is what matters: the engine's own solver then picks
the battery up as part of its live set and propagates outward from it, so the
whole branch behind it comes alive rather than only its immediate neighbour. The
explicit `0x1BB700` calls afterwards top that up.

An empty battery declares nothing and goes back to being an ordinary node.

### Voltage only spreads inside a transfer — and a full storage never transfers

Even declaring itself live was not enough, and the probe is what said why:

```
battery  probe: type 19  cap 5000.00  amount 1433.09  quality 1.000   <- the battery
battery  probe: type 18  cap    2.50  amount    2.50  quality 0.000   <- the substation
battery  probe: type 6   cap    0.26  amount    0.26  quality 0.000   <- and its factory
```

The substation's storage is **full** and its quality is zero. Quality is only
ever raised inside `0x1BDD40`, a transfer only happens into a shortfall, and a
storage sitting at capacity is not short of anything. The branch had the energy
and no way to notice it had arrived — which is exactly the reported symptom:
voltage appears the instant a wire is connected, when the storages are still
empty and a transfer does happen, and dies as soon as they fill.

Two useful facts fell out of the same dump. `V` — the float behind
`building+0x10E8` — is **always** equal to the slot's quality: they are one
field reached two ways, not two fields. And every building on the branch behind
the battery had quality 0 while everything on the power plant's branch had 1,
which is what localised it to the battery's own hand-out.

### Where the voltage actually goes, and the seam that fixes it

`spread_voltage` tried to say it directly after the tick, and it did not work.
Tracing the battery's neighbours through three phases of one tick is what
settled it:

```
trace before tick: neighbour type 18  quality 1.000  covers 2  short 0.320
trace after tick : neighbour type 18  quality 0.000  covers 2  short 0.320
trace after share: neighbour type 18  quality 1.000  covers 2  short 0.320
```

The substation covers two buildings, they are short of 0.32, and it **has** a
voltage at the top of the tick. Somewhere inside the tick that voltage is
cleared — and `0x1B9640`, the call that hands its storage to those two
buildings, runs *after* the clearing. It distributes nothing, because
`0x1BDD40` moves nothing out of a source whose slot quality is zero. Every tick,
forever.

That is why restoring the voltage after the dispatcher never helped: the
clearing happens again next tick and still lands first. The restore has to sit
**between** the two, and the only seam there is the substation's own
distribution call.

So `0x1B9640` is hooked. A substation that a charged battery is wired to has its
quality put back to full immediately before it distributes; everything else is
left exactly as the engine left it. Its catchment then draws, its storage
empties, and the shortfall that creates is what the battery's discharge fills
after the tick — the same loop a power plant runs, closed at the one point it
was open.

Which substations count is rebuilt every tick from the batteries' own wire
connections: `conn+0x28` is the line, `line+0x20`/`+0x28` its ends, each end's
`+0x70` vector begins with the building — the walk both engine distributors open
with.

`0x1D1E10`, the factory production tick that `0x1B8EE0` opened with and that had
to be suppressed for it, is not on this path at all. The hook it needed is gone.

## The building

`media_soviet/workshop_wip/9100000005/StationaryAccumulator`, geometry borrowed
from the base game's electric substation.

```ini
$NAME_STR "Stationary Accumulator"
$TYPE_TRANSFORMATOR
$STORAGE RESOURCE_TRANSPORT_ELETRIC 5000
$CONNECTION_ELETRIC_LOW_INPUT  ...
$CONNECTION_ELETRIC_LOW_OUTPUT ...
```

The type matters more than anything else in that file:

| Type | Why not |
|---|---|
| 17 `POWERPLANT` | excluded from the receiver list, so it could never charge |
| 18 `SUBSTATION` | hands its storage out to everything in range every tick through `0x1B9640`, at full capacity — the throttle would have to fight the engine for the same field |
| **19 `TRANSFORMATOR`** | an ordinary grid node. The grid fills its storage; nothing in the base game takes it back out |

`$STORAGE RESOURCE_TRANSPORT_ELETRIC` is not invented here — it is in
`buildings_types/eletric_substation.ini`, commented out with `--`, which is what
established the syntax parses.

Without the plugin the building still stands and still charges. It simply never
gives anything back: a very large, very useless capacitor. Nothing misbehaves.

## Configuration

`build/plugins/accumulator.ini`, section `[accumulator]`:

| Key | Meaning |
|---|---|
| `enabled` | 0 hooks nothing and unloads |
| `building_types` | which building types may be a battery. Default `18,19` |
| `min_capacity` | the class-9 capacity that separates a declared battery from the incidental storage a grid node carries. Default 500 |
| `charge_rate` | units per second into the battery. The only thing standing between a fresh battery and a blackout |
| `discharge_rate` | units per second back out. Costs nothing while the grid is healthy |
| `log_seconds` | one line per battery per interval |
| `probe` | seconds between a dump of every building carrying a class-9 storage. A reverse-engineering aid |

For scale: the coal plant is `$PRODUCTION eletric 70`, the solar plant 70, a
wind turbine 15. `charge_rate = 60` is therefore about "one plant's worth of
spare generation goes into the battery".

## The dials, and "Building is without power supply"

The first session in game charged a battery and reported it as unpowered at the
same time, and both are correct:

```
Voltage  0 KV        Wattage  0.000 MW        - Building is without power supply
battery  probe: type 19 built cap 5000.00 slots 1 amount 125.45 quality 1.000
```

`0x1BD4C0` computes a grid node's voltage as `2 × wattage / breaker`, and
wattage is current passing **through** the node. A battery is a dead end: energy
arrives and stops, so nothing passes through, so wattage is zero and voltage
with it. The warning is downstream of the same number.

`gauges = 1` drives them from what actually moved in and out of the storage this
tick — `+0x10E0` and `+0x10E8` through their pointers, and the smoothed copies
at `+0x1100` and `+0x1128` the two dials read. Charge and discharge are both
current through the battery, so the wattage dial shows their sum over `dt`,
which is what a transformer passing the same energy along would show. Voltage is
full whenever the battery holds anything or moved anything.

`panel = 1` adds a row under the dials:

```
Charge: 4210.5 / 5000  (84.2 %)
```

from a post-hook on `0x702610`. That panel takes the running Y **by pointer**
(its fourth argument) and leaves the next row's Y in it, so the row is drawn at
`*y` and one row height handed back — the same trick as the mine's "Deposit
remaining", one indirection further out. Its parent at `0x6F4A20` calls it for
building types 19, 31, 32 and 35, and for anything whose type descriptor
declares electricity use.

## What the first session showed

Everything the offsets rest on, confirmed on live data:

```
battery  probe: type 19  built  cap 5000.00  slots 1  amount 125.45  quality 1.000  out 0.000
battery  probe: type 17  built  cap   23.34  slots 1  amount  23.30  quality 1.000  out 1.445
battery  probe: type 2   built  cap    0.32  slots 1  amount   0.32  quality 1.000  out 0.017
battery  probe: type 18  built  cap    2.50  slots 1  amount   2.50  quality 1.000  out 0.000
```

- `+0x8C` is the capacity, and it is what `$STORAGE` declared: 5000 for the
  battery, 23.34 for the power plant that generates it, 0.32 for a house.
- `+0x08` of the slot is **content**, not a second capacity — it started at zero
  and climbed.
- `+0x0C` is quality, 1.0 on everything the grid reaches.
- `+0xA8` is what left through the storage this tick: non-zero on the plant and
  on consumers passing energy along, zero on the battery, which had not been
  asked for any.

The battery charged. It was found by type and capacity exactly as intended, and
the dispatcher hook runs on its own thread without upsetting anything.

## Still to check

1. **A substation fed by nothing but a battery stays lit**, and the buildings
   behind it keep working. That is the case the `+0x1130` clearing is for. If it
   still goes dark, the dial itself is worth reading next: `+0x1128` is smoothed
   towards its target by `0x1BD37C` rather than written directly, so the target
   it chases may be a fourth field none of this touches.
2. `log_seconds` prints `in` and `out` separately, per tick and per second. The
   `in` figure should sit at `charge_rate` while generation is spare and fall to
   nothing when it is not; `out` should be non-zero whenever anything downstream
   is short.
3. Cut generation — stop a plant, or wait for a still night with wind turbines —
   and watch `out` become non-zero and the town stay lit.
4. Save, reload, confirm the charge came back. It should: it is in the game's
   own storage. If it does not, the storage is not what is saved and the "no
   state" argument above needs revisiting.
5. **Type 19 in the transformer solver.** The dispatcher runs an iterative pass
   over transformers (`0x139A80`, the loop from `0x13A6xx`) that this has not
   been read against. If a battery upsets it — flickering voltage on that
   branch, or a neighbouring transformer misbehaving — try `$TYPE_SUBSTATION` in
   `building.ini` and `building_types = 18` in the plugin's, and accept that a
   substation also feeds its own catchment at full capacity.

## Heat

The same shape almost certainly works: heat is transport class **14**, heating
plants are building type **70**, and the dispatcher collects them into a vector
of their own exactly as it does power plants. The production tick sets slot
quality for class 14 in the same breath as it does for class 9.

What has not been checked is the distribution function on that side, and whether
it collides with the hot-water tanks buildings already carry
(`fHeating_WaterTankCapacity` in the script VM's `Building` struct). Do that
before assuming this file transfers.
