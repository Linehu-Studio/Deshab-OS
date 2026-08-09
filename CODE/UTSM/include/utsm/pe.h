#ifndef UTSM_PE_H
#define UTSM_PE_H

/* pe.h — Deshab PE/EXE 兼容层公共接口。
 *
 * UTSM 内置 PE32+（64位）与 PE32（32位）加载器：
 *   - PE32+：解析 → 映射节区 → 基址重定位 → import 解析（Windows API shim）→ Ring0 原生执行
 *   - PE32 ：解析 → 映射节区 → 基址重定位 → import 解析 → x86-32 解释器执行
 *
 * pe_service 通过 dsk_boot_context.reserved[4] 暴露给 DSK/cmd.elf，
 * 同地址空间 Ring0 直接调用。
 */

#include <utsm/types.h>

#define PE_SERVICE_MAGIC 0x5045535643200000ULL  /* "PESVC\0\0" */

/* PE 加载结果 */
typedef struct pe_image_info {
    u64 image_base;    /* 实际加载基址（kmem_alloc_aligned 返回） */
    u64 image_size;    /* 映射后总大小（SizeOfImage） */
    u64 entry_point;   /* 绝对入口地址 = image_base + AddressOfEntryPoint */
    int  is_pe32_plus; /* 1=PE32+ 64位原生执行, 0=PE32 32位解释执行 */
    int  load_ok;      /* 1=加载成功 */
} pe_image_info;

/* ===== 窗口模式（P5 桌面嵌入）=====
 * 默认 run() 为全屏独占：shim 直写 Limine framebuffer、直接轮询 PS/2 端口。
 * run_windowed() 把 PE GUI 嵌入桌面受管窗口：
 *   - 渲染目标改为 host 提供的内存 surface（32bpp 0xAARRGGBB）
 *   - PE 消息队列空转（GetMessage/PeekMessage 无消息）时回调 host->pump()，
 *     由 desktop 驱动一帧（渲染全部窗口 + 采集输入 + 注入 PE 队列）
 *   - 输入由 host 经 inject_scancode/inject_pointer 注入（坐标已转换为
 *     surface 局部坐标，shim 内部合成 Win32 消息与修饰键状态）
 *
 * 协作模型约束：
 *   - pump 回调运行在 PE 大栈上，desktop 帧代码栈用量必须远小于 1MB；
 *   - pump 内禁止再次调用 run/run_windowed（禁止嵌套 PE 实例）；
 *   - 关闭 PE 窗口 = inject WM_CLOSE（desktop 经 inject_scancode 无此路径，
 *     使用 inject_input 直接投递消息），等待 PE 自愿退出，run 才返回。 */
typedef struct pe_window_host {
    int  (*pump)(void *ud);   /* PE 消息空转让出点；返回 0 继续，非 0 请求退出 */
    void *ud;
    u8  *surface;             /* PE 渲染目标（32bpp 0xAARRGGBB），NULL=全屏 */
    u32  width;               /* surface 宽（像素） */
    u32  height;              /* surface 高（像素） */
    u32  pitch;               /* 字节/行 */
} pe_window_host;

/* PE 服务表 — 通过 boot context reserved[4] 传递 */
typedef struct pe_service {
    u64 magic;         /* PE_SERVICE_MAGIC */
    /* 加载 PE 到内存（不执行）。返回 0 成功，info 填充。 */
    int  (*load)(const void *pe_data, u64 size, pe_image_info *out);
    /* 加载并执行 PE。cmdline = 命令行字符串（GetCommandLineA 返回值）。
     * 返回 0 成功，*exit_code 为进程退出码。 */
    int  (*run)(const void *pe_data, u64 size, const char *cmdline, u64 *exit_code);
    /* 释放 pe_load 分配的镜像内存。 */
    void (*unload)(pe_image_info *info);
    /* ===== ABI 尾部追加（P5 窗口模式）。同镜像一致性假设：
     * desktop 与 UTSM 由同一 build.ps1 产出，旧 UTSM 配新 desktop
     * 的组合现实中不存在；调用前仍须校验 magic。 ===== */
    /* 窗口模式运行：语义同 run，渲染/输入/让出按 pe_window_host 重定向。
     * host==NULL 或 host->surface==NULL 时等价 run。 */
    int  (*run_windowed)(const void *pe_data, u64 size, const char *cmdline,
                         u64 *exit_code, const pe_window_host *host);
    /* 注入键盘扫描码（set-1，bit7=release；e0_prefix!=0 表示 E0 扩展键）。
     * shim 内部完成 vk 映射/修饰键跟踪/WM_CHAR 合成，与全屏路径共用。 */
    int  (*inject_scancode)(u32 scancode, u32 e0_prefix);
    /* 注入鼠标状态（surface 局部坐标；buttons bit0=L bit1=R bit2=M）。
     * shim 内部与上次状态比较合成 MOUSEMOVE/BUTTONDOWN/UP 边沿。 */
    int  (*inject_pointer)(i32 x, i32 y, u32 buttons);
    /* 直接投递一条 Win32 消息（用于 WM_CLOSE 等宿主发起的控制消息）。 */
    int  (*inject_input)(u32 msg, u64 wparam, u64 lparam, i32 ptx, i32 pty);
} pe_service;

/* 获取全局 PE 服务实例指针（UTSM 内部使用，外部通过 boot context 获取） */
const pe_service *pe_get_service(void);

#endif /* UTSM_PE_H */
