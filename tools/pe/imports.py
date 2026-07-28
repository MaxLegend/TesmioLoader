"""Dump the import table of a PE: IAT slot RVA -> dll!name."""
import struct, sys

path = sys.argv[1]
wanted = [int(x, 16) for x in sys.argv[2:]]
data = open(path, 'rb').read()

pe = struct.unpack_from('<I', data, 0x3C)[0]
nsec = struct.unpack_from('<H', data, pe + 6)[0]
optsize = struct.unpack_from('<H', data, pe + 20)[0]
opt = pe + 24
magic = struct.unpack_from('<H', data, opt)[0]
ddir = opt + (112 if magic == 0x20b else 96)
imp_rva, imp_size = struct.unpack_from('<II', data, ddir + 8)

sects = []
off = pe + 24 + optsize
for i in range(nsec):
    name = data[off:off+8].rstrip(b'\0').decode('latin1')
    vsize, vaddr, rsize, raddr = struct.unpack_from('<IIII', data, off + 8)
    sects.append((name, vaddr, vsize, raddr, rsize))
    off += 40

def o(rva):
    for name, vaddr, vsize, raddr, rsize in sects:
        if vaddr <= rva < vaddr + max(vsize, rsize):
            return raddr + (rva - vaddr)
    return None

def cstr(fo):
    e = fo
    while data[e] != 0:
        e += 1
    return data[fo:e].decode('latin1')

d = o(imp_rva)
i = 0
out = {}
while True:
    oft, ts, fc, nrva, firstthunk = struct.unpack_from('<IIIII', data, d + i * 20)
    if nrva == 0:
        break
    dll = cstr(o(nrva))
    t = o(oft or firstthunk)
    slot = firstthunk
    j = 0
    while True:
        v = struct.unpack_from('<Q', data, t + j * 8)[0]
        if v == 0:
            break
        if v & (1 << 63):
            nm = 'ordinal#%d' % (v & 0xFFFF)
        else:
            nm = cstr(o(v & 0x7FFFFFFF) + 2)
        out[slot + j * 8] = (dll, nm)
        j += 1
    i += 1

if wanted:
    for w in wanted:
        dll, nm = out.get(w, ('?', '?'))
        print('0x%X -> %s!%s' % (w, dll, nm))
else:
    for k in sorted(out):
        print('0x%X %s!%s' % (k, out[k][0], out[k][1]))
