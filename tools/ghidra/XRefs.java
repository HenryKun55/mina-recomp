// Lists the functions that reference each given address (data or code).
// args: <out file> <0xaddr>...
//@category Mina
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import java.io.PrintWriter;
import java.util.TreeMap;

public class XRefs extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        PrintWriter out = new PrintWriter(args[0]);
        for (int i = 1; i < args.length; i++) {
            TreeMap<String, Integer> fns = new TreeMap<>();
            for (Reference r : getReferencesTo(toAddr(args[i]))) {
                Function f = getFunctionContaining(r.getFromAddress());
                String n = (f == null ? "?" : f.getName(true) + " @ " + f.getEntryPoint()) + " (" + r.getReferenceType() + ")";
                fns.merge(n, 1, Integer::sum);
            }
            out.println("// references to " + args[i]);
            for (String n : fns.keySet())
                out.println(fns.get(n) + "  " + n);
        }
        out.close();
    }
}
