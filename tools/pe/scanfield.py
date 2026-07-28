"""Find every instruction in .text that touches a given structure offset.

    python scanfield.py <offset-hex> [opcode-prefix-hex ...]

The game's structures are only known by offset, so "who else reads
building+0xDF8" is a question no symbol can answer - but the displacement is
right there in the encoding. This scans .text for the 4-byte little-endian
displacement and keeps the hits whose preceding bytes look like an instruction
with a mod=10 ModRM, then prints each with context.

With no opcode filter it reports everything, which is noisy; the useful filters
are the ones that say how the field is used:

    F30F10   movss xmm,[reg+off]     read a float
    F30F11   movss [reg+off],xmm     write a float
    8B       mov r32,[reg+off]
    89       mov [reg+off],r32
    488B     mov r64,[reg+off]

This is the same trick 02-findings.md documents for `cmp [reg+0x368], imm8`,
generalised.
"""
import struct, sys

EXE = r"A:\SteamLibrary\steamapps\common\SovietRepublic\SOVIET64.exe"

off = int(sys.argv[1], 16)
filters = [bytes.fromhex(a) for a in sys.argv[2:]]

data = open(EXE, "rb").read()
pe = struct.unpack_from("<I", data, 0x3C)[0]
nsec = struct.unpack_from("<H", data, pe + 6)[0]
optsize = struct.unpack_from("<H", data, pe + 20)[0]

text = None
for i in range(nsec):
    o = pe + 24 + optsize + i * 40
    name = data[o:o+8].rstrip(b"\0").decode("latin1")
    vsize, va, rsize, raw = struct.unpack_from("<IIII", data, o + 8)
    if name == ".text":
        text = (va, raw, min(vsize, rsize))
if text is None:
    sys.exit("no .text")
va, raw, size = text

needle = struct.pack("<I", off)
hits = []
pos = raw
end = raw + size
while True:
    pos = data.find(needle, pos, end)
    if pos < 0:
        break
    # The displacement follows a ModRM with mod=10. Walk back a little looking
    # for one, and for an opcode the caller asked about.
    for back in range(1, 6):
        m = pos - back
        if m < raw:
            break
        modrm = data[m]
        if modrm & 0xC0 != 0x80:
            continue
        if (modrm & 7) == 4 and back < 2:       # SIB byte sits between
            continue
        start = m - 8 if m - 8 >= raw else raw
        pre = data[start:m]
        if filters and not any(pre.endswith(f) for f in filters):
            continue
        hits.append((va + (pos - raw) - back - len(pre) + len(pre),
                     va + (pos - raw), data[max(raw, pos - 12):pos + 4]))
        break
    pos += 1

print("%d site(s) touching +0x%X" % (len(hits), off))
seen = set()
for _, at, ctx in hits:
    key = at
    if key in seen:
        continue
    seen.add(key)
    print("  disp at rva %08X   ...%s" % (at, " ".join("%02X" % b for b in ctx)))
