// Ghidra script: decompiles the functions this project has already located.
//
// Java rather than Python because Ghidra 12 only runs Python scripts under
// PyGhidra, which is not installed here; a .java script needs nothing extra.
//
//   .../support/analyzeHeadless <projDir> <projName> \
//       -import game-iso/SHO/SLES_551.47 \
//       -scriptPath ./tools \
//       -postScript GhidraDecompile.java <addressList> <outDir>
//
// Neither target needs a loader extension. Ghidra's MIPS:LE:32:default is the
// language it maps to IDA's r5900l, and the Wii's Gekko is a PowerPC 750
// derivative covered by PowerPC:BE:32:default. Only R5900 quadword/MMI and
// Gekko paired-single instructions fall outside, and those hardly appear in
// compiler output for game logic.
//
// The address list is `0xADDRESS Name` per line -- the format sho_attrs.py and
// attrmap.py already emit, so the decompiler is aimed straight at the handlers
// instead of trawling a million instructions.
//@category Climax
import java.io.File;
import java.io.PrintWriter;
import java.nio.file.Files;
import java.nio.file.Paths;
import java.util.List;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;

public class GhidraDecompile extends GhidraScript {

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 2) {
            println("[decompile] need <addressList> <outDir>");
            return;
        }
        File outDir = new File(args[1]);
        if (!outDir.isDirectory() && !outDir.mkdirs()) {
            println("[decompile] cannot create " + outDir);
            return;
        }

        List<String> lines = Files.readAllLines(Paths.get(args[0]));
        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);

        int done = 0, failed = 0;
        for (String raw : lines) {
            String line = raw.trim();
            if (line.isEmpty() || line.startsWith("#")) continue;
            String[] parts = line.split("\\s+", 2);
            String addrText = parts[0];
            String name = parts.length > 1 ? parts[1] : addrText;

            Address addr;
            try {
                addr = currentProgram.getAddressFactory()
                        .getDefaultAddressSpace().getAddress(addrText);
            } catch (Exception e) {
                println("[decompile] bad address " + addrText);
                failed++;
                continue;
            }

            Function fn = getFunctionContaining(addr);
            if (fn == null) {
                // Analysis carved nothing here; give the decompiler a function
                // to work with rather than skipping the address.
                fn = createFunction(addr, name);
            }
            if (fn == null) {
                failed++;
                continue;
            }

            DecompileResults res = decomp.decompileFunction(fn, 120, monitor);
            if (!res.decompileCompleted()) {
                failed++;
                continue;
            }

            String safe = name.replaceAll("[^A-Za-z0-9_.-]", "_");
            try (PrintWriter w = new PrintWriter(new File(outDir, safe + ".c"))) {
                w.println("/* " + name + "  @ " + addrText);
                w.println(" * Decompiled by Ghidra from the retail executable.");
                w.println(" * Address recovered by tools/sho_attrs.py.");
                w.println(" */");
                w.println();
                w.print(res.getDecompiledFunction().getC());
            }
            done++;
        }

        decomp.dispose();
        println("[decompile] wrote " + done + " functions, " + failed + " failed");
    }
}
