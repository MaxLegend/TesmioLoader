"""Read the header of a Workers & Resources .nmf mesh: submaterials and nodes.

    python nmf.py <file.nmf> [--nodes] [--mtl]

Cloning a building means writing a .mtl that declares **exactly the submaterial
names the .nmf asks for** - a name that does not match is simply not applied,
and the mesh renders untextured. Those names are in the file, so there is no
reason to guess them.

    +0x00  "B3DMH\\0" "10"     magic and version, 8 bytes
    +0x08  u32                 submaterial count
    +0x0C  u32                 object count (inferred, not verified)
    +0x10  u32                 total file size - a cheap integrity check
    +0x14  submaterial names, 0x40 bytes each, NUL-padded

Verified against clothing_factory, eletric_substation, iron_mine, alumina_plant
and aluminium_plant: the names read out of the header match those buildings'
own .mtl files exactly, and +0x10 matches the file size for all five.

Node names - what $COST_WORK_BUILDING_NODE and
$COST_WORK_VEHICLE_STATION_ACCORDING_NODE refer to - are interleaved with
geometry rather than tabled, so `--nodes` scans for printable strings instead
of parsing. Treat that list as candidates, and confirm against the donor's own
building.ini.

`--mtl` prints a skeleton .mtl with one block per submaterial, ready to have
texture paths filled in. Paths in $TEXTURE are relative to media_soviet/.
"""
import struct, sys, os

HEADER_NAMES_OFF = 0x14
NAME_STRIDE      = 0x40


def read(path):
    d = open(path, "rb").read()
    if d[:6] != b"B3DMH\0":
        raise SystemExit("%s: not a .nmf (magic is %r)" % (path, d[:6]))

    version = d[6:8].decode("latin1")
    nsub, nobj, size = struct.unpack_from("<III", d, 8)

    names = []
    off = HEADER_NAMES_OFF
    for _ in range(nsub):
        names.append(d[off:off + NAME_STRIDE].split(b"\0")[0].decode("latin1", "replace"))
        off += NAME_STRIDE

    return d, version, nsub, nobj, size, names


# Geometry is dense in bytes that happen to be printable, so a plain "run of
# ASCII" scan is almost all noise. A node name is an identifier: it starts with
# a letter and is made of letters, digits, underscores and colons - which is
# what the base game's own names look like (brickShape1, temp:_T_TOWER1).
def _ident(b):
    return (65 <= b <= 90) or (97 <= b <= 122) or (48 <= b <= 57) or b in (0x5F, 0x3A)


def scan_strings(d, start, minlen=6):
    out, cur, at = [], bytearray(), 0
    for i in range(start, len(d)):
        c = d[i]
        if _ident(c):
            if not cur:
                at = i
            cur.append(c)
        else:
            s = cur.decode("latin1")
            if len(s) >= minlen and (s[0].isalpha() or s[0] == "_"):
                out.append((at, s))
            cur = bytearray()
    return out


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    path = sys.argv[1]
    want_nodes = "--nodes" in sys.argv
    want_mtl   = "--mtl" in sys.argv

    d, version, nsub, nobj, size, names = read(path)

    print("%s" % os.path.basename(path))
    print("  version %s, %d submaterial(s), %d object(s)" % (version, nsub, nobj))
    print("  header size %d, file size %d%s"
          % (size, len(d), "" if size == len(d) else "   ** MISMATCH **"))
    print("  submaterials:")
    for i, n in enumerate(names):
        print("    %2d  %s" % (i, n))

    if want_nodes:
        seen = set(names)
        print("  node name candidates (printable strings past the submaterial table):")
        shown = 0
        for at, s in scan_strings(d, HEADER_NAMES_OFF + nsub * NAME_STRIDE):
            if s in seen or "." in s or "/" in s:
                continue
            seen.add(s)
            print("    0x%06X  %s" % (at, s))
            shown += 1
            if shown >= 60:
                print("    ... truncated")
                break

    if want_mtl:
        print("\n; --- skeleton, fill in the texture paths (relative to media_soviet/) ---")
        for n in names:
            print("$SUBMATERIAL %s" % n)
            print("$TEXTURE 0 buildings/CHANGEME.dds")
            print("$TEXTURE 1 buildings/blankspecular.dds")
            print("$TEXTURE 2 buildings/blankbump.dds")
            print("")
            print("$DIFFUSECOLOR 0.9 0.9 0.9 1.0")
            print("$SPECULARCOLOR 1.0 1.0 1.0 1.0")
            print("$AMBIENTCOLOR 0.9 0.9 0.9 1.0")
            print("$SPECULARPOWER 2.0")
            print("")
        print("$END")


main()
