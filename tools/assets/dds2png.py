"""Decode the top mip of a DXT1 .dds to a PNG, for eyeballing a recolour."""
import struct, sys
from PIL import Image

src, dst = sys.argv[1], sys.argv[2]
d = open(src, 'rb').read()
_, _, h, w = struct.unpack_from('<4I', d, 4)
bw, bh = (w + 3) // 4, (h + 3) // 4
img = Image.new('RGB', (w, h))
px = img.load()

def c565(v):
    return (((v >> 11) & 0x1F) * 255 // 31,
            ((v >> 5) & 0x3F) * 255 // 63,
            (v & 0x1F) * 255 // 31)

o = 128
for by in range(bh):
    for bx in range(bw):
        c0, c1, idx = struct.unpack_from('<HHI', d, o)
        o += 8
        a, b = c565(c0), c565(c1)
        if c0 > c1:
            cols = [a, b,
                    tuple((2 * a[i] + b[i]) // 3 for i in range(3)),
                    tuple((a[i] + 2 * b[i]) // 3 for i in range(3))]
        else:
            cols = [a, b, tuple((a[i] + b[i]) // 2 for i in range(3)), (0, 0, 0)]
        for y in range(4):
            for x in range(4):
                px_x, px_y = bx * 4 + x, by * 4 + y
                if px_x < w and px_y < h:
                    px[px_x, px_y] = cols[(idx >> (2 * (y * 4 + x))) & 3]
img.save(dst)
print('%s -> %s  %dx%d' % (src, dst, w, h))
