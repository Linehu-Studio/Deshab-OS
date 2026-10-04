param(
    [ValidateSet('dev','release','realtest','both')][string]$Variant = 'both',
    [switch]$Vhd
)

$ErrorActionPreference = 'Stop'

$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$UtsmDir = Join-Path $Root 'CODE\UTSM'
$DskDir = Join-Path $Root 'CODE\dsk'
$SystemDir = Join-Path $Root 'SYSTEM'
$IsoDir = Join-Path $Root 'ISO'
$ConfigsDir = Join-Path $Root 'build\configs'
$Output = Join-Path $SystemDir 'boot\utsm.elf'
$DskOutput = Join-Path $SystemDir 'system\deshab64\deshab.elf'
$DevImagePath = Join-Path $IsoDir 'deshab-dev.img'
$ReleaseImagePath = Join-Path $IsoDir 'deshab-release.img'
$RealtestImagePath = Join-Path $IsoDir 'deshab-realtest.img'
$BuildTmp = Join-Path $Root '.build_tmp'

# Legacy single-FAT32 ESP size helper (New-GptFat32Image). Dual-partition
# packing uses CODE/linux/pack_system_image.sh and sizes each partition
# from SYSTEM/boot+EFI+limine vs the rest.
function Get-EspSizeMBFromSystem([string]$SourceDir) {
    $sum = (Get-ChildItem -LiteralPath $SourceDir -Recurse -File -Force |
        Measure-Object -Property Length -Sum).Sum
    if (-not $sum) { $sum = 0 }
    $contentMB = [int][Math]::Ceiling($sum / 1MB)
    $need = $contentMB + [int][Math]::Ceiling($contentMB * 0.15) + 256
    if ($need -lt 2048) { $need = 2048 }
    return $need
}
if ($env:ESP_SIZE_MB) {
    $EspSizeMB = [int]$env:ESP_SIZE_MB
} else {
    $EspSizeMB = Get-EspSizeMBFromSystem $SystemDir
}

function ConvertTo-WslUnixPath([string]$WinPath) {
    $full = [System.IO.Path]::GetFullPath($WinPath)
    if ($full -match '^(?<d>[A-Za-z]):\\(?<rest>.*)$') {
        $drive = $Matches['d'].ToLowerInvariant()
        $rest = $Matches['rest'] -replace '\\', '/'
        return "/mnt/$drive/$rest"
    }
    throw "Cannot map Windows path to WSL: $WinPath"
}

# GPT p1 FAT32 ESP (/EFI /limine /boot) + GPT p2 ext4 (rest of SYSTEM/).
function New-GptFat32Ext4Image([string]$SourceDir, [string]$ImagePath) {
    $scriptWin = Join-Path $Root 'CODE\linux\pack_system_image.sh'
    if (-not (Test-Path -LiteralPath $scriptWin)) {
        throw "Missing packer: $scriptWin"
    }
    if (-not (Get-Command wsl -ErrorAction SilentlyContinue)) {
        throw 'WSL is required to pack GPT FAT32+ext4 (mkfs.ext4).'
    }

    $imgDir = Split-Path -Parent $ImagePath
    if (-not (Test-Path -LiteralPath $imgDir)) {
        New-Item -ItemType Directory -Path $imgDir | Out-Null
    }

    $wslScript = ConvertTo-WslUnixPath $scriptWin
    $wslSys = ConvertTo-WslUnixPath $SourceDir
    $wslImg = "$(ConvertTo-WslUnixPath $imgDir)/$([System.IO.Path]::GetFileName($ImagePath))"

    Write-Host '[build] Packing GPT: FAT32 ESP (/EFI /limine /boot) + ext4 data'
    & wsl -u root -- bash $wslScript $wslSys $wslImg
    if ($LASTEXITCODE -ne 0) {
        throw "pack_system_image.sh failed ($LASTEXITCODE)"
    }
}

# Optional persistent rw volume for Linux guest (Phase 4: overlayfs upperdir).
# When SYSTEM/boot/linux-extra-rootfs.img is present, build.ps1 prints a notice;
# limine.conf is expected to declare it as a boot module (added in Phase 4).
$ExtraRootfsPath = Join-Path $SystemDir 'boot\linux-extra-rootfs.img'

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
    if ($Name -cmatch '[a-z]') { return $false }
    $parts = $Name.Split('.')
    if ($parts.Count -gt 2) { return $false }
    if ($parts[0].Length -lt 1 -or $parts[0].Length -gt 8) { return $false }
    if ($parts.Count -eq 2 -and $parts[1].Length -gt 3) { return $false }
    return ($Name -cmatch '^[A-Z0-9_]+$') -or ($Name -cmatch '^[A-Z0-9_]+\.[A-Z0-9_]+$')
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
    # 用原始名（含大小写）判断是否生成 LFN：Test-ShortFatName 内部用
    # -cmatch '[a-z]' 检测小写字母，含小写的名字（如 limine）必须生成
    # LFN 才能在 Windows 上保留原始大小写显示。FAT32 查找仍不区分大小写。
    # 注意：拆成两步赋值——PowerShell 5.1 对 `-not (函数调用) -or (...)` 内联
    # 表达式解析异常返回 $null，分开赋值保证布尔短路正常。
    $isUpperShort = Test-ShortFatName $Name
    $needsLfn = (-not $isUpperShort) -or (($shortName.Trim() -replace ' +', '') -ne ($Name.ToUpperInvariant() -replace '\.', ''))

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

# Legacy: whole SYSTEM/ on a single FAT32 ESP. Prefer New-GptFat32Ext4Image.
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

    # 4KB clusters (8 sectors) on the 2GB default image; 32KB (64 sectors)
    # when SYSTEM/ is large enough that 65525 clusters still fit.
    # 32KB on a 2GB volume would drop below the FAT32 minimum cluster count.
    $sectorsPerCluster = 8
    if ($SizeMB -ge 4096) { $sectorsPerCluster = 64 }
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
        $input = [System.IO.File]::OpenRead($Path)
        try {
            $buf = [byte[]]::new($clusterSize)
            $cluster = $FirstCluster
            $remaining = $input.Length
            while ($remaining -gt 0) {
                $chunk = [int][Math]::Min([int64]$clusterSize, [int64]$remaining)
                $n = $input.Read($buf, 0, $chunk)
                if ($n -le 0) { break }
                [void]$Stream.Seek((Get-ClusterOffset $cluster), [System.IO.SeekOrigin]::Begin)
                $Stream.Write($buf, 0, $n)
                $remaining -= $n
                if ($remaining -gt 0) { $cluster = $fat[$cluster] }
            }
        } finally {
            $input.Close()
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

        # 两遍策略：先收集所有目录项（含 LFN），同时为子目录/文件分配簇链。
        # 子目录需要先分配簇，但目录项总数此时未知——先收集 entries，
        # 再根据 entries 总字节数为本目录分配足够簇，最后回写 FAT 链。
        # 注意：SelfCluster 已由调用方分配（根目录固定 cluster 2，子目录在调用前分配）。
        # 对于子目录，调用方只分配了 1 个簇——如果目录项超过 1 簇，需要扩展链。
        # 解决方案：先收集 entries + 分配子项簇，然后检查本目录需要的簇数，
        # 如果超过已分配的，追加簇并更新 FAT 链。

        foreach ($item in $items) {
            if ($item.PSIsContainer) {
                $childCluster = Allocate-Chain $clusterSize
                Add-DirectoryEntry $entries $item.Name 0x10 $childCluster 0 $used
                Write-Directory $Stream $item.FullName $childCluster $SelfCluster $false
            } else {
                if ($item.Length -ge 4GB) {
                    throw "FAT32 cannot store $($item.FullName) ($([int]($item.Length / 1MB))MB). Keep SYSTEM files under 4GiB."
                }
                $fileSize = [uint32]$item.Length
                $fileCluster = Allocate-Chain $item.Length
                Add-DirectoryEntry $entries $item.Name 0x20 $fileCluster $fileSize $used
                Write-FileContent $Stream $item.FullName $fileCluster
            }
        }

        # 计算本目录需要的簇数（目录项可能跨多簇）
        $entryBytes = $entries.Count * 32
        $neededClusters = [Math]::Max(1, [int](($entryBytes + $clusterSize - 1) / $clusterSize))
        $neededBytes = $neededClusters * $clusterSize

        # 如果本目录需要多簇，扩展 FAT 链（SelfCluster 是首簇）
        if ($neededClusters -gt 1) {
            $cur = $SelfCluster
            for ($i = 1; $i -lt $neededClusters; $i++) {
                $next = $script:fatNextCluster
                $script:fatNextCluster++
                if ($script:fatNextCluster -ge $fat.Length) { throw 'FAT32 image is too small' }
                $fat[$cur] = [UInt32]$next
                $cur = $next
            }
            $fat[$cur] = 0x0fffffff
        }

        $dirBytes = [byte[]]::new($neededBytes)
        $offset = 0
        foreach ($entry in $entries) {
            [Array]::Copy($entry, 0, $dirBytes, $offset, 32)
            $offset += 32
        }

        # 写入所有目录簇（按 FAT 链顺序）
        $writeCluster = $SelfCluster
        $bytesWritten = 0
        while ($bytesWritten -lt $neededBytes) {
            $chunk = [Math]::Min($clusterSize, $neededBytes - $bytesWritten)
            $chunkBytes = [byte[]]::new($clusterSize)
            [Array]::Copy($dirBytes, $bytesWritten, $chunkBytes, 0, $chunk)
            Write-At $Stream (Get-ClusterOffset $writeCluster) $chunkBytes
            $bytesWritten += $clusterSize
            if ($bytesWritten -lt $neededBytes) {
                $writeCluster = $fat[$writeCluster]
            }
        }
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

# Convert a raw GPT disk image to a fixed VHD by appending the 512-byte VHD
# footer. Fixed VHD = raw data + footer, so this is a cheap copy + footer.
# Windows can mount the result natively (double-click / Mount-DiskImage).
function ConvertTo-FixedVhd([string]$RawImage, [string]$VhdPath) {
    if (-not (Test-Path $RawImage)) { throw "Raw image not found: $RawImage" }
    Copy-Item -LiteralPath $RawImage -Destination $VhdPath -Force
    $fs = [System.IO.File]::Open($VhdPath, [System.IO.FileMode]::Open,
        [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::None)
    try {
        $size = $fs.Length
        $totalSectors = [UInt64]($size / 512)

        $footer = [byte[]]::new(512)
        [Array]::Copy([System.Text.Encoding]::ASCII.GetBytes('conectix'), 0, $footer, 0, 8)
        Set-Le32 $footer 8 0x00000002          # features
        Set-Le32 $footer 12 0x00010000         # file format version
        Set-Le64 $footer 16 ([UInt64]::MaxValue) # data offset: fixed disk
        $epoch = [DateTime]::new(2000, 1, 1, 0, 0, 0, [DateTimeKind]::Utc)
        Set-Le32 $footer 24 ([UInt64][Math]::Max(0, [int]([DateTime]::UtcNow - $epoch).TotalSeconds))
        [Array]::Copy([System.Text.Encoding]::ASCII.GetBytes('dshb'), 0, $footer, 28, 4)
        Set-Le32 $footer 32 0x00010000         # creator version
        [Array]::Copy([System.Text.Encoding]::ASCII.GetBytes('Wi2k'), 0, $footer, 36, 4)
        Set-Le64 $footer 40 ([UInt64]$size)    # original size
        Set-Le64 $footer 48 ([UInt64]$size)    # current size

        # Disk geometry (VHD layout is big-endian here: 2-byte cylinders + 1 head + 1 spt)
        $spt = 0; $h = 0
        if ($totalSectors -ge 65535 * 16 * 255) { $spt = 255; $h = 16 }
        elseif ($totalSectors -ge 65535 * 16 * 17) { $spt = 63; $h = 16 }
        else { $spt = 17; $h = 16 }
        $c = [UInt64][Math]::Floor([double]$totalSectors / ($h * $spt))
        if ($c -gt 65535) { $c = 65535 }
        $footer[56] = [byte](($c -shr 8) -band 0xFF)
        $footer[57] = [byte]($c -band 0xFF)
        $footer[58] = [byte]$h
        $footer[59] = [byte]$spt

        Set-Le32 $footer 60 2                   # disk type: fixed
        [Array]::Copy(([Guid]::NewGuid()).ToByteArray(), 0, $footer, 68, 16)
        $footer[84] = 0                         # saved state: normal

        # Checksum: one's complement of the uint32 sum over the whole footer
        $sum = [UInt64]0
        for ($i = 0; $i -lt 512; $i += 4) {
            $sum = ($sum + [BitConverter]::ToUInt32($footer, $i)) -band 0xFFFFFFFF
        }
        Set-Le32 $footer 64 ((0xFFFFFFFF -bxor $sum) -band 0xFFFFFFFF)

        [void]$fs.Seek(0, [System.IO.SeekOrigin]::End)
        $fs.Write($footer, 0, 512)
    } finally {
        $fs.Close()
    }
    Write-Host "[build] VHD (Windows mountable): $VhdPath"
}

# Build a single image variant (dev/release): inject FUCK + firstInit.txt from
# build/configs/<variant>/ into SYSTEM/, then package via New-GptFat32Ext4Image.
# If $ImagePath is $null, only injects config without packaging (used to
# restore SYSTEM dir to dev config after dual-image build).
# NOTE: configs/<variant> dir may be absent; then SYSTEM is packaged as-is.
function Build-ImageVariant([string]$VariantName, [string]$ImagePath) {
    $cfgDir = Join-Path $ConfigsDir $VariantName

    if (Test-Path $cfgDir) {
        $fuckSrc = Join-Path $cfgDir 'FUCK'
        $fuckDst = Join-Path $SystemDir 'system\deshab64\FUCK'
        $fiSrc   = Join-Path $cfgDir 'firstInit.txt'
        $fiDst   = Join-Path $SystemDir 'system\user\use\firstInit.txt'

        if (-not (Test-Path $fuckSrc)) { throw "Missing FUCK template: $fuckSrc" }
        if (-not (Test-Path $fiSrc))   { throw "Missing firstInit.txt template: $fiSrc" }

        Copy-Item -LiteralPath $fuckSrc -Destination $fuckDst -Force
        Copy-Item -LiteralPath $fiSrc   -Destination $fiDst   -Force
        Write-Host "[build] Injected config variant=$VariantName (FUCK + firstInit.txt)"
    } else {
        Write-Host "[build] WARN: configs/$VariantName absent, packaging SYSTEM as-is"
    }

    if ($ImagePath) {
        Write-Host '[build] Packaging SYSTEM/ as GPT: p1 FAT32 ESP (Limine) + p2 ext4 data'
        New-GptFat32Ext4Image $SystemDir $ImagePath
        if ($Vhd) {
            $vhdPath = [System.IO.Path]::ChangeExtension($ImagePath, '.vhd')
            ConvertTo-FixedVhd $ImagePath $vhdPath
        }
    }
}

Write-Host '[build] Building UTSM kernel...'

# VSCode integration Phase 0 hook: detect optional persistent rw volume for
# Linux guest. The actual virtio-blk backend wiring is added in Phase 4.
if (Test-Path $ExtraRootfsPath) {
    Write-Host "[build] NOTE: linux-extra-rootfs.img present at $ExtraRootfsPath"
    Write-Host '[build]       (Phase 4 will wire it as /dev/vdc for overlayfs upperdir)'
}

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

# Ensure make can find sh.exe + Unix tools (mkdir, etc.).
# MAKEFILEs use `mkdir -p` which cmd.exe cannot run; mingw32-make needs sh.exe
# in PATH. Git for Windows' usr/bin provides a complete sh + coreutils set.
foreach ($shDir in @('C:\Program Files\Git\usr\bin', 'C:\msys64\usr\bin')) {
    if ((Test-Path (Join-Path $shDir 'sh.exe')) -and ($env:PATH -notlike "*$shDir*")) {
        $env:PATH = "$shDir;$env:PATH"
        Write-Host "[build] Added Unix tools dir to PATH: $shDir"
        break
    }
}

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
# Generate logo_data.c from Logo.png (RGBA8888 embedded data)
$LogoPng = Join-Path $Root 'Logo.png'
$LogoDataC = Join-Path $DskDir 'logo_data.c'
$PngToRgba = Join-Path $DskDir 'png_to_rgba.py'
if (Test-Path $LogoPng) {
    # Find Python
    $Python = $null
    foreach ($p in @('C:\Users\林濬哲\AppData\Local\Programs\Python\Python312\python.exe',
                     'C:\msys64\mingw64\bin\python.exe',
                     'python', 'python3')) {
        try { $cmd = Get-Command $p -ErrorAction Stop; $Python = $cmd.Source; break } catch {}
    }
    if ($Python) {
        # Regenerate if Logo.png is newer than logo_data.c
        if (-not (Test-Path $LogoDataC) -or ((Get-Item $LogoPng).LastWriteTime -gt (Get-Item $LogoDataC).LastWriteTime)) {
            Write-Host '[build] Generating logo_data.c from Logo.png...'
            & $Python $PngToRgba $LogoPng $LogoDataC
            if ($LASTEXITCODE -ne 0) { Write-Host '[build] WARNING: logo_data.c generation failed, using existing file' }
        }
    } else {
        Write-Host '[build] WARNING: Python not found, cannot regenerate logo_data.c'
    }
} else {
    Write-Host '[build] WARNING: Logo.png not found'
}

# Generate panic_logo_data.inc from ohMyLogo.png (panic screen logo + bg color)
$PanicLogoPng = Join-Path $Root 'ohMyLogo.png'
$PanicLogoInc = Join-Path $UtsmDir 'kernel\panic_logo_data.inc'
$PanicGenPy   = Join-Path $UtsmDir 'kernel\gen_panic_logo.py'
if (Test-Path $PanicLogoPng) {
    $Python = $null
    foreach ($p in @('C:\Users\林濬哲\AppData\Local\Programs\Python\Python312\python.exe',
                     'C:\msys64\mingw64\bin\python.exe',
                     'python', 'python3')) {
        try { $cmd = Get-Command $p -ErrorAction Stop; $Python = $cmd.Source; break } catch {}
    }
    if ($Python) {
        if (-not (Test-Path $PanicLogoInc) -or ((Get-Item $PanicLogoPng).LastWriteTime -gt (Get-Item $PanicLogoInc).LastWriteTime)) {
            Write-Host '[build] Generating panic_logo_data.inc from ohMyLogo.png...'
            & $Python $PanicGenPy $PanicLogoPng $PanicLogoInc
            if ($LASTEXITCODE -ne 0) { Write-Host '[build] WARNING: panic_logo_data.inc generation failed, using existing file' }
        }
    } else {
        Write-Host '[build] WARNING: Python not found, cannot regenerate panic_logo_data.inc'
    }
} else {
    Write-Host '[build] WARNING: ohMyLogo.png not found (panic screen keeps embedded data)'
}

# Generate desktop textures (PNG/webP -> raw RGBA for the desktop material pack)
$GenTex = Join-Path $Root 'CODE\desktop\gen_textures.py'
if (Test-Path $GenTex) {
    $TexSrc   = Join-Path $Root 'SYSTEM\system\deshab64\desktop\textures\startMenuLogo.png'
    $TexStamp = Join-Path $Root 'SYSTEM\system\deshab64\desktop\textures\startMenuLogo.rgba'
    $needTex = -not (Test-Path $TexStamp)
    if ($needTex -and (Test-Path $TexSrc)) {
        $needTex = (Get-Item $TexSrc).LastWriteTime -gt (Get-Item $TexStamp).LastWriteTime
    }
    if ($needTex) {
        Write-Host '[build] Generating desktop textures (gen_textures.py)...'
        $texDone = $false
        $Python = $null
        foreach ($p in @('C:\Users\林濬哲\AppData\Local\Programs\Python\Python312\python.exe',
                         'C:\msys64\mingw64\bin\python.exe',
                         'python', 'python3')) {
            try { $cmd = Get-Command $p -ErrorAction Stop; $Python = $cmd.Source; break } catch {}
        }
        if ($Python) {
            & $Python $GenTex
            if ($LASTEXITCODE -eq 0) { $texDone = $true }
        }
        if (-not $texDone) {
            # Fallback: WSL python3 (has Pillow on this machine)
            $wslPath = ($GenTex -replace '^([A-Za-z]):', '/mnt/$1').ToLower()
            & wsl python3 $wslPath
            if ($LASTEXITCODE -eq 0) { $texDone = $true }
        }
        if (-not $texDone) {
            Write-Host '[build] WARNING: desktop texture generation failed (material pack may be incomplete)'
        }
    }
}
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

$LoginDir = Join-Path $Root 'CODE\login'
$LoginOutput = Join-Path $SystemDir 'system\user\use\login.elf'

Write-Host '[build] Building login.elf...'
try {
    & $make -C "$LoginDir" -f MAKEFILE "CC=$clang" "LD=$lld"
    if ($LASTEXITCODE -ne 0) {
        throw "make login failed with exit code $LASTEXITCODE"
    }
}
finally {
}

if (-not (Test-Path $LoginOutput)) {
    throw "login build did not produce $LoginOutput"
}
Write-Host "[build] login Output: $LoginOutput"

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

$ShellDir = Join-Path $Root 'CODE\shell'
$ShellOutput = Join-Path $SystemDir 'system\deshab64\shell.elf'

Write-Host '[build] Building shell.elf...'
try {
    & $make -C "$ShellDir" -f MAKEFILE "CC=$clang" "LD=$lld"
    if ($LASTEXITCODE -ne 0) {
        throw "make shell failed with exit code $LASTEXITCODE"
    }
}
finally {
}

if (-not (Test-Path $ShellOutput)) {
    throw "shell build did not produce $ShellOutput"
}
Write-Host "[build] shell Output: $ShellOutput"

$DesktopDir = Join-Path $Root 'CODE\desktop'
$DesktopOutput = Join-Path $SystemDir 'system\deshab64\desktop.elf'

Write-Host '[build] Building desktop.elf...'
try {
    & $make -C "$DesktopDir" -f MAKEFILE "CC=$clang" "LD=$lld"
    if ($LASTEXITCODE -ne 0) {
        throw "make desktop failed with exit code $LASTEXITCODE"
    }
}
finally {
}

if (-not (Test-Path $DesktopOutput)) {
    throw "desktop build did not produce $DesktopOutput"
}
Write-Host "[build] desktop Output: $DesktopOutput"

# ---------- CMD (Windows-style command line + PE/EXE compat) ----------
$CmdDir = Join-Path $Root 'CODE\cmd'
$CmdOutput = Join-Path $SystemDir 'system\deshab64\cmd.elf'

Write-Host '[build] Building cmd.elf...'
try {
    & $make -C "$CmdDir" -f MAKEFILE "CC=$clang" "LD=$lld"
    if ($LASTEXITCODE -ne 0) {
        throw "make cmd failed with exit code $LASTEXITCODE"
    }
}
finally {
}

if (-not (Test-Path $CmdOutput)) {
    throw "cmd build did not produce $CmdOutput"
}
Write-Host "[build] cmd Output: $CmdOutput"

# ---------- Tool applications ----------
$ToolsDir = Join-Path $Root 'CODE\tools'
$ToolsOutDir = Join-Path $SystemDir 'system\deshab64\tools'

$toolApps = @('editor', 'fileman', 'browser', 'curl', 'ping', 'settings')

foreach ($tool in $toolApps) {
    $toolDir = Join-Path $ToolsDir $tool
    $toolMake = Join-Path $toolDir 'MAKEFILE'
    if (Test-Path $toolMake) {
        Write-Host "[build] Building ${tool}.elf..."
        try {
            & $make -C "$toolDir" -f MAKEFILE "CC=$clang" "LD=$lld"
            if ($LASTEXITCODE -ne 0) {
                Write-Host "[build] WARNING: make $tool failed with exit code $LASTEXITCODE"
            }
        }
        finally {
        }
    }
}

Write-Host '[build] Building DKM framebuffer driver...'
Build-DkmDriver (Join-Path $Root 'CODE\DKM\console_fb\console_fb.c') (Join-Path $SystemDir 'driver\console\console_fb.drv')

# B7: apic.drv 纳入构建 (原 platform/apic.drv 为预置二进制, 不随 apic.c 更新)
Write-Host '[build] Building DKM platform apic driver...'
Build-DkmDriver (Join-Path $Root 'CODE\DKM\apic\apic.c') (Join-Path $SystemDir 'driver\platform\apic.drv')

Write-Host '[build] Building DKM storage drivers...'
Build-DkmDriver (Join-Path $Root 'CODE\DKM\nvme\nvme.c') (Join-Path $SystemDir 'driver\block\nvme.drv')
Build-DkmDriver (Join-Path $Root 'CODE\DKM\ehci\ehci.c') (Join-Path $SystemDir 'driver\block\ehci.drv')
Build-DkmDriver (Join-Path $Root 'CODE\DKM\xhci\xhci.c') (Join-Path $SystemDir 'driver\block\xhci.drv')

Write-Host '[build] Building DKM network drivers...'
Build-DkmDriver (Join-Path $Root 'CODE\DKM\e1000\e1000.c') (Join-Path $SystemDir 'driver\net\e1000.drv')
Build-DkmDriver (Join-Path $Root 'CODE\DKM\virtio_net\virtio_net.c') (Join-Path $SystemDir 'driver\net\virtio_net.drv')
Build-DkmDriver (Join-Path $Root 'CODE\DKM\ath9k\ath9k.c') (Join-Path $SystemDir 'driver\net\ath9k.drv')

Write-Host "[build] Output: $Output"
Write-Host "[build] DSK Output: $DskOutput"
Write-Host "[build] Variant: $Variant"

# Remove legacy single-image to avoid confusion
$LegacyImg = Join-Path $IsoDir 'deshab.img'
if (Test-Path $LegacyImg) {
    Remove-Item -LiteralPath $LegacyImg -Force
    Write-Host "[build] Removed legacy image: $LegacyImg"
}

Write-Host '[build] Packaging SYSTEM to GPT images (p1 FAT32 ESP + p2 ext4, not a single FAT32 disk)...'
if ($Variant -in @('dev','both')) {
    Write-Host '[build] === Building DEV image (QEMU test, dev_mode=1, debug on) ==='
    Build-ImageVariant 'dev' $DevImagePath
    Write-Host "[build] DEV image done: $DevImagePath"
}
if ($Variant -in @('release','both')) {
    Write-Host '[build] === Building RELEASE image (real hardware, dev_mode=0, debug off) ==='
    Build-ImageVariant 'release' $ReleaseImagePath
    Write-Host "[build] RELEASE image done: $ReleaseImagePath"
}
if ($Variant -in @('realtest','both')) {
    Write-Host '[build] === Building REALTEST image (real hardware, disk_log=1, debug on) ==='
    Build-ImageVariant 'realtest' $RealtestImagePath
    Write-Host "[build] REALTEST image done: $RealtestImagePath"
}
# Restore SYSTEM to dev config (dev is the primary development target)
if ($Variant -eq 'both') {
    Write-Host '[build] Restoring SYSTEM to dev config for development...'
    Build-ImageVariant 'dev' $null
}

# Sidecar AHCI test disk only. Not one of the ISO/*.img system images.
Write-Host '[build] Rebuilding SATA FAT32 sidecar (.build_tmp/sata_fat32_dsk.img, not ISO/*.img)...'
$BuildTmp = Join-Path $Root '.build_tmp'
$SidecarSh = Join-Path $Root 'CODE\linux\make_sata_sidecar.sh'
$SidecarImg = Join-Path $BuildTmp 'sata_fat32_dsk.img'
if ((Test-Path $SidecarSh) -and (Get-Command wsl -ErrorAction SilentlyContinue)) {
    $wslSh = ConvertTo-WslUnixPath $SidecarSh
    $wslSys = ConvertTo-WslUnixPath $SystemDir
    $wslImg = ConvertTo-WslUnixPath $SidecarImg
    & wsl -u root -- bash $wslSh $wslSys $wslImg
    if ($LASTEXITCODE -ne 0) {
        Write-Host "[build] WARNING: sidecar rebuild failed ($LASTEXITCODE), keeping old image"
    } else {
        Write-Host "[build] SATA IMG: $SidecarImg"
    }
} else {
    Write-Host '[build] WARNING: make_sata_sidecar.sh/WSL unavailable, SATA image not rebuilt'
}

# NVMe 测试盘镜像 (64MB, LBA0 带 DESHABNVME0 签名, 用于验证 NVMe block 数据路径)
$NvmeImg = Join-Path $BuildTmp 'nvme_test.img'
if (-not (Test-Path $NvmeImg)) {
    Write-Host '[build] Creating NVMe test disk image...'
    $nfs = [System.IO.File]::Open($NvmeImg, [System.IO.FileMode]::Create, [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::None)
    try {
        $nfs.SetLength(64MB)
        $sigSector = [byte[]]::new(512)
        $sigText = [System.Text.Encoding]::ASCII.GetBytes('DESHABNVME0')
        for ($i = 0; $i -lt 512; $i++) { $sigSector[$i] = $sigText[$i % $sigText.Length] }
        Write-At $nfs 0 $sigSector
    }
    finally {
        $nfs.Close()
    }
    Write-Host "[build] NVMe IMG: $NvmeImg"
}

# USB xHCI test disk image (64MB, for USB Mass Storage driver testing)
$UsbImg = Join-Path $BuildTmp 'usb_test.img'
if (-not (Test-Path $UsbImg)) {
    Write-Host '[build] Creating USB test disk image...'
    $ufs = [System.IO.File]::Open($UsbImg, [System.IO.FileMode]::Create, [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::None)
    try {
        $ufs.SetLength(64MB)
        $sigSector = [byte[]]::new(512)
        $sigText = [System.Text.Encoding]::ASCII.GetBytes('DESHABUSB00')
        for ($i = 0; $i -lt 512; $i++) { $sigSector[$i] = $sigText[$i % $sigText.Length] }
        Write-At $ufs 0 $sigSector
    }
    finally {
        $ufs.Close()
    }
    Write-Host "[build] USB IMG: $UsbImg"
}
