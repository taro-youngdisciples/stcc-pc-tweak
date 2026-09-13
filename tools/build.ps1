# stccfix（dinput.dll プロキシ）をビルドし、必要ならゲームフォルダへ配置する。
#
#   powershell -ExecutionPolicy Bypass -File tools\build.ps1                 # Release ビルドのみ
#   powershell -ExecutionPolicy Bypass -File tools\build.ps1 -Deploy         # ビルドして配置
#   powershell -ExecutionPolicy Bypass -File tools\build.ps1 -Remove         # 配置した DLL を撤去
#
# stccfix.ini は配置先に既にあれば上書きしない（ユーザーの設定を保つ）。

param(
    [ValidateSet('Release', 'Debug')][string]$Config = 'Release',
    [switch]$Deploy,
    [switch]$Remove,
    [string]$GameDir = 'D:\Games\STCC'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$buildDir = Join-Path $root 'build'

if ($Remove) {
    foreach ($n in 'dinput.dll', 'dinput.pdb') {
        $p = Join-Path $GameDir $n
        if (Test-Path $p) { Remove-Item $p -Force; "撤去: $p" }
    }
    return
}

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.CMake.Project -property installationPath
$cmake = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
if (-not (Test-Path $cmake)) { throw "cmake が見つからない: $cmake" }
$major = ((& $vswhere -latest -products * -property installationVersion) -split '\.')[0]
$generator = switch ($major) { '18' { 'Visual Studio 18 2026' } '17' { 'Visual Studio 17 2022' } default { throw "未対応の VS: $major" } }

# PS 5.1 はネイティブコマンドの stderr を例外化するため、ビルド中だけ Continue にする
$ErrorActionPreference = 'Continue'
if (-not (Test-Path (Join-Path $buildDir 'CMakeCache.txt'))) {
    & $cmake -S $root -B $buildDir -G $generator -A Win32
    if ($LASTEXITCODE -ne 0) { throw 'cmake configure 失敗' }
}
& $cmake --build $buildDir --config $Config -- /nologo /v:minimal
if ($LASTEXITCODE -ne 0) { throw 'ビルド失敗' }
$ErrorActionPreference = 'Stop'

$dll = Join-Path $buildDir "$Config\dinput.dll"
"ビルド成果物: $dll"

if ($Deploy) {
    Copy-Item $dll (Join-Path $GameDir 'dinput.dll') -Force
    $pdb = Join-Path $buildDir "$Config\dinput.pdb"
    if (Test-Path $pdb) { Copy-Item $pdb (Join-Path $GameDir 'dinput.pdb') -Force }
    $ini = Join-Path $GameDir 'stccfix.ini'
    if (-not (Test-Path $ini)) { Copy-Item (Join-Path $root 'src\stccfix\stccfix.ini') $ini }
    "配置: $GameDir\dinput.dll"
}
