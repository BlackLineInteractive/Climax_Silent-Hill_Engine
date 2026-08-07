// Reports the scale of the executable: how much of it is already accounted for
// by the handlers this project recovered, and how much is not.
//@category Climax
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;

public class GhidraStats extends GhidraScript {
    @Override
    public void run() throws Exception {
        FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
        long count = 0, bytes = 0, biggest = 0;
        String biggestName = "";
        while (it.hasNext()) {
            Function f = it.next();
            long len = f.getBody().getNumAddresses();
            count++;
            bytes += len;
            if (len > biggest) { biggest = len; biggestName = f.getName(); }
        }
        println("[stats] functions: " + count);
        println("[stats] bytes in functions: " + bytes);
        println("[stats] largest: " + biggestName + " (" + biggest + " bytes)");
        println("[stats] instructions: " + currentProgram.getListing().getNumInstructions());
    }
}
