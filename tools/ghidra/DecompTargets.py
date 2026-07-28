# Decompiles the functions this investigation has pinned down, so the deposit
# map's fate can be read rather than guessed at.
#
#   0x140005920  opens resourcemap.dds, resourcemap2.dds and resourcemap2default
#                - whatever consumes those pixels is in here
#   0x140007C20  small companion that also names both maps
#   0x1400BFFF4  knows exactly the seven texture-backed deposit types
#   0x1402AA7C0  ResourceGet, already hooked - included as a sanity check that
#                the decompiler output lines up with what we learned at runtime
#
# @category Analysis

import os
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

TARGETS = [
    (0x140005920, "map loader (opens both resource maps)"),
    (0x140007C20, "map companion"),
    (0x1400BFFF4, "deposit type -> name"),
    (0x1402AA7C0, "ResourceGet (known good, for comparison)"),
]

args = getScriptArgs()
out_path = args[0] if len(args) > 0 else "decomp_out.c"

decomp = DecompInterface()
decomp.openProgram(currentProgram)
monitor = ConsoleTaskMonitor()

fm = currentProgram.getFunctionManager()
space = currentProgram.getAddressFactory().getDefaultAddressSpace()

out = open(out_path, "w")
for addr_val, note in TARGETS:
    addr = space.getAddress(addr_val)
    fn = fm.getFunctionContaining(addr)

    out.write("// ============================================================\n")
    out.write("// 0x%X  %s\n" % (addr_val, note))
    if fn is None:
        out.write("// no function defined here\n\n")
        print("MISS 0x%X" % addr_val)
        continue

    out.write("// entry %s, body %s\n" % (fn.getEntryPoint(), fn.getBody().getNumAddresses()))
    out.write("// ============================================================\n")

    res = decomp.decompileFunction(fn, 600, monitor)
    if res.decompileCompleted():
        out.write(res.getDecompiledFunction().getC())
        print("OK   0x%X  %s" % (addr_val, fn.getName()))
    else:
        out.write("// decompilation failed: %s\n" % res.getErrorMessage())
        print("FAIL 0x%X  %s" % (addr_val, res.getErrorMessage()))
    out.write("\n\n")

out.close()
print("WROTE " + out_path)
