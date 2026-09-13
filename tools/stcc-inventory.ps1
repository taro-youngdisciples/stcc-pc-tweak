# STCC (Windows版) ファイル棚卸しスクリプト
#
# 使い方:
#   powershell -ExecutionPolicy Bypass -File tools\stcc-inventory.ps1 -Source F:\ -Name disc
#   powershell -ExecutionPolicy Bypass -File tools\stcc-inventory.ps1 -Source "D:\Games\STCC" -Name installed
#
# 出力 (リポジトリ直下の inventory\ 、git 管理外):
#   <Name>.txt  人間向けレポート (ツリー / 拡張子集計 / マジックバイト / PE情報 / テキスト系ファイル)
#   <Name>.csv  機械比較用 (RelPath, Bytes, Modified, SHA256)  ← ディスクとインストール先の差分取りに使う
#
# 差分の取り方:
#   powershell -ExecutionPolicy Bypass -File tools\stcc-inventory.ps1 -Compare disc,installed

param(
    [string]$Source,
    [string]$Name,
    [string[]]$Compare,
    [string]$OutDir = (Join-Path $PSScriptRoot '..\inventory'),
    # 同梱の再配布物 (DirectX / IE4) は既定で除外。含めたい場合は -IncludeRedist
    [switch]$IncludeRedist
)

$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force $OutDir | Out-Null
$OutDir = (Resolve-Path $OutDir).Path

# ------------------------------------------------------------------
# 比較モード
# ------------------------------------------------------------------
if ($Compare) {
    # powershell -File 経由だと "a,b" が1つの文字列で渡るので分割する
    $Compare = @($Compare | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
    if ($Compare.Count -ne 2) { throw '-Compare には2つの Name を指定してください (例: -Compare disc,installed)' }
    $left  = Import-Csv (Join-Path $OutDir "$($Compare[0]).csv")
    $right = Import-Csv (Join-Path $OutDir "$($Compare[1]).csv")
    $l = @{}; $left  | ForEach-Object { $l[$_.RelPath.ToLower()] = $_ }
    $r = @{}; $right | ForEach-Object { $r[$_.RelPath.ToLower()] = $_ }
    $out = Join-Path $OutDir "diff_$($Compare[0])_$($Compare[1]).txt"
    $lines = @("# diff  $($Compare[0])  ->  $($Compare[1])", '')
    $lines += "## $($Compare[1]) にのみ存在 (インストール/実行時の生成物候補)"
    $lines += $r.Keys | Where-Object { -not $l.ContainsKey($_) } | Sort-Object | ForEach-Object { "  + $($r[$_].RelPath)  [$($r[$_].Bytes)]" }
    $lines += '', "## $($Compare[0]) にのみ存在 (コピーされなかったもの)"
    $lines += $l.Keys | Where-Object { -not $r.ContainsKey($_) } | Sort-Object | ForEach-Object { "  - $($l[$_].RelPath)  [$($l[$_].Bytes)]" }
    $lines += '', '## 両方にあるが内容が異なる'
    $lines += $l.Keys | Where-Object { $r.ContainsKey($_) -and $l[$_].SHA256 -ne $r[$_].SHA256 } | Sort-Object | ForEach-Object { "  * $($l[$_].RelPath)  [$($l[$_].Bytes) -> $($r[$_].Bytes)]" }
    $lines | Out-File $out -Encoding UTF8
    Write-Host "完了: $out"
    return
}

if (-not $Source -or -not $Name) { throw '-Source と -Name を指定してください' }
$Source = (Resolve-Path $Source).Path
if (-not $Source.EndsWith('\')) { $Source += '\' }
$txt = Join-Path $OutDir "$Name.txt"
$csv = Join-Path $OutDir "$Name.csv"

function Write-Out($text) { $text | Out-File $txt -Append -Encoding UTF8 }

function Get-Head([string]$path, [int]$n = 64) {
    try {
        $fs = [System.IO.File]::OpenRead($path)
        try {
            $buf = New-Object byte[] $n
            $read = $fs.Read($buf, 0, $n)
            if ($read -le 0) { return @() }
            return $buf[0..($read - 1)]
        } finally { $fs.Close() }
    } catch { return @() }
}

function ConvertTo-HexText($bytes)   { if ($bytes.Count -eq 0) { '(empty)' } else { ($bytes | ForEach-Object { $_.ToString('X2') }) -join ' ' } }
function ConvertTo-AsciiText($bytes) { if ($bytes.Count -eq 0) { '(empty)' } else { -join ($bytes | ForEach-Object { if ($_ -ge 32 -and $_ -lt 127) { [char]$_ } else { '.' } }) } }

# PE ヘッダを読み、種別・マシン・リンク日時・インポートDLL一覧を返す
function Get-PeInfo([byte[]]$b) {
    if ($b.Length -lt 0x40 -or $b[0] -ne 0x4D -or $b[1] -ne 0x5A) { return 'not MZ' }
    $pe = [BitConverter]::ToInt32($b, 0x3C)
    if ($pe -le 0 -or $pe + 24 -ge $b.Length) { return 'MZ (DOS)' }
    $sig = [Text.Encoding]::ASCII.GetString($b, $pe, 2)
    if ($sig -eq 'NE') { return 'NE (16bit Windows)' }
    if ($sig -eq 'LE') { return 'LE (VxD)' }
    if ($sig -ne 'PE') { return "MZ (sig=$sig)" }
    $machine = [BitConverter]::ToUInt16($b, $pe + 4)
    $nsec    = [BitConverter]::ToUInt16($b, $pe + 6)
    $ts      = [BitConverter]::ToUInt32($b, $pe + 8)
    $optSize = [BitConverter]::ToUInt16($b, $pe + 20)
    $opt     = $pe + 24
    $secs = for ($i = 0; $i -lt $nsec; $i++) {
        $s = $opt + $optSize + 40 * $i
        [pscustomobject]@{
            Name = [Text.Encoding]::ASCII.GetString($b, $s, 8).TrimEnd([char]0)
            VSize = [BitConverter]::ToUInt32($b, $s + 8); VA = [BitConverter]::ToUInt32($b, $s + 12)
            RawSize = [BitConverter]::ToUInt32($b, $s + 16); RawPtr = [BitConverter]::ToUInt32($b, $s + 20)
        }
    }
    $rva2off = { param($rva) foreach ($s in $secs) { if ($rva -ge $s.VA -and $rva -lt $s.VA + [Math]::Max($s.VSize, $s.RawSize)) { return [int]($rva - $s.VA + $s.RawPtr) } }; -1 }
    $cstr = { param($o) $e = $o; while ($e -lt $b.Length -and $b[$e] -ne 0) { $e++ }; [Text.Encoding]::ASCII.GetString($b, $o, $e - $o) }
    $dlls = @()
    $impRva = [BitConverter]::ToUInt32($b, $opt + 104)
    $d = & $rva2off $impRva
    while ($d -gt 0 -and $d + 20 -le $b.Length) {
        $nameRva = [BitConverter]::ToUInt32($b, $d + 12)
        if ($nameRva -eq 0) { break }
        $o = & $rva2off $nameRva
        if ($o -lt 0) { break }
        $dlls += & $cstr $o
        $d += 20
    }
    $secText = ($secs | ForEach-Object { '{0}(0x{1:X})' -f $_.Name, $_.VSize }) -join ' '
    'PE machine=0x{0:X4}{1} linked={2:yyyy-MM-dd} sections=[{3}] imports=[{4}]' -f $machine,
        $(if ($machine -eq 0x14C) { ' (i386)' } else { '' }),
        [DateTimeOffset]::FromUnixTimeSeconds($ts).UtcDateTime, $secText, ($dlls -join ', ')
}

# ------------------------------------------------------------------
"STCC inventory  /  source: $Source  /  generated: $(Get-Date -Format 'yyyy-MM-dd HH:mm')" | Out-File $txt -Encoding UTF8

$files = Get-ChildItem $Source -Recurse -File -Force -ErrorAction SilentlyContinue
if (-not $IncludeRedist) {
    $files = $files | Where-Object { $_.FullName.Substring($Source.Length) -notmatch '^(DirectX|IE40Jpn)\\' }
    Write-Out '(DirectX\ と IE40Jpn\ は除外。含めるには -IncludeRedist)'
}

# Get-FileHash は PS 5.1 のパイプライン内で $_ を取り違えるため .NET で直接計算する
$sha = [Security.Cryptography.SHA256]::Create()
function Get-Sha256([string]$path) {
    $fs = [IO.File]::OpenRead($path)
    try { -join ($sha.ComputeHash($fs) | ForEach-Object { $_.ToString('X2') }) } finally { $fs.Close() }
}

$rows = foreach ($f in $files) {
    [pscustomobject]@{
        RelPath  = $f.FullName.Substring($Source.Length)
        Bytes    = $f.Length
        Modified = $f.LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss')
        SHA256   = Get-Sha256 $f.FullName
    }
}
$rows = $rows | Sort-Object RelPath
$rows | Export-Csv $csv -NoTypeInformation -Encoding UTF8

# ------------------------------------------------------------------
Write-Out "`n===== 1. FILE TREE ====="
$rows | Select-Object RelPath, Bytes, Modified, @{n='SHA256(16)'; e={ $_.SHA256.Substring(0, 16) }} |
    Format-Table -AutoSize | Out-String -Width 220 | Out-File $txt -Append -Encoding UTF8

# ------------------------------------------------------------------
Write-Out "`n===== 2. EXTENSION SUMMARY ====="
$files | Group-Object { $_.Extension.ToLower() } |
    Select-Object @{n='Ext'; e={ if ($_.Name) { $_.Name } else { '(none)' } }}, Count,
                  @{n='TotalMB'; e={ [math]::Round((($_.Group | Measure-Object Length -Sum).Sum) / 1MB, 2) }} |
    Sort-Object TotalMB -Descending |
    Format-Table -AutoSize | Out-String -Width 200 | Out-File $txt -Append -Encoding UTF8

# ------------------------------------------------------------------
Write-Out "`n===== 3. MAGIC BYTES (per extension, up to 3 samples) ====="
$files | Group-Object { $_.Extension.ToLower() } | ForEach-Object {
    $ext = if ($_.Name) { $_.Name } else { '(none)' }
    Write-Out "--- $ext  ($($_.Count) files) ---"
    $_.Group | Select-Object -First 3 | ForEach-Object {
        $h = Get-Head $_.FullName 64
        Write-Out "  $($_.FullName.Substring($Source.Length))  [$($_.Length) bytes]"
        Write-Out "    HEX: $(ConvertTo-HexText $h)"
        Write-Out "    ASC: $(ConvertTo-AsciiText $h)"
    }
    Write-Out ''
}

# ------------------------------------------------------------------
Write-Out "`n===== 4. EXECUTABLES (exe / dll / ocx / vxd / drv) ====="
$files | Where-Object { $_.Extension -match '^\.(exe|dll|ocx|vxd|drv)$' } | ForEach-Object {
    Write-Out "--- $($_.FullName.Substring($Source.Length))  [$($_.Length) bytes] ---"
    try { Write-Out ('  ' + (Get-PeInfo ([IO.File]::ReadAllBytes($_.FullName)))) }
    catch { Write-Out "  (読み取り失敗: $($_.Exception.Message))" }
}

# ------------------------------------------------------------------
Write-Out "`n===== 5. TEXT-ISH FILES (ini / cfg / txt / inf / bat / reg) ====="
$sjis = [Text.Encoding]::GetEncoding(932)
$files | Where-Object { $_.Extension -match '^\.(ini|cfg|txt|inf|bat|reg)$' -and $_.Length -lt 32768 } | ForEach-Object {
    Write-Out "--- $($_.FullName.Substring($Source.Length)) ---"
    try { ($sjis.GetString([IO.File]::ReadAllBytes($_.FullName)) -split "`r?`n" | Select-Object -First 80) -join "`n" | Out-File $txt -Append -Encoding UTF8 }
    catch { Write-Out '  (読み取り失敗)' }
    Write-Out ''
}

Write-Host "完了: $txt"
Write-Host "      $csv"
