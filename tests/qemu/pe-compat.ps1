#requires -Version 5.1
<#
.SYNOPSIS
    pe-compat — PE/EXE 兼容层测试（docs/RE/test-cases/pe-compat.md）

.DESCRIPTION
    正常启动到 cmd.elf，用 AUTOEXEC.BAT 自动执行 PE 命令序列。
    因两个已登记系统 bug（详见 docs/RE/Bug记录.txt），拆三个独立会话：

      会话 A (pe64)：pe HELLO64.EXE → PE32+ 原生 Ring0 执行，shim_WriteFile 输出串口。
                     BUG-20260801-006 已修复（setjmp 下沉 pe_service_run 本体）：
                     ExitProcess longjmp 干净返回，"[cmd] PE run end" 稳定出现。
      会话 B (pe32)：pe HELLO32.EXE → PE32 x86emu32 解释执行。
                     BUG-20260801-007 已修复（gp() guest→host 指针转换 +
                     fn64 调用 typedef 显式 ms_abi）："Hello from PE32!" 稳定输出。
      会话 D (fault64)：pe FAULT64.EXE → BUG-20260801-005 验收：DSK 阶段故意
                     #PF 必须打出 [IDT] exception 完整现场（vector=0xe, err=0x2,
                     cr2=0x40000000000）。修复前 halt_all 静默吞异常零日志。
      会话 C (peerr)：pe NONEXIST.EXE + exit → 错误路径：不产生 [cmd] PE run begin；
                     exit 正常返回 DSK（"[DSK] cmd.elf returned"）。

    串口可断言锚点：
      [cmd] PE run begin / [cmd] cmdline: ... / [cmd] PE run end   (logl/swrite, 始终上串口)
      [PE] PE32+ (64-bit) / [PE] running PE32+ natively            (pe_service)
      [PE] running PE32 via x86emu32                               (pe_service)
      Hello from PE32+! / Hello from PE32!                         (shim WriteFile → 串口)

    注意：cmd_pe 传 full_cmdline=args（不含 "pe " 前缀），
    所以串口回显为 "[cmd] cmdline: HELLO64.EXE" 而不是 "pe HELLO64.EXE"。

    WHPX 已知问题：pe_service 为原生 PE32+ 执行写 CR4 启用 SSE 后，
    WHPX 可能以 "Unexpected VP exit code 4" 提前终止 QEMU（WHPX 平台限制，
    非 guest 崩溃）。任一会话在 WHPX 下未命中锚点时自动以 -Accel tcg 重跑
    （可用 -Accel tcg 直接指定跳过 WHPX）。

    peinfo / 无参用法提示只写帧缓冲（cmd 非 dev_mode 无串口镜像），标 MANUAL。

    退出码：全部通过 0，任一 FAIL 1。
#>
param(
    [switch]$Build,
    [string]$Accel = 'whpx'
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'lib\QemuTest.ps1')

Initialize-QemuTest -Build:$Build
$result = New-TestCaseResult 'pe-compat'

# ----------------------------------------------------------------
# 单会话执行一轮 AUTOEXEC：正常启动 → cmd → 命令序列 → 等锚点/会话结束
# 填充 $script:RunLog / RunLogPath；返回命中的锚点或 $null。
# ----------------------------------------------------------------
$script:RunLog = ''
$script:RunLogPath = ''
function Invoke-PeSession {
    param(
        [Parameter(Mandatory)][string]$Name,
        [Parameter(Mandatory)][string]$AccelMode,
        [Parameter(Mandatory)][string[]]$Autoexec,
        [Parameter(Mandatory)][string[]]$Anchors
    )
    Set-DeshabFirstInit -First 1 -DevMode 0
    Set-DeshabAutoexec -Lines $Autoexec

    $timeout = 150
    if ($AccelMode -eq 'tcg') { $timeout = 360 }   # TCG 全模拟显著更慢
    $s = Start-QemuSession -Name "$Name-$AccelMode" -Accel $AccelMode -MonitorPort 45504
    try {
        $hit = Wait-QemuLog -Session $s -Patterns $Anchors -TimeoutSeconds $timeout
        Start-Sleep -Seconds 2
        $script:RunLog = Read-QemuLogText $s.LogPath
        $script:RunLogPath = $s.LogPath
        return $hit
    } finally {
        Stop-QemuSession $s
    }
}

# WHPX 未命中锚点 → TCG 兜底重跑一轮
function Invoke-PeSessionWithFallback {
    param(
        [Parameter(Mandatory)][string]$Name,
        [Parameter(Mandatory)][string[]]$Autoexec,
        [Parameter(Mandatory)][string[]]$Anchors
    )
    $hit = Invoke-PeSession -Name $Name -AccelMode $Accel -Autoexec $Autoexec -Anchors $Anchors
    if (-not $hit -and $Accel -eq 'whpx') {
        $why = 'WHPX 下未命中锚点'
        if ($script:RunLog.Contains('WHPX: Unexpected VP exit')) { $why = 'WHPX 在 PE 执行处终止 QEMU（Unexpected VP exit，WHPX 平台限制）' }
        Add-TestNote $result "${Name}: $why，自动以 TCG 重跑"
        $script:UsedAccel = 'tcg'
        $hit = Invoke-PeSession -Name $Name -AccelMode 'tcg' -Autoexec $Autoexec -Anchors $Anchors
    }
    return $hit
}

$script:UsedAccel = $Accel
try {
    # ================================================================
    # 会话 A：B2.1 PE32+ 原生加载执行（BUG-006 已修复，ExitProcess 干净返航）
    # ================================================================
    $hitA = Invoke-PeSessionWithFallback -Name 'pe64' -Autoexec @('pe HELLO64.EXE') -Anchors @('Hello from PE32+!', '[PANIC]')
    $logA = $script:RunLog
    $result.LogPath = $script:RunLogPath

    Assert-QemuLog $result $logA -Ordered @(
        '[cmd] running AUTOEXEC.BAT',
        '[cmd] PE run begin',
        '[cmd] cmdline: HELLO64.EXE',
        '[PE] PE32+ (64-bit)',
        '[PE] running PE32+ natively',
        'Hello from PE32+!'
    ) -MustNotContain @('[PANIC]')

    # ---- ABI 验证：cmd 启动时 ctx->reserved[4] + PE_SERVICE_MAGIC 校验 ----
    Assert-QemuLog $result $logA -MustContain @('[cmd] PE service ok')

    # ---- B2.6 ExitProcess 返回链：BUG-006 已修复，转硬断言（回归守护） ----
    if ($logA.Contains('[cmd] PE run end')) {
        Add-TestCheck $result 'B2.6: ExitProcess 后 cmd 继续（PE32+ 返回链）' 'PASS'
    } else {
        Add-TestCheck $result 'B2.6: ExitProcess 后 cmd 继续（PE32+ 返回链）' 'FAIL' 'missing [cmd] PE run end（BUG-20260801-006 回归？）'
    }

    # ================================================================
    # 会话 B：B2.2 PE32 x86emu32 解释执行（BUG-007 已修复，shim 指针转换）
    # ================================================================
    $hitB = Invoke-PeSessionWithFallback -Name 'pe32' -Autoexec @('pe HELLO32.EXE') -Anchors @('Hello from PE32!', '[PANIC]', '[PE32] emulator error')
    $logB = $script:RunLog

    Assert-QemuLog $result $logB -Ordered @(
        '[cmd] PE run begin',
        '[cmd] cmdline: HELLO32.EXE',
        '[PE] running PE32 via x86emu32'
    ) -MustNotContain @(
        '[PANIC]',
        '[PE32] emulator error',
        '[PE32] emu init failed',
        '[PE] unimplemented import',
        '[PE32] unimplemented import',
        '[PE] unimplemented API called'
    )

    # ---- B2.2 shim 输出：BUG-007 已修复，转硬断言（回归守护） ----
    if ($logB.Contains('Hello from PE32!')) {
        Add-TestCheck $result 'B2.2: PE32 解释执行 shim 输出' 'PASS'
    } else {
        Add-TestCheck $result 'B2.2: PE32 解释执行 shim 输出' 'FAIL' 'missing Hello from PE32!（BUG-20260801-007 回归？）'
    }

    # ================================================================
    # 会话 D：BUG-20260801-005 验收 — DSK 阶段故意 #PF 必须打出完整异常现场
    #   FAULT64.EXE: movabs rax,0x400000000000 ; mov [rax],1 ; jmp $
    #   预期 vector=0xe / err=0x2(supervisor write non-present) /
    #   cr2=0x0000400000000000；修复前 halt_all 静默吞掉零日志。
    #   异常后 idt_handler 安全停机（hlt 循环），QEMU 存活 → 锚点命中即返回。
    #   若异常投递失效则三重故障 → -no-reboot QEMU 退出 → hit=$null → FAIL。
    # ================================================================
    $hitD = Invoke-PeSessionWithFallback -Name 'fault64' -Autoexec @('pe FAULT64.EXE') -Anchors @('[IDT] exception', '[PANIC]')
    $logD = $script:RunLog

    if (-not $hitD) {
        Add-TestCheck $result 'BUG-005: DSK 阶段 #PF 异常投递' 'FAIL' 'session exited without [IDT] exception（异常被吞/三重故障）'
    } else {
        Add-TestCheck $result 'BUG-005: DSK 阶段 #PF 异常投递' 'PASS'
    }
    Assert-QemuLog $result $logD -Ordered @(
        '[cmd] cmdline: FAULT64.EXE',
        '[PE] running PE32+ natively',
        '[IDT] exception',
        '[IDT] vector=0x000000000000000e',
        '[IDT] err=0x0000000000000002',
        '[IDT] cr2=0x0000400000000000'
    )

    # ================================================================
    # 会话 C：B2.4 文件不存在错误路径 + cmd exit 正常返回 DSK
    # ================================================================
    $hitC = Invoke-PeSessionWithFallback -Name 'peerr' -Autoexec @('pe NONEXIST.EXE', 'exit') -Anchors @('[DSK] cmd.elf returned', '[PANIC]')
    $logC = $script:RunLog

    # NONEXIST 不得进入 PE 执行路径
    if ($logC.Contains('[cmd] cmdline: NONEXIST.EXE') -or $logC.Contains('[cmd] PE run begin')) {
        Add-TestCheck $result 'B2.4: NONEXIST.EXE 不进入 PE 执行路径' 'FAIL' 'unexpected PE run begin / cmdline echo'
    } else {
        Add-TestCheck $result 'B2.4: NONEXIST.EXE 不进入 PE 执行路径' 'PASS'
    }
    Assert-QemuLog $result $logC -MustNotContain @('[PANIC]')

    if ($hitC -eq '[DSK] cmd.elf returned') {
        Add-TestCheck $result '链路: cmd exit 正常返回 DSK' 'PASS'
    } else {
        Add-TestCheck $result '链路: cmd exit 正常返回 DSK' 'FAIL' "hit=$hitC（missing [DSK] cmd.elf returned）"
    }

    Add-TestNote $result "加速方案: $script:UsedAccel"
    Add-TestNote $result 'BUG-006/007 已修复，返回链断言已转硬断言；会话仍按故障域拆分（单会话全序列合并留作后续优化）'

    # ---- 帧缓冲限定项（无串口输出，标 MANUAL） ----
    Add-TestCheck $result 'B2.3: peinfo HELLO64.EXE 头解析' 'MANUAL' 'peinfo 输出仅帧缓冲（cmd 非 dev_mode 无串口镜像），手动验证 Machine: 0x8664'
    Add-TestCheck $result 'B2.7: peinfo HELLO32.EXE 头解析' 'MANUAL' '同上，预期 Machine: 0x14c / image_base=0x400000'
    Add-TestCheck $result 'B2.5: pe 无参用法提示' 'MANUAL' '用法提示仅帧缓冲输出'
} finally {
    Restore-DeshabScenario
}

Write-TestCaseResult $result
if ($result.Status -eq 'FAIL') { exit 1 } else { exit 0 }
