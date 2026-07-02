$ErrorActionPreference = 'Stop'

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$Root = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $ScriptDir))
$Source = Join-Path $Root 'CODE\DKM\stub_driver\stub_driver.c'
$Manifest = Join-Path $Root 'SYSTEM\driver\manifest.json'
$SystemDriver = Join-Path $Root 'SYSTEM\driver'
$BuildDir = Join-Path $Root '.build_tmp\drivers'

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

function Get-DriverClassValue([string]$Class) {
    switch ($Class) {
        'platform' { return 1 }
        'bus' { return 2 }
        'interrupt' { return 3 }
        'timer' { return 4 }
        'console' { return 5 }
        'storage' { return 6 }
        'fs' { return 7 }
        'fs-core' { return 8 }
        'net' { return 9 }
        'input' { return 10 }
        'gpu' { return 11 }
        default { return 0 }
    }
}

$clang = Find-Tool 'clang'
$lld = Find-Tool 'ld.lld'
if (-not $clang) {
    throw 'clang was not found. Install LLVM/Clang.'
}
if (-not $lld) {
    throw 'ld.lld was not found. Install LLVM lld.'
}

New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
$manifestData = Get-Content -Raw -LiteralPath $Manifest | ConvertFrom-Json
$count = 0

foreach ($stage in $manifestData.stages) {
    foreach ($driver in $stage.drivers) {
        $relative = [string]$driver.path
        if (-not $relative.StartsWith('driver/')) {
            throw "Unexpected driver path: $relative"
        }

        $destRelative = $relative.Substring('driver/'.Length).Replace('/', '\')
        $dest = Join-Path $SystemDriver $destRelative
        $destDir = Split-Path -Parent $dest
        New-Item -ItemType Directory -Force -Path $destDir | Out-Null

        $name = [string]$driver.name
        $classValue = Get-DriverClassValue ([string]$driver.class)
        $stageValue = [int]$stage.id
        $flags = 0
        if ([bool]$driver.required) { $flags = $flags -bor 1 }
        if ($stageValue -le 1) { $flags = $flags -bor 2 }

        $obj = Join-Path $BuildDir "$name.o"
        $drv = Join-Path $BuildDir "$name.drv"
        $driverNameDefine = 'DRIVER_NAME=' + $name

        & $clang -target x86_64-unknown-none -std=c11 -ffreestanding -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -Wall -Wextra -O2 -c $Source -o $obj "-D$driverNameDefine" "-DDRIVER_CLASS=$classValue" "-DDRIVER_STAGE=$stageValue" "-DDRIVER_FLAGS=$flags"
        if ($LASTEXITCODE -ne 0) { throw "failed to compile stub driver: $name" }

        & $lld -r -o $drv $obj
        if ($LASTEXITCODE -ne 0) { throw "failed to link stub driver: $name" }

        Copy-Item -LiteralPath $drv -Destination $dest -Force
        Write-Host "[drivers] installed $relative"
        $count++
    }
}

Write-Host "[drivers] installed $count DKM stub drivers"
