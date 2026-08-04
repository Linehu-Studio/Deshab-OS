#requires -Version 5.1
<#
.SYNOPSIS
    performance-baseline — 启动性能基线（docs/RE/test-cases/performance-baseline.md）

.DESCRIPTION
    正常启动场景（firstInit="1\n0", AUTOEXEC=ver/exit），按墙上时钟记录
    关键阶段锚点在串口日志中首次出现的时刻，计算阶段时间差：

      t0            QEMU 进程启动（含 OVMF + Limine bootloader）
      [UTSM] boot   UTSM 内核入口
      [UTSM] SELFTEST PASS
                    UTSM 自检完成（位于 4 stage 共 16 驱动装载之后，
                    即"驱动加载完"时刻）
      [DSK] boot    DSK 主内核接管
      desktop ready 桌面就绪（login skipped → cmd(AUTOEXEC) → desktop）

    输出 .build_tmp\perf_baseline.json（锚点绝对时刻 + 阶段毫秒数 + 环境信息）。
    本用例只测量展示、不做阈值断言（文档标 P2，QEMU 数值仅作回归参考）。
    多次测量取中位数：默认 3 轮，可用 -Runs 调整。

    退出码：测量完成 0；会话/锚点异常 1。
#>
param(
    [switch]$Build,
    [int]$Runs = 3
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'lib\QemuTest.ps1')

Initialize-QemuTest -Build:$Build
$result = New-TestCaseResult 'performance-baseline'

# 阶段锚点定义（顺序即启动顺序）
$anchorDefs = @(
    @{ Key = 'utsm_boot';       Pattern = '[UTSM] boot' },
    @{ Key = 'utsm_selftest';   Pattern = '[UTSM] SELFTEST PASS' },
    @{ Key = 'dsk_boot';        Pattern = '[DSK] boot' },
    @{ Key = 'desktop_ready';   Pattern = 'desktop ready' }
)

# 轮询串口日志，记录每个锚点首次出现的墙上时刻；返回 hashtable 或 $null
function Measure-BootPhases {
    param([Parameter(Mandatory)]$Session, [int]$TimeoutSeconds = 240)
    $marks = @{}
    foreach ($a in $anchorDefs) { $marks[$a.Key] = $null }
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    $remaining = $anchorDefs.Count
    while ((Get-Date) -lt $deadline -and $remaining -gt 0) {
        $text = Read-QemuLogText $Session.LogPath
        foreach ($a in $anchorDefs) {
            if ($null -eq $marks[$a.Key] -and $text.Contains($a.Pattern)) {
                $marks[$a.Key] = Get-Date
                $remaining--
            }
        }
        if (Test-QemuSessionExited $Session) { break }
        Start-Sleep -Milliseconds 200
    }
    return $marks
}

$allRuns = New-Object System.Collections.ArrayList
try {
    for ($run = 1; $run -le $Runs; $run++) {
        Write-Host "`n--- perf run $run/$Runs (正常启动 → desktop ready) ---"
        Set-DeshabFirstInit -First 1 -DevMode 0
        Set-DeshabAutoexec -Lines @('ver', 'exit')

        $t0 = Get-Date
        $s = Start-QemuSession -Name "perf-run$run" -MonitorPort 45507
        try {
            $marks = Measure-BootPhases -Session $s -TimeoutSeconds 240
            $result.LogPath = $s.LogPath
        } finally {
            Stop-QemuSession $s
        }

        if ($null -eq $marks['desktop_ready']) {
            Add-TestCheck $result "run${run}: 锚点采集完整" 'FAIL' 'desktop ready not observed within timeout'
            continue
        }
        Add-TestCheck $result "run${run}: 锚点采集完整" 'PASS'

        $ms = [ordered]@{
            ovmf_bootloader_ms = [int]($marks['utsm_boot'] - $t0).TotalMilliseconds
            utsm_drivers_ms    = [int]($marks['utsm_selftest'] - $marks['utsm_boot']).TotalMilliseconds
            selftest_to_dsk_ms = [int]($marks['dsk_boot'] - $marks['utsm_selftest']).TotalMilliseconds
            dsk_to_desktop_ms  = [int]($marks['desktop_ready'] - $marks['dsk_boot']).TotalMilliseconds
            total_boot_ms      = [int]($marks['desktop_ready'] - $t0).TotalMilliseconds
        }
        [void]$allRuns.Add([pscustomobject]@{
            run        = $run
            started_at = $t0.ToString('yyyy-MM-dd HH:mm:ss')
            metrics    = $ms
        })
        Write-Host ("  ovmf={0}ms utsm+drivers={1}ms dsk_load={2}ms dsk->desktop={3}ms total={4}ms" -f `
            $ms.ovmf_bootloader_ms, $ms.utsm_drivers_ms, $ms.selftest_to_dsk_ms, $ms.dsk_to_desktop_ms, $ms.total_boot_ms)
    }
} finally {
    Restore-DeshabScenario
}

# ---- 中位数汇总 + JSON 输出 ----
if ($allRuns.Count -gt 0) {
    $median = [ordered]@{}
    foreach ($k in @('ovmf_bootloader_ms', 'utsm_drivers_ms', 'selftest_to_dsk_ms', 'dsk_to_desktop_ms', 'total_boot_ms')) {
        $vals = @($allRuns | ForEach-Object { [int]$_.metrics[$k] } | Sort-Object)
        $median[$k] = $vals[[int]($vals.Count / 2)]
    }

    $out = [ordered]@{
        generated_at = (Get-Date).ToString('yyyy-MM-dd HH:mm:ss')
        method       = 'host wall-clock polling of serial log anchors (200ms poll, +/-200ms 精度)'
        environment  = [ordered]@{
            accel    = 'whpx'
            cpu      = 'qemu64,-vmx'
            machine  = 'q35'
            memory   = '2G'
            scenario = 'firstInit=1 dev_mode=0 AUTOEXEC=ver,exit -> desktop ready'
            snapshot = $true
        }
        median_ms    = $median
        runs         = $allRuns
    }
    $jsonPath = Join-Path $script:BuildTmp 'perf_baseline.json'
    ($out | ConvertTo-Json -Depth 6) | Out-File -Encoding utf8 $jsonPath
    Add-TestNote $result "基线 JSON: $jsonPath"

    Add-TestCheck $result ("P1: UTSM 启动+驱动装载 (utsm_boot→SELFTEST) = {0} ms (目标<500ms QEMU, 仅展示)" -f $median.utsm_drivers_ms) 'PASS'
    Add-TestCheck $result ("DSK 加载 (SELFTEST→[DSK] boot) = {0} ms" -f $median.selftest_to_dsk_ms) 'PASS'
    Add-TestCheck $result ("DSK→desktop ready = {0} ms" -f $median.dsk_to_desktop_ms) 'PASS'
    Add-TestCheck $result ("总启动 (QEMU 启动→desktop ready) = {0} ms" -f $median.total_boot_ms) 'PASS'
    Add-TestNote $result '本用例不做阈值断言；QEMU WHPX 数值仅作回归参考（文档：QEMU≠真机，固定加速方案）'
    Add-TestNote $result 'P2/P3/P6（单 ELF 加载计时）、P4 ping RTT、P7 桌面帧率、P8-P11（PE/VMM/Linux）需内核 TSC 打点日志，当前串口无对应锚点，保持 PENDING（见建议锚点清单）'
}

Write-TestCaseResult $result
if ($result.Status -eq 'FAIL') { exit 1 } else { exit 0 }
