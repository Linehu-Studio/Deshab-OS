<#
.SYNOPSIS
    Deshab SDK 一键创建新应用/驱动模块脚手架。

.DESCRIPTION
    从 CODE/sdk/scaffold/ 模板生成新模块，自动替换占位符并计算相对路径。

.PARAMETER Name
    模块名（如 myapp、mydriver）。

.PARAMETER Type
    模块类型：app（应用 .elf）或 driver（驱动 .drv）。默认 app。

.PARAMETER OutDir
    输出目录。默认 app -> CODE/tools/$Name，driver -> CODE/DKM/$Name。

.PARAMETER Class
    驱动类别（DKM_CLASS_*）。仅 Type=driver 时使用。默认 DKM_CLASS_MISC。

.PARAMETER Stage
    驱动加载阶段 0-3。仅 Type=driver 时使用。默认 3。

.EXAMPLE
    .\new-module.ps1 -Name myapp
    .\new-module.ps1 -Name mydriver -Type driver -Class DKM_CLASS_NET -Stage 3
#>
param(
    [Parameter(Mandatory=$true)][string]$Name,
    [ValidateSet('app','driver')][string]$Type = 'app',
    [string]$OutDir,
    [string]$Class = 'DKM_CLASS_MISC',
    [int]$Stage = 3
)

$ErrorActionPreference = 'Stop'
$SdkDir  = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$CodeDir = [System.IO.Path]::GetFullPath((Join-Path $SdkDir '..'))
$RootDir = [System.IO.Path]::GetFullPath((Join-Path $CodeDir '..'))

if (-not $OutDir) {
    if ($Type -eq 'app') {
        $OutDir = Join-Path $CodeDir "tools\$Name"
    } else {
        $OutDir = Join-Path $CodeDir "DKM\$Name"
    }
}
$OutDir = [System.IO.Path]::GetFullPath($OutDir)
if (Test-Path $OutDir) { throw "目录已存在: $OutDir" }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

# 计算从 $from 到 $to 的相对路径（反斜杠）
function Get-RelPath($from, $to) {
    $fromUri = [System.Uri]::new($from.TrimEnd('\') + '\')
    $toUri   = [System.Uri]::new($to.TrimEnd('\') + '\')
    if (Test-Path $to -PathType Leaf) {
        $toUri = [System.Uri]::new($to)
    }
    return $fromUri.MakeRelativeUri($toUri).ToString().Replace('/', '\')
}

if ($Type -eq 'app') {
    $SdkInc   = Get-RelPath $OutDir (Join-Path $SdkDir 'include')
    $Bitmaps  = Get-RelPath $OutDir (Join-Path $CodeDir 'firstInit\ascii_bitmaps.c')
    $OutPath  = Join-Path $RootDir "SYSTEM\system\deshab64\tools\$Name.elf"
    $OutDirR  = Get-RelPath $OutDir (Split-Path $OutPath -Parent)

    # MAKEFILE
    $mf = Get-Content (Join-Path $SdkDir 'scaffold\app\MAKEFILE.tmpl') -Raw
    $mf = $mf.Replace('{{MODULE_NAME}}', $Name).Replace('{{SDK_INC}}', $SdkInc).Replace('{{OUT_DIR}}', $OutDirR)
    Set-Content (Join-Path $OutDir 'MAKEFILE') $mf -NoNewline

    # linker.ld
    Copy-Item (Join-Path $SdkDir 'scaffold\app\linker.ld.tmpl') (Join-Path $OutDir 'linker.ld')

    # main.c
    $mc = Get-Content (Join-Path $SdkDir 'scaffold\app\main.c.tmpl') -Raw
    $mc = $mc.Replace('{{MODULE_NAME}}', $Name).Replace('{{BITMAPS}}', $Bitmaps)
    Set-Content (Join-Path $OutDir 'main.c') $mc -NoNewline

    Write-Host "[sdk] 应用 '$Name' 已创建: $OutDir" -ForegroundColor Green
    Write-Host "[sdk] 构建:  cd $OutDir ; make" -ForegroundColor Cyan
    Write-Host "[sdk] 输出:  $OutPath" -ForegroundColor Cyan
} else {
    # main.c
    $mc = Get-Content (Join-Path $SdkDir 'scaffold\driver\main.c.tmpl') -Raw
    $mc = $mc.Replace('{{MODULE_NAME}}', $Name).Replace('{{CLASS}}', $Class).Replace('{{STAGE}}', [string]$Stage)
    Set-Content (Join-Path $OutDir 'main.c') $mc -NoNewline

    Write-Host "[sdk] 驱动 '$Name' 已创建: $OutDir" -ForegroundColor Green
    Write-Host "[sdk] 构建:  clang -target x86_64-unknown-none -std=c11 -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -fno-pie -mcmodel=kernel -mno-red-zone -mno-sse -mno-sse2 -mno-mmx -msoft-float -m64 -Wall -Wextra -I$(Join-Path $SdkDir 'include') -O2 -c main.c -o $Name.o" -ForegroundColor Cyan
    Write-Host "          ld.lld -r -o $Name.drv $Name.o" -ForegroundColor Cyan
    Write-Host "[sdk] 注册:  在 SYSTEM/driver/manifest.json 添加条目" -ForegroundColor Yellow
    Write-Host "[sdk] 预加载: 在 SYSTEM/boot/limine.conf 添加 module_path" -ForegroundColor Yellow
}
