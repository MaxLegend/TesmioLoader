"""Read a soviet*.btf language file: list, or search by text or by id.

    python btf.py <file> --find <substring>
    python btf.py <file> --id <n>
    python btf.py <file> --dump

Big-endian throughout, the payload included:

    u32 count
    u32 file size
    u32 offset of something past the payload
    count * { u32 id, u32 offset, u16 length }
    UTF-16BE payload

**Offset and length are in UTF-16 code units, not bytes** - entry 0 is
{off 0, len 6} and entry 1 is {off 7, len 5}, so the strings are separated by
one unit and both fields have to be doubled to index the payload. Reading them
as byte counts gives text that is shifted and truncated, which looks like a
different header layout and is not.

Finding the id behind a label the game draws is how a UI function is located:
the id is an immediate in the code, so one grep over .text for it lands on the
exact call site. Ids run 300..580231; anything a mod mints must sit above that.
"""
import struct, sys

path = sys.argv[1]
mode = sys.argv[2] if len(sys.argv) > 2 else "--dump"
arg = sys.argv[3] if len(sys.argv) > 3 else None

d = open(path, "rb").read()
count, size, tail = struct.unpack_from(">III", d, 0)
hdr = 12
entries = []
for i in range(count):
    o = hdr + i * 10
    ident, off, ln = struct.unpack_from(">IIH", d, o)
    entries.append((ident, off, ln))

payload = hdr + count * 10

def text(off, ln):
    raw = d[payload + off * 2: payload + (off + ln) * 2]
    try:
        return raw.decode("utf-16-be")
    except Exception:
        return repr(raw)

if mode == "--find":
    n = 0
    for ident, off, ln in entries:
        t = text(off, ln)
        if arg.lower() in t.lower():
            print("%8d  %s" % (ident, t.replace("\n", "\\n")))
            n += 1
    print("(%d of %d entries)" % (n, count))
elif mode == "--id":
    want = int(arg)
    for ident, off, ln in entries:
        if ident == want:
            print("%8d  %s" % (ident, text(off, ln)))
            break
    else:
        print("id %d not present" % want)
else:
    for ident, off, ln in entries:
        print("%8d  %s" % (ident, text(off, ln).replace("\n", "\\n")))
