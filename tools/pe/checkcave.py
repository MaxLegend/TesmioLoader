"""Re-emit the deposit cave the way tesmioloader.cpp does and disassemble it.

This mirrors EmitParserCase / EmitDispatchCase / EmitRadiusCase byte for byte.
Its point is to prove the generated code is what it is meant to be BEFORE the
loader ever writes a jump into the executable - the failure mode otherwise is a
corrupted process, which is the one thing static reading is bad at catching.

Two deposits are emitted, not one, so that the multi-case chaining (the rel32
forward branches each case lands on the next) is exercised.
"""
import struct
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

EXE_BASE = 0x140000000          # nominal; only relative distances matter
CAVE     = 0x150000000          # far enough that a wrong rel32 would be obvious

P_STRCMP         = 0x84F340
P_PARSER_NEXT    = 0x10EAF8
P_PARSER_DONE    = 0x118815
P_STR_BAUXITE    = 0x8895C0
P_PARSER_TOKEN   = 0x49A0
P_PARSER_BTYPE   = 0x1E10
P_PARSER_DTYPE   = 0x1E18
P_GAMEOBJ        = 0x9941F0
P_SAMPLER        = 0x8360
P_DISPATCH_TAIL  = 0x1DD7B6
P_DISPATCH_BODY6 = 0x1DD77C
P_DEP_TYPE_FIELD = 0x368
P_MAP1_OFF       = 0xF00
P_MAP2_OFF       = 0xF08
P_RADIUS_WATERSURF = 0x90AC38

CAVE_CODE = 0x800

DEPOSITS = [
    dict(name="copper", token="$TYPE_MINE_COPPER", type=10, map=2, component=3, btype=7),
    dict(name="tin",    token="$TYPE_MINE_TIN",    type=11, map=1, component=3, btype=7),
]


class Emit:
    def __init__(self, addr):
        self.buf = bytearray()
        self.addr = addr

    @property
    def here(self):
        return self.addr + len(self.buf)

    def b(self, *vals):
        self.buf.extend(vals)

    def d32(self, v):
        self.buf.extend(struct.pack("<i", v & 0xFFFFFFFF if v >= 0 else v))

    def rel32(self, target):
        self.d32(target - (self.here + 4))

    def jne32(self):
        self.b(0x0F, 0x85)
        at = len(self.buf)
        self.d32(0)
        return at

    def land(self, at):
        rel = len(self.buf) - (at + 4)
        self.buf[at:at + 4] = struct.pack("<i", rel)


def parser_case(e, d, token_addr):
    e.b(0x48, 0x8D, 0x15); e.rel32(token_addr)
    e.b(0x48, 0x8D, 0x8D); e.d32(P_PARSER_TOKEN)
    e.b(0xE8); e.rel32(EXE_BASE + P_STRCMP)
    e.b(0x85, 0xC0)
    nxt = e.jne32()
    e.b(0xC7, 0x85); e.d32(P_PARSER_BTYPE); e.d32(d["btype"])
    e.b(0xC7, 0x85); e.d32(P_PARSER_DTYPE); e.d32(d["type"])
    e.b(0xE9); e.rel32(EXE_BASE + P_PARSER_DONE)
    e.land(nxt)


def dispatch_case(e, d):
    e.b(0x83, 0xBE); e.d32(P_DEP_TYPE_FIELD); e.b(d["type"])
    nxt = e.jne32()
    e.b(0xF2, 0x0F, 0x10, 0x44, 0x24, 0x40)
    e.b(0xF2, 0x0F, 0x11, 0x45, 0x38)
    e.b(0x8B, 0x44, 0x24, 0x48)
    e.b(0x89, 0x45, 0x40)
    e.b(0x4C, 0x8B, 0x0D); e.rel32(EXE_BASE + P_GAMEOBJ)
    e.b(0x4D, 0x8B, 0x89); e.d32(P_MAP2_OFF if d["map"] == 2 else P_MAP1_OFF)
    e.b(0x4C, 0x8D, 0x45, 0x38)
    e.b(0x48, 0x8D, 0x95); e.d32(0xB0)
    e.b(0xE8); e.rel32(EXE_BASE + P_SAMPLER)
    e.b(0xF3, 0x0F, 0x10, 0x40, d["component"] * 4)
    e.b(0xF3, 0x0F, 0x11, 0x44, 0x24, 0x5C)
    e.b(0xE9); e.rel32(EXE_BASE + P_DISPATCH_TAIL)
    e.land(nxt)


def radius_case(e, d, slot):
    e.b(0x83, 0xF9, d["type"])
    nxt = e.jne32()
    e.b(0xF3, 0x0F, 0x10, 0x05); e.rel32(slot)
    e.b(0xC3)
    e.land(nxt)


# --- data ---------------------------------------------------------------
data = CAVE
tokens, radii = [], []
for d in DEPOSITS:
    tokens.append(data)
    data += len(d["token"]) + 1
data = (data + 3) & ~3
for d in DEPOSITS:
    radii.append(data)
    data += 4
assert data < CAVE + CAVE_CODE, "data region overflowed"

# --- code ---------------------------------------------------------------
e = Emit(CAVE + CAVE_CODE)

parser_cave = e.here
for i, d in enumerate(DEPOSITS):
    parser_case(e, d, tokens[i])
e.b(0x48, 0x8D, 0x15); e.rel32(EXE_BASE + P_STR_BAUXITE)
e.b(0x48, 0x8D, 0x8D); e.d32(P_PARSER_TOKEN)
e.b(0xE8); e.rel32(EXE_BASE + P_STRCMP)
e.b(0x85, 0xC0)
e.b(0x0F, 0x85); e.rel32(EXE_BASE + P_PARSER_NEXT)
e.b(0xC7, 0x85); e.d32(P_PARSER_BTYPE); e.d32(7)
e.b(0xC7, 0x85); e.d32(P_PARSER_DTYPE); e.d32(7)
e.b(0xE9); e.rel32(EXE_BASE + P_PARSER_DONE)

dispatch_cave = e.here
for d in DEPOSITS:
    dispatch_case(e, d)
e.b(0x83, 0xBE); e.d32(P_DEP_TYPE_FIELD); e.b(0x06)
e.b(0x0F, 0x85); e.rel32(EXE_BASE + P_DISPATCH_TAIL)
e.b(0xE9); e.rel32(EXE_BASE + P_DISPATCH_BODY6)

radius_cave = e.here
e.b(0x83, 0xF9, 0x09)
nxt = e.jne32()
e.b(0xF3, 0x0F, 0x10, 0x05); e.rel32(EXE_BASE + P_RADIUS_WATERSURF)
e.b(0xC3)
e.land(nxt)
for i, d in enumerate(DEPOSITS):
    radius_case(e, d, radii[i])
e.b(0x0F, 0x57, 0xC0)
e.b(0xC3)

print("cave %#x  data ends %#x  code %#x..%#x  (%d bytes used of %#x)"
      % (CAVE, data, CAVE + CAVE_CODE, e.here, e.here - CAVE, 0x2000))
print("parser %#x   dispatch %#x   radius %#x\n" % (parser_cave, dispatch_cave, radius_cave))

labels = {parser_cave: "PARSER", dispatch_cave: "DISPATCH", radius_cave: "RADIUS"}
named = {
    EXE_BASE + P_STRCMP: "strcmp",
    EXE_BASE + P_PARSER_NEXT: "parser_next_token",
    EXE_BASE + P_PARSER_DONE: "parser_done",
    EXE_BASE + P_SAMPLER: "SampleDeposit",
    EXE_BASE + P_DISPATCH_TAIL: "dispatch_tail(type7)",
    EXE_BASE + P_DISPATCH_BODY6: "dispatch_body(type6)",
}
tokmap = {a: DEPOSITS[i]["token"] for i, a in enumerate(tokens)}
radmap = {a: DEPOSITS[i]["name"] + ".radius" for i, a in enumerate(radii)}

md = Cs(CS_ARCH_X86, CS_MODE_64)
md.detail = False
for ins in md.disasm(bytes(e.buf), CAVE + CAVE_CODE):
    if ins.address in labels:
        print("\n=== %s ===" % labels[ins.address])
    note = ""
    for target, text in list(named.items()) + list(tokmap.items()) + list(radmap.items()):
        if ("%#x" % target).lstrip("0") in ins.op_str.replace("0x", "0x"):
            pass
    # resolve rip-relative and direct targets for annotation
    for target, text in list(named.items()):
        if hex(target) in ins.op_str:
            note = "   ; " + text
    for target, text in list(tokmap.items()) + list(radmap.items()):
        if hex(target) in ins.op_str:
            note = "   ; " + text
    if hex(EXE_BASE + P_GAMEOBJ) in ins.op_str:
        note = "   ; game object"
    if hex(EXE_BASE + P_RADIUS_WATERSURF) in ins.op_str:
        note = "   ; radius[water surface]"
    print("%012x  %-24s %s%s" % (ins.address, ins.mnemonic, ins.op_str, note))
