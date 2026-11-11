# 17 — The v1.1.1.9 snapshot: what the next port starts from

Written on **2026-11-11**, the day a new game version was announced, **before**
Steam replaced the executable. Everything the port to the next build needs is
either on this page or is pointed at by it.

## The binary being left behind

| | |
|---|---|
| version | **v1.1.1.9** (launcher gate: `{ 1, 1, 1, 9 }`) |
| PE TimeDateStamp | `0x6A3EB6AD` = 2026-06-26 17:28:13 UTC |
| size | 10 308 608 bytes |
| SHA-256 | `296644a9f207d609031fc2ae73fed2dcb34619a1d55a35d1c7b51965ce6841b8` |
| sections | `.text` RVA 0x001000 VSZ 0x86AA03 · `.rdata` RVA 0x86C000 VSZ 0x125C62 · `.data` RVA 0x992000 VSZ 0xC4718 · `.pdata` RVA 0xA57000 VSZ 0x29820 |

**A byte copy of both binaries is in `port/backup-v1.1.1.9/`** —
`SOVIET64.exe` (sha256 above) and `C3DDLL64.dll`
(`65fcb6845c3cb7c25a22121f25904a855ea88657e31a3b50a15adac26e231516`). The
old/new diff is the fastest route to every new address; without the copy the
old build is gone the moment Steam updates. Two cross-binary references live
in the sources: `C3D_MESH::LoadFromFile` at C3DDLL64 rva `0xA84C0`
(`resources.cpp`, export-relative, survives an engine rebuild only if the
export order holds) and the texture vtable at C3DDLL64 rva `0x187BF0`
(`tesmioloader.cpp` `TEX_VTABLE_RVA`).

## The two working files beside this page

| file | what it is |
|---|---|
| `port/raw-scan-v1.1.1.9.txt` | every source line carrying an RVA-like literal (0x1000–0xA00000), verbatim, with line numbers — the belt-and-braces copy of *everything*, comments and site tables included, not just `#define`s |
| `port/defines-v1.1.1.9.txt` | the same, filtered to `#define` constants and patch-site table rows — reproduced in full below |
| `port/config-scan-v1.1.1.9.txt` | every `.ini`/`.bat`/`.txt` line carrying an RVA-like literal — config keys and prose comments, the half a source-only scan does not cover |

`port/` is git-ignored (it holds the 12 MB backup); this page is committed,
so the reviewable half of the snapshot is in the repo and the bulky half is
only ever a re-copy away if lost.

## The six address classes, and how each is re-derived

The v1.1.1.7 → v1.1.1.9 port (docs/01-architecture.md, *Porting off
v1.1.1.7*) established that an update does not shift by one constant. Class by
class:

1. **The version gate** (`tesmiolauncher.cpp` `kSupportedVersions`, one row
   today). Read the new build's four version immediates off the `lea` in
   front of `L"v%d.%d.%d.%d (64 bit DX11.1 - GPU: %ls)"` and its PE stamp, add a
   row, keep the old one until its addresses are gone from the tree.
2. **`.text` code sites** — every hook, call site and function the plugins
   name. Re-derive with the prologue/pattern scan over the expected bytes each
   hook already records, filtered against the new `.pdata` so a match must be
   a function start; tie-break by what the body reads. Last port's deltas by
   region: +0 / +0x10 / +0x70 / +0xA0 / +0xD0 / +0x120 / +0x1E0, growing with
   distance into `.text` — expect the same shape, not the same numbers.
3. **`.rdata` float pool** (the 0x909xxx–0x90Cxxx cluster). Moved −0x18 (and
   −0x20 in a second cluster) last time. Find by **value**: the float at the
   candidate must equal the old one — but tie-break by the code's own
   reference, not the value alone, because a neighbouring literal can
   masquerade as a value change: **the v1.1.1.9 day tick still compares
   against 60.0 at `0x90AA78`** (`comiss` at `0x334ABB`, immediately after
   its `PowerTime` call at `0x334AAD`), while `63.0` sits unreferenced 0x18
   higher at `0x90AA90` — the "day length went 60.0 → 63.0" note CLAUDE.md
   and docs/01 carried from the last port is a misread of that neighbour
   (verified 2026-11-11 against the backup: `0x90AA78` has 104 `.text`
   referers, `0x90AA90` has none). Values may also be one ULP low in the base
   game (docs/04-adding-resources.md). Three deposits tint colours rest on
   value match alone (no xref); they were the one soft spot of the last port.
4. **`.rdata` strings** (`P_STR_BAUXITE` 0x8895C0, `resources/%s.png`
   0x899C48). Did not move last time; verify by content.
5. **IAT entries** — most import hooks resolve by name and survive. The two
   exceptions hard-coded as RVAs are in `deposits`: `P_MASK_OPEN_IAT`
   0x86CF38 (`MaskTextureOpen`) and `P_MASK_CLOSE_IAT` 0x86CF30
   (`MaskTextureClose`) — re-resolve from the new import directory by name.
6. **`.data` globals** (game object 0x9D4F10, resource vector 0x9E11C0,
   timer 0x9D4EE0, person vector 0x9E75B8, fonts, DPI…). **Did not move at
   all** in the last update; confirm via the `lea` sites that resolve them
   (e.g. `RVA_GAMEOBJ_LEA` in cities) before trusting any one of them, then
   trust the rest.

Struct offsets (`game+0x11B08`, `building+0x970`, `person+0x118`, …) are
not RVAs and did not change last time; the port that follows should re-verify
the few the code reads by instruction, not re-derive them wholesale. They are
all in the raw scan with their context comments.

## A seventh place addresses live: the config files

`plugins/resources/resources.ini` carries **four RVAs as live config keys**
that **override** the compiled defaults (`resources.cpp` reads them at init;
a present, non-empty key wins over every `DEFAULT_*_RVA`):

| key | value | overrides |
|---|---|---|
| `resource_rva` | `0x2AA830` | `DEFAULT_RESOURCEGET_RVA` |
| `resource_vector_rva` | `0x9E11C0` | `g_vecRva` |
| `price_pass_rva` | `0x2A9340` | `DEFAULT_PRICEPASS_RVA` |
| `customhouse_tick_rva` | `0x1854E0` | `P_CUSTOMHOUSE_TICK_RVA` |

**A port that only re-derives the `#define`s is not done**: a stale key in
the ini silently points the hook at the old address (the byte check then
refuses it — safe, but the feature is dead and the log names the wrong
suspect, the compiled default). After re-deriving, either update the four
keys or delete them so the compiled defaults rule; the keys exist precisely
to override during a port, so an updated tree should not carry them. No other
ini has address-valued keys (`port/config-scan-v1.1.1.9.txt` is the proof).

## Every constant, per file

Line numbers are into the current sources (2026-11-11); `was 0x…` comments
preserve the v1.1.1.7 value the last port moved from — keep them in the tree,
the per-region deltas they record are the first guess for the next shift.

#### src/tesmiolauncher.cpp
(no RVA defines)

#### src/tesmioloader.cpp
605| #define TEX_VTABLE_RVA   0x187BF0
1867| #define P_MENU_VERSION_SITE 0x28B5CC     // v1.1.1.9; was 0x28B55C. LEA RAX,[rip+disp32] - the format argument

#### plugins/resources/resources.cpp
55| #define P_MIDDLEPOINT   0x9EACD0
60| #define DEFAULT_RESOURCEGET_RVA 0x2AA830
337| #define DEFAULT_PRICEPASS_RVA 0x2A9340
2073| #define P_CUSTOMHOUSE_TICK_RVA 0x1854E0   // v1.1.1.9; was 0x185470
2082| #define P_CUSTOMS_SLOT_PUSH_RVA 0x0B14E0   // std::vector<Slot>::push_back, 16-byte value

#### plugins/needs/needs.cpp
73| #define P_PERSON_PLAN_RVA   0x836B40   // v1.1.1.9; was 0x836960
74| #define P_STORAGE_BUILD_RVA 0x0E40F0   // FUN_1400e40f0(parser, storage, class, cap, res)
75| #define P_SHOP_TICK_RVA     0x171E10   // v1.1.1.9; was 0x171DA0. Building type 3
76| #define P_TYPE_LOAD_RVA     0x11D800   // v1.1.1.9; was 0x11D810 - one of the few
78| #define P_BUILDING_TYPES    0x9E6A30   // the vector<TYPE> that function fills
83| #define P_SLOT_PUSH_RVA     0x0B14E0   // std::vector<Slot>::push_back, 16-byte value
84| #define P_RESOURCE_VECTOR   0x9E11C0   // the resource vector: begin, end, capacity
131| #define G_GOODS_BUCKETS     0x12720    // five of them, stride 0x18
132| #define G_GOODS_BUCKET_OTHER 0x12780

#### plugins/depletion/depletion.cpp
78| #define P_MINE_TICK_RVA   0x1B3700   // v1.1.1.9; was 0x1B3690. One mine, one tick
79| #define P_DEPOSIT_RADIUS  0x1DCAE0   // v1.1.1.9; was 0x1DCA70. FUN_1401dca70 -
86| #define P_GAMEOBJ         0x9941F0   // the pointer to the main game object
87| #define P_TIMER_OBJ       0x9D4EE0   // the C3D_TIMER the whole simulation steps on
90| #define P_TIMER_STEP      0x909AFC   // 0.001f, the argument every PowerTime call passes
91| #define P_PROD_PERIOD     0x90AA78   // 60.0f, what the rate is divided by per tick
136| #define P_MINE_PANEL_RVA  0x786BE0   // v1.1.1.9; was 0x786AC0
137| #define P_FONTMANAGER     0x996FB0   // C3D_FONTMANAGER, by address
138| #define P_PANEL_FONT      0x995220   // the C3D_FONT* the panel's rows use
139| #define P_DPI             0x992088   // the scale every layout constant is multiplied by
140| #define P_ROW_STEP        0x90A9CC   // 35 - one row, and the x indent as well
141| #define P_X_BACK          0x90A8CC   // 15
142| #define P_X_INDENT        0x90AB98   // 110

#### plugins/accumulator/accumulator.cpp
108| #define P_DISPATCH_RVA    0x139A70   // was 0x139A80 - one simulation tick
109| #define P_NODE_SHARE_RVA  0x1BB770   // was 0x1BB700 (game, building, what, priority)
110| #define P_SUB_SHARE_RVA   0x1B96B0   // was 0x1B9640 - hands its
137| #define P_TIMER_OBJ       0x9D4EE0   // the C3D_TIMER the simulation steps on
138| #define P_TIMER_STEP      0x909AFC   // 0.001f, the argument every PowerTime passes
150| #define P_ELEC_PANEL_RVA  0x702730   // v1.1.1.9; was 0x702610
151| #define P_FONTMANAGER     0x996FB0   // C3D_FONTMANAGER, by address
152| #define P_PANEL_FONT      0x995220   // the C3D_FONT* the panel's rows use
153| #define P_DPI             0x992088   // the scale every layout constant is multiplied by
154| #define P_ROW_STEP        0x90A9CC   // 35 - one row, and the x indent as well
159| #define G_BUILDINGS       0x11B08    // game -> vector<Building*> begin
160| #define G_BUILDINGS_END   0x11B10    // ...and end
177| #define B_WATTAGE_PTR     0x10E0
178| #define B_VOLTAGE_PTR     0x10E8
179| #define B_BREAKER_PTR     0x10F8
180| #define B_WATT_DIAL       0x1100
181| #define B_VOLT_DIAL       0x1128
188| #define B_UNPOWERED       0x1130

#### plugins/cities/cities.cpp
112| #define P_GAMEOBJ            0x9D4F10
114| #define G_CITY_BEGIN         0x10348   // vector<NamePoint*>
115| #define G_CITY_END           0x10350
116| #define G_BLD_BEGIN          0x11B08   // vector<Building*>
117| #define G_BLD_END            0x11B10
137| #define P_FIND_CITY          0x330690  // was 0x3305F0. NamePoint* (Game*, const Vec3*, bool create)
138| #define P_REASSIGN           0x32FF40  // was 0x32FEA0. void (Game*, const Vec3*)
139| #define P_RENUMBER           0x3303B0  // was 0x330310. void (ignored, NamePoint*) - house numbers
140| #define P_VEC_PUSH           0xB0E90   // unchanged. void (vector<T*>*, T** value) - push_back
141| #define P_CITY_WINDOW        0x7CEF60  // was 0x7CEDB0. void (Game*, Window*, int captionId)
142| #define P_SAVE_CITIES        0x3334E0  // was 0x333440. void (Game*, FILE*)
143| #define P_LOAD_CITIES        0x333870  // was 0x3337D0. void (Game*, FILE*)
144| #define P_SAVE_CALL          0x42DC35  // was 0x42DB95. the one `call 0x3334E0`
145| #define P_LOAD_CALL          0x431DF6  // was 0x431D56. the one `call 0x333870`
146| #define P_SLIDER             0x2727F0  // was 0x272780. the 0..100 slider with - and + buttons
160| #define P_CHECKBOX           0x3956F0  // was 0x395650
161| #define P_WORLD_LOAD         0x430FC0  // was 0x430F20. void (…) - the whole world load
162| #define P_WORLD_LOAD_CALL    0x29436B  // was 0x2942FB. the one `call 0x430FC0`
170| #define RVA_LIMIT_SITE       0x3306F8  // was 0x330658
171| #define RVA_LIMIT_CONST      0x90B1FC  // was 0x90B214. 1e6 = 1000 m squared
172| #define RVA_BOX_DELETE       0x32F61D  // was 0x32F57D. in DeleteCity
173| #define RVA_BOX_CANDELETE    0x32F902  // was 0x32F862. in CanDeleteCity
174| #define RVA_BOX_CONST        0x90B07C  // was 0x90B094. 1000
202| #define RVA_OVL_R_START      0x3123C1  // was 0x312321. movss xmm11,[0x90B07C]
203| #define RVA_OVL_RSQ          0x31267C  // was 0x3125DC. movss xmm11,[0x90B1FC]
204| #define RVA_OVL_STEP_Z       0x3127B1  // was 0x312711. addss xmm6 ,[0x90AA28]
205| #define RVA_OVL_STEP_X       0x3127D0  // was 0x312730. addss xmm8 ,[0x90AA28]
206| #define RVA_OVL_STEP_CONST   0x90AA28  // was 0x90AA40. 50
230| #define RVA_OVL_END_CMP      0x3127EA  // was 0x31274A
255| #define P_DPI                0x992088  // every layout constant is multiplied by this
256| #define P_FONTMGR            0x996FB0  // C3D_FONTMANAGER, by address
257| #define P_ROW_FONT           0x994200  // the C3D_FONT* the city window's rows use
259| #define LAYOUT_15            0x90A8CC  // was 0x90A8E4. 15
260| #define LAYOUT_20            0x90A928  // was 0x90A940. 20 - a checkbox
261| #define LAYOUT_25            0x90A968  // was 0x90A980. 25 - how far 0x395650 draws ABOVE its y
262| #define LAYOUT_35            0x90A9CC  // was 0x90A9E4. 35 - one row
264| #define P_WIDGET_BUSY        0xAD74    // game -> the widget being edited, or -1
1299| #define RVA_GAMEOBJ_LEA 0x43970A   // v1.1.1.9; was 0x43966A

#### plugins/daynight/daynight.cpp
173| #define RVA_WEATHER_TICK  0x333BD0
177| #define RVA_LIGHT_FACTOR  0x5CFC40
183| #define RVA_SNOW_SEASON   0x3343E0
187| #define RVA_SIM_TIMER     0x9D4EE0
193| #define RVA_DAYTICK_POWER 0x334AA0
205| #define RVA_DAILY_POLLUTION 0x4D5F50   // decay the grid, accumulate exposure
206| #define RVA_DAILY_LOANS     0x4B9660   // one day of interest and repayment
207| #define RVA_DAILY_NOTICES   0x4CD380   // age the notification list, drop the expired
210| #define RVA_DAY_LENGTH    0x90AA78
216| #define RVA_FADE_MOV      0x334224

#### plugins/easystart/easystart.cpp
199| #define GAME_RES_FOOD    0xC300   // the four cached shop-goods records
200| #define GAME_RES_MEAT    0xC310
201| #define GAME_RES_CLOTHES 0xC318
202| #define GAME_RES_ELECTR  0xC320
230| #define BLD_OUT_POWER_A  0x1108
231| #define BLD_OUT_POWER_B  0x1110
232| #define BLD_OUT_POWER_C  0x1118
233| #define BLD_OUT_WATER    0x1120
237| #define P_GAMEOBJ           0x9D4F10
242| #define PERSON_VEC_BEGIN    0x9E75B8
243| #define PERSON_VEC_END      0x9E75C0
247| #define GAME_BUILDING_VEC   0x11B08
273| #define RVA_EVALUATE 0x83A6D0
282| #define RVA_LIVING_TICK 0x1BC210
293| #define RVA_POWER_CALL  0x76147B
294| #define RVA_HAS_POWER   0x1BBFE0
322| { 0x1BC39B, kMulsdXmm1, 4, 0x909E38, 0.00021666666666666666, 1, NEED_ELECTRICITY, "electricity a"  },
323| { 0x1BC575, kMulsdXmm1, 4, 0x909E60, 0.00033333333333333332, 1, NEED_ELECTRICITY, "electricity b"  },
324| { 0x1BC74B, kMulssXmm3, 4, 0x909AF8, 0.0009999999310821295,  0, NEED_WATER,       "water happy"    },
328| { 0x488BEA, kMovssXmm7, 4, 0x909B18, 0.001500000013038516,   0, NEED_HEATING,     "heat happy a"   },
329| { 0x488C1C, kMovssXmm7, 4, 0x909B04, 0.0011699999449774623,  0, NEED_HEATING,     "heat happy b"   },
330| { 0x488C06, kMovssXmm8, 5, 0x909B20, 0.0017000000225380063,  0, NEED_HEATING,     "heat health a"  },
331| { 0x488C24, kMovssXmm8, 5, 0x909B10, 0.0013259999686852098,  0, NEED_HEATING,     "heat health b"  },
336| { 0x1B0B37, kMulssXmm2, 4, 0x909ABC, 0.0002500000118743628,  0, NEED_WATER,       "water health a" },
337| { 0x1B0B44, kMulssXmm0, 4, 0x909AC4, 0.00031250002211891115, 0, NEED_WATER,       "water health b" },
338| { 0x1B1203, kMulssXmm1, 4, 0x909AA8, 0.0001875000016298145,  0, NEED_WATER,       "water health c" },
339| { 0x1B1213, kMulssXmm1, 4, 0x909ABC, 0.0002500000118743628,  0, NEED_WATER,       "water health d" },
340| { 0x1B1223, kMulssXmm1, 4, 0x909AC4, 0.00031250002211891115, 0, NEED_WATER,       "water health e" },
356| { 0x823AE9, NEED_SCHOOL,       "school: basic education at birth"   },
357| { 0x824116, NEED_SCHOOL,       "school: education for a new citizen" },
358| { 0x824145, NEED_SCHOOL,       "school: the same, second branch"    },
359| { 0x834D65, NEED_SCHOOL,       "school: education on arrival"       },
360| { 0x7618E7, NEED_SCHOOL,       "school: the panel's warning"        },
361| { 0x824AEB, NEED_KINDERGARTEN, "kindergarten: a parent's job search" },
362| { 0x761928, NEED_KINDERGARTEN, "kindergarten: the panel's warning"  },

#### plugins/deposits/deposits.cpp
34| #define P_MIDDLEPOINT   0x9EACD0
42| #define P_RESOURCEGET   0x2AA830   // v1.1.1.9; was 0x2AA7C0. Independent of
502| #define P_PARSER_SITE      0x10EAB8   // was 0x10EAC8. LEA RDX,[$TYPE_MINE_BAUXITE]
503| #define P_PARSER_NEXT      0x10EAE8   // was 0x10EAF8. next token check
504| #define P_PARSER_DONE      0x118805   // was 0x118815. shared exit
505| #define P_STRCMP           0x84F520   // was 0x84F340
506| #define P_STR_BAUXITE      0x8895C0   // unchanged - .rdata string, not in the float pool
507| #define P_PARSER_TOKEN     0x49A0     // [rbp+..] the token just read
508| #define P_PARSER_BTYPE     0x1E10     // [rbp+..] building type
509| #define P_PARSER_DTYPE     0x1E18     // [rbp+..] deposit type
511| #define P_DISPATCH_SITE    0x1DD7E3   // was 0x1DD773. CMP [RSI+0x368],6
512| #define P_DISPATCH_BODY6   0x1DD7EC   // was 0x1DD77C. body of the type 6 case
513| #define P_DISPATCH_TAIL    0x1DD826   // was 0x1DD7B6. CMP [RSI+0x368],7
514| #define P_GAMEOBJ          0x9941F0   // unchanged - .data does not move in this build
515| #define P_SAMPLER          0x8360
531| #define P_RADIUS_SITE      0x1DCB3D   // was 0x1DCACD
532| #define P_RADIUS_WATERSURF 0x90AC20   // was 0x90AC38 - the constant the type 9 branch returns
552| #define P_MASK_OPEN_SITE   0x1DD509   // was 0x1DD499
553| #define P_MASK_OPEN_NEXT   0x1DD526   // was 0x1DD4B6 - where its JNZ lands
554| #define P_MASK_CLOSE_SITE  0x1DDE78   // was 0x1DDE08
555| #define P_MASK_CLOSE_NEXT  0x1DDE95   // was 0x1DDE25
556| #define P_MASK_OPEN_IAT    0x86CF38   // C3D_TERRAIN::MaskTextureOpen
557| #define P_MASK_CLOSE_IAT   0x86CF30   // C3D_TERRAIN::MaskTextureClose
587| #define P_MAP_OPEN_SITE    0x1DD4C8   // was 0x1DD458
588| #define P_MAP_OPEN_NEXT    0x1DD4E4   // was 0x1DD474 - the types 6-7 check, which re-reads EAX
589| #define P_MAP_CLOSE_SITE   0x1DDE95   // was 0x1DDE25
590| #define P_MAP_CLOSE_NEXT   0x1DDEB5   // was 0x1DDE45 - the types 6-7 close check
670| #define P_WORLD_SAVE_RVA   0x7C20     // FUN_140007c20(self, folder) - SaveToDDS x4
671| #define P_WORLD_SAVE_CALL  0x42CDAE   // was 0x42CD0E - the only `call 0x7C20` in the executable
1040| #define CAVE_SIZE          0x4000
1041| #define CAVE_CODE          0x1000
1531| #define P_MINIMAP_DRAW_RVA 0x4BDE80   // was 0x4BDDE0. FUN_1404bdde0 - draws the coloured overlay
1532| #define P_MINIMAP_ROW_RVA  0x4BFF40   // was 0x4BFEA0. FUN_1404bfea0 - draws + handles the button row
1561| #define G_GAMEOBJ         0x9941F0   // +0xED8 terrain, +0xF08 resourcemap2
1562| #define G_PANEL           0x9BE060   // the one C3D_PANEL2D the whole minimap draws through
1563| #define G_TECHNIQUE       0x9EAD08   // current shader technique, set by the last BeginDraw
1564| #define G_PANEL_FULLSIZE  0x909F58   // was 0x909F70. w/h passed to every Draw() call in this UI
1565| #define G_SLOT_BG_TEX     0x9DFF38   // background box texture, shared by every button
1566| #define G_SLOT_SEL_TEX    0x9E03E8   // "selected" badge texture, shared by every button
1569| #define G_RES_SELF        0x9D4F10   // "self" object every ResourceGet call in this UI uses
1570| #define G_RES_VECTOR      0x9E11C0   // the resource vector records live in: begin, end, capacity
1571| #define G_CLIP_HELPER_OBJ 0x9DFCC0   // passed to the per-icon clip-rect helper
1572| #define G_DPI             0x992088
1573| #define G_ROW_X0          0x90A988   // was 0x90A9A0
1574| #define G_ROW_STEP        0x90AA44   // was 0x90AA5C
1575| #define G_ROW_Y0          0x90AB18   // was 0x90AB30
1576| #define G_ICON_SCALE      0x909E54   // was 0x909E6C
1577| #define G_HITBOX_HALF     0x90A6A8   // was 0x90A6C0
1578| #define G_BADGE_OFFSET    0x909CD8   // was 0x909CF0
1582| #define G_HALF            0x909DDC   // was 0x909DF4
1583| #define G_COLOR_IDLE      0x90C100   // was 0x90C120. float4 tint, not hovered
1584| #define G_COLOR_HOVER     0x90C4C0   // was 0x90C4E0. float4 tint, hovered
1585| #define G_COLOR_OVERLAY   0x90C2D0   // was 0x90C2F0. float4 (1,0,0,1) - the red every deposit layer is drawn in
1597| #define G_LANG            0x997590   // C3D_LANGUAGE
1598| #define G_TOOLTIP_BUF     0x9E24B0   // the wide buffer the hover text is built in
1600| #define P_FORMAT_WIDE     0x5290     // (wchar_t* buf, size_t chars, const wchar_t* fmt, ...)
1603| #define G_PANEL_POS       0x9BE2F0   // 2 floats: x,y of the next Draw()
1604| #define G_PANEL_PAD       0x9BE2F8
1605| #define G_PANEL_SIZE      0x9BE2E8   // 2 floats: w,h of the next Draw()
1606| #define G_PANEL_COLOR     0x9BE30C   // 4 floats: tint of the next Draw()
1607| #define P_CLIP_HELPER     0x4461F0   // was 0x446150. FUN_140446150 - not exported, internal only
2119| #define P_ED_PANEL_RVA    0x233180   // was 0x233110. FUN_140233110 - draws the Resources tab
2120| #define P_ED_DISPATCH_RVA 0x30D1A0   // was 0x30D100. FUN_14030d100 - applies the active tool, every frame
2121| #define P_ED_CURSOR_RVA   0x2F0F10   // was 0x2F0E70. FUN_1402f0e70 - decides which tools get the round cursor
2122| #define P_ED_TEXELS_RVA   0x238B70   // was 0x238B00. FUN_140238b00 - the generic deposit texel writer
2124| #define P_ED_TOOL_FIND    0x03AAA0   // unchanged. FUN_14003aaa0 - tool lookup by name
2125| #define P_ED_DRAW_BUTTON  0x382760   // was 0x3826C0. FUN_1403826c0 - draws one tool button
2126| #define P_ED_PAINT        0x235140   // was 0x2350D0. FUN_1402350d0 - one brush tick for one deposit
2129| #define ED_ACTIVE_TOOL    0xD428     // char* - the descriptor of the selected tool
2130| #define ED_BRUSH_CURSOR   0x10F0     // set per frame for tools that want the round cursor
2158| #define P_ED_TOOLTIP      0x383C70   // was 0x383BD0. FUN_140383bd0(editorSelf, hoveredTool)
2161| #define G_MIDDLEPOINT     0x9EACD0   // C3D_MIDDLEPOINT the button drawer creates textures through
2169| #define G_ED_X_A          0x90AA28   // 50
2170| #define G_ED_X_B          0x90AB14   // 85
2171| #define G_ED_Y_BASE       0x90ADB8   // 250
2172| #define G_ED_Y_CAPTION    0x90AAFC   // 80
2173| #define G_ED_BUTTON       0x909ED4   // 0.85, the size argument every button is drawn with
2174| #define G_ED_ROW_STEP     0x90AB2C   // 90, paint row to erase row
2175| #define G_ED_COL_STEP     0x90AB84   // 105, resource to resource
2196| #define P_ED_ROCKS_PANEL  0x22EEA0   // was 0x22EE30. FUN_14022ee30 - draws the Rocks tab
2197| #define P_ED_PAINT_ROCK   0x235370   // was 0x235300. FUN_140235300 - one rock brush tick
2204| #define G_ED_ROCK_X_A     0x90AA28   // 50
2205| #define G_ED_ROCK_X_B     0x90AB14   // 85 - and the step between buttons
2206| #define G_ED_ROCK_Y_BASE  0x90ADB8   // 250
2207| #define G_ED_ROCK_Y_CAP   0x90AAFC   // 80
2679| #define P_ED_TERRAIN_RVA  0xAEE0     // unchanged in v1.1.1.9. FUN_14000aee0 - contour lines and the resource overlay
2682| #define G_OVL_VEC_C2      0x90BDD0   // was 0x90BDF0 - the float4 the +0x23 branch passes: (0,0,1,0).

#### plugins/walking/walking.cpp
103| #define RVA_WALK_LIMIT   0x12E2CD
110| #define RVA_WALK_CONST   0x90AF20   // 480, as the queued builder and the overlay read it - was 0x90AF38
111| #define RVA_CAR_CONST    0x90B104   // was 0x90B11C
114| #define RVA_WALK_RADIUS  0x90AF58   // was 0x90AF70
117| #define RVA_CAR_RADIUS   0x90B108   // was 0x90B120
142| { 0x12E79F, RVA_WALK_CONST,  VANILLA_WALK,   SLOT_WALK,   "walk build queued" },
143| { 0x43F0EA, RVA_WALK_CONST,  VANILLA_WALK,   SLOT_WALK,   "walk overlay"      },
144| { 0x43F8D5, RVA_WALK_CONST,  VANILLA_WALK,   SLOT_WALK,   "walk overlay 2"    },
145| { 0x12F916, RVA_CAR_CONST,   VANILLA_CAR,    SLOT_CAR,    "car build"         },
146| { 0x440053, RVA_CAR_CONST,   VANILLA_CAR,    SLOT_CAR,    "car overlay"       },
147| { 0x12DE97, RVA_WALK_RADIUS, VANILLA_WALK_R, SLOT_WALK_R, "walk rebuild"      },
148| { 0x12F4F2, RVA_CAR_RADIUS,  VANILLA_CAR_R,  SLOT_CAR_R,  "car rebuild"       },
154| #define RVA_REGEN_CMP    0x438406   // 83 3D <disp32> 6E   cmp dword[ver],110
155| #define RVA_REGEN_TEST   0x438413   // 84 C0 0F 85 ...     test al,al / jnz
156| #define RVA_SAVE_VERSION 0x9E9C3C

#### plugins/aging/aging.cpp
69| #define RVA_PERSON_VECTOR 0x9E75B8
73| #define RVA_WORLD_PTR     0x9941F0

## Errata found while snapshotting

- **"The day length went 60.0 → 63.0 in v1.1.1.9" is false.** The day tick's
  rollover compare reads `comiss xmm0,[0x90AA78]` (= 60.0) at `0x334ABB`;
  `63.0` lives at `0x90AA90`, 0x18 higher, referenced by nothing in `.text`.
  `daynight` and `depletion` both already carry the correct pair
  (`0x90AA78` / 60.0). CLAUDE.md and docs/01-architecture.md carried the
  misread from the last port; both are corrected. The *lesson* the example
  stood for survives: verify a pool constant by the code that reads it, not
  by value alone.
- **Four prose addresses in ini comments were stale v1.1.1.7 values** — the
  last port updated the `.cpp`s and left the `.ini` narration behind. All
  four are fixed in place with the house `v1.1.1.9; was 0x…` style, each new
  value verified against the backup before writing:
  `depletion.ini` panel builder 0x786AC0 → **0x786BE0** (matches
  `P_MINE_PANEL_RVA`); `needs.ini` daily planner 0x836960 → **0x836B40**
  (matches `P_PERSON_PLAN_RVA`); `needs.ini` happiness-subtract site
  0x836DA9 → **0x836F89** (same +0x449 offset inside the moved planner; the
  bytes there read/write `person+0xD8` with a zero clamp, exactly what the
  comment describes); `accumulator.ini` voltage formula 0x1BD4C0 →
  **0x1BD530** (exact `.pdata` function start at the region's +0x70 shift;
  body reads `building+0x10E0/+0x10E8/+0x10F8/+0x1100` and divides by the
  breaker). `needs.ini`'s 0xE40F0 (storage builder) was checked and is still
  current. **Ini prose is now under the same rule as the sources: an address
  in a comment names the build it was written against.**

## The port (2026-11-11): v1.1.1.9 -> v1.1.1.13

The update landed the same day this snapshot was taken. What it actually did,
measured against the backup rather than read off the update notes:

- **New build: v1.1.1.13**, PE stamp `0x6A8D4087` (2026-08-25 07:13:11 UTC),
  10 308 096 bytes, sha256
  `4df5fbecc75300e3b843ae4e6426761c84ae2e989321c011c6f560be79122125`.
  `.text` grew +0x1A0, `.rdata` shrank 0x40, **`.data` byte-identical except
  5 bytes** - every `.data` global carried over untouched. Backup in
  `port/backup-new/`.
- **C3DDLL64.dll changed 34 bytes total** - timestamps and checksums only.
  The texture vtable (`TEX_VTABLE_RVA 0x187BF0`) and the export order are
  unchanged; nothing in the DLL needed re-deriving.
- **`.text` site deltas clustered at 0 (49 sites), -0x80 (51), -0x70 (21),
  +0x1A0 (14)**, plus stragglers (+0x20, -0xC0, -0x281 on three water-health
  sites whose function shrank, +0xD on two). Most of the update's code went
  *ahead* of the hooks (-0x80) - code was removed before them, not added.
- **Only 36% of functions were byte-identical** (5126/14168 by content hash;
  +7868 more paired by cluster delta + prologue + size = 92% of functions
  mapped). The recompile touched game logic - exactly where the hooks live -
  so 15/147 sites resolved through identical functions and the rest needed
  the cluster map plus, for 18 sites, per-site work.
- **The float pool moved FORWARD this time**: +0x8 below ~`0x909F50`, +0x10
  above it, +0xC in the `0x90B104..0x90B208` sub-cluster (and the panel
  fullsize at +0xC). 41 of 43 project constants verified by
  context+value uniqueness; the hover tint rests on the structural distance
  between the three tints (idle->overlay 0x1D0 preserved), as before the
  softest of the three. **The strings did not move at all.**
- **Functions outside `.pdata`** (leaf functions - the snow-season test, the
  deposit-radius switch, the CRT strcmp) cannot be content-mapped by
  RUNTIME_FUNCTION. What worked: **caller bridging** - find every `call
  rel32` to the old function in the old `.text`, translate each caller
  through the function map, read the new call's target. Unanimous votes in
  all three cases (strcmp: 3062 callers). Interior sites of gap functions
  then resolve by their float reference (the radius switch's type-9 arm
  reads the watersurface constant, which mapped +0x10 independently - two
  derivations agreeing).
- **Two expect-arrays changed**: `kMapOpenOrig`/`kMapCloseOrig` carry the
  rip-disp32 to the game object, which moved with the site (+0x80 in the
  disp). Every other array carried over byte for byte - the install-time
  safety net never weakened.
- **The launcher gate row**: `{ 1, 1, 1, 13 }, 0x6A8D4087`; the v1.1.1.9 row
  is dropped, its addresses gone from the tree exactly as v1.1.1.7's were.
- The four `resources.ini` address keys are updated in place (see *A seventh
  place addresses live*); `resource_vector_rva` is `.data` and unchanged.
- **Every changed `#define` carries `v1.1.1.13; was 0x...` with the v1.1.1.9
  value**; the working maps are checked in at `port/funcmap-old-new.json`
  (function map) and `port/site-map.json` (per-site translations with
  method used), `port/edit-map.json` (what the source editor applied).

**Not yet done: the in-game pass.** The byte-level checks above say every
hook will find what it expects; only a launch with `tesmioloader.log` open
confirms `hook ok` on every site the way the v1.1.1.9 port's first launch
did.

## The port checklist

1. Copy the new `SOVIET64.exe` (and `C3DDLL64.dll` if it changed) to
   `port/backup-<newver>/`; record stamp/sha256/sections the same way as above.
2. Diff old/new section layout first — how much `.text` grew tells you how
   much re-deriving is coming before a single scan runs.
3. Add the new row to `kSupportedVersions` (keep the v1.1.1.9 row until no
   old address remains), so the launcher lets the new build through with a
   warning while the port is in flight (`version_check` story unchanged).
4. Re-derive class by class, plugin by plugin, cheapest first: `.data`
   globals (probably unmoved) → strings → IAT by name → float pool by value →
   code sites by pattern+`.pdata`. Every plugin refuses and logs on a byte
   mismatch, so a half-ported tree still launches and names exactly the sites
   still wrong in `tesmioloader.log`.
5. Leave the `was 0x…` trail: every changed `#define` gets a comment with its
   v1.1.1.9 value, exactly as the v1.1.1.7 trail was left — and the ini
   comments move with it, which the last port forgot (see Errata).
6. **Update or clear the four address keys in `plugins/resources/resources.ini`**
   (`resource_rva`, `resource_vector_rva`, `price_pass_rva`,
   `customhouse_tick_rva`) — they override the compiled defaults, so a stale
   key defeats a re-derived `#define` (see *A seventh place addresses live*).
7. Re-verify against the Ghidra project (re-import into `ghidra/proj3`, the
   fresh one; a stale project cost a day last time — see docs/07-pitfalls.md,
   *A matching PE timestamp does not mean an unchanged binary*).
8. In game: launch with the log open; every site must print `hook ok`; any
   refusal names the one site to re-derive (docs/01-architecture.md).

**The one trap worth repeating**: update notes that say "content only".
v1.1.1.9's notes said that and `.text` reflowed throughout. Trust bytes,
not notes.
