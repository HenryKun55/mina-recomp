// Decompiles the named functions (mangled or demangled names, or 0x addresses)
// of the current program to stdout-like output file.
// args: <out file> <name|0xaddr>...
//@category Mina
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Symbol;
import java.io.PrintWriter;

public class DecompileFns extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        PrintWriter out = new PrintWriter(args[0]);
        DecompInterface d = new DecompInterface();
        d.openProgram(currentProgram);
        for (int i = 1; i < args.length; i++) {
            String n = args[i];
            Function f = null;
            if (n.startsWith("0x")) {
                Address a = toAddr(n);
                f = getFunctionContaining(a);
            } else {
                for (Symbol s : currentProgram.getSymbolTable().getSymbols(n)) {
                    f = getFunctionAt(s.getAddress());
                    if (f != null)
                        break;
                }
                if (f == null)
                    for (Function g : currentProgram.getFunctionManager().getFunctions(true))
                        if (g.getName(true).equals(n) || g.getName().equals(n)) {
                            f = g;
                            break;
                        }
            }
            if (f == null) {
                out.println("// " + n + ": not found\n");
                continue;
            }
            DecompileResults r = d.decompileFunction(f, 120, monitor);
            out.println("// " + f.getName(true) + " @ " + f.getEntryPoint());
            out.println(r.decompileCompleted() ? r.getDecompiledFunction().getC() : "// failed: " + r.getErrorMessage());
        }
        out.close();
    }
}
