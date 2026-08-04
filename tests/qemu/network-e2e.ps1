#requires -Version 5.1
<#
.SYNOPSIS
    network-e2e — 网络端到端测试（docs/RE/test-cases/network-e2e.md）

.DESCRIPTION
    场景 N-A（netman 全链路，首启调度）：
      firstInit="0\n0" 首启，挂 e1000(net0) + virtio-net(net1) 双网卡。
      DSK 首启链 mouseInit → netman → FirstInit，netman 读取 conf.conf(mode=dhcp)，
      在 index=0 的 e1000 上执行 DHCP DISCOVER/OFFER/REQUEST/ACK → ARP 网关 → DNS example.com。
      断言锚点（netman log_hex 为 16 位定宽小写十六进制，同行输出）：
        [virtio_net] selftest PASS: RX path ok     (stage3 驱动自测，真实 slirp 后端 DHCP OFFER)
        [netman] netdev_count=0x0000000000000002   (e1000 + virtio_net 双设备)
        [netman] DHCP OFFER ip= / DHCP ACK, leased ip= / DHCP final rc=0x0...0
        [netman] ARP: gateway resolved / ARP final rc=0x0...0
        [netman] DNS example.com= / DNS final rc=0x0...0
        [netman] network final rc=0x0000000000000000
      netman 之后停在 FirstInit 向导即杀会话（向导自动化由 boot-regression B1 覆盖）。

    场景 N-B（curl.elf 主机侧端到端，dev_mode shell）：
      firstInit="1\n1" → shell dev_tests → 交互 `curl`（PATH → /bin/CURL.ELF）。
      主机侧 Start-TestHttpServer 监听 127.0.0.1:8000，guest 输入
      `http://10.0.2.2:8000/` 经 slirp 网关访问主机服务；
      断言主机侧 HTTP 服务器日志收到 "GET / HTTP/1.0"（端到端最强证据），
      guest 侧断言 [curl] boot / [curl] exit / [shell] path: tool returned。
      响应体只写帧缓冲（无串口），标 MANUAL。

    退出码：全部通过 0，任一 FAIL 1。
#>
param(
    [switch]$Build,
    [string[]]$Scenarios = @('NA', 'NB')
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'lib\QemuTest.ps1')

Initialize-QemuTest -Build:$Build
$result = New-TestCaseResult 'network-e2e'

# ----------------------------------------------------------------
function Invoke-ScenarioNA {
    Write-Host "`n--- N-A netman 全链路 (首启, e1000 + virtio-net 双网卡) ---"
    Set-DeshabFirstInit -First 0 -DevMode 0
    # run_qemu.bat 默认仅 e1000；追加第二网卡 virtio-net-pci 挂独立 slirp 后端 net1。
    $s = Start-QemuSession -Name 'nete2e-na' -MonitorPort 45506 -ExtraArgs @(
        '-netdev', 'user,id=net1',
        '-device', 'virtio-net-pci,netdev=net1'
    )
    try {
        $hit = Wait-QemuLog -Session $s -Patterns @(
            '[netman] network final rc=',
            '[netman] DHCP: no OFFER received',
            '[netman] ARP: timeout',
            '[netman] DNS: timeout',
            '[PANIC]'
        ) -TimeoutSeconds 240
        Start-Sleep -Seconds 2
        $log = Read-QemuLogText $s.LogPath
        $result.LogPath = $s.LogPath

        # ---- N1/N2: 双网卡驱动与设备注册 ----
        Assert-QemuLog $result $log -Ordered @(
            '[e1000] netdev registered, index=0x0000000000000000',
            '[virtio_net] netdev registered, index=',
            '[virtio_net] selftest: TX path ok',
            '[virtio_net] selftest: DHCP OFFER yiaddr=',
            '[virtio_net] selftest PASS: RX path ok'
        )
        # ---- netman 全链路（N3 DHCP / N4 ARP / N5 DNS） ----
        Assert-QemuLog $result $log -Ordered @(
            '[DSK] loading netman',
            '[netman] boot',
            '[netman] netdev_count=0x0000000000000002',
            '[netman] DHCP: sending DISCOVER',
            '[netman] DHCP OFFER ip=',
            '[netman] DHCP: sending REQUEST',
            '[netman] DHCP ACK, leased ip=',
            '[netman] DHCP final rc=0x0000000000000000',
            '[netman] ARP: gateway resolved',
            '[netman] ARP final rc=0x0000000000000000',
            '[netman] DNS example.com=',
            '[netman] DNS final rc=0x0000000000000000',
            '[netman] network final rc=0x0000000000000000'
        ) -MustNotContain @(
            '[PANIC]',
            '[netman] DHCP: no OFFER received',
            '[netman] ARP: timeout',
            '[netman] DNS: timeout'
        )

        if ($hit -eq '[netman] network final rc=') {
            Add-TestCheck $result 'N-A: netman DHCP+ARP+DNS 全链路 rc=0' 'PASS'
        } else {
            Add-TestCheck $result 'N-A: netman DHCP+ARP+DNS 全链路 rc=0' 'FAIL' "hit=$hit"
        }
        Add-TestNote $result 'N-A: netman 仅首启调度；后续 FirstInit 向导自动化由 boot-regression.ps1 B1 覆盖，本场景断言后即结束会话'
    } finally {
        Stop-QemuSession $s
    }
}

# ----------------------------------------------------------------
function Invoke-ScenarioNB {
    Write-Host "`n--- N-B curl.elf 主机侧端到端 (dev_mode shell → http://10.0.2.2:8000/) ---"
    Set-DeshabFirstInit -First 1 -DevMode 1
    Set-DeshabAutoexec -Lines @('ver', 'exit')

    $httpLog = Join-Path $script:TestWorkDir 'nete2e-http-requests.log'
    $httpJob = Start-TestHttpServer -Port 8000 -LogPath $httpLog
    $s = Start-QemuSession -Name 'nete2e-nb' -MonitorPort 45506
    try {
        $hit = Wait-QemuLog -Session $s -Patterns @('=== 自动测试完成 ===', '[PANIC]') -TimeoutSeconds 240
        if ($hit -ne '=== 自动测试完成 ===') {
            $result.LogPath = $s.LogPath
            Add-TestCheck $result 'N-B: shell dev_tests 完成' 'FAIL' "hit=$hit"
            return
        }
        Add-TestCheck $result 'N-B: shell dev_tests 完成' 'PASS'

        # --- 启动 curl 工具（PATH → /bin/CURL.ELF） ---
        Start-Sleep -Seconds 1
        Send-QemuText -Session $s -Text 'curl' -Enter
        $h = Wait-QemuLog -Session $s -Patterns @('[curl] boot') -TimeoutSeconds 60
        if (-not $h) { Add-TestCheck $result 'N-B: curl.elf PATH 加载启动' 'FAIL' 'missing [curl] boot'; return }
        Add-TestCheck $result 'N-B: curl.elf PATH 加载启动' 'PASS'

        # --- 输入 URL：经 slirp 网关 10.0.2.2 访问主机 127.0.0.1:8000 ---
        Start-Sleep -Milliseconds 800
        Send-QemuText -Session $s -Text 'http://10.0.2.2:8000/' -Enter -DelayMs 110

        # 主机侧端到端证据：HTTP 服务器收到 guest 的 GET 请求
        $deadline = (Get-Date).AddSeconds(45)
        $hostGot = $false
        while ((Get-Date) -lt $deadline) {
            if ((Test-Path $httpLog) -and ((Get-Content $httpLog -Raw -ErrorAction SilentlyContinue) -match 'GET / HTTP/1\.0')) {
                $hostGot = $true; break
            }
            Start-Sleep -Milliseconds 800
        }
        if ($hostGot) {
            Add-TestCheck $result 'N-B: 主机 HTTP 服务器收到 guest GET /（TCP+IP+ETH 全链路）' 'PASS'
        } else {
            Add-TestCheck $result 'N-B: 主机 HTTP 服务器收到 guest GET /（TCP+IP+ETH 全链路）' 'FAIL' 'no request logged by host http server within 45s'
        }

        # --- Esc 退出 curl 返回 shell ---
        Start-Sleep -Seconds 2
        Send-QemuKeys -Session $s -Keys @('esc')
        $h2 = Wait-QemuLog -Session $s -Patterns @('[curl] exit') -TimeoutSeconds 30
        $logc = Read-QemuLogText $s.LogPath
        if ($h2 -and $logc.Contains('[shell] path: tool returned')) {
            Add-TestCheck $result 'N-B: curl Esc 退出返回 shell' 'PASS'
        } else {
            Add-TestCheck $result 'N-B: curl Esc 退出返回 shell' 'FAIL' 'missing [curl] exit / tool returned'
        }

        # --- Esc 回 DSK → cmd → desktop ---
        Send-QemuKeys -Session $s -Keys @('esc')
        $h3 = Wait-QemuLog -Session $s -Patterns @('desktop ready') -TimeoutSeconds 150
        $log = Read-QemuLogText $s.LogPath
        $result.LogPath = $s.LogPath
        Assert-QemuLog $result $log -MustNotContain @('[PANIC]')
        if ($h3) { Add-TestCheck $result 'N-B: Esc → cmd → desktop ready' 'PASS' }
        else { Add-TestCheck $result 'N-B: Esc → cmd → desktop ready' 'FAIL' 'no desktop ready' }

        Add-TestCheck $result 'N-B: HTTP 200 响应体与 DESHAB-NET-E2E-MARKER 渲染' 'MANUAL' 'curl 响应输出仅帧缓冲（无串口锚点）；主机服务器固定回复 marker 页面，可 screendump 人工核对'
        Add-TestNote $result 'N6/N11 ping 链路：由 userapps-matrix.ps1 交互阶段覆盖（ping 工具 10.0.2.2 四报文）'
        Add-TestNote $result 'N7/N9 外网访问：依赖主机联网，不在自动化断言范围（文档标 🔶）'
    } finally {
        Stop-QemuSession $s
        Stop-TestHttpServer $httpJob
    }
}

# ----------------------------------------------------------------
try {
    foreach ($sc in $Scenarios) {
        switch ($sc.ToUpper()) {
            'NA' { Invoke-ScenarioNA }
            'NB' { Invoke-ScenarioNB }
            default { Add-TestNote $result "未知场景 $sc，跳过" }
        }
    }
} finally {
    Restore-DeshabScenario
}

Write-TestCaseResult $result
if ($result.Status -eq 'FAIL') { exit 1 } else { exit 0 }
