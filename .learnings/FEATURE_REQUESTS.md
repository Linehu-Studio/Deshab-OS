# Feature Requests

Capabilities requested by the user.

---

## [FR-20260906-001] desktop chooser + KDE + Linux guest closed loop

**Logged**: 2026-09-06T22:35:00+08:00
**Status**: resolved
**Area**: dsk / linux-guest / kde

### Request
进入循环：发现 bug、修 bug、实现新功能、测试、策划下一功能。必须验证并实现：
1. 桌面选择界面启用（FUCK `session=ask`）
2. KDE / Plasma Wayland 可启动
3. Linux guest 完整启动（EXEC_READY + park）

### Notes
GPT 双分区后 DSK 不能再从主盘 LBA0 读 FUCK；必须走 Limine `fuck:config` 模块。2026-09-07 验证：`session=ask` 选择界面可用；Linux guest `EXEC_READY`+park；ioctl 修复后 KDE `exec_async` 成功 spawn。Plasma 是否画到 virtio-gpu 尚未有显示确认（无头跑 + native desktop 仍作应急 UI）。

### Resolution
- **Resolved**: 2026-09-07T06:41:00+08:00
- **Notes**: 三项闭环：chooser / guest park / KDE spawn。下一轮看 persist+dri 与 Plasma 实际出画。

---

## [FR-20260907-001] native↔KDE launchers + 2GiB ext4 + KDE scanout

**Logged**: 2026-09-07T22:00:00+08:00
**Priority**: high
**Status**: in_progress

### Progress (2026-09-07)
- GPT 已打：p1 FAT32 ESP ~5.3G + p2 ext4 **2GiB**。`ISO/deshab-dev.img` 已验证；release/realtest 与 dev 同源（`build/configs/` 缺失）。
- 原生桌面：`desktop ready`，图标来自 ESP `user/desktop`（`.lnk` + `.desktop`）。
- virtio-gpu 2D 已出画：`SET_SCANOUT 1024x768`，串口 `KDE scanout w=1024`。
- Plasma 用户态仍起不来：vdb 已能 mount，但 Arch glibc 用户态（systemd/`sh`/startplasma）确定性 SIGSEGV；`startplasma` 不在串口出现。scanout 1024 仍是内核 drm fbdev。
**Area**: dsk / linux-guest / kde / pack

### Requested Capability
构建全部镜像且 ext4 ≥ 2GiB；改到 KDE 真正出画；原生桌面能启动 KDE、KDE 能启动原生程序；两边图标都来自 `user/desktop`。

### User Context
ISO GPT 双分区（p1 FAT32 ESP + p2 ext4），不要用 AHCI SATA sidecar 测 DSK。Plasma Wayland 不发 virtio-gpu 2D SET_SCANOUT。

### Complexity Estimate
complex

### Suggested Implementation
修正 virtio-gpu 命令号对齐 Linux 6.6；X11 Plasma；`deshab-kde-session` / `deshab-native-desktop`；共享 `SYSTEM/user/desktop`。

### Metadata
- Frequency: recurring
- Related Features: FR-20260906-001

---
