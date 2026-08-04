#requires -Version 5.1
<#
.SYNOPSIS
    x86emu32-coverage — x86emu32 解释器覆盖率（docs/RE/test-cases/x86emu32-coverage.md）

.DESCRIPTION
    两层验证：

    1) 静态枚举（源码侧）：
       解析 CODE/UTSM/pe/x86emu32.c，统计 switch case 0xNN 标签数
       （= 已实现操作码处理分支），并校验 hello32.exe 头
       （machine=0x14C, optional magic=0x10B, 确为 PE32 目标样本）。

    2) 动态执行（QEMU 侧）：
       正常启动 firstInit="1\n0" → cmd.elf，AUTOEXEC.BAT 自动执行：
         pe HELLO32.EXE   → PE32 走 x86emu32 解释器
         exit             → 返回 DSK → desktop
       断言锚点（BUG-20260801-007 已修复，全链路硬断言）：
         [cmd] PE run begin / [cmd] cmdline: HELLO32.EXE
         [PE] running PE32 via x86emu32     （解释器接管证据）
         Hello from PE32!                   （解释执行 + shim IAT 拦截证据）
         [cmd] PE run end / [DSK] cmd.elf returned
       已知系统侧阻塞（2026-08-04 monitor 取证定案）：末锚点当前必缺失——
       cmd run_autoexec 共享 f32_data 缓冲被首行 pe 文件加载覆盖，exit 行
       丢失，cmd 停留交互主循环（无异常、无三重故障，guest 存活轮询键盘）。
       解释器与 shim 链路本身无恙；待 cmd 侧修复后末锚点自动转绿。
       注意：[PE] PE32 (32-bit) 仅 pe_load_image(PE32+ 原生路径)输出，
         解释器路径(pe32_load_into)不打印该行，不可作锚点。
       禁止锚点：
         [PE32] emulator error / [PE] unimplemented import /
         [PE] unimplemented API called / [PANIC]
       （出现 unimplemented 即说明 hello32 触到未实现指令/导入，覆盖率倒退）

       历史：BUG-20260801-007（shim fn32 全缺，fn64 回退把 guest 指针
       0x401017 当 host 指针解引用 → #PF → 三重故障）曾截断于首个 shim
       调用；修复后全链路可断言。故障在 host 侧 shim 而非解释器：
       0x68 push / FF /2 call [disp32] / IAT 合成地址拦截链路均正确，
       故"无 emulator error"仍为有效覆盖证据。

    指令级逐条覆盖（文档矩阵 test_arith/test_logic/test_control/test_string）
    依赖尚未存在的分类测试 PE，标 MANUAL。

    WHPX 已知问题：guest 在 PE 兼容层执行阶段可能触发
    "Unexpected VP exit code 4" 直接终止 QEMU（WHPX 平台限制）。
    检测到该特征时自动以 -Accel tcg 重跑一轮（可用 -Accel tcg 直接指定）。

    退出码：全部通过 0，任一 FAIL 1。
#>
param(
    [switch]$Build,
    [string]$Accel = 'whpx'
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'lib\QemuTest.ps1')

Initialize-QemuTest -Build:$Build
$result = New-TestCaseResult 'x86emu32-coverage'

# ================= 静态枚举 =================
$emuSrc = Join-Path $script:RepoRoot 'CODE\UTSM\pe\x86emu32.c'
$src = Get-Content $emuSrc -Raw
$caseCount = ([regex]::Matches($src, 'case\s+0x[0-9A-Fa-f]+')).Count
Add-TestCheck $result "静态: x86emu32.c 已实现操作码分支 case 0xNN × $caseCount" 'PASS' '仅枚举计数，供覆盖率分母参考'
Add-TestNote $result "覆盖率分母: x86emu32.c 共 $caseCount 个 case 0xNN 操作码分支（含 0F 扩展页）"

$h32 = [System.IO.File]::ReadAllBytes((Join-Path $script:SystemDir 'bin\hello32.exe'))
$peOff = [BitConverter]::ToInt32($h32, 0x3C)
$mach  = [BitConverter]::ToUInt16($h32, $peOff + 4)
$optm  = [BitConverter]::ToUInt16($h32, $peOff + 24)
if ($mach -eq 0x14C -and $optm -eq 0x10B) {
    Add-TestCheck $result '静态: hello32.exe 为 PE32 (machine=0x14C, magic=0x10B)' 'PASS'
} else {
    Add-TestCheck $result '静态: hello32.exe 为 PE32 (machine=0x14C, magic=0x10B)' 'FAIL' ("actual machine=0x{0:X} magic=0x{1:X}" -f $mach, $optm)
}

# ================= 动态执行 =================
# 单轮执行：正常启动 → cmd → AUTOEXEC(pe HELLO32.EXE / exit) → 返回 DSK
# 返回 $true=完整跑完；$false=会话提前退出（WHPX 终止等）
function Invoke-EmuRun {
    param([Parameter(Mandatory)][string]$AccelMode)
    Set-DeshabFirstInit -First 1 -DevMode 0
    Set-DeshabAutoexec -Lines @('pe HELLO32.EXE', 'exit')

    $timeout = 180
    if ($AccelMode -eq 'tcg') { $timeout = 420 }   # TCG 全模拟显著更慢
    $s = Start-QemuSession -Name "x86emu32-$AccelMode" -Accel $AccelMode -MonitorPort 45508
    try {
        $script:RunHit = Wait-QemuLog -Session $s -Patterns @('[DSK] cmd.elf returned', '[PANIC]') -TimeoutSeconds $timeout
        Start-Sleep -Seconds 2
        $script:RunLog = Read-QemuLogText $s.LogPath
        $script:RunLogPath = $s.LogPath
        if (-not $script:RunHit) { return $false }
        return $true
    } finally {
        Stop-QemuSession $s
    }
}

$script:RunLog = ''
$script:RunLogPath = ''
$script:RunHit = $null

try {
    $usedAccel = $Accel
    $ok = Invoke-EmuRun -AccelMode $Accel
    if (-not $ok -and $Accel -eq 'whpx' -and $script:RunLog.Contains('WHPX: Unexpected VP exit')) {
        Add-TestNote $result "WHPX 在 PE 兼容层执行处终止 QEMU（Unexpected VP exit，WHPX 平台限制），自动以 TCG 重跑"
        $usedAccel = 'tcg'
        $ok = Invoke-EmuRun -AccelMode 'tcg'
    }
    $log = $script:RunLog
    $result.LogPath = $script:RunLogPath
    Add-TestNote $result "加速方案: $usedAccel"
    $hit = $script:RunHit

    # ---- 解释器接管链（BUG-007 已修复，全链路可稳定断言） ----
    Assert-QemuLog $result $log -Ordered @(
        '[cmd] running AUTOEXEC.BAT',
        '[cmd] PE run begin',
        '[cmd] cmdline: HELLO32.EXE',
        '[PE] running PE32 via x86emu32'
    ) -MustNotContain @(
        '[PANIC]',
        '[PE32] emulator error',
        '[PE32] emu init failed',
        '[PE] unimplemented import',
        '[PE] unimplemented API called'
    )

    # ---- shim 输出与完整返回链：BUG-007/006 已修复，转硬断言（回归守护） ----
    if ($log.Contains('Hello from PE32!')) {
        Add-TestCheck $result '动态: 解释执行 + shim IAT 拦截输出（Hello from PE32!）' 'PASS'
    } else {
        Add-TestCheck $result '动态: 解释执行 + shim IAT 拦截输出（Hello from PE32!）' 'FAIL' 'missing Hello from PE32!（BUG-20260801-007 回归？）'
    }
    if ($log.Contains('[cmd] PE run end') -and $log.Contains('[DSK] cmd.elf returned')) {
        Add-TestCheck $result '动态: ExitProcess 回 cmd，AUTOEXEC 跑完返回 DSK' 'PASS'
    } else {
        # 已知系统侧阻塞（2026-08-04 诊断定案，非 BUG-006/007 回归）：
        # cmd run_autoexec 的 data/size 直指共享静态缓冲 f32_data（fat32_io.h），
        # 首行 `pe HELLO32.EXE` 加载 PE 即覆盖剩余脚本，exit 行丢失 → cmd 停留
        # 交互主循环（monitor 取证 RIP=cmd.elf 内 inb 0x60 轮询点，无异常投递）。
        # pe-compat 会话 C（pe NONEXIST.EXE + exit）通过反证：文件未找到不覆盖
        # 缓冲时 exit 返回链正常。待 cmd 侧修复（脚本行先拷入私有缓冲再执行）。
        Add-TestCheck $result '动态: ExitProcess 回 cmd，AUTOEXEC 跑完返回 DSK' 'FAIL' 'missing [DSK] cmd.elf returned（已知系统 bug：cmd run_autoexec 共享 f32_data 被 pe 文件加载覆盖，exit 行丢失）'
    }

    # 解释器侧覆盖判定：hello32 全指令流（含 shim IAT 拦截与参数读取）
    # 均正确执行——无 emulator error / unimplemented 日志即解释器覆盖率
    # 不记倒退。
    if (-not $log.Contains('[PE] unimplemented') -and -not $log.Contains('[PE32] emulator') -and $log.Contains('[PE] running PE32 via x86emu32')) {
        Add-TestCheck $result '动态: hello32 已执行指令全部命中已实现集（无 unimplemented/emulator error）' 'PASS'
    } else {
        Add-TestCheck $result '动态: hello32 已执行指令全部命中已实现集（无 unimplemented/emulator error）' 'FAIL' 'unimplemented/emulator error anchor present or interpreter not reached'
    }

    # ---- 指令级分类覆盖（依赖尚不存在的分类测试 PE） ----
    foreach ($t in @('数据传送簇 (MOV/PUSH/POP 逐操作数形态)', '算术簇 (ADD/SUB/INC/DEC/MUL/DIV 逐标志验证)',
                     '逻辑簇 (AND/OR/XOR/NOT)', '控制流簇 (JMP/CALL/RET/Jcc)',
                     '字符串簇 (MOVSB/STOSB/REP 前缀)', '堆栈帧 (ENTER/LEAVE) + INT3 断点路径')) {
        Add-TestCheck $result "指令级: $t" 'MANUAL' '需 CODE/tools/pe-samples/ 下分类测试 PE（test_arith.exe 等），当前未构建；hello32 仅综合冒烟'
    }
    Add-TestNote $result "pe-compat.ps1 已覆盖 PE32+ 原生对照组（HELLO64.EXE）与 PE 服务 ABI 锚点"
} finally {
    Restore-DeshabScenario
}

Write-TestCaseResult $result
if ($result.Status -eq 'FAIL') { exit 1 } else { exit 0 }
