# easystart — needs that arrive with the century

> Every address here is for **SOVIET64.exe v1.1.1.9** (`TimeDateStamp`
> `0x6A3EB6AD`). Version 1.0 of this plugin was derived against v1.1.1.7 while
> that build was still on disk, and every code address in it was wrong by
> `+0x70`, `+0xA0` or `+0x1E0` once Steam updated the game. `.data` did not
> move — the two reason ledgers are at the same place in both builds — and
> `.rdata`'s float pool moved by `-0x18`. See
> [07-pitfalls.md](07-pitfalls.md), *Check the build before you read the exe*.

A map that starts pre-populated hands the player a town whose citizens want
electricity, running water, central heating and a cinema on the first day. They
lived without all of it the day before. `easystart` holds each need back until a
year in `plugins/easystart.ini`.

It holds back the **consequences only**. A cinema built in 1925 is still walked
to, a shop still sells clothes, a pub still serves; those things simply do not
feed happiness yet, and their absence costs none. That distinction is the whole
design, and it is what decided every mechanism below.

## What a need actually is

There is no list of needs in the game. There is something better: the game keeps
an itemised ledger of *why* happiness and health moved, and the statistics window
draws it. Finding that ledger is what turned "which needs are there" from a
guess into an enumeration.

Both drawers walk a float array immediately followed by an int array of the same
length, with a base language id incremented once per entry — so one `mov
r12d, 0xD30F` (54031) at `0x8DB95` and a `lea r14,[rdi+0x10B74]` give the whole
table:

| Ledger | Drawer | Reasons | Floats | Counts |
|---|---|---|---|---|
| happiness | `0x8D490` | 23, ids 54031…54053 | `0x9E5970` | `0x9E59CC` |
| health | `0x8C3C0` | 12, ids 54003…54014 | `0x9E58B0` | `0x9E58E0` |

Scanning `.text` for `inc dword ptr [rip+X]` landing in either count array maps
every reason to the code that raises it. That scan is the map of the feature:

| # | Happiness reason | Raised by | Gated as |
|---|---|---|---|
| 0 | Starving | `0x83A6D0` | `food` |
| 1 | Lack of meat | `0x83A6D0` | `meat` |
| 2 | Unable to get clothes | `0x83A6D0`, `0x836960` | `clothes` |
| 3 | Unable to get electronics | `0x83A6D0` | `electronics` |
| 4 | Job | **nobody** — dead in this build | — |
| 5 | No electricity | `0x1BC210` | `electricity` |
| 6 | No water | `0x1BC210` | `water` |
| 7 | Crime | `0x256080`, `0x2571E0` | — |
| 8 | Interior temperature | `0x488B80` | `heating` |
| 9 | TV or radio broadcast | `0x4C21F0` | — (a bonus only) |
| 10…14 | prison, orphanage, a child's death, relocation, expulsion | events | — |
| 15 | Low government loyalty | `0x83A6D0` | — |
| 16 | Unable to visit a hospital | `0x83A6D0` | `health` |
| 17 | Unable to drink alcohol | `0x83A6D0` | `alcohol` |
| 18 | Unable to practice sports | `0x83A6D0` | `sport` |
| 19 | Unable to pray in church | `0x83A6D0` | `religion` |
| 20 | Unable to enjoy culture | `0x83A6D0` | `culture` |
| 21, 22 | prison / orphanage upbringing | events | — |

and on the health side: Starving, Pollution, Stroke, Unable to visit doctor,
Drinking alcohol, Burns from fire, **Insufficient or polluted drinking water**
(`0x1B0950`), Crime, **Low interior temperature** (`0x488B80`), Ambulance delay,
Global pandemic, Lack of meat.

Two things fall out of the table and both matter:

- **There is no "no sewage" reason and no "no school" reason.** Sewage acts
  indirectly — a full sewage store stops the building drawing water and the
  player sees *No water* — so it rides on the `water` gate and needs nothing of
  its own. School and kindergarten are not needs at all; see below.
- **`Job` has no writer.** The reason is drawn and can never be non-zero.

## Nine needs, one function, no code patch

```
rva 0x83A6D0   FUN(game, person)
```

One caller, inside the per-person tick, immediately after that tick has run
the daily planner. It walks the demand array the planner
has just rebuilt, moves `person+0xD8` (happiness) and `person+0xE0` (health), and
attributes each change. Which reason is decided by two fields and nothing else:

```asm
cmp dword [rsi+8],3      ; alcohol      cmp [rsi+0x10], [rbp+0xC300]  ; food
cmp dword [rsi+8],4      ; church       cmp [rsi+0x10], [rbp+0xC310]  ; meat
cmp dword [rsi+8],5      ; culture      cmp [rsi+0x10], [rbp+0xC318]  ; clothes
cmp dword [rsi+8],6      ; sport        cmp [rsi+0x10], [rbp+0xC320]  ; electronics
cmp dword [rsi+8],0xA    ; hospital
```

`rsi` is the demand, `rbp` the world. The four resource comparisons are reached
from kinds 1 and 2, the two a shop serves; `game+0xC300`… are the base game's
four cached shop-goods records — the same four `needs` had to repair a vector
for in `0x198670`.

So **a demand that is not in the array when this function runs affects
nothing**, and one that is in the array everywhere else still routes the citizen
to the pub, the church and the shop, because the movement and shopping code runs
in other functions on other ticks. The demand half is therefore a pre/post
bracket and no patch at all:

1. scan the array; if nothing is locked, do nothing — this runs for every
   citizen on every tick;
2. otherwise copy all `n × 0x80` bytes, compact the locked entries out, write
   the shortened count to `person+0x110`;
3. call the original;
4. put the array and the count back byte for byte.

`savedCount` is set *before* the edit, so a fault mid-compaction still restores.
The evaluator's own scratch lives at `person+0x498`…`+0x4B8` — the eighth demand
slot, which the array never reaches — so the restore cannot tread on it.

**Removing the entry removes the bonus with it.** That is deliberate: it is what
"the early cinema changes nothing" means. There is no arrangement that keeps the
reward and drops the penalty, because the function attributes only the downward
half and computes both from the same demand.

### The status floats have to be held

`person+0xD8` is eleven floats in the script VM's order — happiness, food,
health, soviet, alcohol, culture, sport, religion, clothing, electronic, crime —
and it is the **planner** that decays them, not the evaluator. Left alone,
twenty years of locked culture would rot the culture status to zero and the whole
republic would turn miserable on the day the cinema starts counting. So a locked
need's own status is written back to `locked_status` after every evaluation.

Two are deliberately never written:

- **health** is the citizen's real health, fed by pollution, alcohol, water and
  cold as well. Pinning it at 1.0 would make everybody unkillable.
- **happiness** is the thing being protected, not a per-need status.

`food`'s status is satiety, so pinning it means nobody starves while food is
locked — which is exactly what the grace year is for.

**Alcohol is the one status that runs backwards, and 1.1 pinned it wrong.**
Every other status in the array reads "how content is this citizen with X" —
1.0 is the goal, and pinning it there is what keeps a locked need from feeling
punishing on the day it unlocks. `fStatusAlcohol` instead reads "how much has
this citizen been drinking", and it feeds the *health* ledger's own "Drinking
alcohol" reason directly (health reason list, above) — the same kind of entry
as *Starving* or *Pollution*. Pinning it to `locked_status` (1.0 by default)
told the health system every citizen was drinking at capacity for as long as
alcohol stayed locked: not a benign default, but a permanent, republic-wide
health penalty for a need that was supposed to not exist yet. 1.2 pins it to
0 unconditionally, ignoring `locked_status` — the one status where zero, not
the configured level, is what "does not exist yet" means.

### The nagging message

`person+0x4F0` is the unsatisfied-demand list the planner fills from whatever the
last cycle left over, and it is what raises *"N Citizen(s) were unable to get
X"*. `clear_unsatisfied` compacts locked needs out of it in the same pre-hook,
matching on the same kind and record the demand carries — the entries are
`{ float amount, int kind, Resource* }`, 16 bytes, capped at ten.

## The building side is twelve rates in `.rdata`

Electricity, water and interior temperature never reach a demand array. Three
building functions subtract from every inhabitant directly:

| Function | What |
|---|---|
| `0x1BC210` | the living tick — happiness: *No electricity*, *No water* |
| `0x488B80` | interior temperature — happiness **and** health |
| `0x1B0950` | drinking water — health |

Every one has the same shape. The amount is computed **once, before the loop over
the inhabitants**, scaled by the "Unsatisfied citizens reaction" setting at
`game+0x5C8` (×0.8 / ×1.0 / ×1.2), and then per person:

```asm
movss   xmm0,[rcx+0xD8]
subss   xmm0,<amount>
movss   [rcx+0xD8],xmm0
...
movss   xmm0,[rsi+0x59C]     ; once per calendar day only
ucomiss <amount*k>,0
je      skip                 ; <- zero means the statistics are not touched either
float[reason] += it ; count[reason]++
```

**An amount of zero is a complete no-op** — no subtraction, no clamp at zero, and
the counter is skipped, which is exactly what a need that does not exist yet
should look like in the statistics window.

Each amount is finalised by one rip-relative load of a `.rdata` constant, so the
whole patch is twelve rewritten displacements — no hook, no code cave:

| Site | Instruction | Constant | Value | Need |
|---|---|---|---|---|
| `0x1BC39B` | `mulsd xmm1,[rip]` | `0x909E38` | 0.000216667 (double) | electricity |
| `0x1BC575` | `mulsd xmm1,[rip]` | `0x909E60` | 0.000333333 (double) | electricity |
| `0x1BC74B` | `mulss xmm3,[rip]` | `0x909AF8` | 0.001 | water |
| `0x488BEA` | `movss xmm7,[rip]` | `0x909B18` | 0.0015 | heating, happiness |
| `0x488C1C` | `movss xmm7,[rip]` | `0x909B04` | 0.00117 | heating, happiness |
| `0x488C06` | `movss xmm8,[rip]` | `0x909B20` | 0.0017 | heating, health |
| `0x488C24` | `movss xmm8,[rip]` | `0x909B10` | 0.001326 | heating, health |
| `0x1B0B37` | `mulss xmm2,[rip]` | `0x909ABC` | 0.00025 | water, health |
| `0x1B0B44` | `mulss xmm0,[rip]` | `0x909AC4` | 0.0003125 | water, health |
| `0x1B1203` | `mulss xmm1,[rip]` | `0x909AA8` | 0.0001875 | water, health |
| `0x1B1213` | `mulss xmm1,[rip]` | `0x909ABC` | 0.00025 | water, health |
| `0x1B1223` | `mulss xmm1,[rip]` | `0x909AC4` | 0.0003125 | water, health |

Two per need where the game has two branches: heating splits warm/cold, and
electricity splits on how the shortfall is measured. `0x1B0B44` is a *bonus*
term for clean water rather than a penalty, and it is zeroed too — a need that
does not exist must not pay either way.

**Overwriting the constants in place is the obvious alternative and is wrong for
the usual reason:** `0x909ABC` and `0x909AC4` are each read by two different
blocks, and the literal pool is shared.

## The window counts separately, and that is a third face of the same need

Version 1.0 gated all of the above and the building's own window still said

```
-Building is without power supply
56 Citizen(s) currently without power
56 Citizen(s) are currently without drinking water
```

because none of those three lines reads the happiness ledger. They are drawn by
`0x761300`, the living building's panel, from state the simulation keeps for
itself.

**The two counted lines come from four floats the living tick snapshots** at the
top of every pass:

```
building+0x1104 -> +0x1108      building+0x110C -> +0x1110
building+0x1114 -> +0x1118      building+0x111C -> +0x1120
```

The panel takes `max(+0x1108, +0x1110, +0x1118)` for power (`cvttss2si` at
`0x761FA4`) and `+0x1120` for water (`0x7620A3`). **The tick never reads the
snapshots back** — it works from the accumulators, at `0x1BC3AB` and
`0x1BC793` — so a post-hook on `0x1BC210` that zeroes them is invisible to the
simulation and empties both lines. That is why the fix is one post-hook on the
tick rather than a patch on the panel: one place, and what the window says stays
consistent with what the building is doing.

**"Building is without power supply" is a query, not a counter.** The panel
calls `0x1BBFE0(game, building)` at `0x76147B` and tests the result with
`test al,al`. That query has twenty callers deep in the simulation, so hooking it
would change far more than a line of text; instead the panel's own `call rel32`
is rewritten to a 14-byte absolute jump in a cave, which forwards to the
original unless electricity is still locked. Every other caller is untouched.

## School and kindergarten, with separate years

Neither raises a happiness or a health reason, so neither is a need in the sense
above. They are the game's own **Education simulation** option (language ids
710 Simple / 711 / 712 Complex), whose help text is precisely the two behaviours
wanted:

> id 713 — children automatically reach basic education (no elementary schools
> needed for new born citizens); parents can work even while their children are
> under 6 years old (no kindergarten needed).

It lives at **`game+0x5DC`**, where **`0` is Complex and `1` is Simple**, and it
is written into the save header by `0x42CBD0` alongside the date.

Writing that field directly is not an option — it is saved with the world, and
the interface reads it — so the seven places the *simulation* tests it are
rewritten to `cmp dword ptr [rip+ours], imm8`. That is the same seven bytes in
the plain form and eight (seven plus a `nop`) with a SIB byte; **each site keeps
its own immediate**, because two of them compare against 1 rather than 0.

And because the sites divide cleanly, the two get separate years:

| Group | Site | What it does |
|---|---|---|
| `school` | `0x823AE9` | Simple → a newborn is given basic education outright |
| | `0x824116`, `0x824145` | Complex → education starts near zero and the child is enrolled |
| | `0x834D65` | Simple → an arriving citizen is given education |
| | `0x7618E7` | the panel's *"N Children can't go to school"* |
| `kindergarten` | `0x824AEB` | a parent of a child under six searching the walking connections for a place; failing, it writes `person+0x2E = 1` at `0x824C2E` and the parent stays home |
| | `0x761928` | the panel's *"N Workers can't work because all kindergartens…"* |

Five further readers — three in the statistics window (`0x8BB2A`, `0x8BDAE`,
`0x8BF8D`), one in the renderer (`0x326A50`) and one in the editor
(`0x382A8B`) — are deliberately left alone, and so is every place the build menu
looks, which is why a kindergarten stays buildable in a year that does not need
one yet.

Two things fell out of reading that code and are worth keeping:
**`person+0xA8` is `fEducation`**, the 0..3 level the script API declares, and
**`person+0xD4` is the age in years** — it is what `6.0f` is compared against at
`0x824B06`, and it is the field the panel averages into "Average age". That is
the field `plugins/aging` was written to go looking for.

## Settings

`plugins/easystart.ini`:

| Key | Default | What |
|---|---|---|
| `enabled` | 1 | |
| `demands` | 1 | the nine demand-carried needs |
| `utilities` | 1 | electricity, water, heating |
| `warnings` | 1 | the three lines the building's own window prints |
| `pin_status` | 1 | hold a locked need's status float |
| `locked_status` | 1.0 | at what level (alcohol ignores this and is always held at 0 — see above) |
| `clear_unsatisfied` | 1 | drop locked needs out of `person+0x4F0` too |
| `probe` | 0 | a line per year change, plus one full citizen dump |

`[unlock]` takes a year, `off` or `always` per need. The shipped schedule
assumes a republic founded in 1920 with the first year as a grace period:

```ini
[unlock]
food = 1921        religion = 1921
meat = 1927        clothes  = 1927
alcohol = 1932     sport    = 1932
culture = 1940     health   = 1940
electricity = 1945
water = 1950       heating  = 1950
electronics = 1955
school = 1932      kindergarten = 1945
```

`[unlock_resource]` does the same for a good `needs` added — `list` names them,
comma separated, and each takes a year. The record is matched by name, cached in
a 16-entry table keyed on the record pointer, because resolving it costs a
`VirtualQuery` and this runs per citizen per tick.

**A base-game start is 1960, 1970 or 1980**, at which point every line above is
already in the past and the plugin correctly does nothing. It is for a map that
starts early — the Early Start content, or a date wound back by other means.

## Testing it

**One thing here is confirmed in game and the rest is not.** Version 1.0 was
watched in a 1920s save: every locked need's status bar read 100 %, health read
85 % and happiness 73 % — the plugin's exact fingerprint, since those two are
deliberately never pinned. Alcohol's bar reading 100 % in that same session
*was* the bug 1.2 fixes: for every other need 100 % is the correct pinned
value, but for alcohol it meant the health ledger was crediting every citizen
with drinking at capacity the whole time it was locked. What that session also
showed is what 1.1 is for: the building's own window still printed *Building
is without power supply*, *56 Citizen(s) currently without power*,
*56 Citizen(s) are currently without drinking water*, *13 Children can't go to
school* and *8 Workers can't work because all kindergartens…*.

**Two things from 1.1/1.2 are now confirmed, from a `probe` session that ran a
map from 1920 across the 1921 food-unlock boundary:**

- **`SweepExistingBuildings`'s building-type check was reading the wrong
  struct**, and is fixed. It compared `*(int*)(building + 0x360)` against
  `BUILDINGTYPE_LIVING`, and that field lives on the building's *type
  descriptor* (`*(building + 0x318)`), not on the building itself — the same
  offset `depletion.cpp` and `accumulator.cpp` already read correctly, one
  indirection deeper. The tell was `sweep: 0 of 668 building(s) are living
  quarters` on two separate sessions — impossible on a populated map if the
  check were right. After the fix the same map reads `571 of 670`. See
  [07-pitfalls.md](07-pitfalls.md), *A building type lives on its type
  descriptor, not on the building*. This only ever affected the one-time
  catch-up for a living building's power/water warning lines, never a
  citizen's demand list — building type has nothing to do with `easystart`'s
  own gating, which reads year and locked-need state only.
- **A pre-existing citizen does eventually get a real vanilla food demand**,
  confirmed by name rather than inferred: `[0]kind1/need0` (and once, at the
  same moment, `kind1/need1` meat and `kind1/need3` electronics alongside it)
  showed up for two of the ten watched citizens, each right as their own
  `fStatusFood` moved off its pinned `1.0` for the first time in the session
  (`raw bits 3F800000` → `3F79BB18`). Two shorter sessions before this one,
  each watching the same ten citizens for under a minute past the unlock
  year, caught zero — not because food was gated, but because the planner
  that actually appends a food demand (`0x836B40`) runs on a per-citizen
  "ready to decide" cadence, not every tick the way the evaluator does, and
  ten citizens out of six-plus-thousand is a thin sample for a short window.
  **Not yet watched: the happiness panel itself**, which is the test that
  does not depend on which ten citizens happened to get sampled.

1. Tick `easystart` in the launcher. Set `probe = 1`. Load a save whose year is
   **before** some of the unlock years — winding the date back is the whole
   premise, so a 1970 save proves nothing.
2. The log should carry, at startup, twelve `rate ... now reads ...` lines,
   seven `edu ...` lines, and
   `ready: demand gate on, living counters on, 12 of 12 utility rate(s)`.
   A `refusing` line means the game moved and that one site is left vanilla —
   which after a game update is exactly what should happen.
3. Then, on the first tick, one line per year:
   `easystart  year 1922, still to come: meat clothes electronics alcohol ...`
   If that line never appears the hook is installed but the evaluator is not
   being reached; if it says a year you did not expect, `game+0x594` is not what
   the corner of the screen shows and everything else is moot.
4. The `probe` dump names the first citizen whose list was shortened:
   `4 demand(s), 2 hidden, 2 left for the evaluator`, then a line per demand
   with its kind. Kinds 3/4/5/6/10 are alcohol, church, culture, sport, doctor;
   kinds 1 and 2 are the shop goods. **Two hidden out of four with nothing but
   food and meat in the list is the wrong answer** and means the resource
   comparison missed.
5. **Open a residential building's own window**, which is where 1.0 fell down.
   With `electricity` and `water` locked there must be no *Building is without
   power supply*, no *N Citizen(s) currently without power* and no *N Citizen(s)
   are currently without drinking water*. With `school` and `kindergarten`
   locked there must be no *Children can't go to school* and no *Workers can't
   work because all kindergartens…*, and the "Workers without job" count should
   drop by however many parents were being held at home.
6. **Check a kindergarten and a school are still buildable** in a locked year.
   That is the whole reason the option is repointed per site instead of being
   written, and it is the one claim resting on the site list rather than on
   observation.
7. **The point of the whole plugin** — open the statistics window's *Happiness
   change* panel. Every locked need's line must read zero. A non-zero
   "Unable to enjoy culture" while `culture` is locked means the bracket is not
   taking effect; a non-zero "No water" while `water` is locked means a rate was
   refused or the slot is not being written.
8. **The alcohol fix, specifically** — with `probe = 1`, read a watched
   citizen's `alcohol %.2f` figure while alcohol is locked: it must read 0.00,
   not `locked_status`. It should also show 0.00 in the health ledger's
   "Drinking alcohol" line whether or not the pub is built, since 1.2 pins the
   status to 0 unconditionally rather than following `locked_status`.
9. Watch a citizen use a service that is built but locked — a pub in 1925. They
   should still go in, and the *Unable to drink alcohol* line should stay at
   zero either way.
10. Turn a year: play across an unlock boundary (or edit the ini to one year
    ahead) and check the locked status did not arrive at zero. The culture
    status in the citizen window should be at `locked_status`, not at the
    floor — except alcohol, which should still read 0.00 right up to its own
    unlock year.
11. Check the control: set every `[unlock]` key to `always` and confirm the
    game is exactly as it was. Nothing is patched away in that state — the
    rates hold their vanilla values and the demand bracket short-circuits on
    `g_anyLocked`.

## What this does not do

- **A locked good is still bought.** Shops still stock and sell food, meat,
  clothes and electronics while those needs are locked, because the sale happens
  in `0x171DA0` and is nothing to do with the ledger. Only the consequences are
  gated. Stopping the purchase would mean editing the demand for real, and then
  the citizen would stop visiting the shop, which is the opposite of the point.
- **No status float of its own for a `needs` good.** `[unlock_resource]` gates
  the ledger entry, but a cloned demand had no status float to begin with — see
  [11-needs.md](11-needs.md).
- **Crime and pollution are left alone.** Both are real reasons in both ledgers
  and both are gateable by the same means, but they are consequences of the town
  rather than needs of a citizen.
- **Loyalty is not need-derived.** *Low government loyalty* is a reason for
  happiness falling, not the other way round; loyalty itself comes from
  monuments, broadcasts, personal cars and the secret police.
