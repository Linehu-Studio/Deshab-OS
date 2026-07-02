$ErrorActionPreference = 'Stop'

$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$UtsmDir = Join-Path $Root 'CODE\UTSM'
$DskDir = Join-Path $Root 'CODE\dsk'
$SystemDir = Join-Path $Root 'SYSTEM'
$IsoDir = Join-Path $Root 'ISO'
$Output = Join-Path $SystemDir 'boot\utsm.elf'
$DskOutput = Join-Path $SystemDir 'system\deshab64\deshab.elf'
$ImagePath = Join-Path $IsoDir 'deshab.img'
$BuildTmp = Join-Path $Root '.build_tmp'

function Find-Tool($Name, $ExtraNames = @()) {
    $names = @($Name) + $ExtraNames
    foreach ($n in $names) {
        $cmd = Get-Command $n -ErrorAction SilentlyContinue
        if ($cmd) { return $cmd.Source }
    }

    $dirs = @(
        'C:\Program Files\LLVM\bin',
        'C:\Program Files (x86)\LLVM\bin',
        'C:\LLVM\bin',
        'D:\LLVM\bin',
        'D:\Program Files\LLVM\bin',
        'D:\Code\LLVM\bin',
        'C:\msys64\clang64\bin',
        'C:\msys64\mingw64\bin',
        'C:\mingw64\bin'
    )

    foreach ($dir in $dirs) {
        foreach ($n in $names) {
            $candidate = Join-Path $dir $n
            if (Test-Path $candidate) { return $candidate }
            if (-not $n.EndsWith('.exe')) {
                $candidateExe = Join-Path $dir "$n.exe"
                if (Test-Path $candidateExe) { return $candidateExe }
            }
        }
    }

    return $null
}

function Build-DkmDriver([string]$SourcePath, [string]$OutputPath) {
    $obj = Join-Path $BuildTmp (([System.IO.Path]::GetFileNameWithoutExtension($SourcePath)) + '.o')
    $outDir = Split-Path -Parent $OutputPath
    if (-not (Test-Path $outDir)) { New-Item -ItemType Directory -Force -Path $outDir | Out-Null }
    & $clang -target x86_64-unknown-none -std=c11 -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -fno-pie -mcmodel=kernel -mno-red-zone -mno-sse -mno-sse2 -mno-mmx -msoft-float -m64 -Wall -Wextra -O2 -c $SourcePath -o $obj
    if ($LASTEXITCODE -ne 0) { throw "failed to compile DKM driver: $SourcePath" }
    & $lld -r -o $OutputPath $obj
    if ($LASTEXITCODE -ne 0) { throw "failed to link DKM driver: $OutputPath" }
    Write-Host "[build] DKM driver Output: $OutputPath"
}

function Set-Le16([byte[]]$Buffer, [int]$Offset, [int]$Value) {
    $Buffer[$Offset] = [byte]($Value -band 0xff)
    $Buffer[$Offset + 1] = [byte](($Value -shr 8) -band 0xff)
}

function Set-Le32([byte[]]$Buffer, [int]$Offset, [UInt64]$Value) {
    $Buffer[$Offset] = [byte]($Value -band 0xff)
    $Buffer[$Offset + 1] = [byte](($Value -shr 8) -band 0xff)
    $Buffer[$Offset + 2] = [byte](($Value -shr 16) -band 0xff)
    $Buffer[$Offset + 3] = [byte](($Value -shr 24) -band 0xff)
}

function Set-Le64([byte[]]$Buffer, [int]$Offset, [UInt64]$Value) {
    for ($i = 0; $i -lt 8; $i++) {
        $Buffer[$Offset + $i] = [byte](($Value -shr ($i * 8)) -band 0xff)
    }
}

function Write-At($Stream, [Int64]$Offset, [byte[]]$Data) {
    [void]$Stream.Seek($Offset, [System.IO.SeekOrigin]::Begin)
    $Stream.Write($Data, 0, $Data.Length)
}

function Get-Crc32([byte[]]$Data) {
    $crc = [uint32]4294967295
    foreach ($b in $Data) {
        $crc = $crc -bxor [uint32]$b
        for ($i = 0; $i -lt 8; $i++) {
            if (($crc -band 1) -ne 0) {
                $crc = [uint32](($crc -shr 1) -bxor 0xedb88320)
            } else {
                $crc = [uint32]($crc -shr 1)
            }
        }
    }
    return [uint32]($crc -bxor [uint32]4294967295)
}

function Test-ShortFatName([string]$Name) {
    if ($Name -match '[a-z]') { return $false }
    $parts = $Name.Split('.')
    if ($parts.Count -gt 2) { return $false }
    if ($parts[0].Length -lt 1 -or $parts[0].Length -gt 8) { return $false }
    if ($parts.Count -eq 2 -and $parts[1].Length -gt 3) { return $false }
    return $Name -match '^[A-Z0-9_\$%''\-@~`!\(\)\{\}\^#&]+(\.[A-Z0-9_\$%''\-@~`!\(\)\{\}\^#&]+)?$'
}

function New-ShortFatName([string]$LongName, $Used) {
    $upper = $LongName.ToUpperInvariant()
    if ((Test-ShortFatName $upper) -and -not $Used.ContainsKey($upper)) {
        $Used[$upper] = $true
        $parts = $upper.Split('.')
        $base = $parts[0].PadRight(8, ' ')
        $ext = ''
        if ($parts.Count -eq 2) { $ext = $parts[1] }
        return ($base + $ext.PadRight(3, ' '))
    }

    $dot = $upper.LastIndexOf('.')
    if ($dot -ge 0) {
        $baseRaw = $upper.Substring(0, $dot)
        $extRaw = $upper.Substring($dot + 1)
    } else {
        $baseRaw = $upper
        $extRaw = ''
    }

    $baseClean = ($baseRaw -replace '[^A-Z0-9]', '')
    $extClean = ($extRaw -replace '[^A-Z0-9]', '')
    if ($baseClean.Length -eq 0) { $baseClean = 'FILE' }
    if ($extClean.Length -gt 3) { $extClean = $extClean.Substring(0, 3) }

    for ($i = 1; $i -lt 1000; $i++) {
        $suffix = "~$i"
        $take = [Math]::Min(8 - $suffix.Length, $baseClean.Length)
        $candidateBase = $baseClean.Substring(0, $take) + $suffix
        $display = $candidateBase
        if ($extClean.Length -gt 0) { $display = "$candidateBase.$extClean" }
        if (-not $Used.ContainsKey($display)) {
            $Used[$display] = $true
            return ($candidateBase.PadRight(8, ' ') + $extClean.PadRight(3, ' '))
        }
    }

    throw "Unable to generate FAT short name for $LongName"
}

function Get-ShortNameBytes([string]$ShortName) {
    return [System.Text.Encoding]::ASCII.GetBytes($ShortName)
}

function Get-LfnChecksum([byte[]]$ShortBytes) {
    $sum = 0
    foreach ($b in $ShortBytes) {
        $sum = (((($sum -band 1) * 0x80) + ($sum -shr 1) + $b) -band 0xff)
    }
    return [byte]$sum
}

function New-LfnEntry([string]$Chunk, [int]$Order, [byte]$Checksum) {
    $entry = [byte[]]::new(32)
    for ($i = 0; $i -lt 32; $i++) { $entry[$i] = 0xff }
    $entry[0] = [byte]$Order
    $entry[11] = 0x0f
    $entry[12] = 0x00
    $entry[13] = $Checksum
    $entry[26] = 0x00
    $entry[27] = 0x00

    $positions = @(1,3,5,7,9,14,16,18,20,22,24,28,30)
    $chars = $Chunk.ToCharArray()
    for ($i = 0; $i -lt $positions.Count; $i++) {
        $pos = $positions[$i]
        if ($i -lt $chars.Length) {
            $code = [int][char]$chars[$i]
            $entry[$pos] = [byte]($code -band 0xff)
            $entry[$pos + 1] = [byte](($code -shr 8) -band 0xff)
        } elseif ($i -eq $chars.Length) {
            $entry[$pos] = 0x00
            $entry[$pos + 1] = 0x00
        }
    }
    return $entry
}

function New-ShortEntry([string]$ShortName, [byte]$Attr, [int]$Cluster, [uint32]$Size) {
    $entry = [byte[]]::new(32)
    $nameBytes = Get-ShortNameBytes $ShortName
    [Array]::Copy($nameBytes, 0, $entry, 0, 11)
    $entry[11] = $Attr
    Set-Le16 $entry 20 (($Cluster -shr 16) -band 0xffff)
    Set-Le16 $entry 26 ($Cluster -band 0xffff)
    Set-Le32 $entry 28 $Size
    return $entry
}

function Add-DirectoryEntry($Entries, [string]$Name, [byte]$Attr, [int]$Cluster, [uint32]$Size, $UsedNames) {
    $shortName = New-ShortFatName $Name $UsedNames
    $shortBytes = Get-ShortNameBytes $shortName
    $upper = $Name.ToUpperInvariant()
    $needsLfn = -not (Test-ShortFatName $upper) -or (($shortName.Trim() -replace ' +', '') -ne ($upper -replace '\.', ''))

    if ($needsLfn) {
        $checksum = Get-LfnChecksum $shortBytes
        $chunks = @()
        for ($i = 0; $i -lt $Name.Length; $i += 13) {
            $len = [Math]::Min(13, $Name.Length - $i)
            $chunks += $Name.Substring($i, $len)
        }
        for ($i = $chunks.Count; $i -ge 1; $i--) {
            $order = $i
            if ($i -eq $chunks.Count) { $order = $order -bor 0x40 }
            [void]$Entries.Add((New-LfnEntry $chunks[$i - 1] $order $checksum))
        }
    }

    [void]$Entries.Add((New-ShortEntry $shortName $Attr $Cluster $Size))
}

function Get-GuidBytes([string]$GuidString) {
    return ([Guid]$GuidString).ToByteArray()
}

function New-GptHeader([UInt64]$CurrentLba, [UInt64]$BackupLba, [UInt64]$FirstUsable, [UInt64]$LastUsable, [byte[]]$DiskGuid, [UInt64]$EntriesLba, [uint32]$EntryCount, [uint32]$EntrySize, [uint32]$EntriesCrc) {
    $header = [byte[]]::new(512)
    [Array]::Copy([System.Text.Encoding]::ASCII.GetBytes('EFI PART'), 0, $header, 0, 8)
    Set-Le32 $header 8 0x00010000
    Set-Le32 $header 12 92
    Set-Le64 $header 24 $CurrentLba
    Set-Le64 $header 32 $BackupLba
    Set-Le64 $header 40 $FirstUsable
    Set-Le64 $header 48 $LastUsable
    [Array]::Copy($DiskGuid, 0, $header, 56, 16)
    Set-Le64 $header 72 $EntriesLba
    Set-Le32 $header 80 $EntryCount
    Set-Le32 $header 84 $EntrySize
    Set-Le32 $header 88 $EntriesCrc

    $crcData = [byte[]]::new(92)
    [Array]::Copy($header, 0, $crcData, 0, 92)
    Set-Le32 $crcData 16 0
    $crc = Get-Crc32 $crcData
    Set-Le32 $header 16 $crc
    return $header
}

function New-GptFat32Image([string]$SourceDir, [string]$ImagePath, [int]$SizeMB = 128) {
    $bytesPerSector = 512
    $totalSectors = [UInt64](($SizeMB * 1024 * 1024) / $bytesPerSector)
    $lastLba = $totalSectors - 1
    $entryCount = [uint32]128
    $entrySize = [uint32]128
    $entrySectors = [UInt64](($entryCount * $entrySize) / $bytesPerSector)
    $firstUsable = [UInt64](2 + $entrySectors)
    $lastUsable = [UInt64]($lastLba - $entrySectors - 1)
    $partStart = [UInt64]2048
    $partEnd = $lastUsable
    $partSectors = [UInt64]($partEnd - $partStart + 1)
    $partitionOffset = [Int64]($partStart * $bytesPerSector)

    $sectorsPerCluster = 1
    $reservedSectors = 32
    $numFats = 2
    $fatSectors = 1
    do {
        $dataSectors = [Int64]$partSectors - $reservedSectors - ($numFats * $fatSectors)
        $clusterCount = [int]($dataSectors / $sectorsPerCluster)
        $neededFatSectors = [int]((($clusterCount + 2) * 4 + $bytesPerSector - 1) / $bytesPerSector)
        if ($neededFatSectors -eq $fatSectors) { break }
        $fatSectors = $neededFatSectors
    } while ($true)

    if ($clusterCount -lt 65525) {
        throw "Invalid FAT32 cluster count: $clusterCount"
    }

    $fatOffset = [Int64]($partitionOffset + $reservedSectors * $bytesPerSector)
    $dataOffset = [Int64]($partitionOffset + ($reservedSectors + $numFats * $fatSectors) * $bytesPerSector)
    $clusterSize = $sectorsPerCluster * $bytesPerSector
    $fat = [UInt32[]]::new($clusterCount + 2)
    $fat[0] = 0x0ffffff8
    $fat[1] = 0x0fffffff
    $fat[2] = 0x0fffffff
    $script:fatNextCluster = 3

    function Allocate-Chain([Int64]$ByteCount) {
        $needed = [Math]::Max(1, [int](($ByteCount + $clusterSize - 1) / $clusterSize))
        $first = $script:fatNextCluster
        for ($i = 0; $i -lt $needed; $i++) {
            $cluster = $script:fatNextCluster
            $script:fatNextCluster++
            if ($script:fatNextCluster -ge $fat.Length) { throw 'FAT32 image is too small' }
            if ($i -eq $needed - 1) {
                $fat[$cluster] = 0x0fffffff
            } else {
                $fat[$cluster] = [UInt32]($cluster + 1)
            }
        }
        return $first
    }

    function Get-ClusterOffset([int]$Cluster) {
        return [Int64]($dataOffset + (($Cluster - 2) * $clusterSize))
    }

    function Write-FileContent($Stream, [string]$Path, [int]$FirstCluster) {
        $bytes = [System.IO.File]::ReadAllBytes($Path)
        $remaining = $bytes.Length
        $srcOffset = 0
        $cluster = $FirstCluster
        while ($remaining -gt 0) {
            $chunk = [Math]::Min($clusterSize, $remaining)
            [void]$Stream.Seek((Get-ClusterOffset $cluster), [System.IO.SeekOrigin]::Begin)
            $Stream.Write($bytes, $srcOffset, $chunk)
            $srcOffset += $chunk
            $remaining -= $chunk
            if ($remaining -gt 0) { $cluster = $fat[$cluster] }
        }
    }

    function Write-Directory($Stream, [string]$Path, [int]$SelfCluster, [int]$ParentCluster, [bool]$IsRoot) {
        $entries = [System.Collections.ArrayList]::new()
        $used = @{}

        if (-not $IsRoot) {
            [void]$entries.Add((New-ShortEntry '.          ' 0x10 $SelfCluster 0))
            [void]$entries.Add((New-ShortEntry '..         ' 0x10 $ParentCluster 0))
        }

        $items = Get-ChildItem -LiteralPath $Path -Force | Where-Object { $_.Name -notlike '*.tmp*' } | Sort-Object @{Expression={$_.PSIsContainer};Descending=$true}, Name
        foreach ($item in $items) {
            if ($item.PSIsContainer) {
                $childCluster = Allocate-Chain $clusterSize
                Add-DirectoryEntry $entries $item.Name 0x10 $childCluster 0 $used
                Write-Directory $Stream $item.FullName $childCluster $SelfCluster $false
            } else {
                $fileSize = [uint32]$item.Length
                $fileCluster = Allocate-Chain $item.Length
                Add-DirectoryEntry $entries $item.Name 0x20 $fileCluster $fileSize $used
                Write-FileContent $Stream $item.FullName $fileCluster
            }
        }

        $entryBytes = $entries.Count * 32
        $neededBytes = [Math]::Max($clusterSize, [int](($entryBytes + $clusterSize - 1) / $clusterSize) * $clusterSize)
        $dirBytes = [byte[]]::new($neededBytes)
        $offset = 0
        foreach ($entry in $entries) {
            [Array]::Copy($entry, 0, $dirBytes, $offset, 32)
            $offset += 32
        }
        Write-At $Stream (Get-ClusterOffset $SelfCluster) $dirBytes
    }

    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $ImagePath) | Out-Null
    $fs = [System.IO.File]::Open($ImagePath, [System.IO.FileMode]::Create, [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::None)
    try {
        $fs.SetLength($SizeMB * 1024 * 1024)

        $mbr = [byte[]]::new(512)
        $mbr[446 + 4] = 0xee
        Set-Le32 $mbr (446 + 8) 1
        $protectiveSectors = [UInt64]($totalSectors - 1)
        if ($protectiveSectors -gt [UInt64]4294967295) { $protectiveSectors = [UInt64]4294967295 }
        Set-Le32 $mbr (446 + 12) $protectiveSectors
        $mbr[510] = 0x55
        $mbr[511] = 0xaa
        Write-At $fs 0 $mbr

        $partitionEntries = [byte[]]::new($entryCount * $entrySize)
        $espType = Get-GuidBytes 'c12a7328-f81f-11d2-ba4b-00a0c93ec93b'
        $partGuid = ([Guid]::NewGuid()).ToByteArray()
        [Array]::Copy($espType, 0, $partitionEntries, 0, 16)
        [Array]::Copy($partGuid, 0, $partitionEntries, 16, 16)
        Set-Le64 $partitionEntries 32 $partStart
        Set-Le64 $partitionEntries 40 $partEnd
        $nameBytes = [System.Text.Encoding]::Unicode.GetBytes('Deshab ESP')
        [Array]::Copy($nameBytes, 0, $partitionEntries, 56, $nameBytes.Length)
        $entriesCrc = Get-Crc32 $partitionEntries
        $diskGuid = ([Guid]::NewGuid()).ToByteArray()

        $primaryHeader = New-GptHeader 1 $lastLba $firstUsable $lastUsable $diskGuid 2 $entryCount $entrySize $entriesCrc
        $backupEntriesLba = [UInt64]($lastLba - $entrySectors)
        $backupHeader = New-GptHeader $lastLba 1 $firstUsable $lastUsable $diskGuid $backupEntriesLba $entryCount $entrySize $entriesCrc

        Write-At $fs ($bytesPerSector * 1) $primaryHeader
        Write-At $fs ($bytesPerSector * 2) $partitionEntries
        Write-At $fs ([Int64]($backupEntriesLba * $bytesPerSector)) $partitionEntries
        Write-At $fs ([Int64]($lastLba * $bytesPerSector)) $backupHeader

        $boot = [byte[]]::new(512)
        $boot[0] = 0xeb; $boot[1] = 0x58; $boot[2] = 0x90
        [Array]::Copy([System.Text.Encoding]::ASCII.GetBytes('DESHAB  '), 0, $boot, 3, 8)
        Set-Le16 $boot 11 $bytesPerSector
        $boot[13] = [byte]$sectorsPerCluster
        Set-Le16 $boot 14 $reservedSectors
        $boot[16] = [byte]$numFats
        Set-Le16 $boot 17 0
        Set-Le16 $boot 19 0
        $boot[21] = 0xf8
        Set-Le16 $boot 22 0
        Set-Le16 $boot 24 63
        Set-Le16 $boot 26 255
        Set-Le32 $boot 28 $partStart
        Set-Le32 $boot 32 $partSectors
        Set-Le32 $boot 36 $fatSectors
        Set-Le16 $boot 40 0
        Set-Le16 $boot 42 0
        Set-Le32 $boot 44 2
        Set-Le16 $boot 48 1
        Set-Le16 $boot 50 6
        $boot[64] = 0x80
        $boot[66] = 0x29
        Set-Le32 $boot 67 0x44534842
        [Array]::Copy([System.Text.Encoding]::ASCII.GetBytes('DESHAB     '), 0, $boot, 71, 11)
        [Array]::Copy([System.Text.Encoding]::ASCII.GetBytes('FAT32   '), 0, $boot, 82, 8)
        $boot[510] = 0x55; $boot[511] = 0xaa
        Write-At $fs $partitionOffset $boot
        Write-At $fs ($partitionOffset + 6 * $bytesPerSector) $boot

        $fsinfo = [byte[]]::new(512)
        Set-Le32 $fsinfo 0 0x41615252
        Set-Le32 $fsinfo 484 0x61417272
        Set-Le32 $fsinfo 488 4294967295
        Set-Le32 $fsinfo 492 3
        $fsinfo[508] = 0x00; $fsinfo[509] = 0x00; $fsinfo[510] = 0x55; $fsinfo[511] = 0xaa
        Write-At $fs ($partitionOffset + $bytesPerSector) $fsinfo
        Write-At $fs ($partitionOffset + 7 * $bytesPerSector) $fsinfo

        Write-Directory $fs $SourceDir 2 2 $true

        $fatBytes = [byte[]]::new($fatSectors * $bytesPerSector)
        for ($i = 0; $i -lt $fat.Length; $i++) {
            $off = $i * 4
            if ($off + 3 -ge $fatBytes.Length) { break }
            Set-Le32 $fatBytes $off $fat[$i]
        }
        Write-At $fs $fatOffset $fatBytes
        Write-At $fs ($fatOffset + $fatSectors * $bytesPerSector) $fatBytes
    }
    finally {
        $fs.Close()
    }

    Write-Host "[build] GPT + FAT32 ESP IMG: $ImagePath"
}

Write-Host '[build] Building UTSM kernel...'

$clang = Find-Tool 'clang'
$lld = Find-Tool 'ld.lld'
$make = Find-Tool 'make' @('mingw32-make')

if (-not $clang) {
    throw 'clang was not found. Install LLVM/Clang or provide a freestanding x86_64 ELF toolchain.'
}

if (-not $lld) {
    throw 'ld.lld was not found. Install LLVM lld or provide a compatible linker.'
}

if (-not $make) {
    throw 'make was not found. Install make.'
}

Write-Host "[build] clang: $clang"
Write-Host "[build] ld.lld: $lld"
Write-Host "[build] make: $make"

New-Item -ItemType Directory -Force -Path $BuildTmp | Out-Null
$env:TMP = $BuildTmp
$env:TEMP = $BuildTmp

Push-Location $UtsmDir
try {
    & $make -f MAKEFILE "CC=$clang" "LD=$lld"
    if ($LASTEXITCODE -ne 0) {
        throw "make UTSM failed with exit code $LASTEXITCODE"
    }
}
finally {
    Pop-Location
}

Write-Host '[build] Building DSK main kernel...'
try {
    & $make -C "$DskDir" -f MAKEFILE "CC=$clang" "LD=$lld"
    if ($LASTEXITCODE -ne 0) {
        throw "make DSK failed with exit code $LASTEXITCODE"
    }
}
finally {
}

if (-not (Test-Path $Output)) {
    throw "UTSM build did not produce $Output"
}
if (-not (Test-Path $DskOutput)) {
    throw "DSK build did not produce $DskOutput"
}

$FirstInitDir = Join-Path $Root 'CODE\firstInit'
$FirstInitOutput = Join-Path $SystemDir 'system\user\use\FirstInit.elf'

Write-Host '[build] Building FirstInit.elf...'
try {
    & $make -C "$FirstInitDir" -f MAKEFILE "CC=$clang" "LD=$lld"
    if ($LASTEXITCODE -ne 0) {
        throw "make FirstInit failed with exit code $LASTEXITCODE"
    }
}
finally {
}

if (-not (Test-Path $FirstInitOutput)) {
    throw "FirstInit build did not produce $FirstInitOutput"
}
Write-Host "[build] FirstInit Output: $FirstInitOutput"

$MouseDir = Join-Path $Root 'CODE\mouse'
$MouseOutput = Join-Path $SystemDir 'system\deshab64\mouse\mouseInit.elf'

Write-Host '[build] Building mouseInit.elf...'
try {
    & $make -C "$MouseDir" -f MAKEFILE "CC=$clang" "LD=$lld"
    if ($LASTEXITCODE -ne 0) {
        throw "make mouseInit failed with exit code $LASTEXITCODE"
    }
}
finally {
}

if (-not (Test-Path $MouseOutput)) {
    throw "mouseInit build did not produce $MouseOutput"
}
Write-Host "[build] mouseInit Output: $MouseOutput"

$NetmanDir = Join-Path $Root 'CODE\netman'
$NetmanOutput = Join-Path $SystemDir 'system\deshab64\network\netman.elf'

Write-Host '[build] Building netman.elf...'
try {
    & $make -C "$NetmanDir" -f MAKEFILE "CC=$clang" "LD=$lld"
    if ($LASTEXITCODE -ne 0) {
        throw "make netman failed with exit code $LASTEXITCODE"
    }
}
finally {
}

if (-not (Test-Path $NetmanOutput)) {
    throw "netman build did not produce $NetmanOutput"
}
Write-Host "[build] netman Output: $NetmanOutput"

Write-Host '[build] Building DKM network drivers...'
Build-DkmDriver (Join-Path $Root 'CODE\DKM\e1000\e1000.c') (Join-Path $SystemDir 'driver\net\e1000.drv')
Build-DkmDriver (Join-Path $Root 'CODE\DKM\virtio_net\virtio_net.c') (Join-Path $SystemDir 'driver\net\virtio_net.drv')

Write-Host "[build] Output: $Output"
Write-Host "[build] DSK Output: $DskOutput"
Write-Host '[build] Packaging SYSTEM to GPT + FAT32 IMG...'
New-GptFat32Image $SystemDir $ImagePath 128
Write-Host "[build] Done: $ImagePath"

# Rebuild SATA FAT32 disk image (DSK reads deshab.elf/FirstInit.elf/mouseInit.elf from here)
Write-Host '[build] Rebuilding SATA FAT32 disk image...'
$BuildTmp = Join-Path $Root '.build_tmp'
$MkFat32 = Join-Path $BuildTmp 'mkfat32.exe'
if (Test-Path $MkFat32) {
    Push-Location $BuildTmp
    try {
        & $MkFat32
        if ($LASTEXITCODE -ne 0) {
            Write-Host "[build] WARNING: mkfat32 exited with $LASTEXITCODE"
        }
    }
    finally {
        Pop-Location
    }
    Write-Host "[build] SATA IMG: $(Join-Path $BuildTmp 'sata_fat32_dsk.img')"
} else {
    Write-Host '[build] WARNING: mkfat32.exe not found, SATA image not rebuilt'
}
