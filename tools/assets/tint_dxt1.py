"""Recolour a DXT1 .dds by rewriting only the two RGB565 endpoints of each block.

A DXT1 block is [color0:u16][color1:u16][indices:u32]; the 2-bit indices pick
between the endpoints and their interpolants, so hue can be changed without
touching a single index. The one invariant that must survive is the comparison
color0 > color1, which is what selects 4-colour opaque mode over 3-colour +
transparent - a tint that flips it would turn opaque texels transparent.
"""
import struct, sys

def unpack565(v):
    r = (v >> 11) & 0x1F
    g = (v >> 5) & 0x3F
    b = v & 0x1F
    return (r * 255 + 15) // 31, (g * 255 + 31) // 63, (b * 255 + 15) // 31

def pack565(r, g, b):
    return ((r * 31 + 127) // 255) << 11 | ((g * 63 + 127) // 255) << 5 | ((b * 31 + 127) // 255)

def recolour(r, g, b, tint, strength):
    lum = (0.299 * r + 0.587 * g + 0.114 * b) / 255.0
    tl = 0.299 * tint[0] + 0.587 * tint[1] + 0.114 * tint[2]
    out = []
    for i in range(3):
        v = tint[i] / tl * lum * 255.0
        v = (1.0 - strength) * (r, g, b)[i] + strength * v
        out.append(max(0, min(255, int(round(v)))))
    return out

def main(src, dst, tint, strength):
    d = bytearray(open(src, 'rb').read())
    assert d[:4] == b'DDS ', src
    assert d[84:88] == b'DXT1', d[84:88]
    n = (len(d) - 128) // 8
    fixed = 0
    for i in range(n):
        o = 128 + i * 8
        c0, c1 = struct.unpack_from('<HH', d, o)
        opaque = c0 > c1
        n0 = pack565(*recolour(*unpack565(c0), tint=tint, strength=strength))
        n1 = pack565(*recolour(*unpack565(c1), tint=tint, strength=strength))
        # Swapping the endpoints would invert every index, so the mode is kept
        # by nudging one of them instead.
        if opaque and n0 <= n1:
            if n0 == 0:
                n0 = 1
            n1 = n0 - 1
            fixed += 1
        elif not opaque and n0 > n1:
            n1 = n0
            fixed += 1
        struct.pack_into('<HH', d, o, n0 & 0xFFFF, n1 & 0xFFFF)
    open(dst, 'wb').write(bytes(d))
    print('%s -> %s  (%d blocks, %d mode fixes)' % (src, dst, n, fixed))

if __name__ == '__main__':
    src, dst = sys.argv[1], sys.argv[2]
    tint = tuple(float(x) for x in sys.argv[3].split(','))
    strength = float(sys.argv[4]) if len(sys.argv) > 4 else 0.9
    main(src, dst, tint, strength)
