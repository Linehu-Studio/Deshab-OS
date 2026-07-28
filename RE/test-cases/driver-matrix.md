# 14 DKM 驱动功能矩阵

> 对应计划 Phase C3：每个驱动的加载、探测、功能验证点

## 状态: ⬜ PENDING

## 驱动清单

| # | 驱动 | stage | class | required | QEMU 参数 | 期望串口日志 | 功能验证点 | 状态 | Bug# |
|---|---|---|---|---|---|---|---|---|---|
| 1 | timer | 0 | timer | ✅ | 默认 | `[DKM:timer]` | 100ms busy-wait 精度 | ⬜ | |
| 2 | apic | 0 | interrupt | ✅ | 默认 | `[DKM:apic] MADT found` | LAPIC MMIO 只读 | ⬜ | |
| 3 | acpi | 0 | platform | ❌ | 默认 | `[DKM:acpi]` | RSDP/XSDT 枚举 | ⬜ | |
| 4 | pci | 0 | bus | ✅ | 默认 | `[DKM:pci]` | PCI config space 扫描 | ⬜ | |
| 5 | console_fb | 1 | console | ❌ | 默认 | `[DKM:console_fb]` | 淡蓝色背景清屏 | ⬜ | |
| 6 | ahci | 1 | storage | ❌ | +SATA 测试盘 | `[DKM:ahci] ahci0 registered` | LBA0 读取 | ⬜ | |
| 7 | nvme | 1 | storage | ❌ | `-device nvme` | `[DKM:nvme] 1b36:0010 found` | BAR0 读取（高位 MMIO 未解决则 SKIP） | ⏭ | |
| 8 | bootfs | 1 | fs | ✅ | 默认 | `[DKM:bootfs]` | Limine boot module FS | ⬜ | |
| 9 | vfs | 2 | fs-core | ✅ | 默认 | `[DKM:vfs]` | 挂载 bootfs | ⬜ | |
| 10 | fat32 | 2 | fs | ❌ | +SATA 测试盘 | `[DKM:fat32]` | BPB/FAT/目录项解析 | ⬜ | |
| 11 | devfs | 2 | fs | ❌ | 默认 | `[DKM:devfs]` | /dev/version, /dev/fb0 等 | ⬜ | |
| 12 | e1000 | 3 | net | ❌ | `-device e1000` | `tx=ready rx=ready` | DHCP + ping | ⬜ | |
| 13 | virtio_net | 3 | net | ❌ | `-device virtio-net-pci` | `[DKM:virtio_net] 1af4:1001 found` | capability 枚举 | ⬜ | |
| 14 | ath9k | 3 | net | ❌ | （QEMU 不支持 WiFi） | `no Atheros WiFi device found` | 跳过（QEMU 限制） | ⏭ | |
| 15 | ps2kbd | 3 | input | ❌ | 默认 | scan code 读取 | 输入回显 | ⬜ | |

## stage 加载顺序

```
stage 0 (platform): timer → apic → acpi → pci
stage 1 (boot):     console_fb → ahci → nvme → bootfs
stage 2 (fs):       vfs → fat32 → devfs
stage 3 (optional): e1000 → virtio_net → ath9k → ps2kbd
```

## 测试设备 QEMU 参数

### AHCI + FAT32 测试盘

```
-drive id=sata0,format=raw,file=.build_tmp/sata_fat32_dsk.img,if=none ^
-device ide-hd,drive=sata0,bus=ide.0 ^
```

### NVMe

```
-device nvme,drive=nvme0,serial=UTSMTEST ^
-drive id=nvme0,format=raw,file=.build_tmp/nvme_test.img,if=none ^
```

### e1000

```
-netdev user,id=net0 ^
-device e1000,netdev=net0,mac=52:54:00:12:34:56 ^
```

### virtio-net

```
-netdev user,id=net0 ^
-device virtio-net-pci,netdev=net0 ^
```

## 已知限制

- NVMe BAR0 在 QEMU 中常位于 4G 以上，当前 HHDM 不能安全访问，需 Phase 0 高位 MMIO 映射解决
- ath9k 在 QEMU 中无对应设备模拟，永远走 "not found" 路径
- 多个 NIC 同时挂载会改变 PCI 拓扑，可能影响 e1000 探测位置
