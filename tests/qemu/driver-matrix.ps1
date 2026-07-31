#requires -Version 5.1
<#
.SYNOPSIS
    driver-matrix — DKM 驱动功能矩阵（docs/RE/test-cases/driver-matrix.md）

.DESCRIPTION
    单次 QEMU 会话挂全设备（SATA 测试盘 + NVMe 测试盘 + xHCI USB 盘 + e1000），
    验证 4 个 stage 共 16 个清单驱动的加载顺序与功能锚点：

      stage0: timer / apic / acpi / pci
      stage1: console_fb / ahci / nvme / xhci / bootfs
      stage2: vfs / fat32 / devfs
      stage3: e1000 / virtio_net(无设备,降级) / ath9k(FUCK 禁用) / ps2kbd

    已知限制（与文档一致）：
      - virtio_net 在无 virtio-net 设备时走 not-found 降级路径（断言降级锚点）
      - ath9k 被 FUCK [drivers] ath9k=0 禁用（断言 DSM 禁用锚点；QEMU 无 WiFi 设备）
      - xhci USB MSC 枚举在当前实现下可能失败，仅断言驱动就绪

    退出码：全部通过 0，任一 FAIL 1。
#>
param([switch]$Build)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'lib\QemuTest.ps1')

Initialize-QemuTest -Build:$Build
$result = New-TestCaseResult 'driver-matrix'

Write-Host "`n--- 全设备单会话驱动矩阵 ---"
$s = Start-QemuSession -Name 'driver-matrix'
try {
    $hit = Wait-QemuLog -Session $s -Patterns @('[DSK] boot') -TimeoutSeconds 150
    Start-Sleep -Seconds 2
    $log = Read-QemuLogText $s.LogPath
    $result.LogPath = $s.LogPath

    # ---- stage 加载顺序（driver ready 锚点按 manifest 顺序） ----
    Assert-QemuLog $result $log -Ordered @(
        '[timer] driver ready',
        '[apic] driver ready',
        '[acpi] driver ready',
        '[pci] driver ready',
        '[console_fb] ready',
        '[ahci] driver ready',
        '[nvme] driver ready',
        '[xhci] driver ready',
        '[bootfs] driver ready',
        '[vfs] driver ready',
        '[fat32] driver ready',
        '[devfs] driver ready',
        '[e1000] driver ready',
        '[virtio_net] driver ready',
        '[kbd] driver ready',
        '[UTSM] SELFTEST PASS'
    )

    # ---- 功能验证点 ----
    Assert-QemuLog $result $log -MustContain @(
        '[ahci] block provider index=',          # ahci: ahci0 block provider 注册
        '[nvme] block provider index=',          # nvme: block provider 注册
        '[fat32] using block provider image',    # fat32: 经块设备解析测试镜像
        '[e1000] netdev registered',             # e1000: 注册 netdev
        '[e1000] link=up',                       # e1000: 链路就绪
        '[kbd] IRQ1 registered',                 # ps2kbd: IRQ1 注册
        '[virtio_net] virtio-net device not found',  # virtio_net: e1000-only 拓扑降级
        '[DSM] driver disabled by FUCK'          # ath9k: FUCK 禁用路径
    )

    # ---- DSM 装载统计：16 个 manifest 驱动中 15 个 active（ath9k 禁用） ----
    Assert-QemuLog $result $log -CountPattern '[DKM] external driver active' -MinCount 15

    # ---- 4 个 stage 均完成 ----
    Assert-QemuLog $result $log -CountPattern '[DSM] stage end' -MinCount 4

    if ($hit) { Add-TestCheck $result '系统到达 DSK' 'PASS' }
    else { Add-TestCheck $result '系统到达 DSK' 'FAIL' 'timeout waiting [DSK] boot' }

    Add-TestNote $result 'ath9k: QEMU 无 WiFi 设备模拟，FUCK ath9k=0，走 DSM 禁用路径（文档第 14 行 ⏭）'
    Add-TestNote $result 'xhci: USB MSC 枚举当前可能失败（已知限制），仅断言 [xhci] driver ready'
} finally {
    Stop-QemuSession $s
}

Write-TestCaseResult $result
if ($result.Status -eq 'FAIL') { exit 1 } else { exit 0 }
