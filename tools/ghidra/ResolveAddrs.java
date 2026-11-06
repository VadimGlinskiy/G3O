import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import ghidra.app.decompiler.*;
import java.io.*;
import java.util.*;

public class ResolveAddrs extends GhidraScript {
    public void run() throws Exception {
        String[] args = getScriptArgs();
        PrintWriter out = new PrintWriter(new FileWriter(args[0]));
        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        for (int i = 1; i < args.length; i++) {
            Address a = toAddr(args[i]);
            Function f = getFunctionContaining(a);
            out.println("=== " + args[i] + " -> " + (f == null ? "NONE" : f.getName() + " @ " + f.getEntryPoint() + " size " + f.getBody().getNumAddresses()));
            if (f == null) continue;
            Set<String> callees = new TreeSet<>();
            for (Function c : f.getCalledFunctions(monitor)) callees.add(c.getName() + "@" + c.getEntryPoint());
            out.println("callees: " + callees);
            if (f.getBody().getNumAddresses() < 3000) {
                DecompileResults r = di.decompileFunction(f, 60, monitor);
                if (r.decompileCompleted()) out.println(r.getDecompiledFunction().getC());
            }
        }
        out.close();
    }
}
