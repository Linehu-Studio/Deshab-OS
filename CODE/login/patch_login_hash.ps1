# patch_login_hash.ps1 — login.elf .text 防篡改哈希补丁
# 用法: powershell -File patch_login_hash.ps1 <login.elf>
# 计算 .text 段的 SHA256，写入 .hashseg 段（__expected_hash，32 字节）。
param([Parameter(Mandatory=$true)][string]$ElfPath)

if (-not (Test-Path $ElfPath)) { Write-Error "not found: $ElfPath"; exit 1 }

$bytes = [System.IO.File]::ReadAllBytes($ElfPath)

# ELF64 header: e_shoff @ 0x28 (u64), e_shentsize @ 0x3A (u16), e_shnum @ 0x3C (u16)
function U64($b, $o) { [UInt64]$b[$o] -bor ([UInt64]$b[$o+1] -shl 8) -bor ([UInt64]$b[$o+2] -shl 16) -bor ([UInt64]$b[$o+3] -shl 24) -bor ([UInt64]$b[$o+4] -shl 32) -bor ([UInt64]$b[$o+5] -shl 40) -bor ([UInt64]$b[$o+6] -shl 48) -bor ([UInt64]$b[$o+7] -shl 56) }
function U16($b, $o) { [UInt32]$b[$o] -bor ([UInt32]$b[$o+1] -shl 8) }
function U32($b, $o) { [UInt32]$b[$o] -bor ([UInt32]$b[$o+1] -shl 8) -bor ([UInt32]$b[$o+2] -shl 16) -bor ([UInt32]$b[$o+3] -shl 24) }

$shoff = U64 $bytes 0x28
$shentsize = U16 $bytes 0x3A
$shnum = U16 $bytes 0x3C

$textOff = 0; $textSize = 0; $hashOff = 0
for ($i = 0; $i -lt $shnum; $i++) {
    $sh = $shoff + $i * $shentsize
    $nameIdx = U32 $bytes $sh
    $shType = U32 $bytes ($sh + 4)
    $shOffset = U64 $bytes ($sh + 24)
    $shSize = U64 $bytes ($sh + 32)
    # sh_name indexes .shstrtab; resolve names via that section lazily
    if ($shType -eq 3 -and -not $shstr) { $shstrOff = $shOffset; $shstrSize = $shSize; $shstr = $bytes[$shstrOff..($shstrOff+$shstrSize-1)] }
}

function Get-Name([int]$idx) {
    $end = $idx
    while ($shstr[$end] -ne 0) { $end++ }
    [System.Text.Encoding]::ASCII.GetString($shstr, $idx, $end - $idx)
}

for ($i = 0; $i -lt $shnum; $i++) {
    $sh = $shoff + $i * $shentsize
    $nameIdx = U32 $bytes $sh
    $shType = U32 $bytes ($sh + 4)
    $shOffset = U64 $bytes ($sh + 24)
    $shSize = U64 $bytes ($sh + 32)
    $nm = Get-Name $nameIdx
    if ($nm -eq '.text' -and $shType -eq 1) { $textOff = $shOffset; $textSize = $shSize }
    if ($nm -eq '.hashseg') { $hashOff = $shOffset }
}

if ($textOff -eq 0 -or $hashOff -eq 0) { Write-Error 'section .text/.hashseg not found'; exit 1 }

$sha = [System.Security.Cryptography.SHA256]::Create()
$textBytes = New-Object byte[] $textSize
[Array]::Copy($bytes, [int]$textOff, $textBytes, 0, $textSize)
$hash = $sha.ComputeHash($textBytes)

[Array]::Copy($hash, 0, $bytes, [int]$hashOff, 32)
[System.IO.File]::WriteAllBytes($ElfPath, $bytes)
Write-Host "[patch_login_hash] .text($textSize B) SHA256 -> .hashseg @ $hashOff : $(($hash[0..3] | ForEach-Object { $_.ToString('x2') }) -join '')..."
exit 0
