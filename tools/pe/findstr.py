import re, sys, struct

path = sys.argv[1]
pats = sys.argv[2:]
data = open(path, 'rb').read()

# PE section table -> map file offset to RVA
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

def to_rva(fo):
    for name, vaddr, vsize, raddr, rsize in sects:
        if raddr <= fo < raddr + rsize:
            return vaddr + (fo - raddr), name
    return None, None

for p in pats:
    print('=== pattern: %r ===' % p)
    pb = p.encode('latin1')
    # ascii
    for m in re.finditer(re.escape(pb), data):
        fo = m.start()
        # extend to full C string
        s = fo
        while s > 0 and 0x20 <= data[s-1] < 0x7f and fo - s < 120:
            s -= 1
        e = fo
        while e < len(data) and 0x20 <= data[e] < 0x7f and e - fo < 120:
            e += 1
        rva, sec = to_rva(fo)
        print('  A file=0x%X rva=0x%X (%s) : %r' % (fo, rva or 0, sec, data[s:e].decode('latin1')))
    # utf-16
    pw = p.encode('utf-16-le')
    for m in re.finditer(re.escape(pw), data):
        fo = m.start()
        rva, sec = to_rva(fo)
        e = fo
        while e + 1 < len(data) and data[e+1] == 0 and 0x20 <= data[e] < 0x7f and e - fo < 240:
            e += 2
        print('  W file=0x%X rva=0x%X (%s) : %r' % (fo, rva or 0, sec, data[fo:e].decode('utf-16-le', 'replace')))
