r"""Read, unpack and rebuild a soviet*.btf language file.

    python btf.py unpack   <file.btf> [out.txt]     .btf -> editable text
    python btf.py pack     <file.txt> [out.btf]     text -> .btf
    python btf.py patch    <base.btf> <overlay.txt> [out.btf]
    python btf.py info     <file.btf>
    python btf.py dump     <file.btf>
    python btf.py find     <file.btf> <substring>
    python btf.py id       <file.btf> <n>
    python btf.py selftest [dir]                    round-trip every .btf in dir

Legacy form, kept because the docs use it:

    python btf.py <file.btf> --find <substring> | --id <n> | --dump

Add --force to overwrite an existing output file, --sort to write the text
sorted by id, --quiet to drop the header comment block.


The container
-------------

Big-endian throughout, the payload included:

    u32 count
    u32 file size in bytes
    u32 payload length in UTF-16 code units
    count * { u32 id, u32 offset, u16 length }       <- 10 bytes on disk
    payload: <payload length> UTF-16BE code units

so `file size == 12 + count * 10 + payload length * 2`, exactly, for all
twenty-one shipped files.

**Offset and length are in UTF-16 code units, not bytes** - entry 0 is
{off 0, len 6} and entry 1 is {off 7, len 5}, so the strings are separated by
one unit and both fields have to be doubled to index the payload. Reading them
as byte counts gives text that is shifted and truncated, which looks like a
different header layout and is not.

The one unit between two strings is a NUL: every string is NUL-terminated and
`length` excludes the terminator. The strings sit back to back in entry order
with no padding and no de-duplication - two entries with the same text still
get their own copy - so rebuilding a file from its entries reproduces the
original byte for byte.

That layout is `C3D_LANGUAGE::Initialize(char *file, char *fallback)`,
`C3DDLL64.dll` rva `0x96A50`: three `fread(&field, 4, 1, f)`, then `count`
iterations of `fread(id, 4, 1)` / `fread(off, 4, 1)` / `fread(len, 2, 1)`, then
one `fread(payload, 2, payloadUnits, f)`. Every field is byte-swapped by hand
afterwards, the payload one code unit at a time, which is what makes the file
big-endian on a little-endian machine. The entry array is allocated as
`count * 12` - 12 in memory, 10 on disk - and the payload as
`payloadUnits * 2`, both with `malloc`, so **there is no ceiling on the entry
count**: a rebuilt file may have more entries than the original.

Two things that only show up in `GetString` (rva `0x97F10`) and matter when
writing a file rather than reading one:

- **The lookup is a linear scan** over the entry array, first match wins. Ids
  therefore do not have to be sorted - and in the shipped files they are not -
  but a duplicate id makes every copy after the first unreachable.
- **`length` is not used to fetch the text.** `GetString` takes the offset and
  scans forward to the NUL. It then rejects the result if the string is empty,
  and falls through to the fallback file - the second argument of `Initialize`,
  `sovietEnglish.btf` for every language - so an entry deliberately blanked
  does not blank the label, it un-translates it. `length` is what
  `GetStringLength` returns.

Ids run 300..580231, sparsely, and negative ids are not in the file at all:
`GetString` reads those as `~id` into the runtime custom-string vector. Anything
a mod mints must sit above 580231 - see docs/07-pitfalls.md.

Finding the id behind a label the game draws is how a UI function is located:
the id is an immediate in the code, so one grep over .text for it lands on the
exact call site.


The text form
-------------

    300 = Some label
    301 = First line\r\nSecond line
    302 = abbreviation for tons.

One entry per line, `<id> = <text>`, in file order unless --sort was given.
Blank lines and lines whose first non-space character is `#` are ignored, so the
header block a dump writes is a comment and hand-written files need none of it.
The id is decimal, or hex with a `0x` prefix. The text runs to the end of the
line and is stripped of surrounding whitespace, because editors eat trailing
spaces; a space that belongs to the string is written \u0020, and a dump
escapes any leading or trailing one that way. 3226 of the shipped entries
have one.

Escapes are `\\`, `\n`, `\r`, `\t` and `\uXXXX`, and nothing else is special.
A dump uses `\uXXXX` for control characters, for lone surrogates, and for the
invisible characters that survive a round trip through an editor badly:
U+00A0 no-break space (1270 of them in the French file), U+00AD soft hyphen,
U+200B..200F, U+2028..202F and U+FEFF. Everything else - Cyrillic, CJK, box
drawing - is written as itself in UTF-8.

The file is written UTF-8 with no BOM and CRLF line endings; a BOM is accepted
on the way back in.
"""

import glob
import os
import re
import struct
import sys

HDR = 12
REC = 10
MAX_LEN = 0xFFFF

TEXT_ENCODING = "utf-8"
UTF16 = "utf-16-be"
SURROGATES = "surrogatepass"


class BtfError(Exception):
    pass


# --------------------------------------------------------------------------
# container
# --------------------------------------------------------------------------

def read_btf(path):
    """Return [(id, text), ...] in file order."""
    with open(path, "rb") as f:
        raw = f.read()
    return parse_btf(raw, path)


def parse_btf(raw, name="<bytes>"):
    if len(raw) < HDR:
        raise BtfError("%s: %d bytes, too short for a 12-byte header"
                       % (name, len(raw)))
    count, size, units = struct.unpack_from(">III", raw, 0)
    base = HDR + count * REC
    if base + units * 2 > len(raw):
        raise BtfError("%s: header claims %d entries and %d payload units, "
                       "which needs %d bytes but the file is %d"
                       % (name, count, units, base + units * 2, len(raw)))
    if size != len(raw):
        sys.stderr.write("warning: %s: header says %d bytes, file is %d\n"
                         % (name, size, len(raw)))
    payload = raw[base:base + units * 2]

    entries = []
    for i in range(count):
        ident, off, ln = struct.unpack_from(">IIH", raw, HDR + i * REC)
        if (off + ln) * 2 > len(payload):
            raise BtfError("%s: entry %d (id %d) runs to unit %d, past the "
                           "%d-unit payload" % (name, i, ident, off + ln, units))
        chunk = payload[off * 2:(off + ln) * 2]
        entries.append((ident, chunk.decode(UTF16, SURROGATES)))
    return entries


def build_btf(entries, name="<entries>"):
    """Serialise [(id, text), ...] to the on-disk form."""
    recs = bytearray()
    payload = bytearray()
    unit = 0
    seen = {}
    for i, (ident, text) in enumerate(entries):
        if not 0 <= ident <= 0xFFFFFFFF:
            raise BtfError("%s: entry %d has id %d, which is not a u32"
                           % (name, i, ident))
        if ident in seen:
            sys.stderr.write("warning: %s: id %d appears twice (entries %d and "
                             "%d); the game's lookup is a linear scan, so the "
                             "second is unreachable\n"
                             % (name, ident, seen[ident], i))
        else:
            seen[ident] = i
        blob = text.encode(UTF16, SURROGATES)
        length = len(blob) // 2
        if length > MAX_LEN:
            raise BtfError("%s: id %d is %d code units, over the u16 length "
                           "field's %d" % (name, ident, length, MAX_LEN))
        if length == 0:
            sys.stderr.write("warning: %s: id %d is empty; GetString treats an "
                             "empty string as missing and falls back to "
                             "sovietEnglish.btf\n" % (name, ident))
        recs += struct.pack(">IIH", ident, unit, length)
        payload += blob
        payload += b"\x00\x00"
        unit += length + 1

    size = HDR + len(recs) + len(payload)
    return struct.pack(">III", len(entries), size, len(payload) // 2) \
        + bytes(recs) + bytes(payload)


def write_btf(path, entries):
    blob = build_btf(entries, os.path.basename(path))
    with open(path, "wb") as f:
        f.write(blob)
    return blob


# --------------------------------------------------------------------------
# escaping
# --------------------------------------------------------------------------

_SIMPLE = {"\\": "\\\\", "\n": "\\n", "\r": "\\r", "\t": "\\t"}
_UNSIMPLE = {"\\": "\\", "n": "\n", "r": "\r", "t": "\t"}
_HEX = "0123456789abcdefABCDEF"


def _opaque(o):
    """Code points that must not be written as themselves."""
    if o < 0x20 or o == 0x7F or o == 0x85:
        return True                      # control, and NEL
    if 0xD800 <= o <= 0xDFFF:
        return True                      # lone surrogate
    if o in (0xA0, 0xAD, 0xFEFF):
        return True                      # nbsp, soft hyphen, bom
    if 0x2000 <= o <= 0x200F or 0x2028 <= o <= 0x202F:
        return True                      # exotic spaces, bidi marks
    return False


def escape(s):
    out = []
    last = len(s) - 1
    for i, ch in enumerate(s):
        simple = _SIMPLE.get(ch)
        if simple:
            out.append(simple)
            continue
        o = ord(ch)
        if _opaque(o) or (ch == " " and (i == 0 or i == last)):
            out.append("\\u%04X" % o)
        else:
            out.append(ch)
    return "".join(out)


def unescape(s, where=""):
    out = []
    i = 0
    n = len(s)
    while i < n:
        ch = s[i]
        if ch != "\\":
            out.append(ch)
            i += 1
            continue
        if i + 1 >= n:
            raise BtfError("%sa backslash ends the line; write \\\\ for a "
                           "literal one" % where)
        k = s[i + 1]
        plain = _UNSIMPLE.get(k)
        if plain is not None:
            out.append(plain)
            i += 2
            continue
        if k in "uU":
            digits = s[i + 2:i + 6]
            if len(digits) != 4 or any(c not in _HEX for c in digits):
                raise BtfError("%s\\u must be followed by exactly four hex "
                               "digits, found %r" % (where, s[i + 2:i + 6]))
            out.append(chr(int(digits, 16)))
            i += 6
            continue
        raise BtfError("%sunknown escape \\%s; the escapes are \\\\ \\n \\r "
                       "\\t \\uXXXX" % (where, k))
    return "".join(out)


# --------------------------------------------------------------------------
# text form
# --------------------------------------------------------------------------

def dump_text(entries, source=None, sort=False, header=True):
    rows = sorted(entries, key=lambda e: e[0]) if sort else list(entries)
    out = []
    # The header is a comment and is discarded on the way back in. It is worded
    # without naming the tool on purpose: btfconvert.exe writes the same bytes,
    # so the two outputs can be diffed against each other. Change one, change
    # both - the cross-check is what keeps the C++ port honest.
    if header:
        out.append("# soviet republic language file, unpacked for editing")
        if source:
            out.append("# source: %s" % source)
        out.append("# entries: %d%s" % (len(rows), ", sorted by id" if sort
                                        else ", in file order"))
        out.append("#")
        out.append("# <id> = <text>. Escapes: \\\\ \\n \\r \\t \\uXXXX .")
        out.append("# Surrounding whitespace is not significant - a space that "
                   "belongs to the")
        out.append("# string is written \\u0020. Blank and #-comment lines are "
                   "ignored on pack.")
        out.append("")
    for ident, text in rows:
        out.append("%d = %s" % (ident, escape(text)))
    return "\r\n".join(out) + "\r\n"


def load_text(path):
    """Parse the text form back into [(id, text), ...] in file order."""
    with open(path, "rb") as f:
        raw = f.read()
    if raw.startswith(b"\xef\xbb\xbf"):
        raw = raw[3:]
    try:
        body = raw.decode(TEXT_ENCODING)
    except UnicodeDecodeError as e:
        raise BtfError("%s: not valid UTF-8 (%s). Save the file as UTF-8."
                       % (os.path.basename(path), e))

    # str.splitlines also breaks on U+0085, U+2028 and friends, which a dump
    # escapes but a hand-written file need not; only real terminators count.
    #
    # strip(" \t") rather than strip(): bare .strip() also eats U+00A0, U+0085
    # and the unicode spaces, which in this format are data and not formatting -
    # and it is the one place where the C++ port would have had to reproduce
    # str.isspace() to stay bit-identical. See btfconvert.cpp.
    name = os.path.basename(path)
    entries = []
    for lineno, line in enumerate(re.split(r"\r\n|\r|\n", body), 1):
        stripped = line.strip(" \t")
        if not stripped or stripped.startswith("#"):
            continue
        if "=" not in stripped:
            raise BtfError("%s:%d: no '=' on the line: %r"
                           % (name, lineno, line[:60]))
        key, _, value = stripped.partition("=")
        key = key.strip(" \t")
        try:
            ident = int(key, 16) if key.lower().startswith("0x") else int(key, 10)
        except ValueError:
            raise BtfError("%s:%d: %r is not an id" % (name, lineno, key[:40]))
        if ident < 0:
            raise BtfError("%s:%d: id %d is negative; negative ids are runtime "
                           "custom strings and never in the file"
                           % (name, lineno, ident))
        entries.append((ident, unescape(value.strip(" \t"),
                                        "%s:%d: " % (name, lineno))))
    return entries


# --------------------------------------------------------------------------
# cli
# --------------------------------------------------------------------------

GAME_MEDIA = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                          "..", "..", "..", "media_soviet")


def _guard(path, force):
    if os.path.exists(path) and not force:
        raise BtfError("%s exists; pass --force to overwrite it" % path)


def _swap_ext(path, ext):
    return os.path.splitext(path)[0] + ext


def cmd_unpack(args, opts):
    src = args[0]
    dst = args[1] if len(args) > 1 else _swap_ext(src, ".txt")
    entries = read_btf(src)
    _guard(dst, opts["force"])
    text = dump_text(entries, os.path.basename(src), opts["sort"],
                     not opts["quiet"])
    with open(dst, "wb") as f:
        f.write(text.encode(TEXT_ENCODING))
    print("%s -> %s : %d entries" % (src, dst, len(entries)))


def cmd_pack(args, opts):
    src = args[0]
    dst = args[1] if len(args) > 1 else _swap_ext(src, ".btf")
    entries = load_text(src)
    if opts["sort"]:
        entries.sort(key=lambda e: e[0])
    _guard(dst, opts["force"])
    blob = write_btf(dst, entries)
    print("%s -> %s : %d entries, %d bytes" % (src, dst, len(entries), len(blob)))


def cmd_patch(args, opts):
    base, overlay = args[0], args[1]
    dst = args[2] if len(args) > 2 else _swap_ext(overlay, ".btf")
    entries = read_btf(base)
    over = load_text(overlay)
    index = {}
    for i, (ident, _) in enumerate(entries):
        index.setdefault(ident, i)
    changed = added = same = 0
    for ident, text in over:
        i = index.get(ident)
        if i is None:
            index[ident] = len(entries)
            entries.append((ident, text))
            added += 1
        elif entries[i][1] == text:
            same += 1
        else:
            entries[i] = (ident, text)
            changed += 1
    _guard(dst, opts["force"])
    blob = write_btf(dst, entries)
    print("%s + %s -> %s : %d changed, %d added, %d already equal, "
          "%d entries, %d bytes"
          % (base, overlay, dst, changed, added, same, len(entries), len(blob)))


def cmd_info(args, opts):
    path = args[0]
    with open(path, "rb") as f:
        raw = f.read()
    name = os.path.basename(path)
    entries = parse_btf(raw, name)
    count, size, units = struct.unpack_from(">III", raw, 0)
    ids = [e[0] for e in entries]
    # code units, not characters: an astral character is one of the first and
    # two of the second, and the length field counts units.
    lens = [len(t.encode(UTF16, SURROGATES)) // 2 for _, t in entries]
    print("%s" % path)
    print("  header      count=%d  size=%d  payload=%d units" % (count, size, units))
    print("  on disk     %d bytes, header 12 + %d*10 + %d*2 = %d"
          % (len(raw), count, units, HDR + count * REC + units * 2))
    if entries:
        print("  ids         %d..%d, %d unique, %s"
              % (min(ids), max(ids), len(set(ids)),
                 "ascending" if ids == sorted(ids) else "unsorted"))
        print("  text        longest %d units, %d empty"
              % (max(lens), sum(1 for n in lens if n == 0)))
    print("  round trip  %s" % ("byte identical" if build_btf(entries, name) == raw
                                else "DIFFERS from the original"))


def cmd_dump(args, opts):
    for ident, text in read_btf(args[0]):
        print("%8d  %s" % (ident, escape(text)))


def cmd_find(args, opts):
    entries = read_btf(args[0])
    needle = args[1].lower()
    hits = 0
    for ident, text in entries:
        if needle in text.lower():
            print("%8d  %s" % (ident, escape(text)))
            hits += 1
    print("(%d of %d entries)" % (hits, len(entries)))


def cmd_id(args, opts):
    want = int(args[1], 0)
    for ident, text in read_btf(args[0]):
        if ident == want:
            print("%8d  %s" % (ident, escape(text)))
            return
    print("id %d not present" % want)


def cmd_selftest(args, opts):
    where = args[0] if args else GAME_MEDIA
    files = sorted(glob.glob(os.path.join(where, "*.btf")))
    if not files:
        raise BtfError("no .btf files under %s" % where)
    bad = 0
    for path in files:
        name = os.path.basename(path)
        with open(path, "rb") as f:
            raw = f.read()
        entries = parse_btf(raw, name)
        binary_ok = build_btf(entries, name) == raw
        text = dump_text(entries, name, sort=False, header=True)
        tmp = os.path.join(os.environ.get("TEMP", "."), name + ".selftest.txt")
        with open(tmp, "wb") as f:
            f.write(text.encode(TEXT_ENCODING))
        try:
            back = load_text(tmp)
        finally:
            os.remove(tmp)
        text_ok = back == entries
        via_text_ok = build_btf(back, name) == raw
        ok = binary_ok and text_ok and via_text_ok
        bad += 0 if ok else 1
        print("%-4s %-30s %6d entries   rebuild=%s  text=%s  text->btf=%s"
              % ("ok" if ok else "FAIL", name, len(entries),
                 "ok" if binary_ok else "NO",
                 "ok" if text_ok else "NO",
                 "ok" if via_text_ok else "NO"))
    print("%d of %d files round-trip byte for byte" % (len(files) - bad, len(files)))
    return 1 if bad else 0


COMMANDS = {
    "unpack": (cmd_unpack, 1, 2),
    "pack": (cmd_pack, 1, 2),
    "patch": (cmd_patch, 2, 3),
    "info": (cmd_info, 1, 1),
    "dump": (cmd_dump, 1, 1),
    "find": (cmd_find, 2, 2),
    "id": (cmd_id, 2, 2),
    "selftest": (cmd_selftest, 0, 1),
}

LEGACY = {"--dump": "dump", "--find": "find", "--id": "id"}


def main(argv):
    opts = {"force": False, "sort": False, "quiet": False}
    args = []
    for a in argv:
        if a in ("--force", "-f"):
            opts["force"] = True
        elif a == "--sort":
            opts["sort"] = True
        elif a == "--quiet":
            opts["quiet"] = True
        elif a in ("-h", "--help"):
            print(__doc__)
            return 0
        else:
            args.append(a)

    # legacy: btf.py <file> --find <substring>
    if len(args) >= 2 and args[1] in LEGACY:
        args = [LEGACY[args[1]], args[0]] + args[2:]
    elif len(args) == 1 and args[0] not in COMMANDS:
        args = ["dump", args[0]]

    if not args or args[0] not in COMMANDS:
        print(__doc__)
        return 2
    fn, lo, hi = COMMANDS[args[0]]
    rest = args[1:]
    if not lo <= len(rest) <= hi:
        raise BtfError("%s takes %s argument(s), got %d"
                       % (args[0], lo if lo == hi else "%d..%d" % (lo, hi),
                          len(rest)))
    return fn(rest, opts) or 0


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1:]))
    except BtfError as e:
        sys.stderr.write("error: %s\n" % e)
        sys.exit(1)
