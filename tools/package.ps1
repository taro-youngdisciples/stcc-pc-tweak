# リリース用の zip を作る。ゲームのファイル・改変 exe・dgVoodoo2 本体は決して含めない。
#
#   powershell -ExecutionPolicy Bypass -File tools\package.ps1 -Version 0.1.0
#
# 出力: dist\stcc-pc-tweak-<Version>.zip と、その SHA-256
# 中身: dinput.dll, stccfix.ini, dgVoodoo.conf, README.md, README.ja.md, LICENSE, THIRD_PARTY_NOTICES.md

param(
    [Parameter(Mandatory = $true)][ValidatePattern('^\d+\.\d+\.\d+([-.][0-9A-Za-z.]+)?$')][string]$Version,
    [string]$OutDir
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
if (-not $OutDir) { $OutDir = Join-Path $root 'dist' }

& (Join-Path $PSScriptRoot 'build.ps1') -Config Release

$name = "stcc-pc-tweak-$Version"
$stage = Join-Path $OutDir $name
$zip = Join-Path $OutDir "$name.zip"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force $stage | Out-Null

$files = [ordered]@{
    'dinput.dll'             = 'build\Release\dinput.dll'
    'stccfix.ini'            = 'src\stccfix\stccfix.ini'
    'dgVoodoo.conf'          = 'tools\dgvoodoo\dgVoodoo.conf'
    'README.md'              = 'README.md'
    'README.ja.md'           = 'README.ja.md'
    'LICENSE'                = 'LICENSE'
    'THIRD_PARTY_NOTICES.md' = 'THIRD_PARTY_NOTICES.md'
}
foreach ($dest in $files.Keys) {
    $src = Join-Path $root $files[$dest]
    if (-not (Test-Path $src)) { throw "見つからない: $src" }
    Copy-Item $src (Join-Path $stage $dest)
}

if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip
Remove-Item $stage -Recurse -Force

"作成: $zip"
Get-FileHash $zip -Algorithm SHA256 | ForEach-Object { "SHA-256: $($_.Hash)" }
