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

/* shim 伪模块句柄（LoadLibrary 对已知 shim DLL 返回值）。
 * 真实 DLL 句柄 = pe_dll_load 镜像基址，与此值域区分。 */
#define PE_SHIM_FAKE_MODULE 0x10000020ULL

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

/* 获取 ExitProcess longjmp 目标 jmpbuf，并重置退出状态。
 *
 * BUG-20260801-006：__builtin_setjmp 必须出现在 PE 执行全程存活的
 * 栈帧内（pe_service_run 本体）。此前由 pe_shim_setup_exit() 包装
 * 调用——该函数调用 setjmp 后即返回，C 标准明确规定"含 setjmp 的
 * 函数已返回后再 longjmp"为未定义行为（实测 longjmp 尾声从脏栈
 * pop 出 log_hex64 缓冲区 ASCII 残片充当 R12-R15/RBP → 野指针
 * call → 三重故障）。
 *
 * 调用方标准写法（setjmp 直接出现在调用方函数本体内）：
 *   if (__builtin_setjmp(pe_shim_exit_jmpbuf()) == 0) {
 *       ... 执行 PE ...
 *   } else {
 *       code = pe_shim_get_exit_code();
 *   } */
void **pe_shim_exit_jmpbuf(void);

/* 获取上次 ExitProcess 的退出码 */
u64 pe_shim_get_exit_code(void);

/* 判断地址是否为 ExitProcess shim（解释器用） */
int pe_shim_is_exit_call(u64 target);

/* 获取 cmdline（shim 内部用） */
const char *pe_shim_get_cmdline(void);

/* BUG-20260801-007：设置 PE32 解释器内存基址（guest→host 指针转换）。
 * PE32 解释器路径下，fn64 回退 shim 收到的指针参数是 32 位 guest VA
 * （相对解释器 mem 基址的偏移），直接当 host 指针解引用 → 低地址 #PF
 * → 三重故障。执行 PE32 前设置 base=mem，shim 内部 gp() 在指针解引用点
 * 完成转换；base=0 表示非解释器上下文（原生 PE32+ 路径），gp() 恒等映射。 */
void pe_shim_set_emu_base(u64 base);

/* ===== P5 窗口模式（desktop 嵌入）=====
 * pe_service 在 run_windowed 前调用 pe_shim_set_window_host 挂起配置；
 * pe_shim_init（run 开头）重置 shim 状态后应用该配置。host==NULL 或
 * host->surface==NULL 时保持全屏独占行为（默认）。 */
struct pe_window_host;   /* 定义见 utsm/pe.h */
void pe_shim_set_window_host(const struct pe_window_host *host);

/* 窗口模式输入注入（仅窗口模式生效，全屏返回 -1）。
 * inject_scancode: set-1 扫描码（bit7=release），e0_prefix!=0 为 E0 扩展。
 * inject_pointer : surface 局部坐标 + 按钮位（bit0/1/2=L/R/M），
 *                  内部与上次状态比较合成 MOUSEMOVE/BUTTON 边沿。
 * inject_input   : 直接投递任意 Win32 消息（WM_CLOSE 等宿主控制消息）。 */
int pe_shim_inject_scancode(u32 scancode, u32 e0_prefix);
int pe_shim_inject_pointer(i32 x, i32 y, u32 buttons);
int pe_shim_inject_input(u32 msg, u64 wparam, u64 lparam, i32 ptx, i32 pty);

/* 窗口模式是否激活（1=渲染目标为 host surface）。 */
int pe_shim_window_mode(void);

#endif /* PE_SHIM_H */
