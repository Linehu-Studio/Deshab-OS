# Debug Session: pe-compat-p1

- Session ID: `pe-compat-p1`
- 创建: 2026-08-03
- 状态: [OPEN]
- 范围: BUG-20260801-005 / 006 / 007（PE 兼容层三条 P1）修复 + QEMU 端到端验证

## 前置证据状态

三条缺陷**根因已定案**（非猜测），证据链完整登记于 `docs/RE/Bug记录.txt`：

| Bug | 定案证据 | 根因一句话 |
|-----|---------|-----------|
| 006 | QEMU `-d int` v=0e e=0003 + utsm.elf 反汇编（`pe_shim_setup_exit` 尾声从脏栈 pop 7 槽）+ R12/R13= log_hex64 缓冲区 ASCII 残片 | `pe_shim_setup_exit()` 内 `__builtin_setjmp` 后函数即返回，longjmp 跨已返回帧 = C 标准 UB |
| 007 | `-d int` v=0e RIP 落在 shim_printf 体内（反汇编确认）；串口止于 "[PE] running PE32 via x86emu32" | shim 表 fn32 列全 NULL，fn64 回退把 32 位 guest VA（0x401017）当 host 指针解引用 |
| 005 | `-d int`: `old:0xe new:0xd`（投递 #PF 即 #GP）→ `old:0x8 new:0xd` → Triple | `idt_halt_all()` 静默 halt 门在 DSK 阶段异常无法投递，零诊断 |

本会话不再重复根因调查，直接进入**修复实施 → 修复后对比验证**阶段。
可证伪假设（针对修复设计的验证点，非根因猜测）：

- **H1 (006)**: `__builtin_setjmp` 下沉到 `pe_service_run` 本体（PE 执行全程存活的帧）后，longjmp 恢复的 RSP/RBP/R12-15 来自存活帧，HELLO64 能干净走完 `[PE] ExitProcess code=` → `[cmd] PE run end`。
  证伪条件: 若仍三重故障 → pe_call_on_stack 切栈与 longjmp 存在第二处交互问题。
- **H2 (007)**: `g_emu_mem_base` + 各 shim 解引用点 `gp()` 转换（guest VA < 0x10000000 时加 emu->mem 基址）可让 hello32 `printf(0x401017)` 输出 "Hello from PE32!"。
  证伪条件: 输出乱码=gp 遗漏解引用点；仍 #PF=base 设置时机/阈值有误。
- **H3 (005)**: DSK 交接以诊断 IDT（0-31→isr_stub 走 idt_handler 打现场；32-255→halt stub 保持 IRQ 防御）替换全静默 halt 后，故意 #PF 串口可见 `[IDT] exception vector=0xe cr2=...`。
  证伪条件: 仍三重故障 → DSK 阶段投递机制存在更深层问题（GDT/TSS），需 `-d int` 再定位。
  旁证: BUG-20260729-013 登记行为表明 halt_all 引入**之前** DSK 阶段异常能送达 idt_handler（isr_stub 门投递在 DSK 阶段是已验证可用路径）。
- **H4 (回归)**: `gp()` 在 `g_emu_mem_base=0`（原生路径）为恒等映射，HELLO64/Deaicup/cmd.exe 行为不变。
  证伪条件: Deaicup 首帧 BitBlt 消失或输出异常。

## 修复方案（与 Bug 记录方向一致）

1. **006**: pe_shim.c 删除 `pe_shim_setup_exit()` 包装；新增 `pe_shim_exit_jmpbuf()` 返回 jmpbuf 指针；pe_service.c 两条路径（PE32+/PE32）在 `pe_service_run` 本体内直接 `__builtin_setjmp(pe_shim_exit_jmpbuf())`。
2. **007**: pe_shim.c 新增 `g_emu_mem_base` + `pe_shim_set_emu_base()` + `gp()` 内联；x86emu32.c `try_iat_call` 调 shim 前设 base；各 shim 在**指针解引用点**（非参数入口盲目转换——避免把退出码/长度等小整数误转）套 gp()。user32/gdi32 结构体布局 32/64 位不同，PE32 GUI 明确不在本轮范围。
3. **005**: idt.c 新增 `idt_install_dsk_diag()`（0-31 diag + 32-255 halt + lidt），`idt_install_pe_diag` 改为其别名；dsk_loader.c 交接处以 `idt_install_dsk_diag()` 替换 `idt_halt_all()`；gen_pe.py 新增 `fault64.exe`（`mov [0x10],imm` 故意 #PF）作验收样本，pe-compat.ps1 加会话 D 断言 `[IDT] exception`。

## 修复日志

（实施中填写）

## 验证记录

（QEMU 实测后填写 pre/post 对比）

## 清理清单

- [ ] debug 文件收尾（用户确认后归档状态）
- [ ] Bug记录.txt 三条状态 OPEN→FIXED，填修复提交与验证结果
