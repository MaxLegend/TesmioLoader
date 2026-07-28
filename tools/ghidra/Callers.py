# Lists everything that calls the deposit sampler and decompiles those callers.
# The sampler itself only returns a colour; whoever turns that colour into a
# deposit type is the code that has to learn about copper.
#
# @category Analysis

from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

args = getScriptArgs()
out_path = args[0]
target = int(args[1], 16)
limit = int(args[2]) if len(args) > 2 else 6

space = currentProgram.getAddressFactory().getDefaultAddressSpace()
fm = currentProgram.getFunctionManager()
target_addr = space.getAddress(target)

refs = getReferencesTo(target_addr)
callers = []
for r in refs:
    fn = fm.getFunctionContaining(r.getFromAddress())
    if fn is None:
        continue
    key = fn.getEntryPoint().toString()
    if key not in [c[0] for c in callers]:
        callers.append((key, fn, r.getFromAddress()))

print("callers of 0x%X: %d" % (target, len(callers)))
for key, fn, site in callers:
    print("  %s  %s  (%d bytes)  call at %s" % (key, fn.getName(), fn.getBody().getNumAddresses(), site))

decomp = DecompInterface()
decomp.openProgram(currentProgram)
monitor = ConsoleTaskMonitor()

out = open(out_path, "w")
for key, fn, site in callers[:limit]:
    out.write("// ============================================================\n")
    out.write("// caller %s (%d bytes), call site %s\n" % (key, fn.getBody().getNumAddresses(), site))
    out.write("// ============================================================\n")
    res = decomp.decompileFunction(fn, 600, monitor)
    if res.decompileCompleted():
        out.write(res.getDecompiledFunction().getC())
    else:
        out.write("// failed: %s\n" % res.getErrorMessage())
    out.write("\n\n")
out.close()
print("WROTE " + out_path)
