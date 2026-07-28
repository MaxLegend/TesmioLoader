"""Per-channel occupancy of every shipped resourcemap pair.

The maps are 1024x1024 BGRA with a 128-byte DDS header, so byte i*4+c of the
payload is component c of pixel i.  Component order here is the order the
engine's sampler returns (C3DFCOLOR .x/.y/.z/.w), which for BGRA in memory is
B,G,R,A -> but what matters is only "is this channel all zero", which is
order-independent.
"""
import glob
import os
import sys

ROOT = r"A:/SteamLibrary/steamapps/common/SovietRepublic/media_soviet"

rows = []
for path in sorted(glob.glob(os.path.join(ROOT, "*", "resourcemap*.dds"))):
    data = open(path, "rb").read()
    payload = data[128:]
    if len(payload) < 1024 * 1024 * 4:
        rows.append((path, "short file %d bytes" % len(data)))
        continue
    counts = []
    for c in range(4):
        ch = payload[c::4]
        nonzero = len(ch) - ch.count(0)
        counts.append((nonzero, max(ch)))
    rows.append((path, counts))

name_w = max(len(os.path.relpath(p, ROOT)) for p, _ in rows)
print("%-*s   %-22s %-22s %-22s %-22s" % (name_w, "map", "c0", "c1", "c2", "c3 (alpha)"))
for path, counts in rows:
    rel = os.path.relpath(path, ROOT)
    if isinstance(counts, str):
        print("%-*s   %s" % (name_w, rel, counts))
        continue
    cells = ["%9d px max %3d" % (nz, mx) for nz, mx in counts]
    print("%-*s   %s" % (name_w, rel, " ".join("%-22s" % c for c in cells)))
