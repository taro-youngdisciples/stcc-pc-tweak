# 表示アスペクト比を切り替える。配置済みの stccfix.ini と dgVoodoo.conf を同時に書き換える。
#
#   powershell -ExecutionPolicy Bypass -File tools\aspect.ps1 16:9
#   powershell -ExecutionPolicy Bypass -File tools\aspect.ps1 32:9 -RenderHeight 960
#   powershell -ExecutionPolicy Bypass -File tools\aspect.ps1 4:3            # ワイド化なし
#
# dgVoodoo の描画解像度は「高さ RenderHeight × 比率」にする（既定 1920 = 4K 画面で窓の高さを 4 倍にした場合）。
# 32:9 など横に長い比率では窓の高さが小さくなるので、RenderHeight を窓に合わせて下げると無駄な描画が減る。

param(
    [Parameter(Mandatory = $true, Position = 0)][ValidatePattern('^\d+:\d+$')][string]$Ratio,
    [int]$RenderHeight = 1920,
    [string]$GameDir = 'D:\Games\STCC'
)

$ErrorActionPreference = 'Stop'
$w, $h = $Ratio -split ':' | ForEach-Object { [int]$_ }
$renderW = [int][Math]::Round($RenderHeight * $w / $h)

$ini = Join-Path $GameDir 'stccfix.ini'
$conf = Join-Path $GameDir 'dgVoodoo.conf'
foreach ($p in $ini, $conf) {
    if (-not (Test-Path $p)) { throw "見つからない: $p" }
}

# stccfix.ini は ASCII のまま書き戻す（Win32 プロファイル API が ANSI で読むため）
$lines = Get-Content $ini
if (-not ($lines -match '^AspectRatio=')) { throw "$ini に AspectRatio= の行がない" }
$lines -replace '^AspectRatio=.*$', "AspectRatio=$Ratio" | Set-Content $ini -Encoding ascii

$lines = Get-Content $conf
if (-not ($lines -match '^\s*Resolution\s*=')) { throw "$conf に Resolution の行がない" }
$lines -replace '^(\s*Resolution\s*=\s*).*$', "`${1}${renderW}x$RenderHeight" | Set-Content $conf -Encoding ascii

Select-String -Path $ini -Pattern '^AspectRatio=' | ForEach-Object { "stccfix.ini  : $($_.Line)" }
Select-String -Path $conf -Pattern '^\s*Resolution\s*=' | ForEach-Object { "dgVoodoo.conf: $($_.Line.Trim())" }
