# Errors

Command failures and integration errors.

---

## [ERR-20260906-001] linux_compat exec returns -4 after guest park

**Logged**: 2026-09-06T23:10:00+08:00
**Command**: DSK `lxc->exec` / `exec_async` for KDE preflight and `deshab-kde-session`
**Error**: rc=0xfffffffffffffffc (-4 超时无 EXEC_EXIT)
**Context**: Linux guest already EXEC_READY + parked; virtio-gpu and extra-rootfs backends bound; chooser sent key 2. `linux_resume` returned via HLT but `linux_to_utsm` ring had no EXEC_EXIT. Guest serial interleaved (`[utsm-linux] ini[VMEXIT]`), switch_root/vdc lines not recovered. WSL host only 7.6G so QEMU is 6G with a 3.9G extra-rootfs module.

**Next**: confirm daemon recv of EXEC_REQUEST after first park; check initramfs vs switch_root; raise WSL memory before blaming KDE packages.

**Root cause (2026-09-07)**: `UTSM_IOCTL_RECV_MSG` / `SEND_MSG` in `utsm_hcall.c` called `copy_to_user(umsg.data)` / `copy_from_user(..., umsg.data)` after `umsg` was already a kernel stack copy. That is `-EFAULT`. Daemon `recv_msg` fails, parks with no `EXEC_EXIT` → host `-4`. Empty `EXEC_READY` send worked because `data_len==0` skipped the bad copy. Fix: `memcpy` within the kernel struct, then a single `copy_to_user` of the whole ioctl arg.

### Resolution
- **Resolved**: 2026-09-07T06:41:00+08:00
- **Notes**: Rebuilt bzImage + patched ESP. QEMU verify: `KDE preflight rc=0`, `exec_async rc=0`, `KDE spawned`; desktop `guest ping rc=0`.

---

## [ERR-20260907-001] virtio-blk vdb/vdc sector 0 I/O error

**Logged**: 2026-09-07T22:50:00+08:00
**Priority**: high
**Status**: in_progress
**Area**: backend

### Summary
Linux 6.6 能 probe virtio-blk（blk_size 修好后），但 `vdb`/`vdc` 读 sector 0 曾立即 I/O error。2026-09-08 已能 `EXT4-fs (vdb): mounted`，串口无 `I/O error`。

### Error
```
virtio_blk virtio2: [vdb] 1572864 512-byte logical blocks
I/O error, dev vdb, sector 0 op 0x0:(READ)
EXT4-fs (vdb): Could not load journal inode
I/O error, dev vdc, sector 0
```

### Context
e820 已改为实际映射大小后仍失败。host 已 log extra-rootfs bound。`virtio_gpa_to_host` 对请求缓冲返回 NULL、或 chain 解析漏间接描述符，最可疑。

### Suggested Fix
对 `rootfs_blk_handle_chain` 在 IOERR 路径打 GPA/len 日志；确认 desc.addr 落在 `0x05000000` RAM 窗口。

### Progress (2026-09-08)
- e820 `0x00400000-0x03FFFFFF` 标成 60MB RAM，但 EPT 只映射了 kernel/initrd 等几页。virtio-blk READ 缓冲落在空洞且 vCPU 从未 store → `virtio_gpa_to_host` 返回 NULL → IOERR。
- 修复：`virtio_gpa_to_host` 按需映射；启动时填满该窗口空洞（~34MB）；`size_max` 改为 65535 **字节**；描述符链支持 INDIRECT + 多段；rootfs 内存可写。
- 验证：vdb mount OK，无 I/O error。Arch `/sbin/init`(systemd) 与 `/bin/sh` 在 glibc 同一偏移 SIGSEGV（`segfault at ffffffff`，确定性，非随机坏块）。PID 1 改为 `utsm_exec_daemon` 后 `EXEC_READY` 恢复。KDE 用户态仍起不来：host exec 的动态链接 sh/startplasma 同样崩；vdc overlay 本轮未见挂上。

### Progress (2026-09-08 later)
- **vdc 已挂上**：init 必须在 `mount --move /dev` 之前 mount vdc。现串口有 `persist moved to new root /mnt/persist` + overlay /opt /home /root。
- WRMSR IA32_FS_BASE 必须写入 `VMCS_GUEST_FS_BASE`（忽略写会留下 TLS=0）。
- musl busybox（无 SHSTK note）在 `push` 上 `segfault at 0`；改回 Arch `/bin/sh`→bash 后变成 glibc `#GP`，IP 正好是 `endbr64`（libc RX 映射 +0x175140）。
- 在 host 上 `wrmsr IA32_U_CET=0` 或临时打开 `CR4.CET` 会让 UTSM 在 Linux park 前就死掉，不要再试。
- 当前 blocker：动态链接 glibc 的 IBT landing pad（`endbr64`）在 nested VMX 下 #GP。Plasma 仍起不来。

### Progress (2026-09-10)
- 盘上 libc `0x199140` 已是 `90 90 90 90`；guest 仍在同一 RIP #GP。
- 用户态 #GP **会** VM-exit 到 L1；`guest_fetch` 读到全 0，ENDBR skip 从未命中。
- `ibt=off` 内核认；`user_shstk=off` 在 Linux 6.6 未知。
- nested KVM **不提供** VMCS CET save/load。
- `/bin/sh`→busybox.static 后：`segfault at 0` error 6（PUSH/SSP=0）。
- 不要用 `0F 1F 40 00` 替换 ENDBR。

### Progress (2026-09-10 afternoon)
- 限制 RIP 的盲 skip：glibc `0x7f…140` 连续 skip 8×4 字节后 `segfault at 7f…d000`；busybox.static `push r13` 仍写地址 0（RSP 正常，是 SHSTK/SSP=0）。
- L1 `CR4=0x20a0`（无 CET）；L1 CPUID 也无 CET_SS/IBT。不能在 L1 `WRMSR` CET。
- VM-entry MSR-load `IA32_U_CET=0`：VMLAUNCH 指令成功，随后 **exit reason 34**（MSR loading failure），Linux 未 park。已停用该 list。
- QEMU 进程 `arch_prctl(ARCH_SHSTK_DISABLE)`：WSL 返回 **EINVAL**，`/proc/cpuinfo` 无 shstk/ibt。LD_PRELOAD 无效。
- Guest `rdmsrl_safe`/`wrmsrl_safe(IA32_U_CET)`：**rd=wr=-EIO**（#GP）。L2 也碰不到 CET MSR，但硬件 IBT/SHSTK 仍作用于用户态。
- CET 在 L0 生效且对 L1/L2 都不可编程。在禁止 host `wrmsr` 的前提下 Plasma 起不来。

### Progress (2026-09-10 evening)
- 去掉 #PF 拦截后 park + `desktop ready` 恢复。
- `KVM_SET_MSRS` CET 返回 **r=0**（0 个 MSR 写成功），KVM 不虚拟化 CET。
- `patch_boot_on_img.sh` 把 `/bin/sh` 改成 busybox.static 会让所有脚本在 `0x4bede0` PUSH 写地址 0 上死掉。已改回 `/bin/sh -> bash`。
- bash/glibc 用户态 #GP：RIP `0x7f…140`，`ilen=2` 或 `3`（不是 ENDBR 的 4），页表走到 hole 零页（`mem=0`）。盲 skip 4 会到 `…144` 继续 #GP。
- `CR3_LOAD_EXITING` 能拦住 `MOV CR3`（见 `0x514c000` 等高位 PGD），但故障时 tracked CR3=`0x2fa2000` 仍把 libc GVA 走到零页。

### Progress (2026-09-10 EPT misconfig)
- persist 能挂上 `/dev/vdc`；`desktop ready` 后 `EPT misconfig gpa=0x3e20000` 把 vCPU 停掉。
- guest `clear_page_erms` #GP，`RDI=0x0005088000005000`（非规范），随后 `ext4lazyinit` 在已 tainted 内核里 oops。
- 根因更像 EPT leaf 未 Ignore PAT + misconfig 杀 vCPU，而不是这一次的 `EXT4-fs error`。

### Metadata
- Reproducible: yes (glibc SIGSEGV)
- Related Files: CODE/UTSM/vmm/virtio_blk.c, CODE/UTSM/vmm/virtio_mmio.c, CODE/UTSM/vmm/linux_loader.c, CODE/linux/initramfs/init
---

## [ERR-20260910-001] verify kde_ok=2 after persist check stall

**Logged**: 2026-09-10T20:22:00+08:00
**Command**: `patch_boot_on_img.sh` + `tmp_reverify2.sh` (landing-pad rebuild)
**Error**: exit 1; `guest_ok=1 session_ok=1 kde_ok=2 sent_kde=1 desktop_ready=1`; last session kmsg `checking persist mount`; no `persist check done` / `entering KDE`
**Context**: WSL NAT warning on stdout; QEMU 480s; serial 2 spawned `deshab-kde-session`. Host KDE loop still `exec sleep`/`test -f` every frame. drm `KDE scanout w=1024` is fbdev, not Plasma.

**Next**: `run_slice` pump + persist remount kmsg; rebuild UTSM/desktop/DSK; inject session via `patch_boot_on_img.sh` (do not loop-mount extra-rootfs from drvfs).

### Follow-up (2026-09-11)
`run_slice` 打进镜像后仍 `kde_ok=2`：`checking persist mount` 后无 `vdc present`/`persist check done`。同步 `test -f` 仍 `EXEC_EXIT -4`。async 时 daemon `sched_yield` 空转占满 vCPU；init `mount --move` 失败后未 umount，二次 mount 因 vdc busy 失败。

## [ERR-20260911-001] verify6 froze at Limine, no SUMMARY

**Logged**: 2026-09-11T21:35:00+08:00
**Command**: `tmp_run_verify6.sh` (initrd+patch+qemu_verify_loop)
**Error**: serial log 20KB Limine cursor text only; no UTSM/DSK markers. Wrapper `exec >file` hid later output. QEMU gone on resume.
**Next**: re-run verify only with `stdbuf -oL` + `timeout --kill-after=15 540`.

## [ERR-20260911-002] Cursor-attached wsl.exe kills the VM in ~45s

**Logged**: 2026-09-11T22:02:00+08:00
**Command**: long-running `wsl -d Ubuntu` from Cursor Shell (even `sleep 620`)
**Error**: wsl.exe exits 1 at ~45s; dmesg `systemd-shutdow` + journal "uncleanly shut down"; QEMU dies while Limine is still `Loading module linux-extra-rootfs.img`. Serial stays ~20KB cursor text. `STATUS_STACK_BUFFER_OVERRUN` / UTF-16 NAT warning on the Windows pipe.
**Next**: run `tmp_run_verify9.sh` from Windows Terminal, not Cursor. Image already patched (initrd+daemon+session+desktop).

### Follow-up (2026-09-11 22:07)
User ran it in Windows Terminal: no tty output (script `>>file`), QEMU gone at ~1 min, serial still 20KB Limine extra-rootfs, no `timed_exit`. Same kill during 3.9G module load from drvfs, not only Cursor. Next: `tmp_run_visible.sh` (tee + heartbeat + copy image to `/home/deshab`).

## [ERR-20260911-003] persist remount failed, no startplasma

**Logged**: 2026-09-11T23:04:00+08:00
**Command**: `tmp_run_visible.sh` (native `/home/deshab/deshab-dev.img`)
**Error**: `kde_ok=2`. Session: `persist remount failed`, `ERROR: no startplasma at /mnt/persist`. Init: overlays on persist then `mount --move` fail; umount+remount also fail because overlay upperdirs keep vdc busy.
**Next**: bind-mount persist into the new root; session bind from existing `/dev/vdc` mount if remount EBUSY.

## [ERR-20260912-001] verify dies at log=21013 in Windows Terminal

**Logged**: 2026-09-12T12:40:00+08:00
**Priority**: critical
**Status**: in_progress
**Area**: infra

### Summary
`tmp_run_visible.sh` 在 WT 里 t=26s、`log=21013` 整段退出，没有 SUMMARY。Limine 仍在 Loading linux-extra-rootfs.img。

### Error
```
[verify] t=26s log=21013 guest=0 session=0 desktop=0 kde=0
PS C:\Windows\system32>
```

### Context
Native `/home/deshab/deshab-dev.img` 已完整。第一次成功启动曾在 21013 停约 80s。之后多次 26–45s 被掐。不是用户提前 Ctrl+C。

### Suggested Fix
停止 Limine 预加载 extra-rootfs；vdc 改为 ESP FAT32+AHCI 映射。

### Follow-up (2026-09-12 13:21)
Limine 已过：DSK 20s、guest+desktop。init 报 `/dev/vdc missing`：`linux_launch` 在 `dsm_load`（AHCI）之前，盘上映射没有块设备。已改为先加载 manifest stage 0–1。

### Metadata
- Reproducible: yes
- Related Files: SYSTEM/limine/limine.conf, CODE/UTSM/vmm/virtio_blk.c
- See Also: ERR-20260911-002
---

## [ERR-20260912-002] entering KDE then bind /dev failed

**Logged**: 2026-09-12T14:10:18+08:00
**Priority**: high
**Status**: in_progress
**Area**: backend

### Summary
Guest parked, persist has startplasma, session reached `entering KDE root`, then `mount --rbind /dev` failed. Verify printed `VERIFY_KDE_PLASMA_OK` because `entering KDE` was treated as Plasma.

### Error
```
[deshab-kde] persist already has startplasma
[deshab-kde] entering KDE root at /mnt/persist
[deshab-kde] ERROR: could not bind /dev into the KDE root
```

### Context
e2fsck of Arch vdb was clean. extra-rootfs `/dev` `/proc` `/sys` `/run` are 0555 empty placeholders. Session stderr is `/dev/null` (async daemon). drm `SET_SCANOUT` at 7.2s is virtio-gpu probe, not Plasma.

### Suggested Fix
chmod 0755 placeholders, `mount --bind` (not rbind), log mount stderr to kmsg, seed /dev/dri on failure. Verify `kde_ok=1` only on `starting Plasma`.

### Follow-up (2026-09-12 14:16)
Real bind error: persist `/usr/bin/mount` + vdb ld.so → `libmount.so.1: Error 74` (EBADMSG). vdb `ext4_lookup` checksum invalid on inodes 1673/1810/1826 (`/usr/bin/mount` is 1826). Use persist `ld.so --library-path` to run persist mount/chroot.

### Follow-up (2026-09-12 14:45)
Static `utsm_bindmount` binds /dev /proc /sys /run /tmp. persist `mount`/`ld.so` are unusable (SHSTK or XSAVE). e2fsck of vdb is clean; checksums are runtime virtio reads (grep=1673 mkdir=1810 mount=1826). chroot persist bash exits 127 (new glibc wants XSAVE/YMM; CPUID hides them). Overlay vdb ld.so+libc → chroot 139 (SIGSEGV). Plasma still not painted.

---

## [ERR-20260912-003] idle=poll still on guest cmdline after header edit

**Logged**: 2026-09-12T19:50:00+08:00
**Priority**: critical
**Status**: in_progress
**Area**: backend

### Summary
PARK/HLT 分流已进树，但 guest 仍带着 `nohlt idle=poll`，Xorg 等待期间 `cpu_idle_poll` 空指针，idle task panic。

### Error
```
Command line: ... no_timer_check nohlt idle=poll noapic nolapic ...
RIP: cpu_idle_poll.isra.0+0x2b
RDX: 0000000000000000
Kernel panic - not syncing: Attempted to kill the idle task!
```

### Context
`linux_loader.o` 未随头文件重编（缺 `.d`）。`SYSTEM/boot/utsm.elf` 19:34 链接仍含旧字符串。验证镜像是 `/home/deshab/deshab-dev.img`。

### Suggested Fix
删除 `vmm/linux_loader.o` 后 make；确认二进制无 `nohlt`/`idle=poll`；同时 patch ISO 与 native GPT。

### Metadata
- Reproducible: yes
- Related Files: CODE/UTSM/vmm/linux_loader.c, CODE/UTSM/include/utsm/linux_loader.h
- See Also: LRN-20260912-003

### Follow-up (2026-09-12 20:33)
删掉 `linux_loader.o` 后 cmdline 已干净，idle panic 消失。vdb `/tmp`/`/run` bind 导致只读，改为 tmpfs 后 seatd socket 出现。persist `/usr/lib/Xorg` ELF 头损坏；换官方包后 Xorg 不再 139，改为 `libXdmcp.so.6: invalid ELF header`。debugfs 确认 `libXdmcp.so.6.0.0` 在 extra-rootfs 上就不是 ELF。
---

