#requires -Version 5.1
<#
.SYNOPSIS
    linux-compat — UTSM+Linux 双内核兼容层端到端（docs/RE/test-cases/linux-compat.md）

.DESCRIPTION
    动态检测 VMM/Linux guest 可用性，自动分流两条验证路径：

    路径 A（guest VMX 可用，真机/KVM）：
      [UTSM] VMM init ok → VMM self-test PASS → bzImage 解析 → IPC shm →
      vmlaunch → Linux version → initramfs init → daemon park →
      [LNXC] service ready → DSK → shell dev_tests → dsl 命令实测

    路径 B（QEMU WHPX/TCG 无 nested VMX —— 本环境预期路径）：
      [UTSM] VMM unavailable → Linux loader 跳过 → DSK 正常启动不崩溃 →
      shell 收到 linux_compat 服务表 → dsl 报告运行态不可用 → desktop。
      A5.1~A5.12 标记 SKIP（环境限制，非产品缺陷）。

    依据 ISO/run_qemu.bat 说明：WHPX 下 VMXON 会触发致命 hypervisor exit，
    因此框架默认 -cpu qemu64,-vmx 关闭 guest VMX CPUID，UTSM 走优雅降级。
    若未来 WHPX/KVM 支持 nested VMX，可用 -Cpu 'max' 复测路径 A。

    退出码：全部通过 0，任一 FAIL 1。
#>
param(
    [switch]$Build,
    [string]$Cpu = ''
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'lib\QemuTest.ps1')

Initialize-QemuTest -Build:$Build
$result = New-TestCaseResult 'linux-compat'

Set-DeshabFirstInit -First 1 -DevMode 1
Set-DeshabAutoexec -Lines @('ver', 'exit')

$s = Start-QemuSession -Name 'linux-compat' -Cpu $Cpu
try {
    # ---- 分流点：VMM 自检通过 还是 VMM 不可用 ----
    $fork = Wait-QemuLog -Session $s -Patterns @(
        '[UTSM] VMM self-test PASS',
        '[UTSM] VMM unavailable'
    ) -TimeoutSeconds 180

    if ($fork -eq '[UTSM] VMM self-test PASS') {
        # ================= 路径 A：VMX 可用，完整双内核流程 =================
        Write-Host "`n--- 路径 A: guest VMX 可用，验证完整 Linux 兼容层 ---"
        $hit = Wait-QemuLog -Session $s -Patterns @('=== 自动测试完成 ===', '[PANIC]') -TimeoutSeconds 300
        $log = Read-QemuLogText $s.LogPath
        $result.LogPath = $s.LogPath

        # A5.1 ~ A5.7 启动链
        Assert-QemuLog $result $log -Ordered @(
            '[UTSM] VMM init ok',
            '[UTSM] VMM self-test PASS',
            '[LINUX] found bzImage module:',
            '[UTSM] Linux loader init ok',
            '[UTSM] IPC shm init ok',
            '[VMM] launching guest',
            'Linux version',
            '[utsm-linux] init started',
            '[LINUX] guest parked (daemon ready)',
            '[LNXC] service ready',
            '[DSK] boot',
            '[shell] linux_compat service ok'
        ) -MustNotContain @('[PANIC]')
        if (-not $hit -or $hit -eq '[PANIC]') {
            Add-TestCheck $result 'A5.7: DSK/shell 接管' 'FAIL' 'dev_tests did not complete'
        } else {
            Add-TestCheck $result 'A5.7: DSK/shell 接管' 'PASS'
        }

        # A5.8 ~ A5.12 shell dsl 交互（dev_mode 串口镜像开启）
        if ($hit -eq '=== 自动测试完成 ===') {
            Start-Sleep -Seconds 1
            Send-QemuText -Session $s -Text 'dsl' -Enter
            $h1 = Wait-QemuLog -Session $s -Patterns @('Linux 兼容层已就绪') -TimeoutSeconds 60
            if ($h1) { Add-TestCheck $result 'A5.8: dsl 状态查询' 'PASS' }
            else { Add-TestCheck $result 'A5.8: dsl 状态查询' 'FAIL' 'missing 兼容层已就绪' }

            Send-QemuText -Session $s -Text 'dsl ls' -Enter
            $h2 = Wait-QemuLog -Session $s -Patterns @('bin', 'etc', 'deshab#') -TimeoutSeconds 60
            if ($h2) { Add-TestCheck $result 'A5.9: dsl ls 目录列表' 'PASS' }
            else { Add-TestCheck $result 'A5.9: dsl ls 目录列表' 'FAIL' 'no listing output' }

            Send-QemuText -Session $s -Text 'dsl uname -a' -Enter
            $h3 = Wait-QemuLog -Session $s -Patterns @('Linux') -TimeoutSeconds 60
            if ($h3) { Add-TestCheck $result 'A5.10: dsl uname -a' 'PASS' }
            else { Add-TestCheck $result 'A5.10: dsl uname -a' 'FAIL' 'no uname output' }

            Send-QemuText -Session $s -Text 'dsl nosuchprog' -Enter
            $h4 = Wait-QemuLog -Session $s -Patterns @('deshab# ') -TimeoutSeconds 60
            Start-Sleep -Seconds 1
            $log2 = Read-QemuLogText $s.LogPath
            if ($h4 -and -not $log2.Contains('[PANIC]')) {
                Add-TestCheck $result 'A5.12: dsl 错误路径不崩溃' 'PASS'
            } else {
                Add-TestCheck $result 'A5.12: dsl 错误路径不崩溃' 'FAIL' 'shell did not recover'
            }

            Send-QemuKeys -Session $s -Keys @('esc')
            $h5 = Wait-QemuLog -Session $s -Patterns @('desktop ready') -TimeoutSeconds 150
            if ($h5) { Add-TestCheck $result 'A5.x: Esc 后到 desktop' 'PASS' }
            else { Add-TestCheck $result 'A5.x: Esc 后到 desktop' 'FAIL' 'no desktop ready' }
        }
        Add-TestNote $result '路径 A 执行：guest VMX 可用（真机/KVM 环境）'
    }
    elseif ($fork -eq '[UTSM] VMM unavailable') {
        # ================= 路径 B：无 nested VMX，优雅降级 =================
        Write-Host "`n--- 路径 B: guest 无 VMX（QEMU 预期），验证优雅降级 ---"
        $hit = Wait-QemuLog -Session $s -Patterns @('=== 自动测试完成 ===', '[PANIC]') -TimeoutSeconds 240
        $log = Read-QemuLogText $s.LogPath
        $result.LogPath = $s.LogPath

        # 降级锚点 + 系统继续正常启动
        Assert-QemuLog $result $log -MustContain @('[UTSM] VMM unavailable')
        Assert-QemuLog $result $log -Ordered @(
            '[UTSM] SELFTEST PASS',
            '[UTSM] VMM unavailable',
            '[DSK] boot',
            '[DSK] dev_mode=1 (developer auto-test)',
            '[shell] boot',
            '[shell] linux_compat service ok'
        ) -MustNotContain @('[PANIC]')
        if ($hit -eq '=== 自动测试完成 ===') {
            Add-TestCheck $result '降级路径: dev_tests 完成（Linux 缺失不阻塞启动）' 'PASS'
        } else {
            Add-TestCheck $result '降级路径: dev_tests 完成（Linux 缺失不阻塞启动）' 'FAIL' "hit=$hit"
        }

        # dsl 运行态不可用报告（服务表已传递，is_available()=0）
        if ($hit -eq '=== 自动测试完成 ===') {
            Start-Sleep -Seconds 1
            Send-QemuText -Session $s -Text 'dsl' -Enter
            $h = Wait-QemuLog -Session $s -Patterns @('linux_compat: not initialized') -TimeoutSeconds 40
            if ($h) { Add-TestCheck $result '降级路径: dsl 报告运行态不可用' 'PASS' }
            else { Add-TestCheck $result '降级路径: dsl 报告运行态不可用' 'FAIL' 'missing linux_compat: not initialized' }

            Send-QemuKeys -Session $s -Keys @('esc')
            $h2 = Wait-QemuLog -Session $s -Patterns @('desktop ready') -TimeoutSeconds 150
            if ($h2) { Add-TestCheck $result '降级路径: Esc 后经 cmd 到 desktop' 'PASS' }
            else { Add-TestCheck $result '降级路径: Esc 后经 cmd 到 desktop' 'FAIL' 'no desktop ready' }
        }

        # A5.1~A5.12 环境性跳过
        foreach ($t in @('A5.1 VMM self-test', 'A5.2 bzImage 加载', 'A5.3 EPT+vmlaunch',
                         'A5.4 Linux 内核启动', 'A5.5 init 脚本', 'A5.6 daemon park',
                         'A5.8 dsl 状态就绪', 'A5.9 dsl ls', 'A5.10 dsl uname',
                         'A5.11 dsl cat hostname', 'A5.12 dsl 错误路径')) {
            Add-TestCheck $result $t 'SKIP' 'QEMU WHPX/TCG 无 nested VMX（见 run_qemu.bat 注释），需真机或 KVM 复测'
        }
        Add-TestCheck $result 'A5.7: Linux 缺失时 DSK 正常接管' 'PASS'
        Add-TestNote $result '路径 B 执行：QEMU 无 nested VMX，Linux guest 不可用属预期环境限制'
        Add-TestNote $result '如需完整验证：真机（VT-x）或 Linux KVM (-enable-kvm -cpu host) 下以 -Cpu max 重跑'
    }
    else {
        $result.LogPath = $s.LogPath
        Add-TestCheck $result 'VMM 分流探测' 'FAIL' 'neither VMM self-test PASS nor VMM unavailable within 180s'
    }
} finally {
    Stop-QemuSession $s
    Restore-DeshabScenario
}

Write-TestCaseResult $result
if ($result.Status -eq 'FAIL') { exit 1 } else { exit 0 }
