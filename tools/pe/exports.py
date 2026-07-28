"""Dump or search the export table of a PE, and optionally disassemble one export.

    python exports.py <dll> [name-substring] [--disasm N]

C3DDLL64.dll ships 2218 MSVC-mangled exports, so its export table is a symbol
file: this is how a named engine function is turned into an address without
importing the DLL into Ghidra at all.
"""
import struct, sys

path = sys.argv[1]
needle = sys.argv[2] if len(sys.argv) > 2 and not sys.argv[2].startswith("--") else None
ndis = 0
if "--disasm" in sys.argv:
    ndis = int(sys.argv[sys.argv.index("--disasm") + 1])

data = open(path, "rb").read()
pe = struct.unpack_from("<I", data, 0x3C)[0]
nsec = struct.unpack_from("<H", data, pe + 6)[0]
optsize = struct.unpack_from("<H", data, pe + 20)[0]
opt = pe + 24
magic = struct.unpack_from("<H", data, opt)[0]
ddir = opt + (112 if magic == 0x20B else 96)
exp_rva, exp_size = struct.unpack_from("<II", data, ddir)

sects = []
off = pe + 24 + optsize
for i in range(nsec):
    name = data[off:off+8].rstrip(b"\0").decode("latin1")
    vsize, vaddr, rsize, raddr = struct.unpack_from("<IIII", data, off + 8)
    sects.append((name, vaddr, max(vsize, rsize), raddr))
    off += 40

def o(rva):
    for name, vaddr, vsize, raddr in sects:
        if vaddr <= rva < vaddr + vsize:
            return raddr + (rva - vaddr)
    return None

def cstr(fo):
    e = data.index(b"\0", fo)
    return data[fo:e].decode("latin1")

eo = o(exp_rva)
nfuncs, nnames = struct.unpack_from("<II", data, eo + 20)
addr_rva, names_rva, ords_rva = struct.unpack_from("<III", data, eo + 28)
ao, no, oo = o(addr_rva), o(names_rva), o(ords_rva)

hits = []
for i in range(nnames):
    nrva = struct.unpack_from("<I", data, no + i * 4)[0]
    name = cstr(o(nrva))
    if needle and needle.lower() not in name.lower():
        continue
    idx = struct.unpack_from("<H", data, oo + i * 2)[0]
    frva = struct.unpack_from("<I", data, ao + idx * 4)[0]
    hits.append((frva, name))

for frva, name in sorted(hits):
    print("%08X  %s" % (frva, name))

if ndis and len(hits) == 1:
    from capstone import Cs, CS_ARCH_X86, CS_MODE_64
    frva, name = hits[0]
    fo = o(frva)
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    print("\n--- %s, %d bytes ---" % (name, ndis))
    for ins in md.disasm(data[fo:fo+ndis], frva):
        print("%08X  %-24s %s %s" % (ins.address,
              " ".join("%02X" % b for b in ins.bytes), ins.mnemonic, ins.op_str))
elif ndis:
    print("\n(--disasm needs the substring to match exactly one export; %d matched)" % len(hits))
