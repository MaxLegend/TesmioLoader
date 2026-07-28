# Dumps raw disassembly for an address range, with bytes, so a patch can be
# written against the actual instructions rather than the decompiler's guess.
#
# @category Analysis

args = getScriptArgs()
out_path = args[0]
start = int(args[1], 16)
end = int(args[2], 16)

space = currentProgram.getAddressFactory().getDefaultAddressSpace()
listing = currentProgram.getListing()

out = open(out_path, "w")
addr = space.getAddress(start)
stop = space.getAddress(end)

while addr.compareTo(stop) < 0:
    ins = listing.getInstructionAt(addr)
    if ins is None:
        out.write("%s  (no instruction)\n" % addr)
        addr = addr.add(1)
        continue

    raw = ins.getBytes()
    hexs = " ".join("%02X" % (b & 0xFF) for b in raw)
    out.write("%s  %-32s %s\n" % (addr, hexs, ins.toString()))
    addr = addr.add(ins.getLength())

out.close()
print("WROTE " + out_path)
