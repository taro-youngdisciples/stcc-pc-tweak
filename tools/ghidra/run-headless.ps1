# Ghidra headless でスクリプトを実行するラッパー。
#   powershell -ExecutionPolicy Bypass -File tools\ghidra\run-headless.ps1 FindImportCallers.java [args...]
# 事前に analyzeHeadless で STCC.EXE をインポート・解析済みであること（プロジェクト: D:\Games\STCC_work\ghidra\STCC）。

param(
    [Parameter(Mandatory = $true, Position = 0)][string]$Script,
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$ScriptArgs,
    [string]$GhidraHome = 'D:\Tools\ghidra_12.1.3_PUBLIC',
    [string]$ProjectDir = 'D:\Games\STCC_work\ghidra',
    [string]$ProjectName = 'STCC',
    [string]$Program = 'STCC.EXE'
)

$ErrorActionPreference = 'Stop'
if (-not $env:JAVA_HOME) {
    $jdk = Get-ChildItem 'C:\Program Files\Eclipse Adoptium' -Directory -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($jdk) { $env:JAVA_HOME = $jdk.FullName }
}

$argv = @($ProjectDir, $ProjectName, '-process', $Program, '-noanalysis', '-readOnly',
          '-scriptPath', $PSScriptRoot, '-postScript', $Script) + $ScriptArgs

# Ghidra のログ行は除き、スクリプトの println 出力（"<Script> > ..."）だけを表示する
# PS 5.1 はネイティブコマンドの stderr を ErrorRecord 化するため、ここでは Stop にしない
$ErrorActionPreference = 'Continue'
& (Join-Path $GhidraHome 'support\analyzeHeadless.bat') @argv 2>&1 |
    ForEach-Object { "$_" } |
    Where-Object { $_ -match '\.java> ' -or $_ -match 'ERROR' } |
    ForEach-Object { $_ -replace '^.*?\.java> ', '' }
