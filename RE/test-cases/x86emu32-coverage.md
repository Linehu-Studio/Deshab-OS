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
