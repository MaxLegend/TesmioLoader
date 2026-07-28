# Reads .rdata constants out of SOVIET64.exe by RVA.
#
#   python rdata.py <rva> [rva ...]
#
# Prints each as float, int and hex. Almost every magic number in the deposit
# and mine code is a `DAT_140xxxxxx` the decompiler leaves unresolved; this
# turns one into a value in a second, without opening Ghidra.
import struct, sys, os

EXE = os.environ.get("SOVIET_EXE",
      r"A:\SteamLibrary\steamapps\common\SovietRepublic\SOVIET64.exe")

def sections(buf):
    pe = struct.unpack_from("<I", buf, 0x3C)[0]
    nsec = struct.unpack_from("<H", buf, pe + 6)[0]
    opt = struct.unpack_from("<H", buf, pe + 20)[0]
    out = []
    for i in range(nsec):
        o = pe + 24 + opt + i * 40
        name = buf[o:o+8].rstrip(b"\0").decode("latin1")
        vsize, va, rsize, raw = struct.unpack_from("<IIII", buf, o + 8)
        out.append((name, va, max(vsize, rsize), raw))
    return out

def rva2off(secs, rva):
    for name, va, size, raw in secs:
        if va <= rva < va + size:
            return raw + (rva - va), name
    return None, None

buf = open(EXE, "rb").read()
secs = sections(buf)

for a in sys.argv[1:]:
    rva = int(a, 16)
    off, sec = rva2off(secs, rva)
    if off is None:
        print("%08X  not mapped" % rva)
        continue
    raw = buf[off:off+8]
    f, = struct.unpack_from("<f", raw)
    d, = struct.unpack_from("<d", raw)
    i, = struct.unpack_from("<i", raw)
    print("%08X  [%-8s] %-12s float=%-16g int=%-12d double=%g"
          % (rva, sec, raw[:4].hex(), f, i, d))
