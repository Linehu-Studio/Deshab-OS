<#
.SYNOPSIS
    Deshab SDK 编译验证脚本。
.DESCRIPTION
    验证所有 SDK 头文件可编译（自洽性），示例和工具可构建。
    用法: .\sdk-test.ps1 [-LlvmBin <path>]
#>
param(
    [string]$LlvmBin = 'C:\Program Files\LLVM\bin'
)

$ErrorActionPreference = 'Stop'
$SdkDir  = $PSScriptRoot
$RootDir = [System.IO.Path]::GetFullPath((Join-Path $SdkDir '..\..'))

$Clang = Join-Path $LlvmBin 'clang.exe'
if (-not (Test-Path $Clang)) { throw "clang not found: $Clang" }
$env:PATH = "$LlvmBin;$env:PATH"

$global:pass = 0
$global:fail = 0

function Test-Step($name, [scriptblock]$sb) {
    Write-Host -NoNewline "[sdk-test] $name ... "
    try {
        & $sb
        if ($LASTEXITCODE -ne 0) { throw "exit code $LASTEXITCODE" }
        Write-Host "PASS" -ForegroundColor Green
        $global:pass++
    } catch {
        Write-Host "FAIL: $_" -ForegroundColor Red
        $global:fail++
    }
}

# ---- 1. 头文件自洽性测试 ----
$tmpDir = Join-Path $env:TEMP 'deshab-sdk-test'
New-Item -ItemType Directory -Force -Path $tmpDir | Out-Null
$testC = @'
#include "deshab/types.h"
/* 提供字体位图 dummy 定义，使 font.h/ui.h 可编译 */
static const u8 *g_ascii[95] = {0};
static i64 g_ascii_w = 8;
static i64 g_ascii_h = 16;
#include "deshab/portio.h"
#include "deshab/serial.h"
#include "deshab/tsc.h"
#include "deshab/string.h"
#include "deshab/kernel_api.h"
#include "deshab/boot_context.h"
#include "deshab/driver.h"
#include "deshab/block.h"
#include "deshab/app.h"
#include "deshab/fb.h"
#include "deshab/cursor.h"
#include "deshab/input.h"
#include "deshab/ui.h"
#include "deshab/rtc.h"
#include "deshab/fat32.h"
#include "deshab/net.h"

/* 触发 static inline 实例化，确保头文件无编译错误 */
void sdk_selftest(void) {
    dsb_app_context ac;  dsb_cursor cur;  dsb_mouse ms;  dsb_block blk;
    (void)ac; (void)cur; (void)ms; (void)blk;
}
'@
$testFile = Join-Path $tmpDir 'sdk_selftest.c'
Set-Content $testFile $testC

Test-Step "headers self-consistency" {
    & $Clang -target x86_64-unknown-none -std=c11 -ffreestanding -fno-builtin `
        -fno-stack-protector -fpie -mno-red-zone -mno-sse -mno-sse2 -mno-mmx `
        -msoft-float -m64 -Wall -Wextra "-I$(Join-Path $SdkDir 'include')" -O2 `
        -c $testFile -o (Join-Path $tmpDir 'sdk_selftest.o') 2>&1
}

# ---- 2. hello-app 编译+链接 ----
Test-Step "hello-app build" {
    Push-Location (Join-Path $SdkDir 'examples\hello-app')
    & $Clang -target x86_64-unknown-none -std=c11 -ffreestanding -fno-builtin `
        -fno-stack-protector -fpie -mno-red-zone -mno-sse -mno-sse2 -mno-mmx `
        -msoft-float -m64 -Wall -Wextra -I../../include -O2 -c main.c -o main.o 2>&1
    & (Join-Path $LlvmBin 'ld.lld.exe') -nostdlib -static -pie -z max-page-size=0x1000 `
        -T linker.ld -o hello.elf main.o 2>&1
    Pop-Location
}

# ---- 3. hello-driver 编译+链接 ----
Test-Step "hello-driver build" {
    Push-Location (Join-Path $SdkDir 'examples\hello-driver')
    & $Clang -target x86_64-unknown-none -std=c11 -ffreestanding -fno-builtin `
        -fno-stack-protector -fno-pic -fno-pie -mcmodel=kernel -mno-red-zone `
        -mno-sse -mno-sse2 -mno-mmx -msoft-float -m64 -Wall -Wextra -I../../include -O2 `
        -c main.c -o hello.o 2>&1
    & (Join-Path $LlvmBin 'ld.lld.exe') -r -o hello.drv hello.o 2>&1
    Pop-Location
}

# ---- 4. sysinfo 编译+链接 ----
Test-Step "sysinfo build" {
    Push-Location (Join-Path $RootDir 'CODE\tools\sysinfo')
    & $Clang -target x86_64-unknown-none -std=c11 -ffreestanding -fno-builtin `
        -fno-stack-protector -fpie -mno-red-zone -mno-sse -mno-sse2 -mno-mmx `
        -msoft-float -m64 -Wall -Wextra -I../../sdk/include -O2 -c main.c -o main.o 2>&1
    & (Join-Path $LlvmBin 'ld.lld.exe') -nostdlib -static -pie -z max-page-size=0x1000 `
        -T linker.ld -o sysinfo.elf main.o 2>&1
    Pop-Location
}

# ---- 汇总 ----
Write-Host ""
Write-Host "[sdk-test] Result: $global:pass passed, $global:fail failed" `
    -ForegroundColor $(if ($global:fail -eq 0) {'Green'} else {'Red'})
exit $global:fail
