# Learnings

Corrections, insights, and knowledge gaps captured during development.

**Categories**: correction | insight | knowledge_gap | best_practice

## [LRN-20260912-003] correction

**Logged**: 2026-09-12T19:50:00+08:00
**Priority**: critical
**Status**: in_progress
**Area**: backend

### Summary
只改 `LINUX_DEFAULT_CMDLINE` 再 `ld.lld` 不够：`linux_loader.o` 若没有对应 `.d`，make 不会重编，旧 `nohlt idle=poll` 会继续打进 guest。

### Details
`linux_loader.h` 19:33 已去掉 `nohlt idle=poll`，`utsm.elf` 19:34 重新链接，但 `linux_loader.o` 仍是 9 月 10 日。串口仍是 `cpu_idle_poll` NULL deref + `Attempted to kill the idle task`。验证跑的是 `/home/deshab/deshab-dev.img`，只在体积变化时才从 ISO 整盘复制，所以必须同时 patch 两份 GPT。

### Suggested Action
改 cmdline 后强制删 `vmm/linux_loader.o` 再 make；`strings utsm.elf` 确认无 `nohlt`/`idle=poll`；`patch_boot_on_img.sh` 打 ISO **和** `/home/deshab/deshab-dev.img`。不要再加 `idle=poll`/`nohlt`。Xorg 诊断不要只写 `/dev/kmsg`（限流会吞 sed 输出），用 `log()` 打 `/dev/console`。vdb 是 ro，不能 bind `/tmp`/`/run` 进 persist（会变成只读，Xorg 建不了 socket、seatd 建不了 sock）；这两个目录要挂 tmpfs。persist `/usr/lib/Xorg` 文件大小与官方包相同（2642568）但 ELF 头被砸坏（`e_entry=0`，.text 只有 14KB）；官方包入口是 `0x35900`。不要改 extra-rootfs，把完好 Xorg 放 vdb `/usr/local/lib/Xorg.good` 再 bind。Xorg.good 起来后 persist 的 `libXdmcp.so.6.0.0` 不是 ELF。官方 Xorg NEEDED 闭包用 `LD_LIBRARY_PATH=/usr/local/lib/xorg-libs` 覆盖后，X socket 能起来。Plasma 立刻 127 不是 startplasma 本身缺文件：`runuser` 的传递依赖 `libcap-ng.so.0` 头坏了（PATH 若先搜 sbin，会跑到更直接依赖它的 `/usr/sbin/runuser`）。用官方 `libcap-ng.so.0` 放进 xorg-libs 后，runuser 能过，下一步是 Plasma 状态 135（128+SIGBUS），stderr 空，要看 dmesg 才知道是哪段库或 vdc 校验。不要把只有 `starting Plasma`、还没确认没有 `Plasma failed` 的窗口当成 `VERIFY_KDE_PLASMA_OK`。

### Metadata
- Source: error
- Related Files: CODE/UTSM/include/utsm/linux_loader.h, CODE/UTSM/vmm/linux_loader.c, CODE/linux/tmp_run_visible.sh
- Tags: idle, cmdline, kde, xorg
- See Also: LRN-20260912-002

---

## [LRN-20260912-002] insight

**Logged**: 2026-09-12T14:45:00+08:00
**Priority**: high
**Status**: pending
**Area**: backend

### Summary
KDE extra-rootfs glibc 带 XSAVE/YMM/ZMM GNU property；L2 CPUID 隐藏 XSAVE 后 persist bash/Xorg 在 chroot 里立刻 127。不要 `objcopy --remove-section=.note.gnu.property`（会留下零长度 GNU_PROPERTY 头，interpreter 无法加载）。

### Details
vdb e2fsck 干净，运行时 `ext4_lookup` checksum 打在 grep/mkdir/mount。静态 `utsm_bindmount` 能 bind /dev。persist ld.so 直接跑会 SHSTK 写地址 0；chroot persist bash=127；叠 vdb ld.so+libc=139。

### Suggested Action
要么给 L2 做完整 XSAVE 保存/恢复（并放开 CPUID），要么 extra 换无 AVX glibc / Xfbdev 小用户态。不要再拆 GNU property section。

### Resolution
- **Resolved**: 2026-09-12T15:42:00+08:00 (XSAVE half)
- **Notes**: L2 XSAVE+AVX 已落地（XCR0=7，832B，内核 `Enabled xstate features 0x7`）。persist bash ISA needed 只是 x86-64-baseline。直接 exec persist ld.so 仍在 `_dl_find_dso_for_object` 写 -40 崩；vdb ld.so + persist bash 能起来。vdc 必须 `ro,noload` + overlay/tmpfs upper，绝不能 remount vdb/vdc rw。镜像 e2fsck 干净时仍可能 runtime checksum（mkdir/chmod/touch/libreadline）——virtio 读路径问题，不要靠给 extra-rootfs 反复 inject 修。`/usr/bin/Xorg` 是 271 字节 wrapper（会 exec suid Xorg.wrap）；真服务器是 `/usr/lib/Xorg`，且会 daemonize，要用 `-keeptty` 并等 socket 而不是等父进程。guest clocksource 被标成 refined-jiffies，nanosleep 依赖 PIT IRQ0。不要再拆 GNU property。

### Metadata
- Source: conversation
- Related Files: CODE/linux/guest/deshab-kde-session, CODE/UTSM/vmm/vmexit.c, CODE/linux/initramfs/bin/utsm_bindmount.c
- Tags: kde, xsave, persist, chroot
---

---

## [LRN-20260906-001] correction

**Logged**: 2026-09-06T18:30:00+08:00
**Priority**: high
**Status**: pending
**Area**: backend

### Summary
VMX TRUE CTLS 低半字是 allowed-0（bit=0 必须为 1），不是 must-be-1 位图；旧 OR-low 公式漏 Virtual NMIs 等 must-be-1 位。

### Details
交接与初版计划把 Intel/KVM 极性说反，并把 cpu bit26、VPID 非零+enable=0 当成 error 7。复核后：SDM A.3 低 32=allowed-0、高 32=allowed-1；`(desired & high) | low` 与 KVM `(desired | low) & high` 都不会补 low=0 的必须-1 位。自测 pin=0x1f 缺 bit 5，同 MSR 下 Intel 公式应得 0x29。VPID 禁用时字段非零不是 SDM 列出的检查。`.build_tmp/kvm_smoke_serial.log` 从未 vmlaunch。

### Suggested Action
嵌套 KVM 实测：`CTLS2 allowed0=0`，`~allowed0` 会打开全部 secondary 位并继续 error 7。正确公式是 KVM 极性 `(desired | allowed0) & allowed1`。继续查 EPTP/MSR bitmap 等一致性。

### Resolution
- **Resolved**: 2026-09-06T18:45:00+08:00
- **Notes**: `~low` 会把 CTLS2 全打开。自测真正过线的是 pin 加上 Virtual NMIs（0x3f）。EPTP 改 UC 会回归 error 7，保持 WB。

### Metadata
- Source: conversation
- Related Files: CODE/UTSM/vmm/vmx.c, CODE/UTSM/vmm/vmm.c, CODE/UTSM/vmm/linux_boot.c
- Tags: vmx, error7, true-ctls
- Pattern-Key: harden.input_validation

---

## [LRN-20260906-002] correction

**Logged**: 2026-09-06T21:35:00+08:00
**Priority**: high
**Status**: promoted
**Promoted**: CLAUDE.md
**Area**: infra

### Summary
发行物必须全部落入 SYSTEM/；打包只扫描 SYSTEM/，不从 /home 或其它路径取镜像。

### Details
Phase 0 曾把 KDE/Python rootfs 留在 WSL `/home/deshab/`，避免塞进 2GiB FAT32 ESP。用户明确要求：所有软件装在 SYSTEM/，打包时不要从其它地方找，直接把整个 SYSTEM 打成一个镜像。FAT32 单文件不能超过 4GiB-1，因此 extra-rootfs 默认改为 3900MB。

### Suggested Action
构建脚本最终输出一律写 SYSTEM/boot；build.ps1 按 SYSTEM 体积自动扩 ESP；Limine 模块路径只引用 SYSTEM/boot 下的文件。

### Metadata
- Source: user_feedback
- Related Files: build.ps1, SYSTEM/limine/limine.conf, CODE/linux/build_extra_rootfs.sh
- Tags: system, pack, fat32
- Pattern-Key: harden.input_validation

---

## [LRN-20260906-003] correction

**Logged**: 2026-09-06T22:20:00+08:00
**Priority**: high
**Status**: promoted
**Promoted**: CLAUDE.md
**Area**: infra

### Summary
磁盘镜像改为 GPT 双分区：FAT32 ESP 只放引导（EFI/limine/boot），其余 SYSTEM/ 进 ext4。

### Details
用户要求把文件系统换成 ext4 + Fat32 引导分区 boot 文件夹。原先 New-GptFat32Image 把整个 SYSTEM/ 打进单一 FAT32 ESP。现由 WSL `pack_system_image.sh` 写 p1 FAT32 + p2 ext4；Limine 用固定 ext4 UUID 加载 /driver 与 /system，boot(): 仅指向 ESP 上的 /boot。FAT32 4GiB 单文件上限仍约束 SYSTEM/boot 里的 extra-rootfs。

### Suggested Action
打包只走 pack_system_image.sh；改 limine 数据路径时同步 EXT4_UUID。DSK 运行时 FAT32 读取仍走独立 SATA 测试盘（整盘 LBA0），不要假设主 virtio 盘 LBA0 是 BPB。

### Metadata
- Source: user_feedback
- Related Files: CODE/linux/pack_system_image.sh, build.ps1, SYSTEM/limine/limine.conf
- Tags: gpt, fat32, ext4, pack
- Pattern-Key: harden.input_validation

---

## [LRN-20260906-004] knowledge_gap

**Logged**: 2026-09-06T22:45:00+08:00
**Priority**: high
**Status**: promoted
**Promoted**: CLAUDE.md
**Area**: boot

### Summary
Limine 10 不能从 ext4 加载模块；`uuid()` 只能指向它能读的 FAT 卷。

### Details
GPT 双分区后把 driver/system 放到 ext4 并用 `uuid(...):/driver/...`，Limine 10.8.5 在加载 utsm.elf 后 PANIC：Failed to open module。boot(): /boot/utsm.elf 成功是因为文件在 FAT32 ESP。

### Suggested Action
ESP 必须包含 Limine 要加载的全部路径：/EFI /limine /boot /driver /system。ext4 只作数据分区。

### Metadata
- Source: qemu_serial_loop1.log
- Related Files: SYSTEM/limine/limine.conf, CODE/linux/pack_system_image.sh
- Tags: limine, ext4, fat32
- Pattern-Key: harden.input_validation

---

## [LRN-20260907-001] insight

**Logged**: 2026-09-07T06:41:00+08:00
**Priority**: high
**Status**: resolved
**Area**: backend

### Summary
`/dev/utsm` ioctl 的 payload 已在 `copy_from_user` 整结构之后进入内核栈，必须 `memcpy`，不能再 `copy_*_user` 结构体内部数组。

### Details
`UTSM_IOCTL_RECV_MSG` 对 `umsg.data`（内核地址）做 `copy_to_user` 得到 `-EFAULT`。daemon 当成空 ring，立刻 park，host `lxc_exec` 返回 `-4`。`EXEC_READY` 能过是因为 `data_len==0`。同一 bug 也让 desktop `guest ping` 误报 “Linux guest not running”。

### Suggested Action
ioctl 先整包 `copy_from_user`，内核内 `memcpy` payload，再整包 `copy_to_user`。改 `CODE/linux/patches/utsm_hcall.c` 后必须拷到 WSL 原生树 `/home/deshab/linux-6.6` 重编 bzImage，不要经 `/mnt/d` 改内核源。

### Metadata
- Source: error
- Related Files: CODE/linux/patches/utsm_hcall.c, CODE/linux/initramfs/bin/utsm_exec_daemon.c, CODE/UTSM/vmm/linux_compat.c
- Tags: ioctl, ipc, linux-guest, kde
- See Also: ERR-20260906-001

---

## [LRN-20260907-002] correction

**Logged**: 2026-09-07T21:20:00+08:00
**Priority**: high
**Status**: pending
**Area**: tooling

### Summary
KDE/DSK 验证必须用 `ISO/deshab-dev.img`。`.build_tmp/sata_fat32_dsk.img` 只是 QEMU 额外挂的 AHCI 测试盘；UTSM 优先从 block0 FAT32 读 `deshab.elf`，挂上它就会把 ISO ESP 上的 DSK 盖掉。

### Details
用户明确纠正：测的是 ISO 下的 dev 镜像，不是 SATA。`run_qemu_kvm.sh` 默认挂 SATA，导致热补 ESP 的 `deshab.elf` 不生效。另外 QEMU 开着时 `pack_system_image.sh` 最后 `cp` 到 `ISO/*.img` 会因文件占用失败，三个镜像时间戳都不动。`build/configs/` 目前不存在，`both` 只会 WARN 后用同一份 SYSTEM 打三份。打包日志里的 FAT32 是 GPT p1 ESP（Limine 不能读 ext4），p2 才是 ext4；`linux-extra-rootfs.img` 必须放在 ESP，所以镜像看起来像“大 FAT32”。SATA sidecar 重建也会打出 FAT32 字样，但那不是 ISO 系统盘。

### Suggested Action
验证脚本设 `SATA_IMG_OVERRIDE` 为不存在的路径。打包前先停 QEMU。不要把 SATA 测试盘当成系统镜像。

### Metadata
- Source: conversation
- Related Files: ISO/run_qemu_kvm.sh, CODE/linux/qemu_verify_loop.sh, CODE/UTSM/kernel/dsk_loader.c, build.ps1
- Tags: qemu, sata, iso, dsk-loader
- Pattern-Key: harden.input_validation

---

## [LRN-20260907-003] insight

**Logged**: 2026-09-07T22:30:00+08:00
**Priority**: high
**Status**: resolved
**Area**: backend

### Summary
ESP ≥ 4GiB 时 pack 使用 32KiB FAT 簇。DSK 仍按 8 扇区读 4KiB 缓冲，desktop.elf 入口是垃圾代码，一跳就复位。

### Details
修好后串口有 `fat32 spc=0x40`、`desktop ready`、`user/desktop` 图标。firstInit 走 LFN `system/user/use/firstInit.txt`。

### Suggested Action
簇缓冲 ≥ 32KiB，读长度用 BPB `spc`。

### Metadata
- Source: error
- Related Files: CODE/dsk/main.c, CODE/tools/fat32_io.h, CODE/linux/pack_system_image.sh
- Tags: fat32, cluster, desktop
- See Also: LRN-20260907-002

---

## [LRN-20260907-004] insight

**Logged**: 2026-09-07T22:45:00+08:00
**Priority**: high
**Status**: resolved
**Area**: backend

### Summary
virtio-mmio 未实现的 SHM 长度必须是 `(u64)-1`，否则 Linux virtio-gpu probe `-EBUSY`。virtio-blk config 必须与 Linux uapi 同布局。

### Details
修好后 guest `Initialized virtio_gpu` + `SET_SCANOUT 1024x768`。`vdb`/`vdc` 仍 sector 0 I/O error（见 ERR-20260907-001）。

### Metadata
- Source: error
- Related Files: CODE/UTSM/vmm/virtio_mmio.c, CODE/UTSM/vmm/virtio_blk.c
- Tags: virtio, gpu, shm
---

## [LRN-20260908-001] insight

**Logged**: 2026-09-08T23:30:00+08:00
**Priority**: high
**Status**: pending
**Area**: backend

### Summary
initramfs 必须在 `mount --move /dev` 之前挂 `/dev/vdc`；KDE 在 extra-rootfs 的 `/mnt/persist`。glibc `#GP` 在 `endbr64`，不是 drm fbdev 的 SET_SCANOUT。

### Details
`[ -b /dev/vdc ]` 在 `/dev` 被 move 走之后恒为假。kmsg 日志才能从串口里捞到 init 行。验证脚本不能把启动时 virtio-gpu fbdev `KDE scanout w=` 当成 Plasma。 musl busybox 无 SHSTK 会在 push 上对地址 0 写崩溃；Arch bash/glibc 有 IBT，崩在 `endbr64`。Host 侧不要 `wrmsr` CET MSR（#GP 掉 UTSM）。

### Suggested Action
给 Linux guest 做正确的 VMX CET 虚拟化，或换一套无 IBT 的用户态。在此之前 startplasma 起不来。

### Metadata
- Source: error
- Related Files: CODE/linux/initramfs/init, CODE/UTSM/vmm/vmexit.c, CODE/linux/guest/deshab-kde-session
- Tags: overlay, vdc, cet, ibt, glibc
---

## [LRN-20260910-001] insight

**Logged**: 2026-09-10T12:30:00+08:00
**Priority**: high
**Status**: pending
**Area**: backend

### Summary
Nested KVM 不虚拟化 CET VMCS；用户态 #GP 能进 L1 但 `guest_fetch` 读到 0，不能按字节 skip ENDBR。Host SHSTK 让 musl busybox `push` 写地址 0。

### Details
`insn0=0` 但 RIP 仍是 libc `...140`。盲 skip 4 字节会消掉 dmesg 里的 `#GP`，但 busybox.static 仍 `segfault at 0 ip=0x4bede0 error 6`。`IA32_VMX_TRUE_EXIT/ENTRY_CTLS` 的 CET save/load 位 nested 不允许。不要在 host 上 `wrmsr` CET。`exec_async` 的 stderr 是 `/dev/null`，session 日志要写 `/dev/kmsg`。

### Suggested Action
修好用户态 GVA walk（别读到 zeropage），再按真实 ENDBR/NOP 跳过 4 字节。在 CET 漏进 L2 的情况下 Plasma/glibc 起不来。

### Metadata
- Source: error
- Related Files: CODE/UTSM/vmm/vmexit.c, CODE/UTSM/vmm/linux_boot.c, CODE/linux/guest/deshab-kde-session
- Tags: cet, nested-kvm, guest-fetch, busybox
- See Also: LRN-20260908-001
---

## [LRN-20260910-002] insight

**Logged**: 2026-09-10T13:40:00+08:00
**Priority**: critical
**Status**: pending
**Area**: backend

### Summary
Nested KVM 让 L0 CET 作用于 L2 用户态，但 L1/L2 都无法读写 CET MSR。

### Details
L1 CR4.CET=0 且 CPUID 无 CET，WRMSR #GP。VMCS CET save/load 位 nested 不允许。VM-entry MSR-load U_CET=0 得到 exit 34。L2 `wrmsrl_safe(0x6a0)` 返回 -EIO。WSL `arch_prctl(ARCH_SHSTK_DISABLE)` EINVAL。busybox 在 `0x4bede0` 的 `push r13` 写地址 0（SHSTK）；glibc 在 `…140` #GP（IBT）。盲 skip 4 字节会把 libc RIP 跑飞，不要再用。不要 host 全局 `wrmsr IA32_U_CET`。

### Suggested Action
需要 L0 真正虚拟化 CET（或换一套会给 vCPU 加载 CET=0 的 KVM），否则无法启动 glibc Plasma。不要再盲 skip ENDBR。

### Metadata
- Source: error
- Related Files: CODE/UTSM/vmm/linux_boot.c, CODE/UTSM/vmm/vmexit.c, CODE/linux/patches/utsm_hcall.c, ISO/run_qemu_kvm.sh
- Tags: cet, nested-kvm, shstk, ibt, busybox
- See Also: LRN-20260910-001
---

## [LRN-20260910-003] insight

**Logged**: 2026-09-10T14:40:00+08:00
**Priority**: high
**Status**: pending
**Area**: backend

### Summary
park 之后拦截 #PF 会让 `utsm_exec_daemon` 在 `0x402661` 写 `0x4a36a0` 崩溃；nested 下 #PF 不能当普通异常 reinject。

### Details
16 字节对齐盲 skip 同样会弄坏 daemon。CR2 不是 VMCS 字段，resume 前 `mov cr2` 也不能修好。SHSTK 模拟因此没机会跑。不要在 Linux guest 的 exception bitmap 里开 #PF。

### Suggested Action
CET 仍只能靠 L0 虚拟化。保持 #GP/#CP 仅在真实 ENDBR/NOP 字节上 skip。

### Metadata
- Source: error
- Related Files: CODE/UTSM/vmm/linux_resume.c, CODE/UTSM/vmm/vmexit.c
- Tags: cet, page-fault, nested-kvm, daemon
- See Also: LRN-20260910-002
---

## [LRN-20260910-004] insight

**Logged**: 2026-09-10T16:30:00+08:00
**Priority**: high
**Status**: pending
**Area**: backend

### Summary
L1 用 VMCS/tracked CR3 走 libc GVA 会落到 hole 零页；CPU 仍在执行真实 libc。盲 skip 4 会把 RIP 拆到下一条再 #GP。

### Details
`KVM_GET/SET_MSRS` 对 CET 返回 nmsrs=0。`arch_prctl` EINVAL。不要把 `/bin/sh` 指到 musl busybox.static。用户态 #GP 的 `VMCS_INSTRUCTION_LENGTH` 是 2/3，不是 ENDBR 的 4。`ept_gpa_to_hpa` 对 walk GPA 有 HPA，但内容全 0。CR3 拦截有效，故障时 CR3=`0x2fa2000`。不要再对 fetch-fail 的 0x7f 16 字节对齐 RIP 盲 skip 4。

### Suggested Action
对第一次用户态 #GP 打完整页表 walk（PML4[255] 等）。用能看见真实 libc 字节的翻译再 skip ENDBR。不要 skip `ilen`（nested 报短了）。

### Metadata
- Source: error
- Related Files: CODE/UTSM/vmm/vmexit.c, CODE/linux/patch_boot_on_img.sh, CODE/linux/qemu_disable_cet.c
- Tags: cet, page-walk, cr3, endbr, busybox
- See Also: LRN-20260910-003
---

## [LRN-20260910-005] insight

**Logged**: 2026-09-10T16:45:00+08:00
**Priority**: critical
**Status**: pending
**Area**: backend

### Summary
Nested KVM 下 EPT leaf 未设 Ignore PAT 时，guest PAT=WC（virtio-gpu shmem）与 EPT WB 组合成保留类型，触发 EPT misconfig；VMM 若因此 `resume=0` 会直接杀掉 Linux vCPU。

### Details
串口：`desktop ready` 后 `EPT misconfig gpa=0x3e20000`，随后 `sh` 在 `clear_page_erms` 对非规范地址 `RDI=0x0005088000005000` #GP。KVM 对 RAM leaf 一律打 `VMX_EPT_IPAT_BIT`。不要把 2MB PD leaf 当成页表指针去写 4K PTE。不要从 `/mnt/d` loop-mount 4GiB extra-rootfs。

### Suggested Action
EPT leaf（含 2MB）写 WB+Ignore PAT；misconfig 时 repair leaf 再 resume。extra-rootfs 只在 WSL 本机盘上 mkfs/mount。

### Metadata
- Source: error
- Related Files: CODE/UTSM/vmm/ept.c, CODE/UTSM/vmm/vmexit.c, CODE/linux/patch_boot_on_img.sh
- Tags: ept, nested-kvm, pat, virtio-gpu, extra-rootfs
- Pattern-Key: harden.input_validation
---

## [LRN-20260910-006] insight

**Logged**: 2026-09-10T19:45:00+08:00
**Priority**: critical
**Status**: pending
**Area**: backend

### Summary
单 vCPU 上 daemon `sched_yield` 后立刻 PARK/HLT，会在 virtio 完成中断注入前冻住整机；`linux_resume()` 必须能在非 HLT 的 preemption timeslice 上回到 DSK。

### Details
async 子进程（KDE session）对 persist/ext4 的 `[ -x ]` 会进入 D-state。PID1 yield 立刻返回，64×1024 次 yield 远短于 10ms 抢占周期，随后 HLT，virtio IRQ 来不及注入。`idle=poll` 不能救：runnable 的 PID1 不会把时间让给 idle。HLT park 路径上调用 `vmexit_before_resume()`（virtio poll）会在 host 栈上 #PF（CR2=3）。timeslice 只允许在 `linux_resume()` 期间 armed，`linux_launch()` 不可提前归还。

### Suggested Action
async 时 daemon 禁止 park；`linux_compat` 循环 `linux_resume` 直到 EXEC_EXIT；HLT/timeslice 出口不要跑 guest virtio poll，改在下次 `linux_resume` 入口 `vmx_linux_prepare_entry()`。

### Metadata
- Source: error
- Related Files: CODE/linux/initramfs/bin/utsm_exec_daemon.c, CODE/UTSM/vmm/vmexit.c, CODE/UTSM/vmm/linux_compat.c, CODE/UTSM/vmm/linux_resume.c
- Tags: hlt, virtio, timeslice, kde, persist
- Pattern-Key: harden.input_validation
- See Also: LRN-20260910-007
---

## [LRN-20260910-007] correction

**Logged**: 2026-09-10T20:15:00+08:00
**Priority**: critical
**Status**: pending
**Area**: backend

### Summary
`linux_resume()` 从 park 跳回 host 时 CPU 上仍是 guest GPR。用栈上 `failed`（`%rbp` 寻址）会在 CR2=3 处 host #PF。必须像 `linux_launch` 一样写 RIP 相对全局变量。

### Details
`vmexit_asm` 先恢复 guest GPR 再 jmp `g_saved_return_rip`。vscode `test -e` 第一次 resume 后 RIP=`0xffffffff80006535`，RAX=3，Linux 直映寄存器仍在 R9。DSK 从未启动。改成 `g_linux_resume_failed(%rip)` 并 push/pop rbx/rbp/r12-r15 后：vscode skip、desktop ready、KDE session 打出 `checking persist mount`。

### Suggested Action
所有 vmlaunch/vmresume 着陆垫禁止依赖 guest 破坏后的 rbp 栈槽。

---

## [LRN-20260910-008] correction

**Logged**: 2026-09-10T20:22:00+08:00
**Priority**: high
**Status**: pending
**Area**: backend

### Summary
KDE 宿主循环里每帧 `exec("sleep 0.05")` / `test -f` 会占满唯一 vCPU：daemon `waitpid` 同步 exec，async session 停在 `checking persist mount`。应追加 ABI 兼容的 `run_slice`（只 `linux_resume`，无 IPC）。

### Details
verify: `interpreter alive` + `checking persist mount`，从未 `persist check done` / `entering KDE`。`kde_ok=2`（spawned）。guest TSC 不稳时 `sleep` 可能空转。`linux_compat_service.run_slice` 放在表尾。

### Suggested Action
desktop/DSK KDE 循环用 `run_slice`；原生切换探测降到每 32 次 slice 一次。不要再 `exec sleep`。

### Metadata
- Source: error
- Related Files: CODE/UTSM/include/utsm/linux_compat.h, CODE/desktop/lxc.c, CODE/dsk/main.c, CODE/linux/guest/deshab-kde-session
- Tags: kde, run_slice, persist, sleep
- Pattern-Key: harden.input_validation
- See Also: LRN-20260910-006
---

## [LRN-20260912-001] best_practice

**Logged**: 2026-09-12T12:45:00+08:00
**Priority**: critical
**Status**: pending
**Area**: infra

### Summary
不要把 linux-extra-rootfs.img 当 Limine boot module。4GiB 打进 RAM 会在 `log=21013` 把 6G WSL QEMU 掐死。

### Details
Windows Terminal 也会在 t=26s 回到提示符，不只是 Cursor 的 45s 管道问题。virtio-blk /dev/vdc 应映射 ESP 上的 FAT32 文件，经 AHCI 读写。

### Suggested Action
limine.conf 注释 extra-rootfs module；`virtio_extra_rootfs_blk_init` 走 ESP 簇映射。

### Metadata
- Source: error
- Related Files: CODE/UTSM/vmm/virtio_blk.c, SYSTEM/limine/limine.conf
- Tags: limine, extra-rootfs, wsl, qemu
- See Also: ERR-20260911-002
---

### Metadata
- Source: error
- Related Files: CODE/UTSM/vmm/linux_resume.c, CODE/UTSM/vmm/vmexit_asm.S
- Tags: linux_resume, gpr, rbp, idt, vscode
---

## [LRN-20260912-002] insight

**Logged**: 2026-09-12T14:45:00+08:00
**Priority**: high
**Status**: pending
**Area**: backend

### Summary
KDE extra-rootfs glibc 带 XSAVE/YMM/ZMM GNU property；L2 CPUID 隐藏 XSAVE 后 persist bash/Xorg 在 chroot 里立刻 127。不要 `objcopy --remove-section=.note.gnu.property`。

### Details
vdb e2fsck 干净；运行时 checksum 打在 grep/mkdir/mount。静态 `utsm_bindmount` 已能 bind /dev。persist ld.so 直接跑会 SHSTK 写地址 0；chroot persist bash=127；叠 vdb ld.so+libc=139。

### Suggested Action
给 L2 做 XSAVE 保存/恢复并放开 CPUID，或 extra 换无 AVX glibc / Xfbdev。不要再拆 GNU property section。

### Metadata
- Source: conversation
- Related Files: CODE/linux/guest/deshab-kde-session, CODE/UTSM/vmm/vmexit.c, CODE/linux/initramfs/bin/utsm_bindmount.c
- Tags: kde, xsave, persist, chroot
---



---

## [LRN-20260912-003] best_practice

**Logged**: 2026-09-12T21:55:00+08:00
**Priority**: high
**Status**: pending
**Area**: tooling / desktop

### Summary
`mcopy -o` 热补 ESP 内已存在的长名文件会删除 LFN 链（只剩 8.3 短名），fat32_io.h 的 LFN 查找随之 -3；热补长名文件必须 `mdel` + `mcopy` 重建条目。

### Details
M1 zhfont 集成：宿主 zhfont_test 全 PASS 但 QEMU 内 `f32_read_path_lfn_to` 对 simhei_16.dbf 返回 -3。Python 直读 FONT 目录簇发现 dbf 条目无 0x0F LFN 项（simhei.ttf 的完好）。根因是先前的 `mcopy -o` 热补剥离了 LFN。修复后 `[zhfont] dbf loaded (16px ok)`。另：Windows QEMU 验证 desktop 时 q35 内置 AHCI 用 `-device ide-hd,bus=ide.0` 挂盘会被 ahci.drv 识别为端口 2 ATAPI 签名（identified ports=0），须按 run_qemu_kvm.sh 的 `-device ahci,id=ahci0 ... bus=ahci0.0` 显式建控制器；2G 内存验证需临时注释 limine.conf 的 4.9GB Linux 模块行（sed 注释 → 验证 → 恢复）。

### Suggested Action
热补长名文件：`mdel` + `mcopy`，之后 `mdir` 确认行尾长名仍在。QEMU desktop 冒烟：显式 ahci0 + 2G + lite limine.conf。详细记录见 .learnings/hotpatch-mcopy-lfn-pitfall.md。

### Metadata
- Source: error
- Related Files: CODE/desktop/zhfont.c, CODE/desktop/f32io.c, ISO/run_qemu_kvm.sh, SYSTEM/limine/limine.conf
- Tags: mtools, lfn, hotpatch, qemu, ahci, zhfont
