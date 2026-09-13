# 描画ラッパー（dgVoodoo2）をゲームフォルダに配置/撤去して、素の挙動と比較できるようにする。
#
#   powershell -ExecutionPolicy Bypass -File tools\wrapper.ps1 status
#   powershell -ExecutionPolicy Bypass -File tools\wrapper.ps1 enable
#   powershell -ExecutionPolicy Bypass -File tools\wrapper.ps1 disable
#
# 設定は tools\dgvoodoo\dgVoodoo.conf（リポジトリ管理）を正とし、enable 時にコピーする。

param(
    [Parameter(Mandatory = $true, Position = 0)][ValidateSet('status', 'enable', 'disable')][string]$Action,
    [string]$GameDir = 'D:\Games\STCC',
    [string]$DgVoodooDir = 'D:\Games\STCC_work\dgVoodoo2'
)

$ErrorActionPreference = 'Stop'
$conf = Join-Path $PSScriptRoot 'dgvoodoo\dgVoodoo.conf'
$files = @(
    @{ Src = Join-Path $DgVoodooDir 'MS\x86\DDraw.dll';  Dst = Join-Path $GameDir 'DDraw.dll' },
    @{ Src = Join-Path $DgVoodooDir 'MS\x86\D3DImm.dll'; Dst = Join-Path $GameDir 'D3DImm.dll' },
    @{ Src = $conf;                                       Dst = Join-Path $GameDir 'dgVoodoo.conf' }
)

switch ($Action) {
    'status' {
        foreach ($f in $files) {
            '{0,-14} {1}' -f (Split-Path $f.Dst -Leaf), $(if (Test-Path $f.Dst) { 'present' } else { '-' })
        }
    }
    'enable' {
        foreach ($f in $files) {
            if (-not (Test-Path $f.Src)) { throw "見つからない: $($f.Src)" }
            Copy-Item $f.Src $f.Dst -Force
            "配置: $($f.Dst)"
        }
    }
    'disable' {
        foreach ($f in $files) {
            if (Test-Path $f.Dst) { Remove-Item $f.Dst -Force; "撤去: $($f.Dst)" }
        }
    }
}
