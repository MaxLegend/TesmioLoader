# Lists, without decompiling anything, every function that references each
# address given on the command line. Fast: use it to map the call graph before
# deciding what is worth handing to the decompiler.
#
#   Xrefs.py <out.txt> <addr> [addr...]
#
# @category Analysis

args = getScriptArgs()
out_path = args[0]
targets = [int(a, 16) for a in args[1:]]

space = currentProgram.getAddressFactory().getDefaultAddressSpace()
fm = currentProgram.getFunctionManager()

out = open(out_path, "w")
for t in targets:
    addr = space.getAddress(t)
    fn = fm.getFunctionContaining(addr)
    label = fn.getName() if fn else "?"
    out.write("=== refs to 0x%X (%s) ===\n" % (t, label))

    seen = {}
    for r in getReferencesTo(addr):
        src = r.getFromAddress()
        caller = fm.getFunctionContaining(src)
        if caller is None:
            out.write("  %s  <no function>  %s\n" % (src, r.getReferenceType()))
            continue
        key = caller.getEntryPoint().toString()
        seen.setdefault(key, [caller, []])[1].append("%s %s" % (src, r.getReferenceType()))

    for key in sorted(seen):
        caller, sites = seen[key]
        out.write("  %s  %s  (%d bytes)  %d site(s): %s\n"
                  % (key, caller.getName(), caller.getBody().getNumAddresses(),
                     len(sites), "; ".join(sites)))
    out.write("\n")
    print("0x%X: %d calling function(s)" % (t, len(seen)))

out.close()
print("WROTE " + out_path)
