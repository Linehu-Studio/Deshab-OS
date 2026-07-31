#requires -Version 5.1
<#
.SYNOPSIS
    boot-regression — 启动链路回归测试（docs/RE/test-cases/boot-regression.md）

.DESCRIPTION
    场景覆盖：
      B1 首次启动  firstInit.txt="0\n0"  UTSM → DSK → mouseInit → netman → FirstInit 向导(自动按键) → desktop
      B2 正常启动  firstInit.txt="1\n0"  UTSM → DSK → login(skip) → cmd(AUTOEXEC=ver/exit) → desktop
      B3 dev_mode  firstInit.txt="1\n1"  DSK → shell dev_tests → Esc → cmd → desktop
      B4 无 SATA 盘                     UTSM DSK 加载回退 Limine boot module，DSK 降级运行不崩溃
      B5 无 NIC                         e1000/virtio_net not found，系统继续启动到 desktop
      B6 Linux 兼容层                    见 linux-compat.ps1

    退出码：全部通过 0，任一 FAIL 1。
#>
param(
    [switch]$Build,
    [string[]]$Scenarios = @('B1', 'B2', 'B3', 'B4', 'B5')
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'lib\QemuTest.ps1')

Initialize-QemuTest -Build:$Build
$result = New-TestCaseResult 'boot-regression'

# ----------------------------------------------------------------
# FirstInit 设置向导自动填表：
#   账户卡片(3 字段) → 偏好页(5×Enter) → 网络页(3×Enter) → 完成
# 每步都等串口锚点再发键，字段验证失败自动重试。
# ----------------------------------------------------------------
function Complete-FirstInitWizard {
    param([Parameter(Mandatory)]$Session)

    $hit = Wait-QemuLog -Session $Session -Patterns @('[FI] fadein done') -TimeoutSeconds 150
    if (-not $hit) { Add-TestCheck $result 'B1: FirstInit 账户卡片就绪' 'FAIL' 'missing [FI] fadein done'; return $false }

    $done = $false
    for ($attempt = 1; $attempt -le 3 -and -not $done; $attempt++) {
        Send-QemuText -Session $Session -Text 'testpc' -Enter
        Start-Sleep -Milliseconds 400
        Send-QemuText -Session $Session -Text 'tester' -Enter
        Start-Sleep -Milliseconds 400
        Send-QemuText -Session $Session -Text 'pass123' -Enter
        $hit = Wait-QemuLog -Session $Session -Patterns @('[FI] all fields done', '[FI] validation failed') -TimeoutSeconds 30
        if ($hit -eq '[FI] all fields done') { $done = $true }
        else { Add-TestNote $result "B1: FirstInit 字段验证失败，重试第 $attempt 次" }
    }
    if (-not $done) { Add-TestCheck $result 'B1: FirstInit 账户字段填写' 'FAIL' 'validation did not pass after 3 attempts'; return $false }
    Add-TestCheck $result 'B1: FirstInit 账户字段填写' 'PASS'

    Start-Sleep -Milliseconds 600
    Send-QemuKeys -Session $Session -Keys @('ret', 'ret', 'ret', 'ret', 'ret') -DelayMs 350
    $hit = Wait-QemuLog -Session $Session -Patterns @('[FirstInit] entering network setup page') -TimeoutSeconds 40
    if (-not $hit) { Add-TestCheck $result 'B1: FirstInit 偏好页' 'FAIL' 'did not reach network setup page'; return $false }
    Add-TestCheck $result 'B1: FirstInit 偏好页(5xEnter)' 'PASS'

    Start-Sleep -Milliseconds 400
    Send-QemuKeys -Session $Session -Keys @('ret', 'ret', 'ret') -DelayMs 350
    $hit = Wait-QemuLog -Session $Session -Patterns @('[FirstInit] user setup finished') -TimeoutSeconds 40
    if (-not $hit) { Add-TestCheck $result 'B1: FirstInit 网络页' 'FAIL' 'setup did not finish'; return $false }
    Add-TestCheck $result 'B1: FirstInit 网络页(3xEnter)' 'PASS'
    return $true
}

# ----------------------------------------------------------------
function Invoke-ScenarioB1 {
    Write-Host "`n--- B1 首次启动 (firstInit=0) ---"
    Set-DeshabFirstInit -First 0 -DevMode 0
    $s = Start-QemuSession -Name 'bootreg-b1'
    try {
        if (-not (Complete-FirstInitWizard -Session $s)) { return }
        $hit = Wait-QemuLog -Session $s -Patterns @('desktop ready') -TimeoutSeconds 120
        $log = Read-QemuLogText $s.LogPath
        $result.LogPath = $s.LogPath
        Assert-QemuLog $result $log -Ordered @(
            '[UTSM] SELFTEST PASS',
            '[DSK] boot',
            '[DSK] firstInit=0',
            '[DSK:SCHED] decision: is_first=1 -> mouseInit -> netman -> FirstInit -> desktop',
            '[DSK] loading mouseInit',
            '[DSK] loading netman',
            '[DSK] loading FirstInit',
            '[FirstInit] boot',
            '[FirstInit] user setup finished',
            '[DSK] FirstInit returned, persisting user.conf',
            '[DSK] writing USER.CONF',
            '[DSK] firstInit.txt flip rc=',
            '[DSK] loading desktop',
            'desktop ready'
        ) -MustNotContain @('[PANIC]')
        if ($hit) { Add-TestCheck $result 'B1: desktop ready' 'PASS' }
        else { Add-TestCheck $result 'B1: desktop ready' 'FAIL' 'timeout waiting desktop ready' }
    } finally {
        Stop-QemuSession $s
    }
}

function Invoke-ScenarioB2 {
    Write-Host "`n--- B2 正常启动 (firstInit=1, AUTOEXEC=ver/exit) ---"
    Set-DeshabFirstInit -First 1 -DevMode 0
    Set-DeshabAutoexec -Lines @('ver', 'exit')
    $s = Start-QemuSession -Name 'bootreg-b2'
    try {
        $hit = Wait-QemuLog -Session $s -Patterns @('desktop ready') -TimeoutSeconds 150
        $log = Read-QemuLogText $s.LogPath
        $result.LogPath = $s.LogPath
        Assert-QemuLog $result $log -Ordered @(
            '[UTSM] SELFTEST PASS',
            '[UTSM] loading DSK',
            '[DSK] boot',
            '[DSK] context ok',
            '[DSK] firstInit != 0',
            '[DSK:SCHED] decision: is_first=0 -> login then desktop',
            '[DSK:SCHED] login skipped by FUCK config',
            '[DSK:SCHED] default_shell=cmd -> cmd.elf',
            '[cmd] boot',
            '[cmd] running AUTOEXEC.BAT',
            '[DSK] cmd.elf returned, falling back to desktop',
            '[DSK:SCHED] loading desktop',
            'desktop ready'
        ) -MustNotContain @('[PANIC]')
        if ($hit) { Add-TestCheck $result 'B2: desktop ready' 'PASS' }
        else { Add-TestCheck $result 'B2: desktop ready' 'FAIL' 'timeout waiting desktop ready' }
    } finally {
        Stop-QemuSession $s
    }
}

function Invoke-ScenarioB3 {
    Write-Host "`n--- B3 dev_mode (firstInit=1\1) ---"
    Set-DeshabFirstInit -First 1 -DevMode 1
    Set-DeshabAutoexec -Lines @('ver', 'exit')
    $s = Start-QemuSession -Name 'bootreg-b3'
    try {
        $hit = Wait-QemuLog -Session $s -Patterns @('[8] rm TEST.TXT') -TimeoutSeconds 150
        if (-not $hit) {
            $result.LogPath = $s.LogPath
            Add-TestCheck $result 'B3: shell dev_tests 执行' 'FAIL' 'missing [8] rm TEST.TXT'
            return
        }
        Start-Sleep -Seconds 2
        Send-QemuKeys -Session $s -Keys @('esc')
        $hit2 = Wait-QemuLog -Session $s -Patterns @('desktop ready') -TimeoutSeconds 150
        $log = Read-QemuLogText $s.LogPath
        $result.LogPath = $s.LogPath
        Assert-QemuLog $result $log -Ordered @(
            '[DSK] dev_mode=1 (developer auto-test)',
            '[DSK:SCHED] decision: dev_mode -> shell.elf',
            '[shell] boot',
            '[1] ls',
            '[4] cp TEST.TXT COPY.TXT',
            '[8] rm TEST.TXT',
            '[shell] esc -> return to DSK',
            '[DSK] shell auto-test returned',
            '[DSK:SCHED] default_shell=cmd -> cmd.elf',
            'desktop ready'
        ) -MustNotContain @('[PANIC]')
        if ($hit2) { Add-TestCheck $result 'B3: Esc 返回后到 desktop' 'PASS' }
        else { Add-TestCheck $result 'B3: Esc 返回后到 desktop' 'FAIL' 'timeout waiting desktop ready' }
    } finally {
        Stop-QemuSession $s
    }
}

function Invoke-ScenarioB4 {
    Write-Host "`n--- B4 无 SATA 盘 (DSK 回退 Limine module) ---"
    $s = Start-QemuSession -Name 'bootreg-b4' -NoSata
    try {
        $hit = Wait-QemuLog -Session $s -Patterns @('[DSK] firstInit.txt not found', '[DSK] desktop load failed', '[PANIC]') -TimeoutSeconds 120
        Start-Sleep -Seconds 3
        $log = Read-QemuLogText $s.LogPath
        $result.LogPath = $s.LogPath
        Assert-QemuLog $result $log -Ordered @(
            '[UTSM] SELFTEST PASS',
            '[UTSM] loading DSK',
            '[UTSM] FAT32 block path unavailable; trying Limine module',
            '[DSK] boot',
            '[DSK] context ok',
            '[DSK] firstInit.txt not found, assuming first boot'
        ) -MustContain @('[DSK] mouseInit not found') -MustNotContain @('[PANIC]')
        if ($hit -eq '[PANIC]') { Add-TestCheck $result 'B4: 无盘降级不崩溃' 'FAIL' 'kernel panicked' }
        else { Add-TestCheck $result 'B4: 无盘降级不崩溃' 'PASS' }
    } finally {
        Stop-QemuSession $s
    }
}

function Invoke-ScenarioB5 {
    Write-Host "`n--- B5 无 NIC (e1000/virtio_net not found, 继续启动) ---"
    Set-DeshabFirstInit -First 1 -DevMode 0
    Set-DeshabAutoexec -Lines @('ver', 'exit')
    $s = Start-QemuSession -Name 'bootreg-b5' -NoNet
    try {
        $hit = Wait-QemuLog -Session $s -Patterns @('desktop ready') -TimeoutSeconds 150
        $log = Read-QemuLogText $s.LogPath
        $result.LogPath = $s.LogPath
        Assert-QemuLog $result $log -MustContain @(
            '[e1000] device not found',
            '[virtio_net] virtio-net device not found'
        ) -Ordered @(
            '[UTSM] SELFTEST PASS',
            '[DSK] boot',
            '[DSK] firstInit != 0',
            'desktop ready'
        ) -MustNotContain @('[PANIC]')
        if ($hit) { Add-TestCheck $result 'B5: 无 NIC 到 desktop' 'PASS' }
        else { Add-TestCheck $result 'B5: 无 NIC 到 desktop' 'FAIL' 'timeout waiting desktop ready' }
    } finally {
        Stop-QemuSession $s
    }
}

# ----------------------------------------------------------------
try {
    foreach ($sc in $Scenarios) {
        switch ($sc.ToUpper()) {
            'B1' { Invoke-ScenarioB1 }
            'B2' { Invoke-ScenarioB2 }
            'B3' { Invoke-ScenarioB3 }
            'B4' { Invoke-ScenarioB4 }
            'B5' { Invoke-ScenarioB5 }
            default { Add-TestNote $result "未知场景 $sc，跳过" }
        }
    }
    Add-TestNote $result 'B6 (Linux 兼容层启用) 由 linux-compat.ps1 覆盖'
} finally {
    Restore-DeshabScenario
}

Write-TestCaseResult $result
if ($result.Status -eq 'FAIL') { exit 1 } else { exit 0 }
