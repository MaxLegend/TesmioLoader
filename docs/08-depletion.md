# Deposit depletion

**A plugin**, not part of the loader: `plugins/depletion/depletion.cpp` builds to
`build/plugins/depletion.dll`, and deleting that DLL removes the feature
entirely. See [09-plugins.md](09-plugins.md) for the mechanism.

In the base game a deposit is infinite. A mine samples the map once when it is
built, averages what it found into "quality of source", and produces at that
rate for the rest of the game. Nothing ever writes the richness back down.

This makes it finite with **one inline hook and no code patch** — every part it
needs already exists in the executable and only had to be joined up.

> **Maps past the engine's two.** A deposit on `resourcemap3` or above is
> drained exactly like one on the game's own pair — same bracket, same texel
> arithmetic — but its texture is not at an offset in the game object, so it
> comes from `TsmDepositApi::texture` instead. `MAP_EXTRA_FIRST` in
> `depletion.cpp` is where that numbering starts, and `g_extraSvc` is the one
> registry index per map that reaches the accessor. See
> [05-deposits.md](05-deposits.md).

It is on when the plugin is present (`enabled = 1` in `plugins/depletion.ini`).
Worth knowing before playing a save you care about: it changes game balance, and
once a map has been mined the change is in the terrain's own `resourcemap`,
which the world writer saves. Removing the plugin afterwards does not put it
back.

## How the game runs a mine

`FUN_1401b3690` at rva `0x1B3690` is one mine, one tick. The building
dispatcher at `0x139A80` calls it for every building whose **type is 7**. Its
twin `0x1B1220` — reached through `0x188FC0` — is the type-92 water well, which
already resamples itself and is left alone.

Everything the mine keeps lives in the building object:

| Offset | Contents |
|---|---|
| `+0xDDC` | production rate this tick. Recomputed on every call as `workers × quality × max`, then scaled again by shift count, power, and how much room the storage had. **Zero whenever the mine did not actually produce.** |
| `+0xDE0` / `+0xDE4` | the rolling per-second figure the UI shows, and its accumulator |
| `+0xDE8` | the timer that rolls `+0xDE4` over, once per in-game second |
| `+0xDF8` | quality of source, 0..1. Written once at construction; for an ore mine, never again |
| `+0xE90` | `std::vector` of the sample points the scan produced — `begin`, then `end` at `+0xE98` |
| `+0xFC0` | the water re-scan timer. Untouched for an ore mine |

### The sample points

`FUN_1401dd190` at rva `0x1DD190` fills that vector. Stride **28 bytes**
(`0x1C`):

| Offset | Field |
|---|---|
| `+0x00` | world x |
| `+0x04` | world y, the terrain height at that point |
| `+0x08` | world z |
| `+0x0C` | richness, the sampled colour component |
| `+0x10` | weight, `1 - distance / radius` |
| `+0x14` | per-point progress. Used only by wood; zero for everything else |
| `+0x18` | distance from the building |

It walks a square of ±radius around the building in steps of
`[0x90A840]` = **10** world units, keeps the points inside the circle, skips
anything closer to another mine of the same type (`radius × 0.7`), and pushes one
record each. An ore mine's radius is `[0x90AD50]` = **210**, so a full footprint
is about 1385 points.

Quality is then

```c
quality = sum(w * r) / sum(w)
```

over that vector, which is exactly what both tick functions compute for a water
well and what `0x2BAD70` computes when the mine is placed.

**`0x1DD190`'s first act is `vec.clear()`** — `param_2[1] = *param_2`. Calling it
again is therefore a resample and nothing else, which is precisely what the water
branch of `0x1B3690` does every `[0x90AA40]` = 50 seconds.

### The call

```
RCX   = game object
RDX   = &building[0xE90]        the vector to fill
R8    = C3DVECTOR3* position    from C3D_NODE::GetPosition(building + 0x320)
XMM3  = rotation.y              from C3D_NODE::GetRotation(building + 0x320)
[RSP+0x20] = building[0x318]    the type descriptor, which carries +0x360 and +0x368
```

## What the plugin adds

Two halves, and the split is the whole design — see
[07-pitfalls.md](07-pitfalls.md) for the crash that produced it.

### The tick — arithmetic only

A post-hook on `0x1B3690`. It reads two floats out of the building, does sums,
and writes one float back. No engine call but `PowerTime`, no texture, no
allocation — so whatever thread the building dispatcher runs on, it cannot upset
anything.

1. `step = C3D_TIMER::PowerTime(timer, 0.001)` — the same call the tick itself
   opens with.
2. `tonnes = step / 60 × building[0xDDC]`. `60` is `[0x90AA90]`, and this is the
   game's own conversion: it is how the water branch of `0x1B1220` turns that
   same field into a quantity.
3. `units = tonnes × 255 / tonnes_per_texel`, taken off the mine's `reserve` and
   added to its `spent`.
4. `building[0xDF8] = baseQuality × reserve / reserve0`.

Step 4 is not an approximation. Quality of source is the weighted **average**
richness under the mine, and draining the footprint evenly lowers that average
in exact proportion to what has been taken out — so the linear model is what the
game's own formula gives once the map is drawn down uniformly.

### The flusher — everything that touches the texture

An import hook on `C3D_TERRAIN::Render`, so unambiguously the render thread, and
rate-limited to `flush_seconds` (never more than four times a second whatever
that says). One `TextureAccessOpen`/`Close` pair per map covers every mine on
it:

- a mine the tick has not seen before is **seeded**: its footprint box is
  computed from `C3D_NODE::GetPosition` and the game's own radius table at
  `0x1DCA70`, and `reserve0` is the sum of the component over every texel inside
  it. `baseQuality` is whatever `+0xDF8` said at that moment.
- a mine with `spent >= 1` has that many texels lowered by one, sweeping the
  footprint with a cursor so the drain spreads evenly instead of boring a hole
  under the building.

One unit is one step of one texel's value, so the reserve is an integer count of
things the flusher can actually take away and the model can never promise the
map more than it holds.

**Why not resample with the game's own scan.** Because `0x1DD190` maps the
deposit texture, and the earlier version of this plugin called it from the tick.
It crashed the graphics driver. The whole story is in
[07-pitfalls.md](07-pitfalls.md); the short version is that
`TextureAccessOpen` is `CreateTexture2D` + `CopyResource` +
`ID3D11DeviceContext::Map`, and the immediate context belongs to the render
thread.

### The row in the mine's window

`FUN_140786ac0` at rva `0x786AC0` builds the panel that shows **Quality of
source** and **Current production per workday**. Found by scanning `.text` for
reads of `building+0xDF8` and matching the label ids it hands to
`C3D_LANGUAGE::GetString` — `0xC82` is "Quality of source", `0xC83` is "Current
production per workday". `tools/pe/scanfield.py` does the scan and
`tools/assets/btf.py` resolves an id to its text.

Its layout is a fixed X and a running Y, both plain locals:

```c
x = DPI*[0x90ABB0] + (window[0x04] - DPI*[0x90A8E4] + window[0x28] + DPI*[0x90A9E4]);
y = DPI*[0x90AA08] + window[0x08] + window[0x2C];     // the first row
y += DPI*[0x90A9E4];                                  // 35, after each row
```

and the last thing it does is `window[0x250] = y`. **That field is the panel's
bottom edge**, which is what makes an extra row cheap: a post-hook reads where
the game stopped, prints there, and writes the new bottom back, so the window
grows by exactly one line instead of the text landing on the frame.

```
Quality of source              69 %
Current production per workday 41.60
Deposit remaining: 251.4 kt / 259.3 kt  (96.9 %)
```

The row is drawn with the game's own `C3D_FONTMANAGER::PrintLeftUnicode`, its
own panel font at `[0x995220]`, and the colour every other label on that panel
uses, `0xFF990000`. Nothing the game draws is touched — the hook is additive,
runs under `__try`, and switches itself off for the session on a fault.

`panel = 0` removes it; `panel_caption` is the label, ASCII only for the same
reason `menu_tag` is.

### Why the map and not a counter

A private "tonnes left" per mine would have been fewer lines. Draining the
texture instead buys four things for free:

- **It persists.** The world writer at `0x7C20` puts both maps back with
  `SaveToDDS`, so a depleted map is what a save contains.
- **It shows.** The minimap deposit overlay samples the same texture.
- **It is shared.** Two mines over one patch draw down the same reserve, and a
  mine built later finds what is left rather than a fresh deposit.
- **It survives the plugin.** Deleting the DLL leaves the map as mined, which is
  the honest outcome; nothing is stored anywhere the base game cannot read, and
  a save does not depend on the plugin being installed.

### Which deposits, and where each one lives

| Deposit | Type | Texture | Component |
|---|---|---|---|
| oil | 0 | `resourcemap` | 0 |
| iron | 1 | `resourcemap` | 1 |
| coal | 2 | `resourcemap` | 2 |
| uranium | 6 | `resourcemap2` | 0 |
| bauxite | 7 | `resourcemap2` | 1 |
| **gravel** | **3** | **terrain mask, `terrain+0x158`** | **2** |
| anything in `deposits.ini` | 10+ | as declared | as declared |

Wood (4) regrows and water (8, 9) is not a stock, so neither is depletable.

**Gravel is the odd one.** It is not in a resource map at all. The type-3 case
at `0x1DD910` reads

```asm
mov  rax,[0x1409941F0]          ; the game object
mov  rcx,[rax+0xED8]            ; C3D_TERRAIN
mov  r9,[rcx+0x158]             ; its mask texture  <-- not a resource map
lea  r8,[rbp-0x50]
call 0x140008360                ; the sampler
movss xmm0,[rax+8]              ; component 2
```

`terrain+0x158` is the terrain's **material mask** — the same texture
`C3D_TERRAIN::EditMask` paints and the editor's ground-material brush edits.
Two consequences:

- Draining it **wears the ground texture away** under a working gravel pit.
  Defensible as a look, but it is a visible, permanent change to the terrain
  rather than to an invisible channel. Drop `gravel` from `vanilla` to
  keep the base game's behaviour.
- Bracketing goes through `C3D_TERRAIN::MaskTextureOpen` / `MaskTextureClose`
  rather than the texture's own vtable slots 16 and 18, because that is what
  `0x1DD190` does for type 3 and the terrain has other state to keep in step.
  Both are imported by name; if either is missing, gravel is dropped and the
  rest of the feature carries on.
- Gravel's search radius is `[0x90A9B8]` = **30** against an ore mine's 210, so
  its footprint is a few dozen texels instead of a couple of thousand. At the
  same tonnes-per-texel it would run out almost immediately, which is why
  `vanilla` accepts `gravel:30000`.

Persistence for gravel goes through `C3D_TERRAIN::SaveToFolder` rather than the
world writer's `SaveToDDS`, and **that path has not been traced** — the terrain
editor's material painting does survive a save, so it almost certainly works,
but it has not been confirmed for this.

### The texel arithmetic

Read out of the editor brush at `0x238B00`, which is where the loader's editor
patch already gets it. World position to texel is the sampler's mapping:

```c
tx = (x - terrainOffset.x) / terrainSize.x * texture[0x14]   // width
ty = (z - terrainOffset.z) / terrainSize.y * texture[0x18]   // height
```

with `C3D_TERRAIN::GetOffset` and `GetTerrainSize` both returning through a
hidden buffer pointer in RDX. Within the ARGB word `TextureAccesGetTexel`
returns, the component sits at:

| Component | Bits | Checked against |
|---|---|---|
| 0 | 16..23 | oil, editor channel 1 |
| 1 | 8..15 | bauxite, editor channel 6 |
| 2 | 0..7 | coal, editor channel 3 |
| 3 | 24..31 | copper, editor channel 4 |

Writes go through vtable slot 23, reads through slot 20, bracketed by 16 and 18
— the same four slots the brush uses.

## Configuration

`build/plugins/depletion.ini`, section `[depletion]`:

| Key | Meaning |
|---|---|
| `enabled` | 0 leaves deposits infinite. Nothing is hooked and the plugin unloads |
| `tonnes_per_texel` | what one fully saturated texel is worth in mine output. The main balance knob |
| `vanilla` | which base-game deposits run out: names, `all`, or `none`. A name may carry its own figure after a colon — `gravel:30000` |
| `flush_seconds` | real seconds between writing depletion back into the map. Not a simulation setting — production follows extraction every tick regardless |
| `log_seconds` | real seconds between progress lines in the log, per mine. 0 = off |
| `panel` | add the remaining-deposit row to the mine's information window |
| `panel_caption` | the label on that row. ASCII only |

A `deposits.ini` section may carry its own `deplete = <tonnes>`, or
`deplete = 0` to stay infinite. The loader does not read that key — it keeps it
verbatim and hands it over through `TsmHost::depositSetting`, which is how a
plugin carries per-deposit settings in the file the deposit is declared in.

The default is `oil,iron,coal,uranium,bauxite,gravel:30000` — every base-game
deposit a mine draws a stock from.

## Calibrating

The one thing that cannot be derived is the unit `building+0xDDC` is in. The
conversion above is the game's own, but the resulting tonnage is only meaningful
against a real mine, so the log prints both halves the first time each mine is
seen:

```
deplete  iron mine at (2140, 3880): 1372 sample points, ~2160 texels, quality 0.641, full reserve about 2592000 t
deplete  iron mine producing: rate 41.60, quality 0.641, about 1661472 t under it -> roughly 2396000 in-game seconds at this rate
```

Compare the reserve against how long the mine should last and scale
`tonnes_per_texel` by the ratio. Halving it halves every deposit's life.

## What the player sees

There is no separate "the ore is running out" test anywhere, and nothing needed
one. The signal is the deposit itself:

- A row in the mine's window reads **`Deposit remaining: 251.4 kt / 259.3 kt
  (96.9 %)`**, right under the production line.
- The **quality of source** above it falls, continuously and every tick, in
  proportion to what the mine has taken out — it is the same number as that
  percentage, scaled.
- **Production falls with it**, because `+0xDDC` is `workers × quality × max`.
- The **minimap overlay fades and shrinks**, a few seconds behind the
  simulation — that is `flush_seconds`. The disc under the mine loses one value
  step at a time, spread evenly by the cursor sweep, so the patch pales overall
  while its thin edges — which started at a low value — reach zero first and
  the visible spot shrinks inward.
- When every texel in the footprint is zero the mine's quality is 0 and it
  simply stops. Building another mine on the same spot finds nothing, because
  the map is what a new mine samples too.

It is not an eraser stroke: the brush at `0x238B00` applies its delta to every
texel in range at once, with falloff. This takes one texel per unit of output,
skipping texels already at zero, so the total taken out of the map is exactly
what the mine produced — and a small rich patch inside a large radius drains at
the same rate as a large one, rather than the whole disc fading in lockstep.

## What has not been verified in game

Version 1.0 was tried and **crashed the graphics driver** the moment a coal mine
was built; that is what the tick/flusher split above exists to fix, and it is
written up in [07-pitfalls.md](07-pitfalls.md). Version 1.1 loads cleanly but
**no mine has been watched running down yet**. Five things to check on the first
real session, in order:

1. The game survives building a mine at all. The log's first line for it is
   `deplete  <name> mine: W x H texel box, N units under it (T t), quality Q` —
   printed by the flusher, on the render thread.
2. That box and reserve look sane for the deposit under the mine.
3. Quality of source falls in the building window over the following minutes,
   and production with it.
4. The minimap overlay for that deposit visibly shrinks under the mine.
5. Save, reload, and confirm the deposit is still drawn down — that is the
   `SaveToDDS` path doing its job. Separately for gravel, whose map is saved by
   the terrain rather than by that writer, and whose ground texture is expected
   to change as the pit works.

The log prints, once, which thread the mine tick runs on and which the render
hook runs on. On the run that produced this text they were **26972 and 29444** —
different, which settles it: the crash was a threading violation on the
immediate context, not re-entrancy.

`log_seconds` then prints a line per mine per minute:

```
deplete  coal mine: 22 x 22 texel box, 55099 units under it (259289 t), quality 0.687
deplete  coal mine: 99.86% left, 258926 of 259289 t, quality 0.686
```

which is the only way to see anything happening at the default figure — 259 289
tonnes is a few in-game **years** of a coal mine, so on the minute scale a
working plugin and an idle one look identical.

If the rate turns out to be in a different unit than assumed, only
`tonnes_per_texel` needs to move; no code changes.

## Where else deposit type is read

`0x1DD190` is not the only per-type code, and depletion touches none of the
others, but the scan is worth repeating after a game update. `.text` for
`cmp dword ptr [reg+0x368], imm8` — `83 [B8-BF] 68 03 00 00 imm8` — finds all 18
functions that branch on deposit type; `0x2BAD70` and `0x2A9902` still have not
been examined.
