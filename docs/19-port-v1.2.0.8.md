# Port to v1.2.0.8

TesmioLoader 0.4.4 targets the 64-bit DX11.1 game build with PE timestamp
`0x6B0097B2`. The installed `SOVIET64.exe` and `C3DDLL64.dll` matched the
copies in `port/backup-vNEXT/` byte for byte. The previous game build is
documented in [the v1.1.1.13 snapshot](18-port-v1.1.1.13.md).

| File | SHA-256 |
|---|---|
| `SOVIET64.exe` | `91a2ff66fd02e02a51f38f4d2106ccb56af9510af9a6fb7e7b93ae4249358ea7` |
| `C3DDLL64.dll` | `331895c798092c1c24d8d7586631d8c514bcb0070dcb0d189eca7cf29ba8a483` |

- The executable's section starts are `.text` at `0x1000`, `.rdata` at
  `0x8B1000`, `.data` at `0x9DC000` and `.pdata` at `0xAA1000`. The
  texture vtable export in the engine DLL is now at RVA `0x188C58`.
- The engine constructs 61 vanilla resource records at stride `0x380`.
  Four entries now occupy indices 29 through 32. Transport blocks 18 and 19
  follow the existing blocks, which moves the material family to `+0x34C`
  and the cargo mesh pointers to `+0x358` through `+0x378`. The cache next
  to the vector retains its 57-pointer span.
- `resources.ini` ships the four new RVAs. When a user keeps an older
  config, the plugin recognizes the exact v1.1.1.13 defaults and substitutes
  the new ones in memory. Other valid overrides still apply.
- `needs` uses the new resource and building type strides. The building
  type grows from `0xBE8` to `0xE98`. The storage vector moves from
  `+0x3A0` to `+0x3A8` within the type descriptor.
- `deposits` uses the new parser locals, sampler address and sample output
  buffer. The terrain pointer, resource maps and editor tool fields move as
  well. `daynight` sizes its temporary light object through the relocated
  weather field. `easystart` uses the new year, education, cached goods and
  building vector fields.

## Verification

`build.bat --package` builds the launcher, DLL and nine enabled plugin targets.
The build leaves the `aging` probe and `buildings` generator parked.
`--find` reports v1.2.0.8 as supported and exits 0. Against the v1.1.1.13 backup
it reports an unsupported build and exits 2. Offline checks matched 41
expected-byte sites, all 61 names in the resource table, the city and deposit
save call targets, and the walking, city, and needs-policy patch operands.

For an in-game check, use the rebuilt plugins and look for `hook ok` and any
refusals in `tesmioloader.log`. Start a fresh map and check a custom resource
and deposit, a shop carrying an added good, and a calendar day rollover.
Check the terrain editor's Rocks tab and save/reload on a test map.
