# read_usb_logs.ps1 - Read Deshab USB disk, parse GPT+FAT32, extract test logs
#
# Deshab 实机测试日志回读工具：U 盘（GPT ESP + FAT32）在 Windows 上无法
# 直接挂载（removable media 不支持盘符分配），此脚本以管理员权限直读
# 物理磁盘，解析 GPT 分区表 + FAT32 文件系统，把 SYSTEM/DESHAB64/DEV/ 下的
# 日志文件（BOOTLOG.TXT、BOOTxx.TXT、LOG 等）和根目录 DEVLOG 提取到本地。
#
# Usage (admin PowerShell):
#   powershell -ExecutionPolicy Bypass -File "CODE\tools\read_usb_logs.ps1"
#   powershell -ExecutionPolicy Bypass -File "CODE\tools\read_usb_logs.ps1" -DiskNumber 2
#   powershell -ExecutionPolicy Bypass -File "CODE\tools\read_usb_logs.ps1" -OutDir ".\logs"
#
# 输出目录: .build_tmp\usb_logs\ (或 -OutDir 指定)
param(
    [int]$DiskNumber = 1,
    [string]$OutDir = 'd:\Code\DEAICUP\Deshab\.build_tmp\usb_logs'
)

$ErrorActionPreference = 'Stop'
$diskPath = '\\.\PhysicalDrive' + $DiskNumber

# ---- 检查管理员权限（打开物理磁盘需要） ----
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    Write-Host "[ERROR] Administrator privileges required to open $diskPath" -ForegroundColor Red
    Write-Host "        Run this script from an elevated PowerShell (Win+X -> Terminal (Admin))."
    exit 1
}

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$script:fs = $null
$script:partStart = 0
$script:dataSec = 0
$script:rsvd = 0
$script:spc = 0
$script:csize = 0

function Read-Cluster([uint32]$clus) {
    $lba = $dataSec + ($clus - 2) * $spc
    $fs.Position = ([int64]$partStart + $lba) * 512
    $b = [byte[]]::new($csize)
    [void]$fs.Read($b, 0, $csize)
    return ,$b
}

function Read-FatEntry([uint32]$c) {
    $fatSec = [Math]::Floor(($c * 4) / 512)
    $fs.Position = ([int64]$partStart + $rsvd + $fatSec) * 512
    $fb = [byte[]]::new(512)
    [void]$fs.Read($fb, 0, 512)
    return ([BitConverter]::ToUInt32($fb, ($c * 4) % 512) -band 0x0FFFFFFF)
}

function Decode-Short([byte[]]$n) {
    $s=''; for($i=0;$i -lt 8 -and $n[$i] -ne 0x20 -and $n[$i] -ne 0;$i++){$s+=[char]$n[$i]}
    $e=''; for($i=8;$i -lt 11 -and $n[$i] -ne 0x20 -and $n[$i] -ne 0;$i++){$e+=[char]$n[$i]}
    if($e){return "$s.$e"}else{return $s}
}

function Get-EntryCluster([byte[]]$buf, [int]$o) {
    $clow = [BitConverter]::ToUInt16($buf, $o + 26)
    $chigh = [BitConverter]::ToUInt16($buf, $o + 20)
    return ($clow -bor ([int]$chigh -shl 16))
}

function Read-File([uint32]$firstClus, [uint32]$size) {
    if ($size -eq 0) { return ,([byte[]]::new(0)) }
    $data = [byte[]]::new($size)
    $c = $firstClus; $off = 0; $remaining = $size
    while ($c -ge 2 -and $c -lt 0x0FFFFFF8 -and $remaining -gt 0) {
        $lba = $dataSec + ($c - 2) * $spc
        $fs.Position = ([int64]$partStart + $lba) * 512
        $chunk = [byte[]]::new($csize)
        [void]$fs.Read($chunk, 0, $csize)
        $n = [Math]::Min($csize, $remaining)
        [Array]::Copy($chunk, 0, $data, $off, $n)
        $off += $n; $remaining -= $n
        $c = Read-FatEntry $c
    }
    return ,$data
}

function Scan-Dir([uint32]$dirClus) {
    $result = @{}
    $c = $dirClus
    while ($c -ge 2 -and $c -lt 0x0FFFFFF8) {
        $buf = Read-Cluster $c
        for ($e = 0; $e -lt ($csize / 32); $e++) {
            $o = $e * 32
            if ($buf[$o] -eq 0) { return $result }
            if ($buf[$o] -eq 0xE5) { continue }
            $at = $buf[$o + 11]
            if (($at -band 0x0F) -eq 0x0F) { continue }   # LFN entry, skip
            if ($at -band 0x08) { continue }              # volume label
            $name = Decode-Short $buf[$o..($o + 10)]
            $result[$name] = @{ Cluster = (Get-EntryCluster $buf $o); Size = [BitConverter]::ToUInt32($buf, $o + 28); Attr = $at }
        }
        $c = Read-FatEntry $c
    }
    return $result
}

function Show-Head([byte[]]$data) {
    if (-not $data -or $data.Length -eq 0) { Write-Host "      [empty]"; return }
    $txt = [System.Text.Encoding]::ASCII.GetString($data)
    $len = [Math]::Min(400, $txt.Length)
    $clean = ''
    for ($i = 0; $i -lt $len; $i++) {
        $code = [int]$txt[$i]
        if (($code -ge 32 -and $code -le 126) -or $code -eq 10 -or $code -eq 13 -or $code -eq 9) { $clean += $txt[$i] } else { $clean += '.' }
    }
    Write-Host "      [head] $clean"
}

Write-Host "=== Open $diskPath ==="
$fs = [System.IO.File]::Open($diskPath, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)

$fs.Position = 512
$lba1 = [byte[]]::new(512); [void]$fs.Read($lba1, 0, 512)
$sig = [System.Text.Encoding]::ASCII.GetString($lba1, 0, 8)
Write-Host "LBA1 signature: '$sig'"
if ($sig -ne 'EFI PART') { Write-Host "[ERROR] Not a GPT disk"; $fs.Close(); exit 1 }

$fs.Position = 1024
$pe = [byte[]]::new(128); [void]$fs.Read($pe, 0, 128)
$partStart = [BitConverter]::ToUInt64($pe, 32)
Write-Host "ESP start LBA: $partStart"

$fs.Position = $partStart * 512
$bpb = [byte[]]::new(512); [void]$fs.Read($bpb, 0, 512)
$bps = [BitConverter]::ToUInt16($bpb, 11)
$spc = $bpb[13]
$rsvd = [BitConverter]::ToUInt16($bpb, 14)
$nf = $bpb[16]
$spf = [BitConverter]::ToUInt32($bpb, 36)
$rootClus = [BitConverter]::ToUInt32($bpb, 44)
$dataSec = $rsvd + $nf * $spf
$csize = $bps * $spc
Write-Host "BPB: bps=$bps spc=$spc rsvd=$rsvd nf=$nf spf=$spf root=$rootClus dataSec=$dataSec"

$root = Scan-Dir $rootClus
Write-Host "=== ROOT ==="
$root.Keys | Sort-Object | ForEach-Object { Write-Host ("  {0} clus={1} size={2}" -f $_, $root[$_].Cluster, $root[$_].Size) }

if (-not $root.ContainsKey('SYSTEM')) { Write-Host "[WARN] no SYSTEM dir"; $fs.Close(); exit 0 }
$sys = Scan-Dir $root['SYSTEM'].Cluster
if (-not $sys.ContainsKey('DESHAB64')) { Write-Host "[WARN] no DESHAB64 dir"; $fs.Close(); exit 0 }
$d64 = Scan-Dir $sys['DESHAB64'].Cluster

Write-Host "=== DESHAB64 ==="
$d64.Keys | Sort-Object | ForEach-Object { Write-Host ("  {0} size={1}" -f $_, $d64[$_].Size) }

if (-not $d64.ContainsKey('DEV')) { Write-Host "[WARN] no DEV dir"; $fs.Close(); exit 0 }
$dev = Scan-Dir $d64['DEV'].Cluster
Write-Host "=== DESHAB64/DEV ==="
$dev.Keys | Sort-Object | ForEach-Object { Write-Host ("  {0} size={1}" -f $_, $dev[$_].Size) }

Write-Host "=== EXTRACT to $OutDir ==="
foreach ($k in $dev.Keys) {
    if (($dev[$k].Attr -band 0x10) -ne 0) { continue }
    $data = Read-File $dev[$k].Cluster $dev[$k].Size
    $safe = ($k -replace '[^A-Za-z0-9._-]', '_')
    $outPath = Join-Path $OutDir $safe
    [System.IO.File]::WriteAllBytes($outPath, $data)
    Write-Host ("  -> {0} ({1} bytes)" -f $safe, $data.Length)
    Show-Head $data
}
foreach ($k in @('DEVLOG','TEST.TXT','COPY.TXT','MOVED.TXT','BOOTLOG')) {
    if ($root.ContainsKey($k)) {
        $data = Read-File $root[$k].Cluster $root[$k].Size
        $outPath = Join-Path $OutDir ("ROOT_" + $k)
        [System.IO.File]::WriteAllBytes($outPath, $data)
        Write-Host ("  [root] -> {0} ({1} bytes)" -f $k, $data.Length)
        Show-Head $data
    }
}
$fs.Close()
Write-Host "=== DONE ==="