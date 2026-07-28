# Finds defined strings containing a substring, and decompiles every function
# that references them.
# @category Analysis

from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

args = getScriptArgs()
out_path = args[0]
targets = args[1:]

listing = currentProgram.getListing()
refman = currentProgram.getReferenceManager()
fm = currentProgram.getFunctionManager()

decomp = DecompInterface()
decomp.openProgram(currentProgram)
monitor = ConsoleTaskMonitor()

out = open(out_path, "w")

done_funcs = set()

data_iter = listing.getDefinedData(True)
count = 0
for d in data_iter:
    count += 1
    if not d.hasStringValue():
        continue
    try:
        s = d.getValue()
    except:
        continue
    if s is None:
        continue
    try:
        sval = unicode(s)
    except:
        continue
    hit = None
    for t in targets:
        if t in sval:
            hit = t
            break
    if hit is None:
        continue

    addr = d.getAddress()
    safe_repr = repr(sval)
    out.write("\n==== STRING @ %s (matched %r): %s ====\n" % (addr, hit, safe_repr))
    refs = refman.getReferencesTo(addr)
    any_ref = False
    for r in refs:
        any_ref = True
        fromAddr = r.getFromAddress()
        fn = fm.getFunctionContaining(fromAddr)
        if fn is None:
            out.write("  ref from %s (no containing function)\n" % fromAddr)
            continue
        key = fn.getEntryPoint().toString()
        out.write("  ref from %s in function %s @ %s\n" % (fromAddr, fn.getName(), key))
        if key in done_funcs:
            continue
        done_funcs.add(key)
        res = decomp.decompileFunction(fn, 600, monitor)
        out.write("\n---- decompiled %s @ %s, %d bytes ----\n" % (fn.getName(), key, fn.getBody().getNumAddresses()))
        if res.decompileCompleted():
            out.write(res.getDecompiledFunction().getC())
        else:
            out.write("// failed: %s\n" % res.getErrorMessage())
        out.write("\n")
    if not any_ref:
        out.write("  (no references)\n")

out.write("\n\nscanned %d defined data items\n" % count)
out.close()
print("WROTE " + out_path)
