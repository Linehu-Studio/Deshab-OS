# patch_login_hash.ps1 - patch login.elf .text SHA256 into .hashseg
# usage: powershell -File patch_login_hash.ps1 [login.elf]
param([string]$ElfPath = "..\..\SYSTEM\system\user\use\login.elf")

if (-not (Test-Path $ElfPath)) { Write-Error ("not found: " + $ElfPath); exit 1 }

$bytes = [System.IO.File]::ReadAllBytes($ElfPath)

# ELF64 header: e_shoff @ 0x28 (u64), e_shentsize @ 0x3A (u16), e_shnum @ 0x3C (u16), e_shstrndx @ 0x3E (u16)
function U64([byte[]]$b, [int]$o) { [UInt64]$b[$o] -bor ([UInt64]$b[$o+1] -shl 8) -bor ([UInt64]$b[$o+2] -shl 16) -bor ([UInt64]$b[$o+3] -shl 24) -bor ([UInt64]$b[$o+4] -shl 32) -bor ([UInt64]$b[$o+5] -shl 40) -bor ([UInt64]$b[$o+6] -shl 48) -bor ([UInt64]$b[$o+7] -shl 56) }
function U16([byte[]]$b, [int]$o) { [UInt32]$b[$o] -bor ([UInt32]$b[$o+1] -shl 8) }
function U32([byte[]]$b, [int]$o) { [UInt32]$b[$o] -bor ([UInt32]$b[$o+1] -shl 8) -bor ([UInt32]$b[$o+2] -shl 16) -bor ([UInt32]$b[$o+3] -shl 24) }

$shoff = U64 $bytes 0x28
$shentsize = U16 $bytes 0x3A
$shnum = U16 $bytes 0x3C
$shstrndx = U16 $bytes 0x3E

$textOff = 0; $textSize = 0; $hashOff = 0
# e_shstrndx points at .shstrtab directly — picking the first SHT_STRTAB is wrong
# when .dynsym/.strtab exist (they come before .shstrtab and would garble names)
$shstrHdr = $shoff + $shstrndx * $shentsize
$shstrOff = U64 $bytes ($shstrHdr + 24)
$shstrSize = U64 $bytes ($shstrHdr + 32)
$shstr = $bytes[[int]$shstrOff..[int]($shstrOff+$shstrSize-1)]

# Hash range = first PT_LOAD segment (offset/filesz). This matches exactly what
# the DSK loader copies into memory and what the runtime hashes via
# __text_start..__text_end (the output section may include tail padding that
# the .text section header's sh_size does not cover — hashing sh_size caused
# LOGIN-E01 integrity FAIL whenever lld emitted padding after .text).
$phoff = U64 $bytes 0x20
$phentsize = U16 $bytes 0x36
$phnum = U16 $bytes 0x38
for ($i = 0; $i -lt $phnum; $i++) {
    $ph = $phoff + $i * $phentsize
    $pType = U32 $bytes $ph
    if ($pType -eq 1) { $textOff = U64 $bytes ($ph + 8); $textSize = U64 $bytes ($ph + 32); break }
}
if ($textOff -eq 0) { Write-Error 'PT_LOAD text segment not found'; exit 1 }

function Get-SectName([byte[]]$strtab, [int]$idx) {
    $end = $idx
    while ($strtab[$end] -ne 0) { $end++ }
    [System.Text.Encoding]::ASCII.GetString($strtab, $idx, $end - $idx)
}

for ($i = 0; $i -lt $shnum; $i++) {
    $sh = $shoff + $i * $shentsize
    $nameIdx = U32 $bytes $sh
    $shType = U32 $bytes ($sh + 4)
    $shOffset = U64 $bytes ($sh + 24)
    $shSize = U64 $bytes ($sh + 32)
    $nm = Get-SectName $shstr $nameIdx
    if ($nm -eq '.hashseg') { $hashOff = $shOffset }
}

if ($textOff -eq 0 -or $hashOff -eq 0) { Write-Error 'PT_LOAD text/.hashseg not found'; exit 1 }

$sha = [System.Security.Cryptography.SHA256]::Create()
$textBytes = New-Object byte[] $textSize
[Array]::Copy($bytes, [int]$textOff, $textBytes, 0, $textSize)
$hash = $sha.ComputeHash($textBytes)

[Array]::Copy($hash, 0, $bytes, [int]$hashOff, 32)
[System.IO.File]::WriteAllBytes($ElfPath, $bytes)
$hex = ($hash[0..3] | ForEach-Object { $_.ToString('x2') }) -join ''
Write-Host (".text(" + $textSize + " B) SHA256 -> .hashseg @ " + $hashOff + " : " + $hex + "...")
exit 0
