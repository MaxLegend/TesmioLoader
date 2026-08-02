# Needs — a fifth thing the citizens buy

The base game gives a citizen four goods to buy in a shop: **food**, **meat**,
**clothes** and **electronics**. There is no table of them. They are four
straight-line blocks of code inside one 15 KB function, each with the resource
name spelled out as a literal.

There is, however, a *shape*. A citizen carries an array of **demands**; each
demand points at an ordinary resource record; and the shop's tick is entirely
generic over that array. Nothing in the selling path knows what food is. So a
fifth demand pointing at a fifth resource is sold by the same code that sells
the other four — **with no code patch anywhere**, only two post-hooks.

That is the `needs` plugin. `plugins/needs.ini`:

```ini
[list]
furniture = eletronics, 1.0, advanced
```

and department stores start ordering furniture, trucks deliver it, and citizens
buy it on the same trip.

## How a citizen wants something

### The person

`operator new(0x750)` in the constructor at rva `0x823290`, kept in a global
`Person*` array at `0x9E75B8`…`0x9E75C0`.

| Offset | Contents |
|---|---|
| `+0x0D8` | eleven status floats, in the script VM's own order: happiness, food, health, soviet, alcohol, culture, sport, religion, clothing, electronic, crime |
| `+0x0C8` | non-zero suppresses every service demand — a foreign worker or tourist |
| `+0x110` | **demand count** |
| `+0x118` | **demand array**, stride `0x80`, capacity **7** |
| `+0x4F0` | unsatisfied-demand count, then ten 16-byte entries at `+0x4F8`: `{ float amount, int kind, Resource* }` |
| `+0x71C` | 0 citizen, 1 soviet tourist, 2 western tourist |
| `+0x734` | money this person has spent, in the currency `+0x71C` selects |

The status block was found by scanning the planner for `movss [rbx+disp]` and
finding ten consecutive float offsets; the constructor at `0x823290`
randomises all of them in one run, which is what pins the order to the VM's.

### One demand, `0x80` bytes

| Offset | Contents |
|---|---|
| `+0x00` | float — amount still wanted |
| `+0x04` | float — amount wanted in total |
| `+0x08` | int **kind**. `0xF` while the entry is being built; **1 and 2 are the two the shop tick serves** |
| `+0x10` | `Resource*` |
| `+0x18` | `Target[0]`, `0x34` bytes — where the demand can be satisfied |
| `+0x4C` | `Target[1]` |

`0x18 + 2 * 0x34 == 0x80`, which is what settles the sub-structure: the
initialisation loop writes `0xE` to both `+0x18` and `+0x4C`, food writes `1`
to both and electronics writes `9`, and every other field the two sites touch
falls at the same offset inside one or the other.

**Kind is urgency, and it is load-bearing.** A food demand is kind 1, and the
planner clears all four "wants a service" flags the moment it finds a demand of
kind ≠ 2 for food or meat. A hungry citizen therefore carries no service
demands at all, and that is what keeps the list inside its seven slots.

### The planner

```
rva 0x836960   FUN_140836960(game, person)
```

Reached from `0x830640` when a person is at home and ready to decide what to do
with the day. It:

1. walks the old list and copies everything with a non-zero amount into the
   unsatisfied list at `+0x4F0`, capped at ten — this is what raises
   *"N Citizen(s) were unable to get X"*, string id 2306, whose `%ls` is the
   resource's own caption;
2. decays the status floats;
3. writes `person+0x110 = 0` at `0x836F8B` and **rebuilds the list from
   scratch** through nine conditional append sites.

The nine, in order: food (`0x837757`), meat (`0x837F1B`), clothes
(`0x838573`), electronics (`0x838C71`), then five service demands
(`0x8392BD`, `0x8396A6`, `0x839A1A`, `0x839D8A`, `0x83A1D3`) that carry no
resource. Each one is the same eleven instructions:

```asm
movsxd rax,[rbx+0x110]      ; n
mov    rdi,rax
shl    rdi,7                ; n * 0x80
inc    eax
mov    [rbx+0x110],eax      ; n + 1
...
call   ResourceGet          ; "food" / "meat" / "clothes" / "eletronics"
mov    [rdi+rbx+0x128],rax
```

**None of them bounds-checks.** The ceiling is arithmetic, not a test.

### The seven-slot ceiling

The array begins at `+0x118` and the unsatisfied-demand count sits at `+0x4F0`.
`0x4F0 - 0x118 = 0x3D8`, and `0x3D8 / 0x80 = 7`. An eighth entry would start at
`+0x498` and run over the count.

Seven is also exactly what the game can produce, and the two facts are the same
fact:

| Case | Demands |
|---|---|
| hungry — a food demand of kind 1 exists | food, meat, clothes, electronics, services suppressed → **4** |
| fed | clothes, electronics + five services → **7** |

So `max_demands` in `needs.ini` is a **hard ceiling clamped to 7 in code**, not
a preference. When the list is full the plugin either skips the citizen for
that cycle (`when_full = skip`, the default) or takes the donor's own entry
(`when_full = replace`), so the citizen shops for one of the two goods today
and the other tomorrow.

Skipping costs less than it sounds: the common case is the hungry one, at four.

### The sale

```
rva 0x171DA0   FUN_140171da0(game, building)      building type 3, the shop
```

Reached from the building dispatcher at `0x13DE28`. Its second half is the
whole of shopping, and it is generic:

```c
for (customer in building+0xBD8 .. +0xBE0)          // people inside
  for (d = 0; d < customer[0x110]; d++)             // their demands
    if (demand.kind == 1 || demand.kind == 2)
      for (storage in building+0x970, stride 0xE0)
        for (slot in storage, stride 0x10)
          if (slot.resource == demand.resource && slot.content > 0)
              move min(slot.content, demand.total * dt) across
```

and the tourist branch charges `resource+0x64` (RUB) or `resource+0x60` (USD)
per unit. **Nothing here has to be touched.** It already sells anything a
demand points at.

## How a shop comes to stock it

```
rva 0xE40F0   FUN_1400e40f0(parser, storage, class, capacity, resource)
```

Builds one storage's slot list while `building.ini` is parsed — so it runs at
load, four call sites, once per storage per building type, and never again.

`storage+0x88` records which `$STORAGE_*` produced it: `-1` plain, 0 basic, 1
medium, 2 advanced/mediumadvanced, 3 hotel, 10 carplant, 50 fuel. The two
shapes that matter:

| Token | Slots it pushes |
|---|---|
| `$STORAGE_DEMAND_BASIC` | `food` (covered), `meat` (cooler) |
| `$STORAGE_DEMAND_MEDIUM` | `food`, `clothes` |
| `$STORAGE_DEMAND_ADVANCED` | `food`, `clothes`, `eletronics` |
| `$STORAGE_DEMAND_MEDIUMADVANCED` | `clothes`, `eletronics` |
| `$STORAGE_DEMAND_HOTEL` | `food`, `alcohol`, `meat` |
| `$STORAGE`, `$STORAGE_IMPORT`, … | every resource of the class, in a loop |

Each slot is `{ Resource*, float factor * capacity, float }`, pushed through
the 16-byte `std::vector::push_back` at `0xB14E0`, where the factor is
`resource + 0xCC + class * 0x20` — **the same field that makes a storage
declared with the wrong transport class report `0.00 of 0.00 t`**. A companion
byte at `+0xE8 + class*0x20` excludes the pair outright.

## What the plugin does

Two post-hooks, no code patch, no vtable, no import swap.

| Hook | Site | What it does |
|---|---|---|
| citizen demand plan | `0x836960`, 20 bytes stolen | after the game has rebuilt the list, appends one cloned entry per declared need |
| storage slot list | `0xE40F0`, 19 bytes stolen | after the game has filled a building **type's** storage, appends one slot per declared need |
| shop tick | `0x171DA0`, 21 bytes stolen | the same rule against a **built** shop's own storages, once per building |
| building type loader | `0x11D810`, 23 bytes stolen | the same rule against every building **type**, so the build menu is right before anything is built |

The third one exists because the second is not enough. `0xE40F0` teaches the
building *type*, which reaches a shop the game has yet to construct — and only
if the type was still being parsed when the resource existed. A shop standing
in a save made before the need was declared has a storage the type no longer
describes, and no amount of re-parsing gets to it. The shop tick hands over one
building of type 3 per call and its storages are an ordinary vector at
`building+0x970`, stride `0xE0`, so the same rule applies directly. Each
building is remembered after the first pass; the tick runs many times a second.

**The two floats in a slot mean different things on the two sides.** A type
description carries one number, in `+0x08`, and that is what the parser writes.
A built storage splits them:

| Slot field | In a type description | On a built building |
|---|---|---|
| `+0x08` | factor × capacity | **content** — what is on the shelf |
| `+0x0C` | 0 | **limit** — what the shop wants there |

`+0x0C` is *not* the quality field here. For an electric storage it is the
node's voltage, which is where the name in [02-findings.md](02-findings.md)
comes from, but a goods storage uses it as the target level: the shop tick's
first half reads

```c
if (slot[+0x08] < slot[+0x0C] + c)   ->   order a delivery
```

and the building panel prints `"%.2f of %.2f %s - %.0f%%"` from the pair.
Setting only the content and leaving the limit at zero is what made furniture
show a nonsense percentage in the shop window: the shop had nothing, wanted
nothing, and `0/0` came out of the divide.

**`0xE40F0` has never once fired**, and that is worth writing down. The
`building.ini` parser at `0x10E200` carries its own copy of the whole storage
logic — the `$STORAGE_DEMAND_*` `strcmp` chain is at `0x117BAE` and the
per-class factor multiply is `mulss xmm0,[rax+rbx+0xCC]` at `0x117B91`, which
is also the exact instruction that faults when a `$TOKEN` in a comment sends it
a null resource. `0xE40F0` belongs to some other parser. The hook on it is
harmless and is kept for the day something routes through it, but **the shop
tick is what actually stocks the shops.**

## Where the numbers in the shop window come from

Two of the three figures are not in the built building at all.

**The per-resource maximum comes from the building type.** A small shopping
centre declares 25 t; the preview reads `12t of Food` and `8.8t of Electronics`
from its type's storage slots, which is `25 × 0.5` and `25 × 0.35` — the same
`factor * capacity` the parser computes at `0x117B91`. Without a slot there
furniture had no maximum, and the panel printed `0.58 of -0.00 tons`.

**The "Limit amount" percentage comes from a second array.** A storage carries
*two* vectors of the same length, both 16 bytes per slot:

```
+0x00  begin=...73670  end=...736A0  cap=...736A0     0x30 = 3 x 0x10
+0x18  begin=...73D70  end=...73DA0  cap=...73DA0     0x30 = 3 x 0x10
```

`cap == end` in both, so appending to either reallocates. Growing only the
slots left the fourth entry of the second array reading past its end, and the
percentage printed as `-2147483648` — INT_MIN, a NaN cast to int. **Both grow
together, and the new entry is cloned from the donor's** exactly as the slot is.

So the plugin writes in three places, and each answers a different part of the
interface:

| Written | What it fixes |
|---|---|
| the type's storage slots | the build-menu preview, and the maximum every panel divides by |
| the built building's slots | what is actually on the shelf and for sale |
| the built building's second array | the per-resource limit slider |

**And the slot is never computed, only cloned.** Two guesses at what the second
float in a slot means were both wrong — with a 70 t storage food and clothes
hold 2.333 and eletronics 1.633, which is `capacity * factor / 15` and neither
a content nor a limit. Copying the donor's slot whole and changing only the
resource pointer is right by construction whatever the field turns out to be,
and it scales itself, because the donor's slot was built against this very
storage.

## Reaching every type, not only the built ones

Going through a built building leaves the build menu empty until the first shop
of that type exists. The types are all in one place:

```
rva 0x11D810   FUN_14011d810   "Initializing vanilla buildling types"
```

It walks `media_soviet/buildings_types/*.ini` and the Workshop, parses each,
and fills the **`vector<TYPE*>` at `0x9E6A30`** — a vector of pointers, stride
8, which is `mov rax,[rdx] / mov rcx,[rax + r15*8]` at `0x11DD50` inside that
same function. A post-hook walks it and reaches every type the game knows.

The ordering works out: that function runs after `Initializing resources`, so a
mod resource is already in the vector by the time its slot is appended.

Both prologues are compared against the site before anything is written, so a
game update makes the hook refuse rather than corrupt the process.

The type's storage vector itself is found by scanning the descriptor (at
`building+0x318`, stride `0xBE8`) for a `{begin, end, cap}` triple whose span
divides by the storage stride `0xE0`. That shape alone is not proof — the
descriptor is long enough that three pointers can satisfy it by accident — so a
candidate is accepted only when every storage in it also passes the one test a
lookalike cannot fake: a readable slot vector, a whole number of 16-byte slots,
and every slot's resource pointer landing inside the engine's resource vector
on its own 832-byte stride. A fault anywhere in the pass disables it for the
session (`g_doTypes`, logged) rather than re-firing on every later type load.

### One key drives both halves

```ini
[list]
furniture = eletronics, 1.0, advanced
```

`eletronics` is the **donor**, and it is not a category — it is the answer to
both questions at once:

- **the demand** is a `memcpy` of the citizen's own electronics demand, all
  `0x80` bytes, followed by three writes: the resource pointer and the two
  amounts. Everything else in the entry is routing, and it is already correct
  for a shop that sells electronics.
- **the storage** rule is the one that reads the same for every `$STORAGE_*`
  token: *if the donor ended up in this storage and we did not, put us there
  too.* A department store matches. A warehouse that already stocks every
  covered resource fails the second half and is left alone.

Cloning rather than building is deliberate. A synthesised entry would need both
`Target` sub-structures right, and those are the least understood part of the
layout; a cloned one is right by construction.

### And the third field narrows it

The donor implies a category; it does not always imply the *right* one.
`eletronics` lands in `$STORAGE_DEMAND_ADVANCED` **and**
`$STORAGE_DEMAND_MEDIUMADVANCED` — the big department stores and the small
shops. Putting furniture only in the big ones is a thing the donor alone
cannot say, so there is a third field:

```ini
furniture = eletronics, 1.0, advanced
```

The storage builder writes its own `$STORAGE_*` token number into
**`storage+0x94`** — `mov [rdi+0x94],eax` at `0xE4260`, straight from
`parser+0x790`. **A built building does not keep it.** Every live storage reads
back 0, the value for a plain `$STORAGE`, so a department store's shelf and a
warehouse are indistinguishable by that field. Probing a real save is what
settled it:

```
needs probe live storage token 0 class 0 cap 70.00 slots 3 donor@2 mine@-1
```

`cap 70.00` with three slots is exactly `$STORAGE_DEMAND_ADVANCED
RESOURCE_TRANSPORT_COVERED 70` from `shop_prior.ini` — and the token is 0. That
one line is why furniture never reached a shop: the `advanced` filter rejected
every storage it was meant to accept.

What survives is the **composition**, and it is unique per category, because
`0xE40F0` puts a fixed set of names in a COVERED demand storage:

| Category | COVERED slots |
|---|---|
| `basic` | food |
| `medium` | food, clothes |
| `advanced` | food, clothes, eletronics |
| `mediumadvanced` | clothes, eletronics |
| `hotel` | food, alcohol |

No set is a subset of another, so one match is the answer, and the plugin
recovers the category that way whenever the token is gone. Only the COVERED
side can be told apart: every demand storage of class COOLER gets `meat` and
nothing else whatever token made it — one `if` at the end of `0xE40F0` — so a
cooler reports "cannot tell" and the donor rule alone decides. **A filter that
cannot tell never rejects.**

| `category` | Token | `$STORAGE_*` | What the base game puts there |
|---|---|---|---|
| `auto`, `any` | — | any | wherever the donor landed. The default |
| `none` | — | none | **nowhere.** A `building.ini` declares the shelf itself |
| `basic` | 2 | `_DEMAND_BASIC` | food (covered), meat (cooler) |
| `medium` | 3 | `_DEMAND_MEDIUM` | food, clothes |
| `advanced` | 4 | `_DEMAND_ADVANCED` | food, clothes, eletronics |
| `mediumadvanced` | 5 | `_DEMAND_MEDIUMADVANCED` | clothes, eletronics |
| `hotel` | 10 | `_DEMAND_HOTEL` | food, alcohol, meat |
| `plain` | 0 | `$STORAGE` | every resource of the class |
| a number | — | — | for the ones the base game barely uses, `_DEMAND_PRISON` among them |

It only ever narrows: a category never puts the goods somewhere the donor is
not. Naming one whose shops do not sell the donor is allowed and logged as a
`WARN` — the goods would sit where nobody walks for them, because it is the
donor, not the category, that decides where the *demand* sends the citizen.

### `none`, and the shop that sells one thing

`none` narrows it all the way to nothing: **no storage anywhere gets a slot**,
because a `building.ini` already declares the shelf.

That is the case the donor rule cannot express at all. A good meant to be sold
in one building of its own — medicine in a pharmacy — would otherwise land in
every department store in the republic, since that is where its donor is. The
building declares

```ini
$STORAGE_SPECIAL RESOURCE_TRANSPORT_COVERED 8 medicine
```

and nothing else has to happen, because the sale at `0x171DA0` never asks which
`$STORAGE_*` token built a storage: it walks **every** storage of a `$TYPE_SHOP`
building and moves anything whose resource a customer is asking for. The pub
does the same with alcohol.

**The other half of the plugin is untouched by `none`.** The citizen still gets
the demand, cloned from the donor exactly as before — the donor is what the
*demand* is cloned from, and that is a separate question from where the goods
sit. See [13-buildings.md](13-buildings.md), which also says plainly what has
not been established: whether the citizen's target search will actually pick a
shop that stocks only a modded good.

Plain warehouses need no help either way: they stock every resource of the
class already, so the "and we are not in it" half of the rule skips them.

### Why not synthesise, why not patch

The obvious alternative — splice a fifth block into `0x836960` — was never
worth considering. The function is 15 KB of straight-line code with no spare
room, the four blocks differ in every constant they use, and a patch would have
to be re-derived after every game update. The post-hook is nine lines and
survives anything short of the demand array moving.

## Settings

`plugins/needs.ini`, one file for the whole feature, the way `resources.ini`
and `deposits.ini` are.

| Key | Default | What |
|---|---|---|
| `enabled` | 1 | 0 unloads the plugin |
| `[list]` third field | `auto` | which `$STORAGE_DEMAND_*` stocks it, above |
| `demand` | 1 | append the need to citizens |
| `storage` | 1 | add the resource to the storages that stock the donor |
| `max_demands` | 7 | clamped to 7 in code, see above |
| `when_full` | `skip` | or `replace` |
| `probe` | 0 | dump one citizen's whole demand list and status block to the log, once |
| `log_seconds` | 60 | between the "N demand(s) added" lines |

`probe = 1` is the fastest way to see what the planner actually produced:

```
needs    probe  person 0000021F...: 4 demand(s), tourist flag 0
needs    probe    [0] kind 1   food               0.00150 of 0.00150  target 1/1
needs    probe    [1] kind 2   meat               0.00061 of 0.00061  target 1/1
needs    probe    [2] kind 2   clothes            0.00024 of 0.00024  target 9/9
needs    probe    [3] kind 2   eletronics         0.00012 of 0.00012  target 9/9
needs    probe    status  happy 0.71 food 0.55 health 0.83 ...
```

## Furniture, end to end

The one need this ships with, and the whole of it:

**`plugins/resources.ini`**

```ini
[list]
furniture = eletronics, Furniture
```

Cloning from `eletronics` is what gives furniture
`RESOURCE_TRANSPORT_COVERED` — which is what a department store's
`$STORAGE_DEMAND_ADVANCED RESOURCE_TRANSPORT_COVERED` line can hold, and
without it the storage would report `0.00 of 0.00 t`.

**`plugins/needs.ini`**

```ini
[list]
furniture = eletronics, 1.0, advanced
```

**A factory to make it**, `media_soviet/workshop_wip/9100000006`, cloned from
the base game's clothing factory:

```ini
$PRODUCTION furniture 0.012
$CONSUMPTION boards 0.020
$CONSUMPTION fabric 0.008
$STORAGE_IMPORT_SPECIAL RESOURCE_TRANSPORT_OPEN 40 boards
$STORAGE_IMPORT_SPECIAL RESOURCE_TRANSPORT_COVERED 20 fabric
$STORAGE_EXPORT RESOURCE_TRANSPORT_COVERED 20
```

Two import storages because the two inputs are different transport classes —
see [06-building-mods.md](06-building-mods.md).

**One asset**, and only one:

```
tesmioloader/vfs/media_soviet/resources/furniture.png     48x48 RGBA
```

`eletronics` has **no cargo geometry in the base game** — no `.nmf`, no `.mtl`,
no `.dds`, only `eletronics.png`. So does `clothes`, so does `food`: everything
that travels covered is drawn as a box on a truck and never as a pile, and its
record's five mesh slots are all null. A clone of one of those is complete the
moment it has an icon, and the `resources` plugin now says so in the log
instead of warning about meshes it was never going to load:

```
resource  "furniture" has no cargo geometry, like its template -
          media_soviet/resources/furniture.png is the only asset it needs
resource  WARN  "furniture" has no icon - put a 48x48 RGBA PNG at
          media_soviet/resources/furniture.png (tesmioloader\vfs\...)
```

The second line goes away when the file is there.

## Happiness, without a status float

Each vanilla need has one of the eleven floats at `person+0xD8` behind it, and a
cloned demand has none — all eleven are spoken for. But the *mechanism* the game
uses is not the float, it is a plain subtraction, at `0x836DA9` in the planner:

```asm
movss xmm0,[rbx+0xD8]     ; happiness
subss xmm0,xmm1           ; a penalty sized by how far the status has fallen
movss [rbx+0xD8],xmm0
jbe   ...                 ; and clamped at zero
```

The penalty is picked from two thresholds — `0.035` per cycle below the lower
one, less above it — and then `[0x9E5978] += penalty` with `[0x9E59D4]++`, the
global "happiness lost and why" counters.

So the plugin does the same subtraction, sized by `unhappiness` in the ini
rather than by a status. What stands in for the status is the planner's own
record of failure: **the resource being in the citizen's unsatisfied list at
`person+0x4F0`**, which the prologue has just filled from whatever the last
cycle left over. That list means precisely "went shopping for it and came back
without it", which is the thing a status float would have been measuring.

Ordering matters and is deliberate: the penalty is applied in the post-hook
*before* anything is appended, while `+0x4F0` still describes the cycle that
just ended.

## Frequency, without the donor

Cloning the donor's entry ties the new need to it: a citizen wants furniture
exactly when they want electronics, and never otherwise. Breaking that needed
one small thing — **keep the entry**.

The first time a real donor demand is seen on any citizen, its whole `0x80`
bytes are cached. From then on a citizen who is not asking for the donor today
can still be stamped with it, and `chance` decides how often. The cached entry
carries the amounts of whichever citizen it came off — their age and family —
so `factor` remains what sets the size; everything else in it is routing, which
is identical for everyone shopping for the same goods.

Synthesising an entry from nothing was the alternative and was rejected. The
`0x80` bytes are two `0x34`-byte `Target` sub-structures plus the header, and
only some of their fields are understood: `Target[0]+0x00` is the kind of place
that satisfies the demand — 1 for the grocery goods, 8 for clothes, 9 for
electronics, `0xE` for "none" — and the rest are floats whose meaning is not
established. A cached real entry is right in all of them by construction.

## The seven-demand ceiling cannot be raised

It is worth being precise about why, because it looks like a constant that
could just be changed.

The array is **inline in the Person object**, from `+0x118`, and the very next
field is the unsatisfied count at `+0x4F0`. Raising the ceiling means moving
everything after the array, and everything after the array includes:

- `+0x4F0`…`+0x598` the unsatisfied list
- `+0x5A0` **a `C3D_NODE`, constructed in place and handed to the engine** —
  `C3D_NODE::C3D_NODE((C3D_NODE*)(person + 0x5A0))` in the constructor at
  `0x823290`. `C3DDLL64.dll` holds pointers into the middle of the object.
- `+0x71C` the tourist flag, `+0x734` money spent, `+0x73D`, and the rest up to
  the `0x750` the allocation is sized at.

Every access to those is a separate displacement compiled into the executable,
and they are everywhere. Moving the array instead of the fields after it would
mean redirecting every read of `person+0x118`; scanning `.text` for the two
instructions that walk it — `shl reg,7` and `sub reg,-0x80` — finds 67 and 195
sites respectively. Not all belong to this array, but each one would have to be
read to find out, and the ones that do include paths this project has never
looked at: the pub, the church, the sports hall, the doctor.

And the save format carries the Person object.

**What actually helps instead** is that the ceiling is rarely the binding
constraint. A citizen with an urgent food demand carries no service demands at
all and sits at four; probing a live save showed 0, 2 and 4. When the list *is*
full, `when_full = replace` hands the slot over for that cycle. Lowering
`chance` also lowers the pressure, at the cost of the need appearing less often.

## The purchase that had nowhere to be counted

The first citizen to actually buy furniture took the game down, and the cause
was not in this plugin at all. `FUN_140198670` folds a purchase into one of
five running totals, choosing by comparing the resource against four cached
records at `game+0xC300`, `+0xC310`, `+0xC318` and `+0xC320` — the base game's
four shop goods — and falling through to `game+0x12780` for anything else.

That fifth bucket is unreachable in a stock game, so its vector was never
constructed: `begin` null, `end` not. `0x198859` loads the null begin and
`0x198868` reads through it.

The plugin normalises it on every shop tick — a vector with a null `begin` and
a non-null `end` is not a state a live vector can be in, so setting `end` and
`cap` to null is safe and makes the guard `(end - begin) >> 4` do the right
thing. Every tick rather than once, because a world load rebuilds the game
object. Only two functions in the executable touch that bucket, `0x194260` and
`0x198670`, and repairing the vector covers both.

This is the general shape of what a new resource does to this game: it does not
only add a resource, it makes reachable every `else` that the fixed vanilla set
kept dead. See [07-pitfalls.md](07-pitfalls.md).

## What is not done

**No status float of its own**, still. `unhappiness` subtracts from happiness
directly and that is enough to make the need matter, but the citizen's window
has no bar for it and nothing else in the game reads it. A real status would
need one of the eleven floats, and all eleven are taken.

**`unhappiness` and `chance` have not been watched in game.** The rest has:
shops stock furniture, the preview and the maximum are right, citizens carry
the demand and buy it. These two are newer than that.

**Saves.** A need adds a slot to every shop storage that stocks its donor, and
a storage's slot list is part of the save. Treat it exactly as
`plugins/resources.ini`: test on a copy.

## Two things that look like this plugin and are not

**A name in `[list]` that no resource answers to** does nothing at all: the
plugin logs one `no resource named "x"` line and skips every storage and every
citizen. It never invents a record. If the log is silent past the install
lines, that is what happened — declare the resource in
`plugins/resources.ini` first.

**A resource declared with no assets crashes the game**, and the crash looks
nothing like a resource problem: it lands in `C3D_MESH::Render` on the first
frame after a world loads, twenty seconds after the real fault, which the
`resources` plugin caught and logged as a single `cargo mesh load faulted`
line. That is the `resources` plugin's business, not this one's, and it is
fixed there — but if a new need ever coincides with a new resource, read
[07-pitfalls.md](07-pitfalls.md) before suspecting the need.

**Edit `plugins/needs/needs.ini`, not `build/plugins/needs.ini`.** `build.bat`
copies the former over the latter on every build, exactly as it does for every
other plugin.
