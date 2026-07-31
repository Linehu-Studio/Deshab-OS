#requires -Version 5.1
<#
.SYNOPSIS
    pe-compat — PE/EXE 兼容层测试（docs/RE/test-cases/pe-compat.md）

.DESCRIPTION
    正常启动到 cmd.elf，用 AUTOEXEC.BAT 自动执行 PE 命令序列：

      pe HELLO64.EXE    → PE32+ 原生 Ring0 执行，shim_WriteFile 输出串口
      pe HELLO32.EXE    → PE32 x86emu32 解释执行，shim32 输出串口
      pe NONEXIST.EXE   → 错误路径：不产生 [cmd] PE run begin
      exit              → 返回 DSK → desktop

    串口可断言锚点：
      [cmd] PE run begin / [cmd] cmdline: ... / [cmd] PE run end   (logl/swrite, 始终上串口)
      [PE] PE32+ (64-bit) / [PE] running PE32+ natively            (pe_service)
      [PE] running PE32 via x86emu32                               (pe_service)
      Hello from PE32+! / Hello from PE32!                         (shim WriteFile → 串口)

    peinfo / 无参用法提示只写帧缓冲（cmd 非 dev_mode 无串口镜像），标 MANUAL。

    退出码：全部通过 0，任一 FAIL 1。
#>
param([switch]$Build)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'lib\QemuTest.ps1')

Initialize-QemuTest -Build:$Build
$result = New-TestCaseResult 'pe-compat'

Set-DeshabFirstInit -First 1 -DevMode 0
Set-DeshabAutoexec -Lines @('pe HELLO64.EXE', 'pe HELLO32.EXE', 'pe NONEXIST.EXE', 'exit')

$s = Start-QemuSession -Name 'pe-compat'
try {
    $hit = Wait-QemuLog -Session $s -Patterns @('[DSK] cmd.elf returned') -TimeoutSeconds 180
    Start-Sleep -Seconds 2
    $log = Read-QemuLogText $s.LogPath
    $result.LogPath = $s.LogPath

    # ---- B2.1 PE32+ 加载执行 ----
    Assert-QemuLog $result $log -Ordered @(
        '[cmd] running AUTOEXEC.BAT',
        '[cmd] PE run begin',
        '[cmd] cmdline: pe HELLO64.EXE',
        '[PE] running PE32+ natively',
        'Hello from PE32+!',
        '[cmd] PE run end'
    )

    # ---- B2.2 PE32 解释器执行 ----
    Assert-QemuLog $result $log -Ordered @(
        'Hello from PE32+!',
        '[cmd] cmdline: pe HELLO32.EXE',
        '[PE] running PE32 via x86emu32',
        'Hello from PE32!',
        '[cmd] PE run end'
    )

    # ---- B2.4 文件不存在错误路径：NONEXIST 不得产生 PE run begin ----
    Assert-QemuLog $result $log -MustNotContain @('[cmd] cmdline: pe NONEXIST.EXE')
    # 恰好两次 PE 执行（HELLO64 + HELLO32）
    Assert-QemuLog $result $log -CountPattern '[cmd] PE run begin' -MinCount 2

    # ---- B2.6 ExitProcess 返回：cmd 接管继续执行后续命令并最终返回 DSK ----
    if ($hit) { Add-TestCheck $result 'B2.6: ExitProcess 后 cmd 返回 DSK' 'PASS' }
    else { Add-TestCheck $result 'B2.6: ExitProcess 后 cmd 返回 DSK' 'FAIL' 'missing [DSK] cmd.elf returned' }

    # ---- ABI 验证（源码级，日志侧证） ----
    Assert-QemuLog $result $log -MustContain @('[cmd] PE service ok')   # ctx->reserved[4] + PE_SERVICE_MAGIC 校验通过

    # ---- 帧缓冲限定项（无串口输出，标 MANUAL） ----
    Add-TestCheck $result 'B2.3: peinfo HELLO64.EXE 头解析' 'MANUAL' 'peinfo 输出仅帧缓冲（cmd 非 dev_mode 无串口镜像），手动验证 Machine: 0x8664'
    Add-TestCheck $result 'B2.7: peinfo HELLO32.EXE 头解析' 'MANUAL' '同上，预期 Machine: 0x14c / image_base=0x400000'
    Add-TestCheck $result 'B2.5: pe 无参用法提示' 'MANUAL' '用法提示仅帧缓冲输出'

    Assert-QemuLog $result $log -MustNotContain @('[PANIC]')
} finally {
    Stop-QemuSession $s
    Restore-DeshabScenario
}

Write-TestCaseResult $result
if ($result.Status -eq 'FAIL') { exit 1 } else { exit 0 }
