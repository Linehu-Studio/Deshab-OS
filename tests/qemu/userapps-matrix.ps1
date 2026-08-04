#requires -Version 5.1
<#
.SYNOPSIS
    userapps-matrix — 用户态程序功能矩阵（docs/RE/test-cases/userapps-matrix.md）

.DESCRIPTION
    dev_mode 单会话覆盖 shell 内建命令与工具程序（串口镜像开启，全程可断言）。

    会话 A（唯一）：dev_tests 硬编码序列 → help → id → pci 存活 →
      ping（PATH 加载 → 10.0.2.2 四报文 → Esc 退出）→
      run EDITOR.ELF / FILEMAN.ELF / BROWSER.ELF（加载 + 按键 + Esc 退出）→
      Esc 回 DSK → cmd(AUTOEXEC) → desktop

    历史背景（详见 docs/RE/Bug记录.txt，均已 FIXED，相关降级已全部回收）：

      BUG-20260801-001 [shell] y/i/0 吞键（未跟踪 Ctrl 修饰键）：
        已修复（0x1D/0x9D 跟踪 Ctrl，快捷键仅在 Ctrl 按下时生效）
        → id/ping 等含 i 命令恢复自动化断言。

      BUG-20260801-002 [shell] g_run_image 仅 256KB，editor(496KB) 拒绝加载：
        已修复（g_run_image 扩至 1MB，与 desktop 宿主对齐）
        → run EDITOR.ELF 断言升级为 [editor] boot + [editor] exit。

      BUG-20260801-003 [desktop_app.h] da_mouse_poll 吞键盘扫描码 +
      BUG-20260801-008 [desktop_app.h] da_mouse_init 配置字污染：
        均已修复（先判 AUX 位再读 0x60；init 首尾 da_ps2_drain + 读全响应）
        → fileman/editor/browser 键盘路径恢复，Esc 退出断言全部自动化；
          工具不再占住会话，三会合并为单会话顺序执行。

    退出码：全部通过 0，任一 FAIL 1。
#>
param([switch]$Build)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'lib\QemuTest.ps1')

Initialize-QemuTest -Build:$Build
$result = New-TestCaseResult 'userapps-matrix'

# ----------------------------------------------------------------
# monitor sendkey 在 guest 重负载（AHCI 读盘刷屏、帧缓冲重绘）下会丢扫描码。
# 对策：每键 150ms + "发送→等锚点→重试 ≤3 次"，重试前先 backspace 清行。
# 注意：backspace(0x0E) 不在 shell Ctrl+U/W/K 快捷键扫描码列表内，清行可靠。
# ----------------------------------------------------------------
function Invoke-ShellCmdRetry {
    param(
        [Parameter(Mandatory)]$Session,
        [Parameter(Mandatory)][string]$Text,
        [Parameter(Mandatory)][string[]]$Anchors,
        [int]$TimeoutSeconds = 20,
        [int]$MaxAttempts = 3
    )
    for ($a = 1; $a -le $MaxAttempts; $a++) {
        if ($a -gt 1) {
            $bk = @(); for ($i = 0; $i -lt 24; $i++) { $bk += 'backspace' }
            Send-QemuKeys -Session $Session -Keys $bk -DelayMs 50
            Start-Sleep -Milliseconds 400
            Add-TestNote $result "输入 '$Text' 第 $($a-1) 次未命中锚点，清行重试"
        }
        Send-QemuText -Session $Session -Text $Text -Enter -DelayMs 150
        $h = Wait-QemuLog -Session $Session -Patterns $Anchors -TimeoutSeconds $TimeoutSeconds
        if ($h) { return $h }
    }
    return $null
}

# Esc 退出重试：只在锚点未出现时补发，锚点出现即停（避免多余 Esc 漏进 shell/cmd）
function Send-EscRetry {
    param(
        [Parameter(Mandatory)]$Session,
        [Parameter(Mandatory)][string[]]$Anchors,
        [int]$TimeoutSeconds = 15,
        [int]$MaxAttempts = 2
    )
    for ($a = 1; $a -le $MaxAttempts; $a++) {
        Send-QemuKeys -Session $Session -Keys @('esc')
        $h = Wait-QemuLog -Session $Session -Patterns $Anchors -TimeoutSeconds $TimeoutSeconds
        if ($h) { return $h }
        if ($a -lt $MaxAttempts) { Add-TestNote $result "Esc 第 $a 次未命中（$($Anchors -join ' | ')），补发" }
    }
    return $null
}

try {
    # ================================================================
    # 会话 A（唯一）：shell 内建命令 + ping/editor/fileman/browser
    # 工具链全交互 + Esc→cmd→desktop 链路
    # ================================================================
    Set-DeshabFirstInit -First 1 -DevMode 1
    Set-DeshabAutoexec -Lines @('ver', 'exit')

    $s = Start-QemuSession -Name 'userapps-matrix' -MonitorPort 45505
    try {
        $hit = Wait-QemuLog -Session $s -Patterns @('=== 自动测试完成 ===', '[PANIC]') -TimeoutSeconds 240
        $log = Read-QemuLogText $s.LogPath
        $result.LogPath = $s.LogPath

        Assert-QemuLog $result $log -Ordered @(
            '[DSK:SCHED] decision: dev_mode -> shell.elf',
            '[shell] boot',
            '[shell] test.bas not found, fallback to hardcoded',
            '[1] ls',
            '[2] echo "Deshab dev test" > TEST.TXT',
            '[3] cat TEST.TXT',
            '[4] cp TEST.TXT COPY.TXT',
            '[6] mv COPY.TXT MOVED.TXT',
            '[8] rm TEST.TXT',
            '[10] ls'
        ) -MustNotContain @('[PANIC]')
        if ($hit -eq '=== 自动测试完成 ===') {
            Add-TestCheck $result 'shell: dev_tests 完成（ls/echo/cat/cp/mv/rm）' 'PASS'
        } else {
            Add-TestCheck $result 'shell: dev_tests 完成（ls/echo/cat/cp/mv/rm）' 'FAIL' "hit=$hit"
        }

        if ($hit -eq '=== 自动测试完成 ===') {
            # --- help ---
            Start-Sleep -Seconds 1
            $h = Invoke-ShellCmdRetry -Session $s -Text 'help' -Anchors @('dsl push <') -TimeoutSeconds 20
            if ($h) { Add-TestCheck $result 'shell: help 命令列表' 'PASS' }
            else { Add-TestCheck $result 'shell: help 命令列表' 'FAIL' 'missing help output' }

            # --- id（BUG-001 修复回归守护：i 不再被吞，输出上串口） ---
            $h = Invoke-ShellCmdRetry -Session $s -Text 'id' -Anchors @('uid=0(root) gid=0(root)') -TimeoutSeconds 20
            if ($h) { Add-TestCheck $result 'shell: id 用户查询（BUG-001 修复后 i 可键入）' 'PASS' }
            else { Add-TestCheck $result 'shell: id 用户查询（BUG-001 修复后 i 可键入）' 'FAIL' 'missing uid=0(root) output' }

            # --- pci（验证存活：pci 后 ver 仍有响应，锚点取 ver 独有行） ---
            Send-QemuText -Session $s -Text 'pci' -Enter -DelayMs 150
            Start-Sleep -Seconds 3
            $h = Invoke-ShellCmdRetry -Session $s -Text 'ver' -Anchors @('Architecture: x86_64') -TimeoutSeconds 20
            if ($h) { Add-TestCheck $result 'shell: pci 枚举后 shell 存活' 'PASS' }
            else { Add-TestCheck $result 'shell: pci 枚举后 shell 存活' 'FAIL' 'no response after pci' }

            # --- ping：BUG-001 修复后可键入；PATH → /bin/PING.ELF → 10.0.2.2 四报文 → Esc 退出 ---
            $h = Invoke-ShellCmdRetry -Session $s -Text 'ping' -Anchors @('[ping] boot') -TimeoutSeconds 30
            if ($h) {
                Add-TestCheck $result 'ping.elf: PATH 加载启动（[ping] boot）' 'PASS'
                Start-Sleep -Milliseconds 800
                # 4 个 ICMP Echo（每报文超时上限 2s）；reply/统计仅帧缓冲，串口无锚点
                Send-QemuText -Session $s -Text '10.0.2.2' -Enter -DelayMs 150
                Start-Sleep -Seconds 10
                $h2 = Send-EscRetry -Session $s -Anchors @('[ping] exit') -TimeoutSeconds 15 -MaxAttempts 2
                $logp = Read-QemuLogText $s.LogPath
                if ($h2 -and $logp.Contains('[shell] path: tool returned')) {
                    Add-TestCheck $result 'ping.elf: Esc 退出返回 shell' 'PASS'
                } else {
                    Add-TestCheck $result 'ping.elf: Esc 退出返回 shell' 'FAIL' 'missing [ping] exit / path: tool returned'
                }
            } else {
                Add-TestCheck $result 'ping.elf: PATH 加载启动（[ping] boot）' 'FAIL' 'missing [ping] boot'
            }

            # --- editor：BUG-002 已修复（g_run_image 扩至 1MB），run 加载 → 按键插入 → Esc 退出 ---
            $h = Invoke-ShellCmdRetry -Session $s -Text 'run EDITOR.ELF' -Anchors @('[editor] boot') -TimeoutSeconds 30
            if ($h) {
                Add-TestCheck $result 'editor.elf: run 加载启动（[editor] boot，BUG-002 修复后行为）' 'PASS'
                Start-Sleep -Seconds 1
                Send-QemuText -Session $s -Text 'abc' -DelayMs 150   # 编辑模式按键插入（驱动键盘路径，无串口锚点）
                $h2 = Send-EscRetry -Session $s -Anchors @('[editor] exit') -TimeoutSeconds 15 -MaxAttempts 2
                $loge = Read-QemuLogText $s.LogPath
                if ($h2 -and $loge.Contains('[shell] run: tool returned')) {
                    Add-TestCheck $result 'editor.elf: Esc 退出返回 shell（[editor] exit）' 'PASS'
                } else {
                    Add-TestCheck $result 'editor.elf: Esc 退出返回 shell（[editor] exit）' 'FAIL' 'missing [editor] exit / run: tool returned'
                }
            } else {
                Add-TestCheck $result 'editor.elf: run 加载启动（[editor] boot，BUG-002 修复后行为）' 'FAIL' 'missing [editor] boot'
            }

            # --- fileman：BUG-003/008 修复后键盘可用；方向键移动选区 + Esc 退出 ---
            $h = Invoke-ShellCmdRetry -Session $s -Text 'run FILEMAN.ELF' -Anchors @('[fileman] boot') -TimeoutSeconds 30
            if ($h) {
                Add-TestCheck $result 'fileman.elf: run 加载启动（未复现 QEMU 退出）' 'PASS'
                Start-Sleep -Seconds 2   # 等 da_mouse_init + 目录加载 + 首帧渲染
                Send-QemuKeys -Session $s -Keys @('down', 'down', 'up') -DelayMs 200   # 选区移动（仅帧缓冲，驱动键盘路径）
                $h2 = Send-EscRetry -Session $s -Anchors @('[fileman] exit') -TimeoutSeconds 15 -MaxAttempts 2
                $logfm = Read-QemuLogText $s.LogPath
                if ($h2 -and $logfm.Contains('[shell] run: tool returned')) {
                    Add-TestCheck $result 'fileman.elf: 方向键 + Esc 退出返回 shell（BUG-003/008 修复后键盘路径）' 'PASS'
                } else {
                    Add-TestCheck $result 'fileman.elf: 方向键 + Esc 退出返回 shell（BUG-003/008 修复后键盘路径）' 'FAIL' 'missing [fileman] exit / run: tool returned'
                }
            } else {
                Add-TestCheck $result 'fileman.elf: run 加载启动（未复现 QEMU 退出）' 'FAIL' 'missing [fileman] boot'
            }

            # --- browser：启动 + Esc 退出 ---
            $h = Invoke-ShellCmdRetry -Session $s -Text 'run BROWSER.ELF' -Anchors @('[browser] boot') -TimeoutSeconds 30
            if ($h) {
                Add-TestCheck $result 'browser.elf: run 加载启动' 'PASS'
                Start-Sleep -Seconds 2
                $h2 = Send-EscRetry -Session $s -Anchors @('[browser] exit') -TimeoutSeconds 15 -MaxAttempts 2
                $logbr = Read-QemuLogText $s.LogPath
                if ($h2 -and $logbr.Contains('[shell] run: tool returned')) {
                    Add-TestCheck $result 'browser.elf: Esc 退出返回 shell（[browser] exit）' 'PASS'
                } else {
                    Add-TestCheck $result 'browser.elf: Esc 退出返回 shell（[browser] exit）' 'FAIL' 'missing [browser] exit / run: tool returned'
                }
            } else {
                Add-TestCheck $result 'browser.elf: run 加载启动' 'FAIL' 'missing [browser] boot'
            }

            # --- Esc 回 DSK → cmd(AUTOEXEC) → desktop ---
            # 只重试到中间锚点 '[shell] esc -> return to DSK' 出现即停，
            # 避免多余 Esc 漏进 cmd/desktop；desktop ready 纯被动等待。
            $h = Send-EscRetry -Session $s -Anchors @('[shell] esc -> return to DSK') -TimeoutSeconds 20 -MaxAttempts 2
            $hitDesk = Wait-QemuLog -Session $s -Patterns @('desktop ready') -TimeoutSeconds 150
            $logf = Read-QemuLogText $s.LogPath
            Assert-QemuLog $result $logf -Ordered @(
                '[shell] esc -> return to DSK',
                '[DSK] shell auto-test returned'
            ) -MustNotContain @('[PANIC]')
            if ($hitDesk) { Add-TestCheck $result '链路: Esc → cmd → desktop ready' 'PASS' }
            else { Add-TestCheck $result '链路: Esc → cmd → desktop ready' 'FAIL' 'no desktop ready' }
        }
    } finally {
        Stop-QemuSession $s
    }

    # ================= MANUAL / 引用项 =================
    Add-TestCheck $result 'ping.elf: ICMP reply/RTT 与四报文统计' 'MANUAL' 'reply/统计输出仅帧缓冲（无串口锚点）；10.0.2.2 输入与 [ping] boot/exit 链已自动化'
    Add-TestCheck $result 'editor.elf: 编辑内容渲染与保存' 'MANUAL' '编辑区/保存结果仅帧缓冲与磁盘（无串口锚点）；boot/按键插入/exit 链已自动化'
    Add-TestCheck $result 'fileman 浏览内容（列表渲染/进入子目录）' 'MANUAL' '列表渲染仅帧缓冲（无串口锚点）；方向键/Esc 键盘路径已自动化'
    Add-TestCheck $result 'FirstInit 向导（程序 1）' 'MANUAL' '由 boot-regression.ps1 B1 自动化覆盖（monitor 按键填表）'
    Add-TestCheck $result 'login（程序 2）' 'SKIP' 'FUCK [dsk] skip_login=1 跳过；USER.CONF 解密路径需人工验证'
    Add-TestCheck $result 'desktop 双击/窗口拖动/任务栏（程序 3）' 'MANUAL' '无串口锚点，需人工或 screendump 验证'
    Add-TestCheck $result 'cmd pe 兼容（程序 5）' 'MANUAL' '由 pe-compat.ps1 自动化覆盖'
    Add-TestCheck $result 'netman（程序 6）' 'MANUAL' '由 network-e2e.ps1 场景 N-A 自动化覆盖（netman 仅首启调度）'
    Add-TestCheck $result 'mouseInit（程序 7）' 'MANUAL' '由 boot-regression.ps1 B1 首启链路覆盖'
    Add-TestCheck $result 'curl.elf HTTP（程序 11）' 'MANUAL' '由 network-e2e.ps1 场景 N-B 自动化覆盖（主机侧 HTTP 服务器断言）'
} finally {
    Restore-DeshabScenario
}

Write-TestCaseResult $result
if ($result.Status -eq 'FAIL') { exit 1 } else { exit 0 }
