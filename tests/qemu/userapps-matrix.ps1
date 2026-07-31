#requires -Version 5.1
<#
.SYNOPSIS
    userapps-matrix — 用户态程序功能矩阵（docs/RE/test-cases/userapps-matrix.md）

.DESCRIPTION
    dev_mode 单会话覆盖 shell 内建命令与工具程序（串口镜像开启，全程可断言）：

      1) dev_tests 硬编码序列自动执行（ls/echo/cat/cp/mv/rm，SATA 镜像无 test.bas）
      2) 交互阶段经 monitor 按键驱动：
         help / id / pci（存活验证）→
         ping  (PATH 查找 /bin/PING.ELF，输入 10.0.2.2，Esc 退出) →
         run EDITOR.ELF / FILEMAN.ELF / BROWSER.ELF（root 目录加载，Esc 退出）→
         Esc 回 DSK → cmd(AUTOEXEC) → desktop

    串口可断言锚点：
      [shell] path: running /bin/ tool / [shell] path: tool returned
      [shell] run: jumping to tool    / [shell] run: tool returned
      [ping] boot|exit  [editor] boot|exit  [fileman] boot|exit  [browser] boot|exit

    工具内部 UI 输出（编辑器内容、fileman 列表、ping RTT）仅帧缓冲，标 MANUAL。
    desktop 双击/拖动/任务栏无串口锚点，标 MANUAL。

    退出码：全部通过 0，任一 FAIL 1。
#>
param([switch]$Build)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'lib\QemuTest.ps1')

Initialize-QemuTest -Build:$Build
$result = New-TestCaseResult 'userapps-matrix'

Set-DeshabFirstInit -First 1 -DevMode 1
Set-DeshabAutoexec -Lines @('ver', 'exit')

$s = Start-QemuSession -Name 'userapps-matrix'
try {
    # ================= 阶段 1: dev_tests 自动序列 =================
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

    # ================= 阶段 2: 交互命令 =================
    if ($hit -eq '=== 自动测试完成 ===') {
        # --- help ---
        Start-Sleep -Seconds 1
        Send-QemuText -Session $s -Text 'help' -Enter
        $h = Wait-QemuLog -Session $s -Patterns @('dsl push <') -TimeoutSeconds 30
        if ($h) { Add-TestCheck $result 'shell: help 命令列表' 'PASS' }
        else { Add-TestCheck $result 'shell: help 命令列表' 'FAIL' 'missing help output' }

        # --- id（uid/gid 输出为 ASCII 强锚点） ---
        Send-QemuText -Session $s -Text 'id' -Enter
        $h = Wait-QemuLog -Session $s -Patterns @('uid=0(root) gid=0(root)') -TimeoutSeconds 30
        if ($h) { Add-TestCheck $result 'shell: id/whoami 用户查询' 'PASS' }
        else { Add-TestCheck $result 'shell: id/whoami 用户查询' 'FAIL' 'missing uid=0(root)' }

        # --- pci（输出含中文，验证存活：pci 后 ver 仍有响应，锚点取 ver 独有行） ---
        Send-QemuText -Session $s -Text 'pci' -Enter
        Start-Sleep -Seconds 3
        Send-QemuText -Session $s -Text 'ver' -Enter
        $h = Wait-QemuLog -Session $s -Patterns @('Architecture: x86_64') -TimeoutSeconds 30
        if ($h) { Add-TestCheck $result 'shell: pci 枚举后 shell 存活' 'PASS' }
        else { Add-TestCheck $result 'shell: pci 枚举后 shell 存活' 'FAIL' 'no response after pci' }

        # --- ping 工具（PATH → /bin/PING.ELF） ---
        Send-QemuText -Session $s -Text 'ping' -Enter
        $h = Wait-QemuLog -Session $s -Patterns @('[ping] boot') -TimeoutSeconds 40
        if ($h) {
            Add-TestCheck $result 'ping.elf: PATH 加载启动' 'PASS'
            Start-Sleep -Milliseconds 800
            Send-QemuText -Session $s -Text '10.0.2.2' -Enter
            Start-Sleep -Seconds 12   # 4 次 ICMP echo（每次超时上限 2s）
            Send-QemuKeys -Session $s -Keys @('esc')
            $h2 = Wait-QemuLog -Session $s -Patterns @('[ping] exit', '[shell] path: tool returned') -TimeoutSeconds 30
            $logp = Read-QemuLogText $s.LogPath
            if ($h2 -and $logp.Contains('[ping] exit') -and $logp.Contains('[shell] path: tool returned')) {
                Add-TestCheck $result 'ping.elf: Esc 退出返回 shell' 'PASS'
            } else {
                Add-TestCheck $result 'ping.elf: Esc 退出返回 shell' 'FAIL' 'missing [ping] exit / tool returned'
            }
        } else {
            Add-TestCheck $result 'ping.elf: PATH 加载启动' 'FAIL' 'missing [ping] boot'
        }

        # --- run EDITOR.ELF ---
        Send-QemuText -Session $s -Text 'run EDITOR.ELF' -Enter
        $h = Wait-QemuLog -Session $s -Patterns @('[editor] boot') -TimeoutSeconds 40
        if ($h) {
            Add-TestCheck $result 'editor.elf: run 加载启动' 'PASS'
            Start-Sleep -Seconds 2
            Send-QemuKeys -Session $s -Keys @('esc')
            $h2 = Wait-QemuLog -Session $s -Patterns @('[editor] exit') -TimeoutSeconds 30
            $loge = Read-QemuLogText $s.LogPath
            if ($h2 -and $loge.Contains('[shell] run: tool returned')) {
                Add-TestCheck $result 'editor.elf: Esc 退出返回 shell' 'PASS'
            } else {
                Add-TestCheck $result 'editor.elf: Esc 退出返回 shell' 'FAIL' 'missing [editor] exit / run returned'
            }
        } else {
            Add-TestCheck $result 'editor.elf: run 加载启动' 'FAIL' 'missing [editor] boot'
        }

        # --- run FILEMAN.ELF（已知问题观察项：曾触发 QEMU 退出） ---
        Send-QemuText -Session $s -Text 'run FILEMAN.ELF' -Enter
        $h = Wait-QemuLog -Session $s -Patterns @('[fileman] boot') -TimeoutSeconds 40
        if ($h) {
            Add-TestCheck $result 'fileman.elf: run 加载启动' 'PASS'
            Start-Sleep -Seconds 2
            Send-QemuKeys -Session $s -Keys @('esc')
            $h2 = Wait-QemuLog -Session $s -Patterns @('[fileman] exit') -TimeoutSeconds 30
            if ($h2) { Add-TestCheck $result 'fileman.elf: Esc 退出返回 shell（未复现 QEMU 退出）' 'PASS' }
            else { Add-TestCheck $result 'fileman.elf: Esc 退出返回 shell（未复现 QEMU 退出）' 'FAIL' 'missing [fileman] exit（疑似已知问题复现）' }
        } else {
            Add-TestCheck $result 'fileman.elf: run 加载启动' 'FAIL' 'missing [fileman] boot（疑似已知问题复现：QEMU 退出）'
        }

        # --- run BROWSER.ELF ---
        Send-QemuText -Session $s -Text 'run BROWSER.ELF' -Enter
        $h = Wait-QemuLog -Session $s -Patterns @('[browser] boot') -TimeoutSeconds 40
        if ($h) {
            Add-TestCheck $result 'browser.elf: run 加载启动' 'PASS'
            Start-Sleep -Seconds 2
            Send-QemuKeys -Session $s -Keys @('esc')
            $h2 = Wait-QemuLog -Session $s -Patterns @('[browser] exit') -TimeoutSeconds 30
            if ($h2) { Add-TestCheck $result 'browser.elf: Esc 退出返回 shell' 'PASS' }
            else { Add-TestCheck $result 'browser.elf: Esc 退出返回 shell' 'FAIL' 'missing [browser] exit' }
        } else {
            Add-TestCheck $result 'browser.elf: run 加载启动' 'FAIL' 'missing [browser] boot'
        }

        # --- Esc 回 DSK → cmd → desktop ---
        Send-QemuKeys -Session $s -Keys @('esc')
        $h = Wait-QemuLog -Session $s -Patterns @('desktop ready') -TimeoutSeconds 150
        $logf = Read-QemuLogText $s.LogPath
        Assert-QemuLog $result $logf -Ordered @(
            '[shell] esc -> return to DSK',
            '[DSK] shell auto-test returned'
        ) -MustNotContain @('[PANIC]')
        if ($h) { Add-TestCheck $result '链路: Esc → cmd → desktop ready' 'PASS' }
        else { Add-TestCheck $result '链路: Esc → cmd → desktop ready' 'FAIL' 'no desktop ready' }
    }

    # ================= MANUAL / 引用项 =================
    Add-TestCheck $result 'FirstInit 向导（程序 1）' 'MANUAL' '由 boot-regression.ps1 B1 自动化覆盖（monitor 按键填表）'
    Add-TestCheck $result 'login（程序 2）' 'SKIP' 'FUCK [dsk] skip_login=1 跳过；USER.CONF 解密路径需人工验证'
    Add-TestCheck $result 'desktop 双击/窗口拖动/任务栏（程序 3）' 'MANUAL' '无串口锚点，需人工或 screendump 验证'
    Add-TestCheck $result 'cmd pe 兼容（程序 5）' 'MANUAL' '由 pe-compat.ps1 自动化覆盖'
    Add-TestCheck $result 'netman（程序 6）' 'MANUAL' '由 network-e2e.ps1 场景 N-A 自动化覆盖（netman 仅首启调度）'
    Add-TestCheck $result 'mouseInit（程序 7）' 'MANUAL' '由 boot-regression.ps1 B1 首启链路覆盖'
    Add-TestCheck $result 'curl.elf HTTP（程序 11）' 'MANUAL' '由 network-e2e.ps1 场景 N-B 自动化覆盖（主机侧 HTTP 服务器断言）'
    Add-TestCheck $result 'editor 保存/fileman 浏览/ping RTT' 'MANUAL' '工具内部输出仅帧缓冲，无串口锚点'
} finally {
    Stop-QemuSession $s
    Restore-DeshabScenario
}

Write-TestCaseResult $result
if ($result.Status -eq 'FAIL') { exit 1 } else { exit 0 }
