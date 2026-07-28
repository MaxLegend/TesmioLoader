# Disassembles several address ranges in one headless run.
#
#   DisasmMany.py <out.asm> <start:end> [start:end ...]
#
# Ranges are hex, no 0x prefix needed. Each is emitted with a banner, so one
# invocation can cover every call site under investigation instead of paying
# the project-open cost per range.
#
# @category Analysis

args = getScriptArgs()
out_path = args[0]

space = currentProgram.getAddressFactory().getDefaultAddressSpace()
listing = currentProgram.getListing()
fm = currentProgram.getFunctionManager()

out = open(out_path, "w")
for spec in args[1:]:
    lo, hi = [int(x, 16) for x in spec.split(":")]
    fn = fm.getFunctionContaining(space.getAddress(lo))
    out.write("// ==== %08X .. %08X   in %s ====\n" % (lo, hi, fn.getName() if fn else "?"))

    addr = space.getAddress(lo)
    stop = space.getAddress(hi)
    while addr.compareTo(stop) < 0:
        ins = listing.getInstructionAt(addr)
        if ins is None:
            out.write("%s  (no instruction)\n" % addr)
            addr = addr.add(1)
            continue
        raw = " ".join("%02X" % (b & 0xFF) for b in ins.getBytes())
        out.write("%s  %-30s %s\n" % (addr, raw, ins.toString()))
        addr = addr.add(ins.getLength())
    out.write("\n")
    print("range %08X..%08X done" % (lo, hi))

out.close()
print("WROTE " + out_path)
