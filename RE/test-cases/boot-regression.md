# 启动链路回归测试矩阵

> 对应计划 Phase C2：验证不同启动场景下系统行为正确

## 状态: ⬜ PENDING

## 测试场景

| # | 场景 | firstInit.txt | 配置 | 期望串口锚点 | 状态 | Bug# |
|---|---|---|---|---|---|---|
| B1 | 首次启动 | `0\n0` | 空 user.conf | `[DSK] firstInit=0` → `mouseInit` → `FirstInit` → 完成向导 → 写盘 | ⬜ | |
| B2 | 正常启动 | `1\n0` | 已有 user.conf | `[DSK] firstInit=1` → `login` → desktop | ⬜ | |
| B3 | dev_mode | `1\n1` | dev_mode=1 | `[DSK] dev_mode` → shell run_dev_tests 全绿 → Esc → 正常流程 | ⬜ | |
| B4 | 无 SATA 盘 | 任意 | 移除 sata_fat32_dsk.img | DSK fallback 到 Limine module | ⬜ | |
| B5 | 无 NIC | 任意 | 移除 `-netdev` | e1000 not found → netman 警告 → 继续启动 | ⬜ | |
| B6 | Linux 兼容层启用 | 任意 | +linux-bzImage +linux-initrd | UTSM VMM + Linux park → DSK 正常接管 | ⬜ | |

## 串口日志锚点（标准启动）

### 首次启动（B1）

```
[UTSM] boot
[UTSM] SELFTEST PASS
[UTSM] loading DSK
[DSK] boot
[DSK] context ok
[DSK] firstInit=0
[DSK] loading mouseInit.elf
[DSK] loading FirstInit.elf
[FirstInit] ...
[DSK] writing firstInit=1
[DSK] loading shell.elf
```

### 正常启动（B2）

```
[UTSM] boot
[UTSM] SELFTEST PASS
[UTSM] loading DSK
[DSK] boot
[DSK] firstInit=1
[DSK] loading login.elf
[DSK] loading desktop.elf
```

### dev_mode（B3）

```
[UTSM] boot
[UTSM] SELFTEST PASS
[DSK] dev_mode=1
[DSK] loading shell.elf
[shell] run_dev_tests:
[shell]   ls: OK
[shell]   echo>: OK
[shell]   cat: OK
[shell]   cp: OK
[shell]   mv: OK
[shell]   rm: OK
[shell] dev_tests done
... (Esc 返回后正常流程)
```

## 自动化脚本

[ISO/run_qemu_scenario.bat](../../ISO/run_qemu_scenario.bat) 接收场景名参数：

```powershell
.\ISO\run_qemu_scenario.bat firstboot
.\ISO\run_qemu_scenario.bat normal
.\ISO\run_qemu_scenario.bat devmode
.\ISO\run_qemu_scenario.bat nosata
.\ISO\run_qemu_scenario.bat nonic
.\ISO\run_qemu_scenario.bat linux
```

每个场景输出独立日志到 `ISO/logs/<scenario>-<timestamp>.log`。

## 配置文件准备

### B1 首次启动

- `SYSTEM/system/user/use/firstInit.txt`: `0\n0`
- 备份 `user.conf` 后清空内容（仅保留 `DESHAB_USERCONF_V1` 头）

### B3 dev_mode

- `firstInit.txt`: `1\n1`
- 保持现有 user.conf

### B4 无 SATA

- 临时重命名 `.build_tmp/sata_fat32_dsk.img` → `.build_tmp/sata_fat32_dsk.img.bak`
- run_qemu.bat 自动检测，缺失时跳过 `-device ide-hd`
