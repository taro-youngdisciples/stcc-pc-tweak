// 指定アドレス（グローバル変数など）への参照を、読み/書きの別と所属関数つきで列挙する。
// 使い方（headless）:
//   powershell -File tools\ghidra\run-headless.ps1 FindDataRefs.java 00568930 0056c1a0
// @category STCC
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.symbol.Reference;

public class FindDataRefs extends GhidraScript {

    @Override
    protected void run() throws Exception {
        for (String arg : getScriptArgs()) {
            Address target = toAddr(arg);
            println("=== " + target);
            for (Reference ref : getReferencesTo(target)) {
                Address from = ref.getFromAddress();
                Function f = getFunctionContaining(from);
                Instruction insn = getInstructionAt(from);
                println(String.format("  %s %-6s %-28s %s", from, ref.getReferenceType().isWrite() ? "WRITE" : "read",
                    f == null ? "(no function)" : f.getName() + "@" + f.getEntryPoint(),
                    insn == null ? "" : insn.toString()));
            }
        }
    }
}
