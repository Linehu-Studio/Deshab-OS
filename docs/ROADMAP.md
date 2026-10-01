# Deshab-OS 开发路线图（Phase 0–9 历史存档）

> 本文档为**历史存档**：Phase 0–9 内核主线的任务清单与完成记录。
> 当前活跃路线见 [ROADMAP_INSANE.md](./ROADMAP_INSANE.md)（五期填补计划，唯一任务编号 F/U/D/R/E）；
> 子系统状态唯一来源见 [STATUS.md](./STATUS.md)。
> ✅已完成 · 🔶部分完成 · ⬜待实现

---

## 当前状态总览

```text
Limine → utsm.elf → DKM 14驱动 → DSK(deshab.elf)
  → firstInit=0: mouseInit→netman→FirstInit→user.conf写盘→desktop
  → firstInit≠0: login→desktop
```

| 子系统 | 状态 | 说明 |
|--------|------|------|
| 早期启动 | ✅ | IDT 0–47，PIC remap 0x20–0x2f |
| UTSM封缄内存 | 🔶 | selftest闭环，占位XOR stream |
| DKM驱动加载 | ✅ | 14/14闭环 |
| DMA分配器 | ✅ | 物理页bitmap，低4G钳位 |
| AHCI块设备 | ✅ | IDENTIFY/READ/WRITE DMA+ahci0 provider |
| NVMe | 🔶 | PCI discovery only，BAR0>4G待映射 |
| FAT32 | 🔶 | DKM只读；DSK读写(根目录) |
| e1000 | ✅ | RX/TX+DHCP/ARP/UDP/DNS，polling |
| 字体系统 | ✅ | DBF 16/24/32px，GB2312全覆盖 |
| FirstInit | 🔶 | UI+输入+SHA256密码+写盘；B6按键崩溃QEMU未复现；可跳过(skip_firstinit) ✅ |
| 正常启动路径 | ✅ | login→desktop |
| SAS-R0-PCQ | ✅ | O(1)位图调度+LAPIC tick+xv6式切换，selftest闭环 |
| DRR | 🔶 | 看门狗/快照/回滚闭环；页MAC已升级BLAKE2b keyed MAC；segment/system级回滚待验证 |

---

## Phase 0 — 内存与MMIO底座

| 任务 | 状态 | 验收标准 |
|------|------|---------|
| 物理页bitmap分配器 | ✅ | 已根治e1000 DMA |
| 页表映射接口(map/unmap) | ⬜ | mmio_map(phys,size,flags)通用接口 |
| 高位PCI MMIO映射(4G以上BAR) | ⬜ | NVMe BAR0可安全读register |
| 替换arena为真实页分配 | ⬜ | 移除16MB arena上限 |

风险：页表操作需关中断；高位MMIO需独立页表项避免破坏HHDM。

---

## Phase 1 — DMA与块设备数据路径

| 任务 | 状态 | 验收标准 |
|------|------|---------|
| DMA buffer统一接口 | 🔶 | dkm_dma_api已定义，cache屏障待完善 |
| AHCI IDENTIFY/READ | ✅ | SATA盘可读LBA0 |
| AHCI WRITE DMA | ✅ | WRITE DMA EXT(0x35)已注册 |
| AHCI批量read稳定化 | 🔶 | 256扇区成功，单扇区偶发超时 |
| AHCI block provider | ✅ | ahci0已注册 |
| NVMe admin queue/identify | ⬜ | 依赖Phase 0高位MMIO |
| NVMe block provider | ⬜ | nvme0，支持READ |

---

## Phase 2 — IRQ后端升级

| 任务 | 状态 | 验收标准 |
|------|------|---------|
| IDT扩展256 vectors | 🔶 | 当前0–47 |
| Vector allocator | ⬜ | 动态分配中断向量 |
| APIC EOI后端 | ⬜ | 替代PIC EOI |
| IOAPIC redirection | ⬜ | 迁移设备IRQ到IOAPIC |
| LAPIC timer | ⬜ | 替代PIT |
| MSI/MSI-X | ⬜ | NVMe/e1000使用MSI-X |

风险：未扩展IDT前不能禁用PIC或重编IOAPIC RTE。

---

## Phase 3 — 网络数据路径

| 任务 | 状态 | 验收标准 |
|------|------|---------|
| e1000 RX/TX ring | ✅ | LINK_UP+TX_READY+RX_READY |
| e1000中断驱动收发 | ⬜ | 替代polling(依赖Phase 2) |
| virtio-net feature negotiation | ⬜ | 完成 |
| virtio-net virtqueue RX/TX | ⬜ | 建队列收发包 |
| 最小协议栈 | ✅ | DHCP+ARP+DNS(netman闭环) |

---

## Phase 4 — VFS与真实块设备接入

| 任务 | 状态 | 验收标准 |
|------|------|---------|
| FAT32接入真实block provider | 🔶 | 优先block provider回退boot module |
| FAT32子目录路径解析 | ⬜ | 支持完整路径 |
| VFS统一路径语义 | ⬜ | DSK与VFS路径统一 |
| 目录遍历+错误路径完善 | ⬜ | 枚举目录项+正确回退 |

---

## Phase 5 — block write与FAT32 write

| 任务 | 状态 | 验收标准 |
|------|------|---------|
| dkm_block_api.write | ✅ | AHCI WRITE DMA EXT已注册(@+0x18) |
| FAT32文件创建/FAT链更新/覆写 | ✅ | user.conf/firstInit.txt已写盘 |
| firstInit.txt标志写盘 | ✅ | dsk_persist_userconf写1\n+dev_mode |

注：写路径当前由DSK内嵌FAT32实现，DKM fat32驱动仍只读。

---

## Phase 6 — FirstInit完善

| 任务 | 状态 | 验收标准 |
|------|------|---------|
| 按键崩溃修复(B6) | ✅ | QEMU复现测试未复现，全流程PASS |
| 鼠标光标移动 | ✅ | PS/2 IRQ12+包解码+光标（M1完成） |
| user.conf写盘 | ✅ | 重启后login可读取 |
| 网络配置界面+时区/语言 | ⬜ | — |
| 设置完成后跳转+正常启动路径 | ✅ | FirstInit返回→持久化→desktop |

---

## Phase 7 — SAS-R0-PCQ调度器

| 任务 | 状态 | 验收标准 |
|------|------|---------|
| TCB+task_create | 🔶 | 64槽TCB+entry/arg/stack；process_uuid/cap table待接 |
| Per-CPU runqueue | 🔶 | 结构就位（8 CPU），当前BSP-only |
| O(1)位图调度器 | ✅ | ready_bitmap ctz选 prio0最高，selftest PASS |
| context_switch | ✅ | xv6式栈切换(switch.S)，只触碰crypto指针 |
| task_kill crypto erase | ⬜ | key_epoch++, state=DESTROYED |

tick源=LAPIC timer宿主自持(arch/x86_64/lapic_timer.c)；demo窗口含tick失速兜底；
selftest含runqueue O(1)、block/wake、看门狗负向测试（demo_fault=1检出+回滚）。

风险：切换路径禁止扫描capability/计算MAC/重加密。

---

## Phase 8 — DRR恢复系统

| 任务 | 状态 | 验收标准 |
|------|------|---------|
| Emergency Pool+Watchdog+Recovery log | 🔶 | 看门狗register/kick/timeout闭环+负向测试PASS；独立调度器待做 |
| Dirty Shard+Bitmap | ✅ | snapshot_dirty按shard位图收集dirty页 |
| A/B Checkpoint双槽 | ✅ | ckpt_a/ckpt_b+CRC64+active slot切换 |
| Page/Segment/System Rollback | 🔶 | page级回滚+verify PASS；segment/system级待真实加密后验证 |
| 驱动recovery ops | ⬜ | quiesce/reset/reinit |

注：快照区经 dma_alloc_pages 分配，页快照容量编译期 129 页；
页 MAC 已为 BLAKE2b keyed MAC + mac_root 链（Phase 9 早期成果）。
风险：恢复路径禁用普通堆；Emergency Pool耗尽→system rollback。

---

## Phase 9 — 真实加密落地

| 任务 | 状态 | 验收标准 |
|------|------|---------|
| 加密原语选型 | ⬜ | HKDF/BLAKE2/AES-CTR/Poly1305 |
| KDF+Stream cipher(64B line)+MAC(4KB page) | ⬜ | 占位XOR替换 |
| DRR root key管理+PCKC key schedule | ⬜ | 真实key schedule |

风险：加密原语需在-mno-sse下可用（或此阶段启用SSE/SIMD）。

---

## 用户态与生态主线

| 子系统 | 状态 | 说明 |
|--------|------|------|
| desktop.elf | ✅ | 三页面+窗口+任务栏+双命令行+6应用+双缓冲 |
| shell.elf | ✅ | linux/cp/mv/echo/ls/cat/rm+/bin+lls/lcat+Tab补全 |
| cmd.elf | ✅ | Windows风格+pe/peinfo |
| login.elf | ✅ | USER.CONF+skip_login |
| PE/EXE兼容层 | ✅ | PE32+原生/PE32走x86emu32 |
| Linux兼容层 | 🔶 | VMM+park-and-resume+IPC exec+文件传输；**VMX运行时验证待VT-x/KVM** |
| virtio-mmio模拟 | 🔶 | blk/net/rootfs后端；未在真实guest验证 |
| Pacman | ⬜ | 五阶段方案；Arch rootfs已含pacman |

主线依赖：VMX环境→Arch guest pacman→文件传输/命令委托→desktop Linux图标集成。

---

## DKM工程化收尾

| 任务 | 状态 |
|------|------|
| 统一驱动ABI公共头 | ⬜ |
| kernel_api typed sub-struct迁移 | ⬜ |
| 零warning | ⬜ |
| QEMU场景自动化 | ⬜ |
| log API printf变参 | ⬜ |

---

## 里程碑

| 里程碑 | 阶段 | 标志 |
|--------|------|------|
| M1 真实块设备启动 | Phase 0+1+4 | 从AHCI/NVMe FAT32读deshab.elf |
| M2 配置持久化 | Phase 5+6 | FirstInit设置可写盘，重启后正常启动 |
| M3 网络可用 | Phase 2+3 | DHCP+UDP收发 |
| M4 多任务调度 | Phase 7 | SAS-R0-PCQ多任务 |
| M5 容错恢复 | Phase 8+9 | DRR checkpoint/rollback+真实加密 |

---

## 相关文档

- [ARCHITECTURE.md](./ARCHITECTURE.md)
- [BOOT_SEQUENCE.md](./BOOT_SEQUENCE.md)
- [兼容层设计.md](./兼容层设计.md)
- [桌面设计.md](./桌面设计.md)
- [ROADMAP_INSANE.md](./ROADMAP_INSANE.md)（当前活跃路线）
- [CLAUDE.md](../CLAUDE.md)
