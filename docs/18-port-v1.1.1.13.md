# 18 — The v1.1.1.13 snapshot: what the next port starts from

Written on **2026-11-24**, the day the next game update was announced,
**before** Steam replaces the executable. This is the v1.1.1.13 counterpart
of docs/17; the playbook (the six address classes, the checklist, the method
notes) lives there and is not repeated.

## The binary being left behind

| | |
|---|---|
| version | **v1.1.1.13** (launcher gate row: `{ 1, 1, 1, 13 }`) |
| PE TimeDateStamp | `0x6A8D4087` = 2026-08-25 07:13:11 UTC |
| size | 10 308 096 bytes |
| SHA-256 | `4df5fbecc75300e3b843ae4e6426761c84ae2e989321c011c6f560be79122125` |
| sections | `.text` RVA 0x001000 VSZ 0x86ABA3 · `.rdata` RVA 0x86C000 VSZ 0x125C22 · `.data` RVA 0x992000 VSZ 0xC4718 · `.pdata` RVA 0xA57000 VSZ 0x297FC |
| C3DDLL64.dll | sha256 `5d8a0540a01170475392b4650f492582fab4ed20eec8755190a053f410247e3a` |

**Byte copies of both are in `port/backup-v1.1.1.13/`** (renamed from
`port/backup-new/`; the v1.1.1.9 pair is still in `port/backup-v1.1.1.9/`,
so a chained old→older diff is possible). The on-disk files were
hash-verified against the backup on 2026-11-24: nothing drifted between the
port and this snapshot.

## The tree this snapshot was taken from

Post **v0.4.2** (commit `775b9d9`) plus working changes, loader version
**0.4.3**. v0.4.2's only address-relevant change is that plugins now read
their ini from `H->pluginDir` (`build\plugins\`), so the **live** address
keys live in `build/plugins/resources.ini` — kept in sync by `build.bat`
copying the source `plugins/resources/resources.ini` over it at every build
(the source file is the one to port, the copy follows). The four keys today:
`resource_rva = 0x2AA7B0`, `resource_vector_rva = 0x9E11C0`,
`price_pass_rva = 0x2A92C0`, `customhouse_tick_rva = 0x185500`.

## Verified 2026-11-24, offline

The whole tree re-checked against the on-disk binary before this snapshot:

- **34/34 install-time expect-arrays match** the exe at the current
  `#define` addresses — the same check every hook performs at runtime,
  run offline. A launch today would print `hook ok` on every site.
- The four `resources.ini` keys (source and `build\plugins\` copy) match the
  compiled defaults.
- 208 address `#define`s across 12 files, dumped fresh into
  `port/defines-v1.1.1.13.txt` (234 lines with the patch-site tables);
  every address-bearing line in the tree is in `port/raw-scan-v1.1.1.13.txt`
  (577 lines), sources and ini files both.

## What the v1.1.1.9 → v1.1.1.13 port measured — the prior to beat

- `.data` did not move a byte, strings did not move, C3DDLL64 changed only
  34 timestamp bytes. Two updates in a row `.data` stayed put — assume it,
  verify it.
- `.text` deltas clustered at 0 / −0x80 / −0x70 / +0x1A0; the float pool
  moved **forward** (+0x8 / +0x10 / +0xC sub-clusters) after moving backward
  the previous update — the sign is not stable, the per-constant
  value+context verification is what carries.
- Only ~36% of functions were content-identical: the update recompiled game
  logic. The function map still covered 92% of functions via
  cluster-delta + prologue + size pairing (`port/funcmap-old-new.json` holds
  the v1.1.1.9→v1.1.1.13 map; the same script rebuilds one for any pair).
- Leaf functions outside `.pdata` resolved by **caller bridging**: translate
  the old `call` sites, read the new call target. Unanimous every time.
- The port checklist that produced all of it: docs/17, *The port checklist*.
