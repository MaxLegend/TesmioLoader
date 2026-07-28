"""Disassemble a RVA range of a PE with capstone, resolving rip-relative targets."""
import struct, sys
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

path = sys.argv[1]
start = int(sys.argv[2], 16)
end = int(sys.argv[3], 16)
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

def rva_to_off(rva):
    for name, vaddr, vsize, raddr, rsize in sects:
        if vaddr <= rva < vaddr + max(vsize, rsize):
            return raddr + (rva - vaddr)
    return None

def read_cstr(rva, wide=False):
    o = rva_to_off(rva)
    if o is None:
        return None
    if wide:
        e = o
        while e + 1 < len(data) and data[e:e+2] != b'\0\0' and e - o < 400:
            e += 2
        try:
            return data[o:e].decode('utf-16-le')
        except Exception:
            return None
    e = o
    while e < len(data) and data[e] != 0 and e - o < 200:
        e += 1
    s = data[o:e]
    if all(0x20 <= c < 0x7f or c in (9, 10, 13) for c in s) and len(s) > 2:
        return s.decode('latin1')
    return None

o = rva_to_off(start)
buf = data[o:o + (end - start)]
md = Cs(CS_ARCH_X86, CS_MODE_64)
md.detail = False
for insn in md.disasm(buf, start):
    line = '%08X  %-24s %s %s' % (insn.address,
                                  ' '.join('%02X' % b for b in insn.bytes),
                                  insn.mnemonic, insn.op_str)
    if 'rip +' in insn.op_str or 'rip -' in insn.op_str:
        try:
            d = insn.op_str.split('rip')[1].split(']')[0].replace(' ', '')
            v = int(d.replace('+', '').replace('-', ''), 16)
            if d.startswith('-'):
                v = -v
            tgt = insn.address + insn.size + v
            s = read_cstr(tgt) or read_cstr(tgt, True)
            line += '   ; 0x%X' % tgt
            if s:
                line += '  %r' % s
        except Exception:
            pass
    print(line)
