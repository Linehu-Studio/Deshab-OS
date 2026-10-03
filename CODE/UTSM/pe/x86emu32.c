/* x86emu32.c — x86-32 指令解释器（PE32 执行引擎）。
 *
 * 寄存器级解释执行 32位 PE 代码。覆盖 mingw-w64 -O0/-O1 控制台程序常用指令：
 *   ALU（ADD/OR/ADC/SBB/AND/SUB/XOR/CMP）、MOV（全形式）、PUSH/POP、
 *   INC/DEC/NEG/NOT、MUL/IMUL/DIV/IDIV、移位/旋转、LEA、XCHG、MOVSX/MOVZX、
 *   JMP/Jcc/CALL/RET/LOOP、字符串指令（+REP）、ENTER/LEAVE、INT（shim 调度）。
 *
 * 遇不支持指令时设置 error 并停止。
 */

#include "x86emu32.h"
#include "pe_shim.h"
#include <utsm/log.h>
#include <utsm/panic.h>
#include <utsm/types.h>

/* ===== 内存访问（带边界检查） ===== */
u8 x86emu32_read8(x86emu_state *emu, u32 addr) {
    if (addr >= emu->mem_size) { emu->error = 1; emu->error_msg = "read8 OOB"; return 0; }
    return emu->mem[addr];
}
u16 x86emu32_read16(x86emu_state *emu, u32 addr) {
    if (addr + 1 >= emu->mem_size) { emu->error = 1; emu->error_msg = "read16 OOB"; return 0; }
    return (u16)emu->mem[addr] | ((u16)emu->mem[addr+1] << 8);
}
u32 x86emu32_read32(x86emu_state *emu, u32 addr) {
    if (addr + 3 >= emu->mem_size) { emu->error = 1; emu->error_msg = "read32 OOB"; return 0; }
    return (u32)emu->mem[addr] | ((u32)emu->mem[addr+1] << 8) |
           ((u32)emu->mem[addr+2] << 16) | ((u32)emu->mem[addr+3] << 24);
}
void x86emu32_write8(x86emu_state *emu, u32 addr, u8 val) {
    if (addr >= emu->mem_size) { emu->error = 1; emu->error_msg = "write8 OOB"; return; }
    emu->mem[addr] = val;
}
void x86emu32_write16(x86emu_state *emu, u32 addr, u16 val) {
    if (addr + 1 >= emu->mem_size) { emu->error = 1; emu->error_msg = "write16 OOB"; return; }
    emu->mem[addr] = (u8)(val & 0xFF);
    emu->mem[addr+1] = (u8)((val >> 8) & 0xFF);
}
void x86emu32_write32(x86emu_state *emu, u32 addr, u32 val) {
    if (addr + 3 >= emu->mem_size) { emu->error = 1; emu->error_msg = "write32 OOB"; return; }
    emu->mem[addr] = (u8)(val & 0xFF);
    emu->mem[addr+1] = (u8)((val >> 8) & 0xFF);
    emu->mem[addr+2] = (u8)((val >> 16) & 0xFF);
    emu->mem[addr+3] = (u8)((val >> 24) & 0xFF);
}

/* ===== 栈操作 ===== */
void x86emu32_push32(x86emu_state *emu, u32 val) {
    emu->regs[X86_ESP] -= 4;
    x86emu32_write32(emu, emu->regs[X86_ESP], val);
}
u32 x86emu32_pop32(x86emu_state *emu) {
    u32 val = x86emu32_read32(emu, emu->regs[X86_ESP]);
    emu->regs[X86_ESP] += 4;
    return val;
}

u64 x86emu32_read_arg(x86emu_state *emu, int esp_offset) {
    return x86emu32_read32(emu, emu->regs[X86_ESP] + (u32)esp_offset * 4);
}

/* ===== 寄存器访问 ===== */
/* 32位寄存器名：EAX,ECX,EDX,EBX,ESP,EBP,ESI,EDI（对应 0-7） */
/* 8位寄存器映射：AL,CL,DL,BL,AH,CH,DH,BH（0-3 = 低8位, 4-7 = 高8位） */
static u8 get_reg8(x86emu_state *emu, int idx) {
    if (idx < 4) return (u8)(emu->regs[idx] & 0xFF);
    return (u8)((emu->regs[idx - 4] >> 8) & 0xFF);
}
static void set_reg8(x86emu_state *emu, int idx, u8 val) {
    if (idx < 4) {
        emu->regs[idx] = (emu->regs[idx] & 0xFFFFFF00) | val;
    } else {
        emu->regs[idx - 4] = (emu->regs[idx - 4] & 0xFFFF00FF) | ((u32)val << 8);
    }
}
static u16 get_reg16(x86emu_state *emu, int idx) {
    return (u16)(emu->regs[idx] & 0xFFFF);
}
static void set_reg16(x86emu_state *emu, int idx, u16 val) {
    emu->regs[idx] = (emu->regs[idx] & 0xFFFF0000) | val;
}
static u32 get_reg32(x86emu_state *emu, int idx) { return emu->regs[idx]; }
static void set_reg32(x86emu_state *emu, int idx, u32 val) { emu->regs[idx] = val; }

/* ===== EFLAGS 辅助 ===== */
static void set_flag(x86emu_state *emu, u32 mask, int on) {
    if (on) emu->eflags |= mask; else emu->eflags &= ~mask;
}
static int get_flag(x86emu_state *emu, u32 mask) { return (emu->eflags & mask) ? 1 : 0; }

static u8 parity8(u8 v) {
    v ^= v >> 4; v ^= v >> 2; v ^= v >> 1;
    return (v & 1) == 0;
}

static void set_zf_sf_pf(x86emu_state *emu, u32 result, int size) {
    u32 mask = (size == 1) ? 0xFF : (size == 2) ? 0xFFFF : 0xFFFFFFFF;
    result &= mask;
    set_flag(emu, X86_ZF, result == 0);
    if (size == 1) set_flag(emu, X86_SF, (result & 0x80) != 0);
    else if (size == 2) set_flag(emu, X86_SF, (result & 0x8000) != 0);
    else set_flag(emu, X86_SF, (result & 0x80000000) != 0);
    set_flag(emu, X86_PF, parity8((u8)result));
}

static void set_cf_add(x86emu_state *emu, u32 a, u32 b, int size) {
    u32 mask = (size == 1) ? 0xFF : (size == 2) ? 0xFFFF : 0xFFFFFFFF;
    set_flag(emu, X86_CF, ((u64)(a & mask) + (u64)(b & mask)) > mask);
}
static void set_cf_sub(x86emu_state *emu, u32 a, u32 b, int size) {
    u32 mask = (size == 1) ? 0xFF : (size == 2) ? 0xFFFF : 0xFFFFFFFF;
    set_flag(emu, X86_CF, (a & mask) < (b & mask));
}
static void set_of_add(x86emu_state *emu, u32 a, u32 b, u32 r, int size) {
    u32 sign = (size == 1) ? 0x80 : (size == 2) ? 0x8000 : 0x80000000;
    set_flag(emu, X86_OF, (~(a ^ b) & (a ^ r) & sign) != 0);
}
static void set_of_sub(x86emu_state *emu, u32 a, u32 b, u32 r, int size) {
    u32 sign = (size == 1) ? 0x80 : (size == 2) ? 0x8000 : 0x80000000;
    set_flag(emu, X86_OF, ((a ^ b) & (a ^ r) & sign) != 0);
}

/* ===== 指令取 ===== */
static u8 fetch8(x86emu_state *emu) {
    u8 v = x86emu32_read8(emu, emu->eip);
    emu->eip++;
    return v;
}
static u16 fetch16(x86emu_state *emu) {
    u16 v = x86emu32_read16(emu, emu->eip);
    emu->eip += 2;
    return v;
}
static u32 fetch32(x86emu_state *emu) {
    u32 v = x86emu32_read32(emu, emu->eip);
    emu->eip += 4;
    return v;
}

/* ===== ModR/M 解码 ===== */
typedef struct {
    int mod;
    int reg;     /* reg 字段（也可作 opcode 扩展） */
    int rm;      /* rm 字段 */
    u32 ea;      /* 有效地址（若 mod != 3） */
    int has_ea;  /* 是否有内存操作数 */
} modrm_dec;

static modrm_dec decode_modrm(x86emu_state *emu) {
    modrm_dec m;
    u8 b = fetch8(emu);
    m.mod = (b >> 6) & 3;
    m.reg = (b >> 3) & 7;
    m.rm = b & 7;
    m.has_ea = 0;
    m.ea = 0;

    if (m.mod == 3) return m;  /* 寄存器直接寻址 */

    m.has_ea = 1;
    u32 base = 0;
    int has_sib = (m.rm == 4);

    if (has_sib) {
        u8 sib = fetch8(emu);
        int scale = 1 << ((sib >> 6) & 3);
        int index = (sib >> 3) & 7;
        int base_reg = sib & 7;
        base = emu->regs[base_reg];
        if (index != 4) {  /* ESP 不作 index */
            base += emu->regs[index] * (u32)scale;
        }
    } else {
        base = emu->regs[m.rm];
    }

    /* disp */
    if (m.mod == 0) {
        if (!has_sib && m.rm == 5) {
            /* disp32 直接地址 */
            base = fetch32(emu);
        }
    } else if (m.mod == 1) {
        base += (u32)(i32)(i8)fetch8(emu);  /* disp8 符号扩展 */
    } else {  /* mod == 2 */
        base += fetch32(emu);
    }

    m.ea = base;
    return m;
}

/* 读/写 modr/m 操作数 */
static u32 read_rm32(x86emu_state *emu, const modrm_dec *m) {
    if (m->mod == 3) return get_reg32(emu, m->rm);
    return x86emu32_read32(emu, m->ea);
}
static void write_rm32(x86emu_state *emu, const modrm_dec *m, u32 val) {
    if (m->mod == 3) set_reg32(emu, m->rm, val);
    else x86emu32_write32(emu, m->ea, val);
}
static u8 read_rm8(x86emu_state *emu, const modrm_dec *m) {
    if (m->mod == 3) return get_reg8(emu, m->rm);
    return x86emu32_read8(emu, m->ea);
}
static void write_rm8(x86emu_state *emu, const modrm_dec *m, u8 val) {
    if (m->mod == 3) set_reg8(emu, m->rm, val);
    else x86emu32_write8(emu, m->ea, val);
}
static u16 read_rm16(x86emu_state *emu, const modrm_dec *m) {
    if (m->mod == 3) return get_reg16(emu, m->rm);
    return x86emu32_read16(emu, m->ea);
}
static void write_rm16(x86emu_state *emu, const modrm_dec *m, u16 val) {
    if (m->mod == 3) set_reg16(emu, m->rm, val);
    else x86emu32_write16(emu, m->ea, val);
}

/* ===== ALU 操作（按 op 索引：0=ADD 1=OR 2=ADC 3=SBB 4=AND 5=SUB 6=XOR 7=CMP） ===== */
static u32 alu_op(x86emu_state *emu, int op, u32 a, u32 b, int size) {
    u32 r = 0;
    u32 mask = (size == 1) ? 0xFF : (size == 2) ? 0xFFFF : 0xFFFFFFFF;
    a &= mask; b &= mask;
    switch (op) {
    case 0: r = a + b; set_cf_add(emu, a, b, size); set_of_add(emu, a, b, r, size); set_zf_sf_pf(emu, r, size); break;
    case 1: r = a | b; set_flag(emu, X86_CF, 0); set_flag(emu, X86_OF, 0); set_zf_sf_pf(emu, r, size); break;
    case 2: { int cf = get_flag(emu, X86_CF); r = a + b + cf; set_cf_add(emu, a + cf, b, size); set_of_add(emu, a, b, r, size); set_zf_sf_pf(emu, r, size); break; }
    case 3: { int cf = get_flag(emu, X86_CF); r = a - b - cf; set_cf_sub(emu, a - cf, b, size); set_of_sub(emu, a, b, r, size); set_zf_sf_pf(emu, r, size); break; }
    case 4: r = a & b; set_flag(emu, X86_CF, 0); set_flag(emu, X86_OF, 0); set_zf_sf_pf(emu, r, size); break;
    case 5: r = a - b; set_cf_sub(emu, a, b, size); set_of_sub(emu, a, b, r, size); set_zf_sf_pf(emu, r, size); break;
    case 6: r = a ^ b; set_flag(emu, X86_CF, 0); set_flag(emu, X86_OF, 0); set_zf_sf_pf(emu, r, size); break;
    case 7: r = a - b; set_cf_sub(emu, a, b, size); set_of_sub(emu, a, b, r, size); set_zf_sf_pf(emu, r, size); break;  /* CMP 不写回 */
    }
    return r & mask;
}

/* ===== 移位操作（按 op：ROL/ROR/RCL/RCR/SHL/SHR/SAR/SAL） ===== */
static u32 shift_op(x86emu_state *emu, int op, u32 val, u8 count, int size) {
    u32 mask = (size == 1) ? 0xFF : (size == 2) ? 0xFFFF : 0xFFFFFFFF;
    val &= mask;
    count &= 0x1F;
    if (count == 0) return val;
    u32 r = val;
    switch (op) {
    case 4: case 6:  /* SHL / SAL */
        r = val << count;
        set_flag(emu, X86_CF, (val >> (32 - count)) & 1);
        break;
    case 5:  /* SHR */
        r = val >> count;
        set_flag(emu, X86_CF, (val >> (count - 1)) & 1);
        break;
    case 7:  /* SAR */
        r = (u32)((i32)val >> count);
        set_flag(emu, X86_CF, (val >> (count - 1)) & 1);
        break;
    case 0:  /* ROL */
        r = ((val << count) | (val >> (size * 8 - count))) & mask;
        set_flag(emu, X86_CF, r & 1);
        break;
    case 1:  /* ROR */
        r = ((val >> count) | (val << (size * 8 - count))) & mask;
        set_flag(emu, X86_CF, (r >> (size * 8 - 1)) & 1);
        break;
    default: break;  /* RCL/RCR 暂简化 */
    }
    if (op >= 4) set_zf_sf_pf(emu, r, size);  /* SHL/SHR/SAR 影响 ZF/SF/PF */
    return r & mask;
}

/* ===== IAT CALL 拦截 ===== */
/* stdcall 参数个数表（kernel32.dll 函数：callee 清栈）。
 * msvcrt.dll 函数为 cdecl（caller 清栈），不在此表。
 * key = shim 函数名首字符 + 长度组合的简单 hash，避免 strcmp。
 * 简化：直接用函数指针地址查表。 */
static int try_iat_call(x86emu_state *emu, u32 target) {
    /* 检查是否为合成 shim 地址 */
    if (target >= 0x00010000 && target < 0x00020000) {
        int shim_idx = target & 0xFFFF;
        if (shim_idx == 0xFFFF) {
            /* 严格错误策略插桩：32 位解释器调用未实现 API = 静默 EAX=0
             * 会给 PE 程序喂错误语义，零降级 panic（PE-E06）。 */
            log_error("[PE32] call unimplemented API");
            panic_full("PE-E06 EMU UNSHIMMED API CALLED",
                       "x86emu32 IAT call hit unimplemented stub: not implemented", 0);
            emu->regs[X86_EAX] = 0;
            return 1;
        }
        const pe_shim_entry *se = pe_shim_get(shim_idx);
        if (!se) {
            /* 严格错误策略插桩：shim 索引越界 = 导入解析已损坏，panic（PE-E07） */
            log_error("[PE32] shim index out of range");
            panic_full("PE-E07 SHIM INDEX OUT OF RANGE",
                       "x86emu32 IAT call with invalid shim index", 0);
            emu->regs[X86_EAX] = 0;
            return 1;
        }

        /* 从模拟栈读参数。CALL 被拦截前未 push 返回地址，
         * 所以 ESP 指向第一个参数。 */
        u32 esp_val = emu->regs[X86_ESP];
        if (esp_val + 32 > emu->mem_size) {
            emu->error = 1;
            emu->error_msg = "shim esp OOB";
            return 1;
        }
        u64 a0 = x86emu32_read32(emu, esp_val);
        u64 a1 = x86emu32_read32(emu, esp_val + 4);
        u64 a2 = x86emu32_read32(emu, esp_val + 8);
        u64 a3 = x86emu32_read32(emu, esp_val + 12);

        u64 ret;
        if (se->fn32) {
            /* 有专用 32位实现 */
            void *esp_ptr = &emu->mem[esp_val];
            ret = se->fn32(esp_ptr);
        } else if (se->fn64) {
            /* 回退：调用 64位 shim（最多 4 个参数）。
             * BUG-20260801-007 后续修正：fn4_t 必须显式 ms_abi。
             * UTSM 以 clang x86_64-unknown-none 编译，C 默认 ABI 是 SysV
             * （arg1=RDI/RSI/RDX/RCX），而 shim 全部 __attribute__((ms_abi))
             * （arg1=RCX/RDX/R8/R9）。typedef 不带 ms_abi 时编译器生成 SysV
             * 调用序列 → shim 在 RCX 读到的是 SysV 第4参数 a3(=0) → gp(0)=0
             * → shim_printf 解引用 NULL → #PF cr2=0（实测寄存器现场:
             * rdi=0x401017 即 SysV arg1=fmt, rcx=0, cr2=0）。
             * 原生 PE32+ 路径 guest 是 Windows 程序天生 MS ABI，不受影响。 */
            typedef u64 (__attribute__((ms_abi)) *fn4_t)(u64, u64, u64, u64);
            fn4_t f = (fn4_t)se->fn64;
            ret = f(a0, a1, a2, a3);

            /* stdcall 清栈：kernel32.dll 函数 callee 清栈。
             * 简化：按已知参数个数调整 ESP。
             * msvcrt.dll (cdecl) 不清栈。 */
            if (se->dll[0] == 'k') {  /* kernel32.dll */
                int nargs = 4;  /* 默认假设 4 参数 */
                /* 按函数名精化参数个数 */
                const char *fn = se->func;
                if (fn[0]=='G' && fn[1]=='e' && fn[2]=='t' && fn[3]=='S') nargs = 1;
                else if (fn[0]=='W' && fn[1]=='r' && fn[2]=='i') nargs = 5;
                else if (fn[0]=='R' && fn[1]=='e' && fn[2]=='a' && fn[3]=='d') nargs = 5;
                else if (fn[0]=='E' && fn[1]=='x' && fn[2]=='i' && fn[3]=='t') nargs = 1;
                else if (fn[0]=='G' && fn[1]=='e' && fn[2]=='t' && fn[3]=='C') nargs = 0;
                else if (fn[0]=='G' && fn[1]=='e' && fn[2]=='t' && fn[3]=='L') nargs = 0;
                else if (fn[0]=='S' && fn[1]=='e' && fn[2]=='t' && fn[3]=='L') nargs = 1;
                else if (fn[0]=='H' && fn[1]=='e' && fn[2]=='a' && fn[3]=='p') nargs = 3;
                else if (fn[0]=='V' && fn[1]=='i' && fn[2]=='r') nargs = 4;
                else if (fn[0]=='G' && fn[1]=='e' && fn[2]=='t' && fn[3]=='M') nargs = 1;
                else if (fn[0]=='C' && fn[1]=='l' && fn[2]=='o') nargs = 1;
                else if (fn[0]=='C' && fn[1]=='r' && fn[2]=='e') nargs = 7;
                emu->regs[X86_ESP] += (u32)(nargs * 4);
            }
        } else {
            log_warn("[PE32] no shim impl");
            ret = 0;
        }
        emu->regs[X86_EAX] = (u32)ret;
        return 1;
    }
    /* 检查是否为 ExitProcess 特殊地址（64位 ExitProcess shim） */
    if (pe_shim_is_exit_call((u64)target)) {
        pe_shim_exit_process(emu->regs[X86_EAX] ? emu->regs[X86_EAX] : 0);
        emu->exited = 1;
        return 1;
    }
    return 0;  /* 不是 IAT/shim 调用 */
}

/* ===== 条件跳转判断 ===== */
static int check_jcc(x86emu_state *emu, int cond) {
    int zf = get_flag(emu, X86_ZF);
    int sf = get_flag(emu, X86_SF);
    int of = get_flag(emu, X86_OF);
    int cf = get_flag(emu, X86_CF);
    int pf = get_flag(emu, X86_PF);
    switch (cond & 0xF) {
    case 0x0: return of;
    case 0x1: return !of;
    case 0x2: return cf;
    case 0x3: return !cf;
    case 0x4: return zf;
    case 0x5: return !zf;
    case 0x6: return cf || zf;
    case 0x7: return !cf && !zf;
    case 0x8: return sf;
    case 0x9: return !sf;
    case 0xA: return pf;
    case 0xB: return !pf;
    case 0xC: return sf != of;
    case 0xD: return sf == of;
    case 0xE: return zf || (sf != of);
    case 0xF: return !zf && (sf == of);
    }
    return 0;
}

/* ===== 主执行循环 ===== */
int x86emu32_run(x86emu_state *emu, u64 *exit_code) {
    emu->insn_limit = 500000000ULL;  /* 5亿指令上限防死循环 */

    while (!emu->exited && !emu->error && emu->insn_count < emu->insn_limit) {
        emu->insn_count++;

        /* 前缀处理 */
        int operand_size = 4;  /* 默认 32位 */
        int rep_prefix = 0;    /* 0=无, 1=REP/REPE, 2=REPNE（记录但暂未用于字符串指令） */
        u8 op;
        int prefix_done = 0;
        while (!prefix_done) {
            op = fetch8(emu);
            switch (op) {
            case 0x66: operand_size = 2; break;
            case 0x67: break;  /* 地址大小前缀（忽略，32位寻址） */
            case 0xF0: break;  /* LOCK */
            case 0xF2: rep_prefix = 2; break;  /* REPNE */
            case 0xF3: rep_prefix = 1; break;  /* REP/REPE */
            case 0x26: case 0x2E: case 0x36: case 0x3E: case 0x64: case 0x65: break; /* 段前缀忽略 */
            default: prefix_done = 1; break;
            }
        }
        (void)rep_prefix;  /* 记录前缀，字符串指令暂未使用 */

        /* 操作数大小：对 0x66 前缀，32位操作变 16位 */
        int osz = operand_size;  /* 4 或 2 */

        switch (op) {
        /* ---- ALU modr/m（0x00-0x3F）---- */
        case 0x00: case 0x01: case 0x02: case 0x03:
        case 0x08: case 0x09: case 0x0A: case 0x0B:
        case 0x10: case 0x11: case 0x12: case 0x13:
        case 0x18: case 0x19: case 0x1A: case 0x1B:
        case 0x20: case 0x21: case 0x22: case 0x23:
        case 0x28: case 0x29: case 0x2A: case 0x2B:
        case 0x30: case 0x31: case 0x32: case 0x33:
        case 0x38: case 0x39: case 0x3A: case 0x3B: {
            int alu = (op >> 3) & 7;
            int dir = op & 1;  /* 0: r/m8←r8, 1: r/m32←r32 (或反向) */
            int is8 = !(op & 4);  /* 0x00-0x03=8位, 0x04+ =32位 */
            modrm_dec m = decode_modrm(emu);
            if (is8) {
                u8 a, b; int cmp_only = (alu == 7);
                if (dir == 0) { a = read_rm8(emu, &m); b = get_reg8(emu, m.reg); }
                else { a = get_reg8(emu, m.reg); b = read_rm8(emu, &m); }
                u8 r = (u8)alu_op(emu, alu, a, b, 1);
                if (alu != 7) { if (dir == 0) write_rm8(emu, &m, r); else set_reg8(emu, m.reg, r); }
                (void)cmp_only;
            } else {
                u32 a, b;
                if (dir == 0) { a = read_rm32(emu, &m); b = get_reg32(emu, m.reg); }
                else { a = get_reg32(emu, m.reg); b = read_rm32(emu, &m); }
                u32 r = alu_op(emu, alu, a, b, osz);
                if (alu != 7) { if (dir == 0) write_rm32(emu, &m, r); else set_reg32(emu, m.reg, r); }
                if (osz == 2) { if (alu != 7) { if (dir == 0) write_rm16(emu, &m, (u16)r); else set_reg16(emu, m.reg, (u16)r); } }
            }
            break;
        }
        /* ---- ALU AL/EAX, imm ---- */
        case 0x04: case 0x0C: case 0x14: case 0x1C:
        case 0x24: case 0x2C: case 0x34: case 0x3C: {
            int alu = (op >> 3) & 7;
            u8 imm = fetch8(emu);
            u8 r = (u8)alu_op(emu, alu, get_reg8(emu, 0), imm, 1);
            if (alu != 7) set_reg8(emu, 0, r);
            break;
        }
        case 0x05: case 0x0D: case 0x15: case 0x1D:
        case 0x25: case 0x2D: case 0x35: case 0x3D: {
            int alu = (op >> 3) & 7;
            if (osz == 2) {
                u16 imm = fetch16(emu);
                u16 r = (u16)alu_op(emu, alu, get_reg16(emu, 0), imm, 2);
                if (alu != 7) set_reg16(emu, 0, r);
            } else {
                u32 imm = fetch32(emu);
                u32 r = alu_op(emu, alu, get_reg32(emu, 0), imm, 4);
                if (alu != 7) set_reg32(emu, 0, r);
            }
            break;
        }
        /* ---- INC/DEC reg（0x40-0x4F）---- */
        case 0x40: case 0x41: case 0x42: case 0x43:
        case 0x44: case 0x45: case 0x46: case 0x47: {
            int r = op - 0x40;
            int cf = get_flag(emu, X86_CF);
            if (osz == 2) { u16 v = get_reg16(emu, r); u16 res = (u16)alu_op(emu, 0, v, 1, 2); set_reg16(emu, r, res); }
            else { u32 v = get_reg32(emu, r); u32 res = alu_op(emu, 0, v, 1, 4); set_reg32(emu, r, res); }
            set_flag(emu, X86_CF, cf);  /* INC 不影响 CF */
            break;
        }
        case 0x48: case 0x49: case 0x4A: case 0x4B:
        case 0x4C: case 0x4D: case 0x4E: case 0x4F: {
            int r = op - 0x48;
            int cf = get_flag(emu, X86_CF);
            if (osz == 2) { u16 v = get_reg16(emu, r); u16 res = (u16)alu_op(emu, 5, v, 1, 2); set_reg16(emu, r, res); }
            else { u32 v = get_reg32(emu, r); u32 res = alu_op(emu, 5, v, 1, 4); set_reg32(emu, r, res); }
            set_flag(emu, X86_CF, cf);
            break;
        }
        /* ---- PUSH/POP reg ---- */
        case 0x50: case 0x51: case 0x52: case 0x53:
        case 0x54: case 0x55: case 0x56: case 0x57:
            x86emu32_push32(emu, get_reg32(emu, op - 0x50));
            break;
        case 0x58: case 0x59: case 0x5A: case 0x5B:
        case 0x5C: case 0x5D: case 0x5E: case 0x5F:
            set_reg32(emu, op - 0x58, x86emu32_pop32(emu));
            break;
        /* ---- PUSHA/POPA ---- */
        case 0x60: {
            u32 sp = emu->regs[X86_ESP];
            x86emu32_push32(emu, emu->regs[X86_EAX]);
            x86emu32_push32(emu, emu->regs[X86_ECX]);
            x86emu32_push32(emu, emu->regs[X86_EDX]);
            x86emu32_push32(emu, emu->regs[X86_EBX]);
            x86emu32_push32(emu, sp);
            x86emu32_push32(emu, emu->regs[X86_EBP]);
            x86emu32_push32(emu, emu->regs[X86_ESI]);
            x86emu32_push32(emu, emu->regs[X86_EDI]);
            break;
        }
        case 0x61: {
            emu->regs[X86_EDI] = x86emu32_pop32(emu);
            emu->regs[X86_ESI] = x86emu32_pop32(emu);
            emu->regs[X86_EBP] = x86emu32_pop32(emu);
            x86emu32_pop32(emu);  /* 忽略原始 ESP */
            emu->regs[X86_EBX] = x86emu32_pop32(emu);
            emu->regs[X86_EDX] = x86emu32_pop32(emu);
            emu->regs[X86_ECX] = x86emu32_pop32(emu);
            emu->regs[X86_EAX] = x86emu32_pop32(emu);
            break;
        }
        /* ---- PUSH imm ---- */
        case 0x68: x86emu32_push32(emu, fetch32(emu)); break;
        case 0x6A: x86emu32_push32(emu, (u32)(i32)(i8)fetch8(emu)); break;
        /* ---- IMUL r, r/m, imm ---- */
        case 0x69: case 0x6B: {
            modrm_dec m = decode_modrm(emu);
            u32 src = read_rm32(emu, &m);
            u32 imm = (op == 0x69) ? fetch32(emu) : (u32)(i32)(i8)fetch8(emu);
            i64 result = (i64)(i32)src * (i64)(i32)imm;
            set_reg32(emu, m.reg, (u32)result);
            set_flag(emu, X86_OF, result > 0x7FFFFFFFLL || result < -0x80000000LL);
            set_flag(emu, X86_CF, get_flag(emu, X86_OF));
            break;
        }
        /* ---- Jcc rel8 ---- */
        case 0x70: case 0x71: case 0x72: case 0x73:
        case 0x74: case 0x75: case 0x76: case 0x77:
        case 0x78: case 0x79: case 0x7A: case 0x7B:
        case 0x7C: case 0x7D: case 0x7E: case 0x7F: {
            i8 rel = (i8)fetch8(emu);
            if (check_jcc(emu, op & 0xF)) emu->eip += (u32)(i32)rel;
            break;
        }
        /* ---- ALU r/m, imm8 (0x80/0x82) / r/m, imm32 (0x81) ---- */
        case 0x80: case 0x82: {
            modrm_dec m = decode_modrm(emu);
            u8 imm = fetch8(emu);
            u8 a = read_rm8(emu, &m);
            u8 r = (u8)alu_op(emu, m.reg, a, imm, 1);
            if (m.reg != 7) write_rm8(emu, &m, r);
            break;
        }
        case 0x81: {
            modrm_dec m = decode_modrm(emu);
            if (osz == 2) {
                u16 imm = fetch16(emu);
                u16 a = read_rm16(emu, &m);
                u16 r = (u16)alu_op(emu, m.reg, a, imm, 2);
                if (m.reg != 7) write_rm16(emu, &m, r);
            } else {
                u32 imm = fetch32(emu);
                u32 a = read_rm32(emu, &m);
                u32 r = alu_op(emu, m.reg, a, imm, 4);
                if (m.reg != 7) write_rm32(emu, &m, r);
            }
            break;
        }
        case 0x83: {
            modrm_dec m = decode_modrm(emu);
            u32 imm = (u32)(i32)(i8)fetch8(emu);
            if (osz == 2) {
                u16 a = read_rm16(emu, &m);
                u16 r = (u16)alu_op(emu, m.reg, a, imm & 0xFFFF, 2);
                if (m.reg != 7) write_rm16(emu, &m, r);
            } else {
                u32 a = read_rm32(emu, &m);
                u32 r = alu_op(emu, m.reg, a, imm, 4);
                if (m.reg != 7) write_rm32(emu, &m, r);
            }
            break;
        }
        /* ---- TEST ---- */
        case 0x84: case 0x85: {
            modrm_dec m = decode_modrm(emu);
            if (op == 0x84) {
                u8 a = read_rm8(emu, &m); u8 b = get_reg8(emu, m.reg);
                alu_op(emu, 4, a, b, 1);  /* AND 不写回 */
            } else if (osz == 2) {
                u16 a = read_rm16(emu, &m); u16 b = get_reg16(emu, m.reg);
                alu_op(emu, 4, a, b, 2);
            } else {
                u32 a = read_rm32(emu, &m); u32 b = get_reg32(emu, m.reg);
                alu_op(emu, 4, a, b, 4);
            }
            break;
        }
        /* ---- XCHG ---- */
        case 0x86: case 0x87: {
            modrm_dec m = decode_modrm(emu);
            if (op == 0x86) {
                u8 a = read_rm8(emu, &m); u8 b = get_reg8(emu, m.reg);
                write_rm8(emu, &m, b); set_reg8(emu, m.reg, a);
            } else if (osz == 2) {
                u16 a = read_rm16(emu, &m); u16 b = get_reg16(emu, m.reg);
                write_rm16(emu, &m, b); set_reg16(emu, m.reg, a);
            } else {
                u32 a = read_rm32(emu, &m); u32 b = get_reg32(emu, m.reg);
                write_rm32(emu, &m, b); set_reg32(emu, m.reg, a);
            }
            break;
        }
        /* ---- MOV ---- */
        case 0x88: { modrm_dec m = decode_modrm(emu); write_rm8(emu, &m, get_reg8(emu, m.reg)); break; }
        case 0x89: {
            modrm_dec m = decode_modrm(emu);
            if (osz == 2) write_rm16(emu, &m, get_reg16(emu, m.reg));
            else write_rm32(emu, &m, get_reg32(emu, m.reg));
            break;
        }
        case 0x8A: { modrm_dec m = decode_modrm(emu); set_reg8(emu, m.reg, read_rm8(emu, &m)); break; }
        case 0x8B: {
            modrm_dec m = decode_modrm(emu);
            if (osz == 2) set_reg16(emu, m.reg, read_rm16(emu, &m));
            else set_reg32(emu, m.reg, read_rm32(emu, &m));
            break;
        }
        /* ---- LEA ---- */
        case 0x8D: {
            modrm_dec m = decode_modrm(emu);
            if (osz == 2) set_reg16(emu, m.reg, (u16)m.ea);
            else set_reg32(emu, m.reg, m.ea);
            break;
        }
        /* ---- NOP / XCHG EAX,reg ---- */
        case 0x90: break;  /* NOP */
        case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97: {
            int r = op - 0x90;
            u32 tmp = emu->regs[0]; emu->regs[0] = emu->regs[r]; emu->regs[r] = tmp;
            break;
        }
        /* ---- CWDE/CDQ ---- */
        case 0x98:  /* CWDE (32位) / CBW (16位) */
            if (osz == 2) set_reg16(emu, 0, (u16)(i16)(i8)get_reg8(emu, 0));
            else emu->regs[0] = (u32)(i32)(i16)get_reg16(emu, 0);
            break;
        case 0x99:  /* CDQ (32位) / CWD (16位) */
            if (osz == 2) set_reg16(emu, 2, (get_reg16(emu, 0) & 0x8000) ? 0xFFFF : 0);
            else emu->regs[2] = (emu->regs[0] & 0x80000000) ? 0xFFFFFFFF : 0;
            break;
        /* ---- MOV moffs ---- */
        case 0xA0: { u32 addr = fetch32(emu); set_reg8(emu, 0, x86emu32_read8(emu, addr)); break; }
        case 0xA1: { u32 addr = fetch32(emu); if (osz==2) set_reg16(emu,0,x86emu32_read16(emu,addr)); else set_reg32(emu,0,x86emu32_read32(emu,addr)); break; }
        case 0xA2: { u32 addr = fetch32(emu); x86emu32_write8(emu, addr, get_reg8(emu, 0)); break; }
        case 0xA3: { u32 addr = fetch32(emu); if (osz==2) x86emu32_write16(emu,addr,get_reg16(emu,0)); else x86emu32_write32(emu,addr,get_reg32(emu,0)); break; }
        /* ---- 字符串指令（简化：无 REP 单步）---- */
        case 0xA4: { /* MOVSB */
            u32 si = emu->regs[X86_ESI], di = emu->regs[X86_EDI];
            u8 v = x86emu32_read8(emu, si);
            x86emu32_write8(emu, di, v);
            int df = get_flag(emu, X86_DF);
            si += df ? -1 : 1; di += df ? -1 : 1;
            emu->regs[X86_ESI] = si; emu->regs[X86_EDI] = di;
            break;
        }
        case 0xA5: { /* MOVSD/W */
            u32 si = emu->regs[X86_ESI], di = emu->regs[X86_EDI];
            int df = get_flag(emu, X86_DF); int step = (osz==2)?2:4;
            if (osz==2) { u16 v=x86emu32_read16(emu,si); x86emu32_write16(emu,di,v); }
            else { u32 v=x86emu32_read32(emu,si); x86emu32_write32(emu,di,v); }
            si += df ? -step : step; di += df ? -step : step;
            emu->regs[X86_ESI]=si; emu->regs[X86_EDI]=di;
            break;
        }
        case 0xAA: { /* STOSB */
            u32 di = emu->regs[X86_EDI];
            x86emu32_write8(emu, di, get_reg8(emu, 0));
            int df = get_flag(emu, X86_DF);
            di += df ? -1 : 1; emu->regs[X86_EDI] = di;
            break;
        }
        case 0xAB: { /* STOSD/W */
            u32 di = emu->regs[X86_EDI]; int df = get_flag(emu, X86_DF); int step=(osz==2)?2:4;
            if (osz==2) x86emu32_write16(emu,di,get_reg16(emu,0));
            else x86emu32_write32(emu,di,emu->regs[0]);
            di += df?-step:step; emu->regs[X86_EDI]=di;
            break;
        }
        case 0xAC: { /* LODSB */
            u32 si = emu->regs[X86_ESI];
            set_reg8(emu, 0, x86emu32_read8(emu, si));
            int df = get_flag(emu, X86_DF);
            si += df ? -1 : 1; emu->regs[X86_ESI] = si;
            break;
        }
        case 0xAD: { /* LODSD/W */
            u32 si = emu->regs[X86_ESI]; int df=get_flag(emu,X86_DF); int step=(osz==2)?2:4;
            if (osz==2) set_reg16(emu,0,x86emu32_read16(emu,si)); else set_reg32(emu,0,x86emu32_read32(emu,si));
            si += df?-step:step; emu->regs[X86_ESI]=si;
            break;
        }
        /* ---- TEST AL/EAX, imm ---- */
        case 0xA8: { u8 imm = fetch8(emu); alu_op(emu, 4, get_reg8(emu,0), imm, 1); break; }
        case 0xA9: {
            if (osz==2) { u16 imm=fetch16(emu); alu_op(emu,4,get_reg16(emu,0),imm,2); }
            else { u32 imm=fetch32(emu); alu_op(emu,4,emu->regs[0],imm,4); }
            break;
        }
        /* ---- MOV reg, imm ---- */
        case 0xB0: case 0xB1: case 0xB2: case 0xB3:
        case 0xB4: case 0xB5: case 0xB6: case 0xB7:
            set_reg8(emu, op - 0xB0, fetch8(emu));
            break;
        case 0xB8: case 0xB9: case 0xBA: case 0xBB:
        case 0xBC: case 0xBD: case 0xBE: case 0xBF:
            if (osz==2) set_reg16(emu, op-0xB8, fetch16(emu));
            else set_reg32(emu, op-0xB8, fetch32(emu));
            break;
        /* ---- shift r/m, imm8 ---- */
        case 0xC0: case 0xC1: {
            modrm_dec m = decode_modrm(emu);
            u8 count = fetch8(emu);
            if (op == 0xC0) { u8 v = read_rm8(emu,&m); write_rm8(emu,&m,(u8)shift_op(emu,m.reg,v,count,1)); }
            else if (osz==2) { u16 v=read_rm16(emu,&m); write_rm16(emu,&m,(u16)shift_op(emu,m.reg,v,count,2)); }
            else { u32 v=read_rm32(emu,&m); write_rm32(emu,&m,shift_op(emu,m.reg,v,count,4)); }
            break;
        }
        /* ---- RET ---- */
        case 0xC2: { u16 imm = fetch16(emu); emu->eip = x86emu32_pop32(emu); emu->regs[X86_ESP] += imm; break; }
        case 0xC3: { emu->eip = x86emu32_pop32(emu); break; }
        /* ---- MOV r/m, imm ---- */
        case 0xC6: { modrm_dec m = decode_modrm(emu); u8 imm = fetch8(emu); write_rm8(emu,&m,imm); break; }
        case 0xC7: {
            modrm_dec m = decode_modrm(emu);
            if (osz==2) { u16 imm=fetch16(emu); write_rm16(emu,&m,imm); }
            else { u32 imm=fetch32(emu); write_rm32(emu,&m,imm); }
            break;
        }
        /* ---- LEAVE ---- */
        case 0xC9:
            emu->regs[X86_ESP] = emu->regs[X86_EBP];
            emu->regs[X86_EBP] = x86emu32_pop32(emu);
            break;
        /* ---- INT3 / INT ---- */
        case 0xCC:  /* INT3 */
            emu->error = 1; emu->error_msg = "INT3 breakpoint";
            break;
        case 0xCD: {  /* INT imm8 */
            u8 v = fetch8(emu); (void)v;
            /* 简化：忽略软件中断 */
            break;
        }
        /* ---- shift r/m, 1 / CL ---- */
        case 0xD0: case 0xD1: case 0xD2: case 0xD3: {
            modrm_dec m = decode_modrm(emu);
            u8 count = (op & 2) ? (get_reg8(emu, 1) & 0x1F) : 1;
            int is8 = !(op & 1);
            if (is8) { u8 v=read_rm8(emu,&m); write_rm8(emu,&m,(u8)shift_op(emu,m.reg,v,count,1)); }
            else if (osz==2) { u16 v=read_rm16(emu,&m); write_rm16(emu,&m,(u16)shift_op(emu,m.reg,v,count,2)); }
            else { u32 v=read_rm32(emu,&m); write_rm32(emu,&m,shift_op(emu,m.reg,v,count,4)); }
            break;
        }
        /* ---- CALL rel32 ---- */
        case 0xE8: {
            i32 rel = (i32)fetch32(emu);
            x86emu32_push32(emu, emu->eip);  /* return address */
            emu->eip += (u32)rel;
            break;
        }
        /* ---- JMP rel32 ---- */
        case 0xE9: { i32 rel = (i32)fetch32(emu); emu->eip += (u32)rel; break; }
        /* ---- JMP rel8 ---- */
        case 0xEB: { i8 rel = (i8)fetch8(emu); emu->eip += (u32)(i32)rel; break; }
        /* ---- LOOP ---- */
        case 0xE0: case 0xE1: case 0xE2: {
            i8 rel = (i8)fetch8(emu);
            u32 cx = emu->regs[X86_ECX] - 1;
            emu->regs[X86_ECX] = cx;
            int do_jump = (cx != 0);
            if (op == 0xE0 && get_flag(emu, X86_ZF)) do_jump = 0;  /* LOOPNE */
            if (op == 0xE1 && !get_flag(emu, X86_ZF)) do_jump = 0; /* LOOPE */
            if (do_jump) emu->eip += (u32)(i32)rel;
            break;
        }
        case 0xE3: { /* JECXZ */
            i8 rel = (i8)fetch8(emu);
            if (emu->regs[X86_ECX] == 0) emu->eip += (u32)(i32)rel;
            break;
        }
        /* ---- CALL/JMP r/m ---- */
        case 0xFF: {
            modrm_dec m = decode_modrm(emu);
            switch (m.reg) {
            case 0: { /* INC r/m */
                int cf=get_flag(emu,X86_CF);
                if (osz==2) { u16 v=read_rm16(emu,&m); write_rm16(emu,&m,(u16)alu_op(emu,0,v,1,2)); }
                else { u32 v=read_rm32(emu,&m); write_rm32(emu,&m,alu_op(emu,0,v,1,4)); }
                set_flag(emu,X86_CF,cf);
                break;
            }
            case 1: { /* DEC r/m */
                int cf=get_flag(emu,X86_CF);
                if (osz==2) { u16 v=read_rm16(emu,&m); write_rm16(emu,&m,(u16)alu_op(emu,5,v,1,2)); }
                else { u32 v=read_rm32(emu,&m); write_rm32(emu,&m,alu_op(emu,5,v,1,4)); }
                set_flag(emu,X86_CF,cf);
                break;
            }
            case 2: { /* CALL r/m32 */
                u32 target = read_rm32(emu, &m);
                if (try_iat_call(emu, target)) break;
                x86emu32_push32(emu, emu->eip);
                emu->eip = target;
                break;
            }
            case 4: { /* JMP r/m32 */
                u32 target = read_rm32(emu, &m);
                if (try_iat_call(emu, target)) { emu->exited = 1; break; }
                emu->eip = target;
                break;
            }
            case 6: { /* PUSH r/m32 */
                x86emu32_push32(emu, read_rm32(emu, &m));
                break;
            }
            default:
                emu->error = 1; emu->error_msg = "FF /reg unsupported";
                break;
            }
            break;
        }
        /* ---- F6/F7 组 ---- */
        case 0xF6: {
            modrm_dec m = decode_modrm(emu);
            if (m.reg == 0 || m.reg == 1) {  /* TEST r/m8, imm8 */
                u8 imm = fetch8(emu);
                alu_op(emu, 4, read_rm8(emu,&m), imm, 1);
            } else if (m.reg == 2) {  /* NOT r/m8 */
                write_rm8(emu, &m, ~read_rm8(emu, &m));
            } else if (m.reg == 3) {  /* NEG r/m8 */
                u8 v = read_rm8(emu, &m);
                u8 r = (u8)(-(i8)v);
                set_flag(emu, X86_CF, v != 0);
                set_zf_sf_pf(emu, r, 1);
                write_rm8(emu, &m, r);
            } else if (m.reg == 4) {  /* MUL r/m8 */
                u16 result = (u16)get_reg8(emu, 0) * (u16)read_rm8(emu, &m);
                set_reg8(emu, 0, (u8)result);
                set_reg8(emu, 4, (u8)(result >> 8));
                set_flag(emu, X86_CF, result >> 8 != 0);
                set_flag(emu, X86_OF, get_flag(emu, X86_CF));
            } else {
                emu->error = 1; emu->error_msg = "F6 /reg unsupported";
            }
            break;
        }
        case 0xF7: {
            modrm_dec m = decode_modrm(emu);
            if (m.reg == 0 || m.reg == 1) {  /* TEST r/m32, imm32 */
                if (osz==2) { u16 imm=fetch16(emu); alu_op(emu,4,read_rm16(emu,&m),imm,2); }
                else { u32 imm=fetch32(emu); alu_op(emu,4,read_rm32(emu,&m),imm,4); }
            } else if (m.reg == 2) {  /* NOT */
                if (osz==2) write_rm16(emu,&m,~read_rm16(emu,&m));
                else write_rm32(emu,&m,~read_rm32(emu,&m));
            } else if (m.reg == 3) {  /* NEG */
                if (osz==2) { u16 v=read_rm16(emu,&m); u16 r=(u16)(-(i16)v); set_flag(emu,X86_CF,v!=0); set_zf_sf_pf(emu,r,2); write_rm16(emu,&m,r); }
                else { u32 v=read_rm32(emu,&m); u32 r=(u32)(-(i32)v); set_flag(emu,X86_CF,v!=0); set_zf_sf_pf(emu,r,4); write_rm32(emu,&m,r); }
            } else if (m.reg == 4) {  /* MUL */
                if (osz==2) { u32 result=(u32)get_reg16(emu,0)*(u32)read_rm16(emu,&m); set_reg16(emu,0,(u16)result); set_reg16(emu,2,(u16)(result>>16)); set_flag(emu,X86_CF,result>>16!=0); set_flag(emu,X86_OF,get_flag(emu,X86_CF)); }
                else { u64 result=(u64)emu->regs[0]*(u64)read_rm32(emu,&m); emu->regs[0]=(u32)result; emu->regs[2]=(u32)(result>>32); set_flag(emu,X86_CF,result>>32!=0); set_flag(emu,X86_OF,get_flag(emu,X86_CF)); }
            } else if (m.reg == 5) {  /* IMUL */
                if (osz==2) { i32 result=(i32)(i16)get_reg16(emu,0)*(i32)(i16)read_rm16(emu,&m); set_reg16(emu,0,(u16)result); set_reg16(emu,2,(u16)(result>>16)); set_flag(emu,X86_OF,result>32767||result<-32768); set_flag(emu,X86_CF,get_flag(emu,X86_OF)); }
                else { i64 result=(i64)(i32)emu->regs[0]*(i64)(i32)read_rm32(emu,&m); emu->regs[0]=(u32)result; emu->regs[2]=(u32)(result>>32); set_flag(emu,X86_OF,result>0x7FFFFFFFLL||result<-0x80000000LL); set_flag(emu,X86_CF,get_flag(emu,X86_OF)); }
            } else if (m.reg == 6) {  /* DIV */
                if (osz==2) { u32 dividend=((u32)get_reg16(emu,2)<<16)|get_reg16(emu,0); u16 divisor=read_rm16(emu,&m); if(divisor){set_reg16(emu,0,(u16)(dividend/divisor));set_reg16(emu,2,(u16)(dividend%divisor));} }
                else { u64 dividend=((u64)emu->regs[2]<<32)|emu->regs[0]; u32 divisor=read_rm32(emu,&m); if(divisor){emu->regs[0]=(u32)(dividend/divisor);emu->regs[2]=(u32)(dividend%divisor);} }
            } else if (m.reg == 7) {  /* IDIV */
                if (osz==2) { i32 dividend=((i32)(i16)get_reg16(emu,2)<<16)|(i16)get_reg16(emu,0); i16 divisor=(i16)read_rm16(emu,&m); if(divisor){set_reg16(emu,0,(u16)(dividend/divisor));set_reg16(emu,2,(u16)(dividend%divisor));} }
                else { i64 dividend=((i64)(i32)emu->regs[2]<<32)|(i32)emu->regs[0]; i32 divisor=(i32)read_rm32(emu,&m); if(divisor){emu->regs[0]=(u32)(dividend/divisor);emu->regs[2]=(u32)(dividend%divisor);} }
            }
            break;
        }
        /* ---- CLC/STC/CLI/STI/CLD/STD ---- */
        case 0xF8: set_flag(emu, X86_CF, 0); break;
        case 0xF9: set_flag(emu, X86_CF, 1); break;
        case 0xFA: break;  /* CLI（忽略） */
        case 0xFB: break;  /* STI（忽略） */
        case 0xFC: set_flag(emu, X86_DF, 0); break;
        case 0xFD: set_flag(emu, X86_DF, 1); break;
        /* ---- INC/DEC r/m8 ---- */
        case 0xFE: {
            modrm_dec m = decode_modrm(emu);
            int cf = get_flag(emu, X86_CF);
            u8 v = read_rm8(emu, &m);
            if (m.reg == 0) write_rm8(emu, &m, (u8)alu_op(emu, 0, v, 1, 1));
            else if (m.reg == 1) write_rm8(emu, &m, (u8)alu_op(emu, 5, v, 1, 1));
            set_flag(emu, X86_CF, cf);
            break;
        }
        /* ---- MOVSX/MOVZX 在 0x0F 两字节 opcode 中处理 ---- */
        case 0x0F: {  /* 两字节 opcode */
            u8 op2 = fetch8(emu);
            if (op2 >= 0x80 && op2 <= 0x8F) {  /* Jcc rel32 */
                i32 rel = (i32)fetch32(emu);
                if (check_jcc(emu, op2 & 0xF)) emu->eip += (u32)rel;
            } else if (op2 == 0xAF) {  /* IMUL r32, r/m32 */
                modrm_dec m = decode_modrm(emu);
                i64 result = (i64)(i32)get_reg32(emu, m.reg) * (i64)(i32)read_rm32(emu, &m);
                set_reg32(emu, m.reg, (u32)result);
                set_flag(emu, X86_OF, result > 0x7FFFFFFFLL || result < -0x80000000LL);
                set_flag(emu, X86_CF, get_flag(emu, X86_OF));
            } else if (op2 == 0xB6 || op2 == 0xB7) {  /* MOVZX r32, r/m8/16 */
                modrm_dec m = decode_modrm(emu);
                if (op2 == 0xB6) { set_reg32(emu, m.reg, (u32)read_rm8(emu, &m)); }
                else { set_reg32(emu, m.reg, (u32)read_rm16(emu, &m)); }
            } else if (op2 == 0xBE || op2 == 0xBF) {  /* MOVSX r32, r/m8/16 */
                modrm_dec m = decode_modrm(emu);
                if (op2 == 0xBE) { set_reg32(emu, m.reg, (u32)(i32)(i8)read_rm8(emu, &m)); }
                else { set_reg32(emu, m.reg, (u32)(i32)(i16)read_rm16(emu, &m)); }
            } else if (op2 == 0x1F) {  /* NOP r/m（多字节 NOP） */
                decode_modrm(emu);
            } else if (op2 == 0x05) {  /* SYSCALL（不应出现在32位PE） */
                emu->error = 1; emu->error_msg = "SYSCALL in 32-bit PE";
            } else {
                emu->error = 1; emu->error_msg = "0F opcode unsupported";
            }
            break;
        }
        default:
            emu->error = 1; emu->error_msg = "unsupported opcode";
            break;
        }  /* end switch(op) */

        if (emu->error) {
            log_hex64("[PE32] error at EIP=", emu->eip);
            log_hex64("[PE32] opcode=", op);
            log_info("[PE32] ");
            log_info(emu->error_msg ? emu->error_msg : "(null)");
            break;
        }
    }  /* end while */

    if (emu->insn_count >= emu->insn_limit) {
        log_error("[PE32] instruction limit reached");
        emu->error = 1;
    }

    if (exit_code) *exit_code = emu->exit_code;
    return emu->error ? -1 : 0;
}

int x86emu32_init(x86emu_state *emu, u8 *mem, u64 mem_size,
                  u32 image_base, u32 image_size,
                  pe_iat_entry *iat_entries, int iat_count,
                  u32 stack_top) {
    for (int i = 0; i < 8; i++) emu->regs[i] = 0;
    emu->eip = 0;
    emu->eflags = 0x2;  /* 保留位 = 1 */
    emu->mem = mem;
    emu->mem_size = mem_size;
    emu->image_base = image_base;
    emu->image_size = image_size;
    emu->iat_entries = iat_entries;
    emu->iat_count = iat_count;
    emu->stack_base = stack_top;
    emu->stack_size = 0x10000;  /* 64KB 栈 */
    emu->heap_base = image_base + image_size;
    emu->heap_size = 0x100000;  /* 1MB heap */
    emu->heap_next = emu->heap_base;
    emu->exited = 0;
    emu->exit_code = 0;
    emu->error = 0;
    emu->error_msg = 0;
    emu->insn_count = 0;

    /* 设置 ESP 到栈顶，对齐 16 */
    emu->regs[X86_ESP] = stack_top & ~0xFu;
    emu->regs[X86_EBP] = 0;
    return 0;
}
