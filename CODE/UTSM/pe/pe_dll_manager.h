#ifndef PE_DLL_MANAGER_H
#define PE_DLL_MANAGER_H

/* pe_dll_manager.h — PE 真实 DLL 加载管理器（SYSTEM/lib 兼容层）。
 *
 * PE 兼容层默认使用内置 shim 表解析 imports。shim 未命中的函数，
 * 由 DLL 管理器从 SYSTEM/lib/ 加载真实 DLL 并解析导出表。
 *
 * 架构决策：
 *   - 仅 PE32+（64 位原生）启用真实 DLL 加载
 *   - PE32（32 位解释器）不加载真实 DLL（x86emu32 不能调 64 位代码）
 *   - 不修改 pe_service 结构体 ABI（通过全局注册模式）
 *   - 递归依赖：pe_dll_load → pe_load_image → resolve_imports → pe_dll_resolve → pe_dll_load
 *   - 循环检测：DLL_LOADING 状态遇到则跳过（填 unimpl stub，安全降级）
 *   - 内存：每个 DLL 用 kmem_alloc_aligned 分配 SizeOfImage + 文件副本（arena 不释放）
 *   - 缓存上限 32 个 DLL
 */

#include <utsm/types.h>

/* DLL 文件读取回调：从 SYSTEM/lib/ 读取 DLL 到内存。
 *   name     : DLL 名（如 "foo.dll"，原样来自 PE import 表）
 *   out_data : 返回内存数据指针（调用方不负责释放）
 *   out_size : 返回字节大小
 * 返回 0 成功，负值失败。 */
typedef int (*pe_dll_reader_fn)(const char *name, u8 **out_data, u32 *out_size);

/* 全局就绪标志（1=DLL 管理器已初始化，可解析真实 DLL）。
 * pe_loader.c resolve_imports 检查此标志决定是否尝试真实 DLL。 */
extern int g_pe_dll_manager_ready;

/* 初始化 DLL 管理器（注册文件读取回调）。
 * 在 DKM 驱动加载完成、block provider 注册后调用。 */
void pe_dll_manager_init(pe_dll_reader_fn reader);

/* 加载 DLL（含递归解析其自身 import）。
 * 已加载返回缓存基址。循环依赖检测：LOADING 状态遇到则返回 -1。
 *   dll_name : DLL 名（如 "foo.dll"，大小写不敏感）
 *   out_base : 输出加载基址
 * 返回 0 成功，负值失败。 */
int pe_dll_load(const char *dll_name, u64 *out_base);

/* 按名称解析导出函数。
 * 若 DLL 未加载，自动调用 pe_dll_load。
 * 管理器未就绪时返回 0。
 *   dll_name  : DLL 名（大小写不敏感）
 *   func_name : 函数名（大小写不敏感，PE 导出表规范）
 * 返回绝对地址，0=未找到。 */
u64 pe_dll_resolve(const char *dll_name, const char *func_name);

/* 带转发深度的内部变体（转发导出链 A→B→C 递归用，外部勿调）。 */
u64 pe_dll_resolve_ex(const char *dll_name, const char *func_name, int depth);

/* 在已加载 DLL 的模块基址上解析导出（GetProcAddress 真实 DLL 路径用）。
 * image_base 必须是 pe_dll_load 返回的基址；func_name 与 ordinal 二选一
 * （ordinal != 0 时按序号）。返回绝对地址，0=未找到。 */
u64 pe_dll_resolve_in_base(u64 image_base, const char *func_name, u32 ordinal);

/* 延迟 DllMain 控制（PE32+ 加载期 SSE/大栈未就绪，DllMain 必须延迟）。
 * pe_service_run 在 pe_load_image 前调 pe_dll_defer_dllmain(1)，
 * 切换到 1MB PE 大栈后调 pe_dll_run_pending_dllmains() 统一执行。 */
void pe_dll_defer_dllmain(int enable);
void pe_dll_run_pending_dllmains(void);

#endif /* PE_DLL_MANAGER_H */
