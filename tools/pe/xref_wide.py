import struct, bisect
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
def o2r(off):
    for n,va,vsz,rsz,ptr in secs:
        if ptr <= off < ptr+rsz: return va+(off-ptr)
T = [s for s in secs if s[0]=='.text'][0]
tstart, tsize, tptr = T[1], T[2], T[4]
pd = [s for s in secs if s[0]=='.pdata'][0]
funcs=[]
o=pd[4]
for k in range(pd[2]//12):
    b,e,u = struct.unpack_from('<III', d, o+k*12)
    if b==0: break
    funcs.append((b,e))
funcs.sort(); fstarts=[f[0] for f in funcs]
def fn_of(rva):
    i = bisect.bisect_right(fstarts, rva)-1
    if i>=0 and funcs[i][0] <= rva < funcs[i][1]: return funcs[i][0]
    return None

# index every 4-byte window in .text as a potential rip-relative displacement
INDEX = {}
code = d[tptr:tptr+tsize]
for i in range(0, tsize-4):
    disp = struct.unpack_from('<i', code, i)[0]
    tgt = tstart + i + 4 + disp
    if tgt < 0 or tgt > 0xB00000: continue
    INDEX.setdefault(tgt, []).append(tstart+i)

def is_lea(rva_disp):
    """rva_disp = rva of the 4-byte displacement; check the bytes before it look like lea r64,[rip+d]"""
    i = rva_disp - tstart
    # REX.W(48-4F) 8D modrm(mod=00 reg=? rm=101)
    if i>=3 and 0x48 <= code[i-3] <= 0x4F and code[i-2]==0x8D and (code[i-1]&0xC7)==0x05:
        return rva_disp-3
    if i>=2 and code[i-2]==0x8D and (code[i-1]&0xC7)==0x05:
        return rva_disp-2
    return None

def xrefs(target_rva, lea_only=True):
    out=[]
    for dr in INDEX.get(target_rva, []):
        ins = is_lea(dr)
        if lea_only and ins is None: continue
        out.append((ins if ins is not None else dr, fn_of(ins if ins is not None else dr)))
    return out
