// How much of the executable the game logic actually reaches.
//
// "7701 functions" is the wrong number to plan a port against: most of it is
// RenderWare, libc and maths that a port replaces rather than reimplements.
// The number that matters is the call-graph closure of the entry points this
// project already identified -- the attribute handlers and the class factories.
//@category Climax
import java.io.File;
import java.io.PrintWriter;
import java.nio.file.Files;
import java.nio.file.Paths;
import java.util.*;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;

public class GhidraReach extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        List<String> lines = Files.readAllLines(Paths.get(args[0]));

        Deque<Function> queue = new ArrayDeque<>();
        Set<Function> seen = new HashSet<>();
        int roots = 0;

        for (String raw : lines) {
            String line = raw.trim();
            if (line.isEmpty() || line.startsWith("#")) continue;
            Address a = currentProgram.getAddressFactory()
                    .getDefaultAddressSpace().getAddress(line.split("\\s+")[0]);
            Function f = getFunctionContaining(a);
            if (f != null && seen.add(f)) { queue.add(f); roots++; }
        }

        long bytes = 0;
        while (!queue.isEmpty()) {
            Function f = queue.poll();
            bytes += f.getBody().getNumAddresses();
            for (Function c : f.getCalledFunctions(monitor))
                if (seen.add(c)) queue.add(c);
        }

        println("[reach] roots: " + roots);
        println("[reach] reachable functions: " + seen.size());
        println("[reach] reachable bytes: " + bytes);

        if (args.length > 1) {
            try (PrintWriter w = new PrintWriter(new File(args[1]))) {
                List<String> names = new ArrayList<>();
                for (Function f : seen)
                    names.add(f.getEntryPoint() + " " + f.getName()
                              + " " + f.getBody().getNumAddresses());
                Collections.sort(names);
                for (String n : names) w.println(n);
            }
        }
    }
}
