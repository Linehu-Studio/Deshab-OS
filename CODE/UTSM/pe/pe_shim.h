#ifndef PE_SHIM_H
#define PE_SHIM_H

/* pe_shim.h — Windows API shim 表。
 *
 * 将常见 Windows DLL 函数映射到 Deshab 等价实现：
 *   kernel32.dll!WriteFile → 写串口 + 帧缓冲
 *   kernel32.dll!HeapAlloc → kmem_alloc
 *   msvcrt.dll!printf      → 串口输出
 *   ...
 *
 * 64位 PE：shim 用 __attribute__((ms_abi)) 声明，PE 代码以 MS x64 ABI 调用，
 *          IAT 直接填 shim 函数地址，原生执行。
 * 32位 PE：解释器在 CALL IAT 时拦截，从模拟栈读参数调用 fn32，结果写 EAX。
 */

#include <utsm/types.h>

/* 32位 shim：从模拟栈读取参数（cdecl 语义，调用者清栈）。
 * esp 指向模拟栈（第一个参数在 esp[0]）。返回值 = EAX。 */
typedef u64 (*pe_shim_fn32)(void *esp);

/* shim 表条目 */
typedef struct pe_shim_entry {
    const char *dll;        /* 小写 DLL 名，如 "kernel32.dll" */
    const char *func;       /* 函数名，如 "WriteFile" */
    void *fn64;             /* 64位 ms_abi 函数地址（可为 NULL） */
    pe_shim_fn32 fn32;      /* 32位实现（可为 NULL） */
} pe_shim_entry;

/* 按 dll+func 名查找 shim。返回表索引，-1 未找到。 */
int pe_shim_lookup(const char *dll, const char *func);

/* 获取 shim 表条目 */
const pe_shim_entry *pe_shim_get(int index);

/* 获取 shim 表大小 */
int pe_shim_count(void);

/* 生成一个唯一的"未实现 stub"地址。
 * 64位：返回一个固定 stub 函数地址，被调用时日志告警并返回 0。
 * 32位：返回一个特殊标记值，解释器识别后日志告警。 */
u64 pe_shim_unimpl_stub(void);

/* 初始化 shim 子系统（注册全局状态，如 cmdline） */
void pe_shim_init(const char *cmdline);

/* ExitProcess 实现：通过 __builtin_longjmp 跳回 pe_run。
 * 64位和32位共用。exit_code = 进程退出码。 */
void pe_shim_exit_process(u64 exit_code);

/* pe_run 调用：保存上下文（__builtin_setjmp）。
 * 返回 0 = 首次（继续执行 PE），1 = ExitProcess 触发（longjmp 回来）。 */
int pe_shim_setup_exit(void);

/* 获取上次 ExitProcess 的退出码 */
u64 pe_shim_get_exit_code(void);

/* 判断地址是否为 ExitProcess shim（解释器用） */
int pe_shim_is_exit_call(u64 target);

/* 获取 cmdline（shim 内部用） */
const char *pe_shim_get_cmdline(void);

#endif /* PE_SHIM_H */
