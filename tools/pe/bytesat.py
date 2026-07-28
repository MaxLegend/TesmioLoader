# Prints the bytes SOVIET64.exe holds at an RVA, in the form the loader's
# k*Prologue arrays use.
#
#   python bytesat.py <rva> [count]
#
# Every inline hook in tesmioloader compares its site against a hard-coded array
# before writing anything; this is how that array is produced and, after a game
# update, how a mismatch is diagnosed.
import struct, sys, os

EXE = os.environ.get("SOVIET_EXE",
      r"A:\SteamLibrary\steamapps\common\SovietRepublic\SOVIET64.exe")

buf = open(EXE, "rb").read()
pe = struct.unpack_from("<I", buf, 0x3C)[0]
nsec = struct.unpack_from("<H", buf, pe + 6)[0]
opt = struct.unpack_from("<H", buf, pe + 20)[0]

def rva2off(rva):
    for i in range(nsec):
        o = pe + 24 + opt + i * 40
        vsize, va, rsize, raw = struct.unpack_from("<IIII", buf, o + 8)
        if va <= rva < va + max(vsize, rsize):
            return raw + (rva - va)
    return None

rva = int(sys.argv[1], 16)
n = int(sys.argv[2]) if len(sys.argv) > 2 else 16
off = rva2off(rva)
if off is None:
    sys.exit("rva %08X not mapped" % rva)

raw = buf[off:off + n]
print("rva %08X, file offset %08X" % (rva, off))
print(" ".join("%02X" % b for b in raw))
print(", ".join("0x%02X" % b for b in raw))
