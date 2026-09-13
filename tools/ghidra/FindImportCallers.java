// 指定した import 関数の呼び出し元（サンク経由も含む）を列挙する。
// 使い方（headless）:
//   analyzeHeadless <projDir> STCC -process STCC.EXE -noanalysis -readOnly \
//     -scriptPath tools\ghidra -postScript FindImportCallers.java DirectInputCreateA DirectDrawCreate mciSendCommandA
// @category STCC
import java.util.HashSet;
import java.util.Set;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.Symbol;

public class FindImportCallers extends GhidraScript {

    @Override
    protected void run() throws Exception {
        String[] names = getScriptArgs();
        if (names.length == 0) {
            names = new String[] { "DirectInputCreateA", "DirectDrawCreate", "DirectDrawEnumerateA",
                "DirectSoundCreate", "mciSendCommandA", "timeGetTime" };
        }
        for (String name : names) {
            println("=== " + name);
            for (Symbol sym : currentProgram.getSymbolTable().getExternalSymbols(name)) {
                Set<Address> visited = new HashSet<>();
                // 外部シンボル → IAT ポインタ → (サンク関数) → 呼び出し元 を辿る
                for (Reference ref : getReferencesTo(sym.getAddress())) {
                    walk(ref.getFromAddress(), 0, visited);
                }
            }
        }
    }

    private void walk(Address from, int depth, Set<Address> visited) {
        if (depth > 4 || !visited.add(from)) {
            return;
        }
        Function f = getFunctionContaining(from);
        String indent = "  ".repeat(depth + 1);
        if (f == null) {
            // IAT エントリ等。そこへの参照をさらに辿る
            for (Reference r : getReferencesTo(from)) {
                walk(r.getFromAddress(), depth, visited);
            }
            return;
        }
        println(String.format("%s%s  in %s @ %s%s", indent, from, f.getName(), f.getEntryPoint(),
            f.isThunk() ? "  [thunk]" : ""));
        if (f.isThunk() || f.getBody().getNumAddresses() < 16) {
            for (Reference r : getReferencesTo(f.getEntryPoint())) {
                walk(r.getFromAddress(), depth + 1, visited);
            }
        }
    }
}
