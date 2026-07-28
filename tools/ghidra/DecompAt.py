# Decompiles whatever functions contain the addresses given on the command line.
# Used to read the deposit sampler, found at runtime by hooking the texture
# vtable: the game samples a 2x2 texel block from four call sites at
# 0x1400084C2, 0x1400084E1, 0x14000851A, 0x140008539.
#
# @category Analysis

from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

args = getScriptArgs()
out_path = args[0]
targets = [int(a, 16) for a in args[1:]]

decomp = DecompInterface()
decomp.openProgram(currentProgram)
monitor = ConsoleTaskMonitor()

fm = currentProgram.getFunctionManager()
space = currentProgram.getAddressFactory().getDefaultAddressSpace()

done = set()
out = open(out_path, "w")
for t in targets:
    fn = fm.getFunctionContaining(space.getAddress(t))
    if fn is None:
        out.write("// 0x%X: no function\n\n" % t)
        print("MISS 0x%X" % t)
        continue

    key = fn.getEntryPoint().toString()
    if key in done:
        print("dup  0x%X -> %s" % (t, key))
        continue
    done.add(key)

    out.write("// ============================================================\n")
    out.write("// contains 0x%X : %s at %s, %d bytes\n" % (t, fn.getName(), key, fn.getBody().getNumAddresses()))
    out.write("// ============================================================\n")

    res = decomp.decompileFunction(fn, 600, monitor)
    if res.decompileCompleted():
        out.write(res.getDecompiledFunction().getC())
        print("OK   0x%X -> %s" % (t, key))
    else:
        out.write("// failed: %s\n" % res.getErrorMessage())
        print("FAIL 0x%X" % t)
    out.write("\n\n")

out.close()
print("WROTE " + out_path)
