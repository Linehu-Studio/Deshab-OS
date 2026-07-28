#ifndef X86EMU32_H
#define X86EMU32_H

/* x86emu32.h — x86-32 指令解释器（PE32 执行引擎）。
 *
 * 寄存器级解释执行 32位 PE 代码。覆盖 mingw-w64 控制台程序常用指令集。
 * 遇不支持的指令时日志告警并返回错误。
 */

#include <utsm/types.h>

/* x86 通用寄存器索引 */
enum {
    X86_EAX = 0, X86_ECX, X86_EDX, X86_EBX,
    X86_ESP, X86_EBP, X86_ESI, X86_EDI
};

/* EFLAGS 位 */
#define X86_CF (1u << 0)
#define X86_PF (1u << 2)
#define X86_AF (1u << 4)
#define X86_ZF (1u << 6)
#define X86_SF (1u << 7)
#define X86_OF (1u << 11)
#define X86_DF (1u << 10)

/* IAT 条目信息（解释器用于 CALL 拦截） */
typedef struct pe_iat_entry {
    u32 iat_rva;          /* IAT 中该条目的 RVA */
    int  shim_index;      /* 对应 shim 表索引，-1 = 未实现 stub */
    u32 target_addr;      /* 模拟地址（iat_rva + image_base） */
} pe_iat_entry;

typedef struct x86emu_state {
    u32 regs[8];          /* EAX,ECX,EDX,EBX,ESP,EBP,ESI,EDI */
    u32 eip;              /* 指令指针（相对 mem 基址的偏移） */
    u32 eflags;

    u8 *mem;              /* guest 内存基址（= PE image_base） */
    u64 mem_size;         /* guest 内存大小 */

    u32 image_base;       /* PE 加载基址（= mem，32位截断） */
    u32 image_size;

    /* IAT 表（用于 CALL 拦截） */
    pe_iat_entry *iat_entries;
    int iat_count;

    /* 栈区 */
    u32 stack_base;       /* 栈底（高地址） */
    u32 stack_size;

    /* heap 区 */
    u32 heap_base;
    u32 heap_size;
    u32 heap_next;        /* 下次分配偏移 */

    /* 退出状态 */
    int  exited;          /* ExitProcess 被调用 */
    u64  exit_code;
    int  error;           /* 解释错误 */
    const char *error_msg;

    /* 指令计数限制（防死循环） */
    u64 insn_count;
    u64 insn_limit;
} x86emu_state;

/* 初始化解释器状态。
 * mem = PE image 加载基址（已映射节区+重定位+IAT 填充）。
 * iat_entries / iat_count = pe_loader 解析的 IAT 表。
 * stack_top = 栈顶 ESP 初始值（相对 mem 的偏移）。
 * 返回 0 成功。 */
int x86emu32_init(x86emu_state *emu, u8 *mem, u64 mem_size,
                  u32 image_base, u32 image_size,
                  pe_iat_entry *iat_entries, int iat_count,
                  u32 stack_top);

/* 执行直到 ExitProcess 或错误。
 * 返回 0 正常退出（exit_code 有效），负数错误。 */
int x86emu32_run(x86emu_state *emu, u64 *exit_code);

/* 从模拟栈读取 N 个参数（cdecl/stdcall）。
 * esp_offset = 参数相对 ESP 的偏移（0=第一个参数）。
 * 返回参数值。 */
u64 x86emu32_read_arg(x86emu_state *emu, int esp_offset);

/* 模拟栈 push/pop（供 shim32 回调使用） */
void x86emu32_push32(x86emu_state *emu, u32 val);
u32  x86emu32_pop32(x86emu_state *emu);

/* 读写模拟内存 */
u8  x86emu32_read8(x86emu_state *emu, u32 addr);
u16 x86emu32_read16(x86emu_state *emu, u32 addr);
u32 x86emu32_read32(x86emu_state *emu, u32 addr);
void x86emu32_write8(x86emu_state *emu, u32 addr, u8 val);
void x86emu32_write16(x86emu_state *emu, u32 addr, u16 val);
void x86emu32_write32(x86emu_state *emu, u32 addr, u32 val);

#endif /* X86EMU32_H */
