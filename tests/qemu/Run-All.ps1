#requires -Version 5.1
<#
.SYNOPSIS
    Run-All — Deshab QEMU 自动化测试套件总运行器

.DESCRIPTION
    顺序执行 tests/qemu 下全部 9 个用例脚本，每用例独立 powershell 子进程
    （独立 QEMU 会话 + firstInit/AUTOEXEC 场景隔离），汇总 PASS/FAIL/耗时。

    用例顺序（依赖与耗时综合考虑，快用例靠后）：
      1. boot-regression.ps1    B1 首启向导 / B2 正常 / B3 dev_mode / B4 无盘 / B5 无 NIC
      2. driver-matrix.ps1      16 清单驱动加载矩阵 + NVMe 写路径回归
      3. linux-compat.ps1       VMM 分流（QEMU 走 VMM unavailable 优雅降级路径）
      4. pe-compat.ps1          PE32+ 原生 + PE32 x86emu32 + 错误路径
      5. deaicup-e2e.ps1        Deaicup GUI PE 端到端（首帧像素/键鼠消息/Esc 退出链）
      6. userapps-matrix.ps1    shell dev_tests + ping/editor/fileman/browser 工具链
      7. network-e2e.ps1        N-A netman 全链路 + N-B curl 主机侧端到端
      8. performance-baseline.ps1
                                启动阶段墙上计时 → .build_tmp/perf_baseline.json
      9. x86emu32-coverage.ps1  x86emu32 静态枚举 + hello32 动态解释执行

.PARAMETER Only
    只跑指定用例（逗号分隔的脚本基名，大小写不敏感）：
      .\Run-All.ps1 -Only network-e2e,x86emu32-coverage

.PARAMETER Build
    先执行一次 build.ps1（全量编译+打包镜像）再跑用例。

.NOTES
    退出码：全部用例 PASS → 0；任一 FAIL → 1。
    每用例完整输出存档 .build_tmp\tests\runall\<case>.log。
#>
param(
    [string[]]$Only = @(),
    [switch]$Build
)
$ErrorActionPreference = 'Stop'
$suiteRoot = $PSScriptRoot
$repoRoot  = [System.IO.Path]::GetFullPath((Join-Path $suiteRoot '..\..'))
$outDir    = Join-Path $repoRoot '.build_tmp\tests\runall'
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

# ---- 跑前清理：只杀本套件历史会话残留的 QEMU（-name deshabtest- 标记）。
#      绝不全局清理——可能有其他代理/人工在并发跑 QEMU（如 monitor 45454 的调试会话）。
#      本套件所有会话默认 -snapshot（镜像只读），无需靠杀进程解决镜像锁。 ----
. (Join-Path $suiteRoot 'lib\QemuTest.ps1')
Stop-ResidualQemu

# ---- 可选：先构建一次 ----
if ($Build) {
    Write-Host '[runall] running build.ps1 ...'
    & powershell -ExecutionPolicy Bypass -File (Join-Path $repoRoot 'build.ps1')
    if ($LASTEXITCODE -ne 0) { Write-Host "[runall] build.ps1 FAILED (exit $LASTEXITCODE)"; exit 1 }
}

$cases = @(
    'boot-regression',
    'driver-matrix',
    'linux-compat',
    'pe-compat',
    'deaicup-e2e',
    'userapps-matrix',
    'network-e2e',
    'performance-baseline',
    'x86emu32-coverage'
)
if ($Only.Count -gt 0) {
    # -File 调用下逗号列表常作为单个字符串传入，统一按逗号再切一次
    $want = @($Only | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim().ToLower() } | Where-Object { $_ })
    $cases = @($cases | Where-Object { $want -contains $_.ToLower() })
    if ($cases.Count -eq 0) { Write-Host "[runall] -Only 过滤后无用例可跑: $($Only -join ',')"; exit 1 }
}

$rows = New-Object System.Collections.ArrayList
$suiteStart = Get-Date
foreach ($case in $cases) {
    $script = Join-Path $suiteRoot "$case.ps1"
    $log    = Join-Path $outDir "$case.log"
    Write-Host ''
    Write-Host ("================ [runall] {0} ================" -f $case)
    $t0 = Get-Date
    & powershell -ExecutionPolicy Bypass -File $script 2>&1 | Tee-Object -FilePath $log | Out-Host
    $rc = $LASTEXITCODE
    $dur = [math]::Round(((Get-Date) - $t0).TotalSeconds, 1)

    $outText = ''
    if (Test-Path $log) { $outText = Get-Content $log -Raw }
    $manualCount = ([regex]::Matches($outText, '\[MANUAL\]')).Count
    $skipCount   = ([regex]::Matches($outText, '\[SKIP\]')).Count
    $status = 'PASS'
    if ($rc -ne 0) { $status = 'FAIL' }

    [void]$rows.Add([pscustomobject]@{
        Case     = $case
        Status   = $status
        Duration = "${dur}s"
        Manual   = $manualCount
        Skip     = $skipCount
        ExitCode = $rc
        Log      = $log
    })
}

# ---- 汇总表 ----
$totalDur = [math]::Round(((Get-Date) - $suiteStart).TotalMinutes, 1)
Write-Host ''
Write-Host ("================ 测试套件汇总 ({0} min) ================" -f $totalDur)
$rows | Format-Table Case, Status, Duration, Manual, Skip, ExitCode -AutoSize | Out-Host

$failCount = @($rows | Where-Object { $_.Status -eq 'FAIL' }).Count
$passCount = @($rows | Where-Object { $_.Status -eq 'PASS' }).Count
Write-Host ("PASS: {0}  FAIL: {1}  TOTAL: {2}" -f $passCount, $failCount, $rows.Count)
Write-Host ("日志目录: {0}" -f $outDir)

if ($failCount -gt 0) { exit 1 } else { exit 0 }
