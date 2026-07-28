# Reverse engineering — the toolkit

Four techniques, in the order to reach for them. Each is cheap where the one
below it is expensive. Most of what is in [02-findings.md](02-findings.md) came
from the first three; Ghidra was needed exactly once, and it was decisive.

## 1. PE structure — free, instant, no game running

Import tables, export tables, sections, the exception directory. Answers
"what does this binary talk to" and "where does this function begin and end".

The exception directory (`.pdata`) is worth knowing about: every function in an
x64 PE has a `RUNTIME_FUNCTION` record with its exact start and end. Given any
address inside a function, its bounds come straight out of that table — no
heuristics. That is how `ResourceGet` was measured at 735 bytes and the deposit
dispatch at 3734.

For `C3DDLL64.dll` the export table is a symbol file: 2218 MSVC-mangled names.
`undname` turns them into signatures.

## 2. String cross-references — the workhorse

The technique that found nearly everything:

1. Locate a distinctive string in `.rdata`.
2. Scan `.text` for RIP-relative `lea` instructions computing its address —
   `REX.W 8D /r` with `mod=00, rm=101`, target `rip + disp32`.
3. Map each hit to its containing function through `.pdata`.

`"ResourceGet - not found %s"` had exactly one reference, and that reference was
inside the resolver. `$TYPE_MINE_*` had two — the .ini parser and the
type-to-name function. `%s/resourcemap.dds` gave the world loader.

**Where it fails:** any function that touches no string. The deposit sampler
works on an already-loaded buffer and references nothing quotable, and no amount
of scanning would have found it.

Two traps, both hit here:

- In PowerShell, `"$TYPE_MINE_COAL"` interpolates to an empty string. Use single
  quotes.
- A naive byte-at-a-time search over a multi-gigabyte process takes ~40 seconds.
  `memchr` is vectorised and turns that into about a second.

## 3. Runtime observation — when static analysis runs out

Hook first, understand later. Deriving behaviour from live data is usually
faster and always more trustworthy than reading disassembly.

This is how the resource system was mapped: hooking `ResourceGet` and logging
every `(name, return value)` pair produced the complete enum in one game load,
including the record stride, the array base, and the failure sentinel — none of
which had to be guessed.

Variants used here:

- **Import hooks** for anything named.
- **Vtable hooks** for virtual calls, which no import hook can see.
- **Guard pages** — `VirtualProtect` with `PAGE_GUARD`, catch the fault in a
  vectored handler, record `RIP`. Finds *who reads this memory* when there is no
  symbol to hook.

The guard-page probe is still in the loader (`probe_map`). It answered its
question — the deposit map buffers are never read by game code — and that
negative result is what justified opening Ghidra. Three lessons from building
it, all learned by crashing:

- `VirtualProtect` rounds to page boundaries. Guarding a range that is not
  page-aligned protects bytes outside the buffer; a guard fault the handler
  cannot attribute goes unhandled and kills the process.
- The scanner must skip regions it has already guarded, or it trips its own trap
  and, worse, disarms it.
- Finding the buffer by scanning is too slow to catch anything that happens at
  load time. Hooking `fread` and taking the destination pointer straight from
  the read is instant. Track several streams: the deposit map is opened twice in
  the same millisecond and a single-slot cache captures the wrong one.

## 4. Ghidra — for control flow that has to be read

**Installed at `A:\Programs\ghidra_11.3.2_PUBLIC`.** Java 21 is on `PATH`.

Use it when the question is *what does this code decide*, not *where is it* or
*what does it do to data*. Concretely: chains of comparisons, switch tables,
struct field arithmetic. It is the only way to answer "which colour channel does
deposit type 6 read", and that answer was unobtainable by any other means here.

Do not use it to find functions — string xrefs are faster. Do not use it to
learn behaviour — hooking is faster and reflects reality.

### Headless usage

No GUI needed. The analyzer imports, analyses and runs a script:

```
A:\Programs\ghidra_11.3.2_PUBLIC\support\analyzeHeadless.bat <project-dir> <project-name>
    -import <binary>            first time only
    -process <binary> -noanalysis    afterwards
    -scriptPath <dir> -postScript <script.py> <args...>
```

First import of `SOVIET64.exe` takes a few minutes. **Keep the project** and use
`-process ... -noanalysis` after that; subsequent scripts run in seconds.

Invoke it through a `.bat` file with absolute paths, launched via
`Start-Process` with `-RedirectStandardOutput`. Passing the command inline to
`cmd /c` fails on quoting, and a relative path to the `.bat` is not found.

### The scripts

In `<scratchpad>/ghidra/`, Jython (Python 2):

| Script | Arguments | Purpose |
|---|---|---|
| `DecompTargets.py` | out.c | decompiles a fixed list of interesting functions |
| `DecompAt.py` | out.c, addresses… | decompiles whatever contains each address |
| `Callers.py` | out.c, address, limit | lists and decompiles callers of a function |
| `Disasm.py` | out.asm, start, end | raw disassembly with bytes — required for writing a patch |

`DecompAt.py` and `Disasm.py` are the ones you will reuse. Decompiled C is
enough to understand a decision; **writing a patch needs `Disasm.py`**, because
you have to know the exact instruction lengths and encodings you are replacing.

### Sanity check the decompiler

Always include one function whose behaviour you already know from runtime. This
project decompiled `ResourceGet` alongside the unknowns: it showed the name
arriving in the second parameter and a pointer coming back, matching the logs
exactly. Agreement there is what makes the rest trustworthy.

## 5. Shader disassembly — for anything drawn on the GPU

`media_soviet/shaders_d3d11/*.inix` are effect files: a technique name followed
by its vertex and pixel `DXBC` blobs, each blob's length at its own `+0x18`.
Feed a blob to `D3DDisassemble` in `d3dcompiler_47.dll` — present on every
Windows machine, no SDK and no `fxc` needed — and it returns annotated assembly
plus a full reflection of every constant buffer, sampler and texture slot,
including which fields the shader actually reads.

```python
dll = ctypes.WinDLL("d3dcompiler_47.dll")
dll.D3DDisassemble(blob, len(blob), 0, None, ctypes.byref(out))   # out is an ID3DBlob
```

`ID3DBlob` has no COM subtleties worth worrying about: `vtbl[3]` is
`GetBufferPointer`, `vtbl[4]` is `GetBufferSize`.

Do this before assuming a shader has to be patched. The minimap deposit overlay
turned out to select its colour channel with a `dp4` against a float4 constant,
which made the unused alpha channel reachable with no patch at all.

## A note on linear disassembly

Capstone stops at the first byte that does not decode, and `.text` is full of
jump tables and padding, so a single `md.disasm` over the section yields a few
hundred instructions out of millions. For a *known* address range it is ideal —
that is how the overlay's texture-bind stages were read. For finding references
across the whole section, use the displacement scan from technique 2 instead:
index every 4-byte window as a candidate RIP-relative operand, then keep the
hits whose preceding bytes decode as `REX.W 8D /r` with `mod=00, rm=101`. No
valid instruction stream required.

## Writing a code patch

Sequence, once `Disasm.py` output is in hand:

1. Pick a site with at least 5 bytes of a single instruction to overwrite. The
   deposit patch uses two 7-byte instructions.
2. Record the original bytes as a constant and compare before writing. A game
   update must make the patch refuse, not corrupt the process.
3. Allocate a cave **within ±2 GB** of the module — `call rel32` and `jmp rel32`
   cannot reach further. Walk allocation granularity outward from the module
   base until `VirtualAlloc` succeeds at a hinted address.
4. Emit the cave with every displacement computed from the runtime base.
   Reproduce the original instruction you displaced, then add yours.
5. Overwrite the site with `jmp rel32` and pad with `0x90`.
6. `FlushInstructionCache`.

Worked example: `PatchDepositType` in `src/tesmioloader.cpp`.
