import struct, capstone, sys
exe = r"A:\SteamLibrary\steamapps\common\SovietRepublic\SOVIET64.exe"
d = open(exe,'rb').read()
pe = struct.unpack_from('<I', d, 0x3c)[0]
nsec = struct.unpack_from('<H', d, pe+6)[0]; optsz = struct.unpack_from('<H', d, pe+20)[0]
IMG = struct.unpack_from('<Q', d, pe+24+24)[0]
secs=[]
for i in range(nsec):
    o = pe+24+optsz+i*40
    secs.append((d[o:o+8].rstrip(b'\0').decode(), struct.unpack_from('<I',d,o+12)[0],
                 struct.unpack_from('<I',d,o+8)[0], struct.unpack_from('<I',d,o+16)[0],
                 struct.unpack_from('<I',d,o+20)[0]))
def r2o(rva):
    for n,va,vsz,rsz,ptr in secs:
        if va <= rva < va+max(vsz,rsz): return ptr+(rva-va)
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
def dump(start, end, label=""):
    print("### %s  %08X..%08X" % (label, start, end))
    off = r2o(start)
    for i in md.disasm(d[off:off+(end-start)], IMG+start):
        rva = i.address - IMG
        extra=""
        if 'rip' in i.op_str:
            try:
                disp = int(i.op_str.split('rip')[1].split(']')[0].replace(' ',''),16) if '0x' in i.op_str.split('rip')[1].split(']')[0] else 0
                tgt = rva + i.size + disp
                extra = "   ; -> %08X" % tgt
                o2 = r2o(tgt)
                if o2 and tgt >= 0x86C000:
                    extra += " = %r" % struct.unpack_from('<f', d, o2)[0]
            except Exception: pass
        print("%08X  %-26s %s %s%s" % (rva, i.bytes.hex(' ').upper(), i.mnemonic, i.op_str, extra))
