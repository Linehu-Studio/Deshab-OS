# x86emu32 指令覆盖率测试

> 对应计划 Phase B3：枚举 x86emu32.c 已实现指令，验证覆盖率

## 状态: ⬜ PENDING

## 已实现指令清单

> 待根据 [x86emu32.c](../../CODE/UTSM/pe/x86emu32.c) 实际代码枚举填充

### 数据传送

| 指令 | 操作码 | 测试程序 | 预期 | 状态 |
|---|---|---|---|---|
| MOV r32, imm32 | B8+rd | `mov eax, 0x12345678` | eax=0x12345678 | ⬜ |
| MOV r32, r/m32 | 8B /r | `mov eax, ebx` | eax=ebx | ⬜ |
| MOV r/m32, r32 | 89 /r | `mov [esi], eax` | [esi]=eax | ⬜ |
| PUSH r32 | 50+rd | `push eax` | [esp]=eax, esp-=4 | ⬜ |
| POP r32 | 58+rd | `pop eax` | eax=[esp], esp+=4 | ⬜ |

### 算术运算

| 指令 | 操作码 | 测试程序 | 预期 | 状态 |
|---|---|---|---|---|
| ADD r32, r/m32 | 03 /r | `add eax, ebx` | eax=eax+ebx | ⬜ |
| SUB r32, r/m32 | 2B /r | `sub eax, ebx` | eax=eax-ebx | ⬜ |
| INC r32 | FF /0 | `inc eax` | eax=eax+1 | ⬜ |
| DEC r32 | FF /1 | `dec eax` | eax=eax-1 | ⬜ |
| MUL r/m32 | F7 /4 | `mul ebx` | edx:eax=eax*ebx | ⬜ |
| DIV r/m32 | F7 /6 | `div ebx` | eax=edx:eax/ebx | ⬜ |

### 逻辑运算

| 指令 | 操作码 | 测试程序 | 预期 | 状态 |
|---|---|---|---|---|
| AND r32, r/m32 | 23 /r | `and eax, ebx` | eax=eax&ebx | ⬜ |
| OR r32, r/m32 | 0B /r | `or eax, ebx` | eax=eax\|ebx | ⬜ |
| XOR r32, r/m32 | 33 /r | `xor eax, ebx` | eax=eax^ebx | ⬜ |
| NOT r/m32 | F7 /2 | `not eax` | eax=~eax | ⬜ |

### 控制流

| 指令 | 操作码 | 测试程序 | 预期 | 状态 |
|---|---|---|---|---|
| JMP rel32 | E9 cw/cd | `jmp label` | rip=label | ⬜ |
| JMP r/m32 | FF /4 | `jmp eax` | rip=eax | ⬜ |
| CALL rel32 | E8 cd | `call func` | push rip, rip=func | ⬜ |
| RET | C3 | `ret` | pop rip | ⬜ |
| Jcc rel32 | 0F 8x cd | `jz label` | 条件跳转 | ⬜ |

### 字符串操作

| 指令 | 操作码 | 测试程序 | 预期 | 状态 |
|---|---|---|---|---|
| MOVSB | A4 | `movsb` | [edi]=[esi], inc/dec | ⬜ |
| STOSB | AA | `stosb` | [edi]=al, inc/dec | ⬜ |
| REP MOVSB | F3 A4 | `rep movsb` | 重复 ecx 次 | ⬜ |
| REP STOSB | F3 AA | `rep stosb` | 重复 ecx 次 | ⬜ |

### 堆栈与控制

| 指令 | 操作码 | 测试程序 | 预期 | 状态 |
|---|---|---|---|---|
| ENTER imm16, imm8 | C8 iw ib | `enter 8, 0` | 建立栈帧 | ⬜ |
| LEAVE | C9 | `leave` | 释放栈帧 | ⬜ |
| NOP | 90 | `nop` | 无操作 | ⬜ |
| INT3 | CC | `int3` | 触发断点 | ⬜ |

## 测试 PE 程序

每个指令类别构造一个最小 PE32 测试程序，在 [CODE/tools/pe-samples/](../../CODE/tools/pe-samples/) 下：

- `test_arith.exe`：算术运算
- `test_logic.exe`：逻辑运算
- `test_control.exe`：控制流
- `test_string.exe`：字符串操作

每个测试程序通过 WriteFile 输出结果到 cmd.elf，与预期对比。

## 覆盖率统计

```
已测试指令数 / 已实现指令数 = 覆盖率
```

目标：核心指令（MOV/ADD/SUB/CMP/JMP/CALL/RET/PUSH/POP）覆盖率 100%。

## 自动化脚本

[tests/qemu/x86emu32-coverage.ps1](../../../tests/qemu/x86emu32-coverage.ps1) 两层验证：

```powershell
.\tests\qemu\x86emu32-coverage.ps1            # 复用现有镜像
.\tests\qemu\x86emu32-coverage.ps1 -Build     # 先全量构建再测
```

1. **静态枚举**（无需 QEMU）：解析 [x86emu32.c](../../CODE/UTSM/pe/x86emu32.c) 统计
   `case 0xNN` 操作码分支数（覆盖率分母，含 0F 扩展页），并校验 hello32.exe 头
   （machine=0x14C / optional magic=0x10B，确为 PE32 样本）。
2. **动态执行**：正常启动 → cmd.elf，AUTOEXEC.BAT 执行 `pe HELLO32.EXE` + `exit`。
   自动断言 `[PE] running PE32 via x86emu32` 解释器接管；禁止 `[PE32] emulator error` /
   `[PE] unimplemented import` / `[PE] unimplemented API called` / `[PANIC]`。
   注意 `[PE] PE32 (32-bit)` 仅 PE32+ 原生路径（pe_load_image）输出，解释器路径
   （pe32_load_into）不打印该行，不可作锚点。

   BUG-20260801-006/007 已修复（[Bug记录.txt](../Bug记录.txt)），原软升级项已转
   硬断言（回归守护）：`Hello from PE32!`（shim IAT 拦截输出）与
   `[cmd] PE run end` → `[DSK] cmd.elf returned`（完整返回链）。

   已知系统侧阻塞（2026-08-04 monitor 取证定案）：末锚点 `[DSK] cmd.elf returned`
   当前必缺失。根因不在 PE 层——cmd `run_autoexec` 的 `data`/`size` 直指共享静态
   缓冲 `f32_data`（fat32_io.h），首行 `pe HELLO32.EXE` 加载 PE 文件即覆盖剩余
   脚本，`exit` 行丢失，cmd 停留交互主循环轮询键盘（guest 存活、无异常投递、
   无三重故障）。反证：pe-compat 会话 C（`pe NONEXIST.EXE` + `exit`，文件未找到
   不覆盖缓冲）返回链正常。待 cmd 侧修复（脚本先拷入私有缓冲再逐行执行）后
   末锚点自动转绿。

指令级逐条覆盖（上方矩阵 test_arith/test_logic/test_control/test_string）依赖
CODE/tools/pe-samples/ 下尚未构建的分类测试 PE，标 MANUAL。
