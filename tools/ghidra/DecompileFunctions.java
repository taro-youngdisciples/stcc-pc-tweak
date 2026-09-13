// 指定アドレスを含む関数を逆コンパイルして標準出力に出す。
// 使い方（headless）:
//   powershell -File tools\ghidra\run-headless.ps1 DecompileFunctions.java 00468040 00435390
// @category STCC
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;

public class DecompileFunctions extends GhidraScript {

    @Override
    protected void run() throws Exception {
        DecompInterface ifc = new DecompInterface();
        ifc.openProgram(currentProgram);
        try {
            for (String arg : getScriptArgs()) {
                Address addr = toAddr(arg);
                Function f = getFunctionContaining(addr);
                if (f == null) {
                    println("### " + arg + ": 関数が見つからない");
                    continue;
                }
                DecompileResults res = ifc.decompileFunction(f, 60, monitor);
                println("### " + f.getName() + " @ " + f.getEntryPoint());
                if (res.decompileCompleted()) {
                    // println は1行ずつ出したほうが run-headless.ps1 のフィルタに乗る
                    for (String line : res.getDecompiledFunction().getC().split("\n")) {
                        println("| " + line);
                    }
                } else {
                    println("  decompile failed: " + res.getErrorMessage());
                }
            }
        } finally {
            ifc.dispose();
        }
    }
}
