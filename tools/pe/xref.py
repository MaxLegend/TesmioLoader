"""Find RIP-relative references to a target RVA inside .text of a PE."""
import struct, sys

path = sys.argv[1]
targets = [int(x, 16) for x in sys.argv[2:]]
data = open(path, 'rb').read()

pe = struct.unpack_from('<I', data, 0x3C)[0]
nsec = struct.unpack_from('<H', data, pe + 6)[0]
optsize = struct.unpack_from('<H', data, pe + 20)[0]
sects = []
off = pe + 24 + optsize
for i in range(nsec):
    name = data[off:off+8].rstrip(b'\0').decode('latin1')
    vsize, vaddr, rsize, raddr = struct.unpack_from('<IIII', data, off + 8)
    sects.append((name, vaddr, vsize, raddr, rsize))
    off += 40

text = [s for s in sects if s[0] == '.text'][0]
_, tva, tvsize, traddr, trsize = text

# .pdata for function bounds
pdata = [s for s in sects if s[0] == '.pdata']
funcs = []
if pdata:
    _, pva, pvsize, praddr, prsize = pdata[0]
    n = pvsize // 12
    for i in range(n):
        b, e, u = struct.unpack_from('<III', data, praddr + i * 12)
        if b == 0 and e == 0:
            continue
        funcs.append((b, e))
    funcs.sort()

def func_of(rva):
    lo, hi = 0, len(funcs) - 1
    best = None
    while lo <= hi:
        mid = (lo + hi) // 2
        if funcs[mid][0] <= rva:
            best = funcs[mid]
            lo = mid + 1
        else:
            hi = mid - 1
    if best and best[0] <= rva < best[1]:
        return best
    return None

body = data[traddr:traddr + trsize]
for tgt in targets:
    print('=== target rva 0x%X ===' % tgt)
    for i in range(len(body) - 4):
        disp = struct.unpack_from('<i', body, i)[0]
        # instruction ends at i+4 -> next rva
        next_rva = tva + i + 4
        if next_rva + disp != tgt:
            continue
        # look back for a plausible instruction start
        start = max(0, i - 3)
        ctx = body[start:i + 4]
        insn_rva = tva + i
        f = func_of(insn_rva)
        print('  ref at rva 0x%X  bytes %s  func %s' % (
            insn_rva - 3,
            ' '.join('%02X' % c for c in body[max(0, i-3):i+4]),
            ('0x%X..0x%X' % f) if f else '?'))
