/* pe_shim.c — Windows API shim 实现.
 *
 * 所有 64位 shim 用 __attribute__((ms_abi)) 声明,匹配 PE32+ 调用约定.
 * 32位 shim 从模拟栈读参数,供 x86emu32 解释器调用.
 *
 * 支持 API:
 *   kernel32.dll  控制台、文件、堆、模块、编码转换、QPC、Sleep、同步、卸载桩
 *   msvcrt.dll    printf、puts、malloc、free、mem 系列、str 系列、_initterm
 *   ntdll.dll     NtWriteFile、RtlNtStatusToDosError
 *   bcryptprimitives.dll  ProcessPrng (rdtsc xorshift)
 *   api-ms-win-core-synch-l1-2-0.dll  WaitOnAddress、WakeByAddress (futex 桩)
 *   user32.dll    窗口类、窗口、消息泵 (PS/2 键盘+AUX 鼠标轮询)、DefWindowProc
 *   gdi32.dll     内存 DC、DIBSection、BitBlt 到 Limine 帧缓冲
 *
 * 窗口模型:单顶层窗口 (固定伪句柄),GetDC 返回窗口 DC,
 * BitBlt(窗口 DC, ...) 把 DIB 后台缓冲按窗口原点拷入帧缓冲.
 * 消息泵:PeekMessageW 轮询 PS/2 (0x64/0x60),按状态位 0x20 分流键盘/鼠标:
 *   键盘 -> WM_KEYDOWN、WM_KEYUP、WM_CHAR;Esc 额外合成 WM_CLOSE;
 *   鼠标 -> 3 字节包解码(同步位/溢出防御),坐标钳位到帧缓冲,
 *          边沿生成 WM_MOUSEMOVE 与 WM_xBUTTONDOWN/WM_xBUTTONUP 系列.
 * 软件光标:BitBlt 上屏后在光标坐标叠画 12x12 箭头(黑边白心),
 *          下一帧 BitBlt 自动覆盖旧光标,无需保存/恢复背景.
 * 鼠标初始化:on-demand(首次轮询时),AUX 启用 + 0xF4 数据报告,
 *          全程超时保护,单步失败只记日志并禁用鼠标,绝不阻塞.
 */

#include "pe_shim.h"
#include "pe_dll_manager.h"
#include "../include/utsm/pe.h"   /* P5: pe_window_host */
#include <utsm/arena.h>
#include <utsm/log.h>
#include <utsm/panic.h>
#include <utsm/types.h>
#include "../arch/x86_64/limine.h"

/* arch/x86_64/io.c */
extern void outb(u16 port, u8 value);
extern u8 inb(u16 port);

/* dkm/boot_modules.c — Limine framebuffer 请求(GDI BitBlt 目标) */
extern volatile struct limine_framebuffer_request g_fb_request;

/* 前向声明:shim 用户态堆(实现见下文"用户态堆"一节) */
static void *shim_heap_alloc(u64 size);
static void  shim_heap_free(void *ptr);
static void *shim_heap_realloc(void *ptr, u64 size);

/* setjmp/longjmp for ExitProcess — 使用 __builtin_setjmp/__builtin_longjmp
 * (clang/gcc 内置,freestanding 可用,无需 libc).
 * __builtin_setjmp 接受 void** 缓冲区,至少需要 8 个指针槽. */
static void *g_exit_jmp[24];
static int g_exit_jmp_set;
static u64 g_exit_code;
static const char *g_cmdline = "";

/* ===== BUG-20260801-007: PE32 guest→host 指针转换 =====
 * PE32 解释器路径下,fn64 回退 shim 收到的指针参数是 32 位 guest VA
 * (相对解释器 mem 基址的偏移),直接当 host 指针解引用 → 低地址 #PF
 * → 三重故障(定案证据:#PF RIP 落在 shim_printf 体内解引用 0x401017)。
 * x86emu32 执行前经 pe_shim_set_emu_base() 设置 mem 基址,各 shim 在
 * **指针解引用点**经 gp() 转换(不在参数入口盲目转换——退出码/长度等
 * 小整数参数会被误转)。阈值 0x10000000 与 pe_service.c 解释器地址
 * 空间上限一致;shim 伪句柄(0x10000001+)与负值句柄天然豁免。
 * 原生 PE32+ 路径 g_emu_mem_base 恒为 0,gp() 恒等映射,零影响。 */
static u64 g_emu_mem_base = 0;

void pe_shim_set_emu_base(u64 base) { g_emu_mem_base = base; }

static inline u64 gp(u64 va) {
    if (g_emu_mem_base && va && va < 0x10000000ULL)
        return g_emu_mem_base + va;
    return va;
}

/* ===== P5 窗口模式状态 =====
 * 全屏模式（默认）：s_wsurf==NULL，shim 直写 Limine fb + 轮询 PS/2。
 * 窗口模式：渲染目标为 host surface（伪造 fb），输入靠 inject_*，
 * 消息空转时回调 host pump（desktop 帧）。 */
static u8  *s_wsurf = 0;
static u32 s_wsurf_w = 0, s_wsurf_h = 0, s_wsurf_pitch = 0;
static int (*s_host_pump)(void *ud) = 0;
static void *s_host_ud = 0;
static struct limine_framebuffer s_wsurf_fb;   /* 伪造 fb（窗口模式） */
static const pe_window_host *s_winhost_pending = 0;  /* service 层挂起配置 */

void pe_shim_set_window_host(const pe_window_host *host) {
    s_winhost_pending = host;
}

int pe_shim_window_mode(void) { return s_wsurf != 0; }

void pe_shim_init(const char *cmdline) {
    g_cmdline = cmdline ? cmdline : "";
    g_exit_jmp_set = 0;
    g_exit_code = 0;
    g_emu_mem_base = 0;  /* BUG-007: 默认非解释器上下文(原生路径恒等映射) */

    /* P5：应用 service 层挂起的窗口模式配置（在状态重置之后生效） */
    if (s_winhost_pending && s_winhost_pending->surface
        && s_winhost_pending->width && s_winhost_pending->height
        && s_winhost_pending->pitch) {
        s_wsurf = s_winhost_pending->surface;
        s_wsurf_w = s_winhost_pending->width;
        s_wsurf_h = s_winhost_pending->height;
        s_wsurf_pitch = s_winhost_pending->pitch;
        s_host_pump = s_winhost_pending->pump;
        s_host_ud = s_winhost_pending->ud;
        /* 伪造 fb：32bpp 0xAARRGGBB（与 BitBlt/SetDIBits fast path 掩码一致） */
        s_wsurf_fb.address = s_wsurf;
        s_wsurf_fb.width = s_wsurf_w;
        s_wsurf_fb.height = s_wsurf_h;
        s_wsurf_fb.pitch = s_wsurf_pitch;
        s_wsurf_fb.bpp = 32;
        s_wsurf_fb.memory_model = 1;
        s_wsurf_fb.red_mask_shift = 16; s_wsurf_fb.red_mask_size = 8;
        s_wsurf_fb.green_mask_shift = 8; s_wsurf_fb.green_mask_size = 8;
        s_wsurf_fb.blue_mask_shift = 0; s_wsurf_fb.blue_mask_size = 8;
        log_info("[shim] window mode: surface target");
    } else {
        s_wsurf = 0;
        s_wsurf_w = s_wsurf_h = s_wsurf_pitch = 0;
        s_host_pump = 0;
        s_host_ud = 0;
    }
    s_winhost_pending = 0;
}

/* 返回 ExitProcess longjmp 目标 jmpbuf(见 pe_shim.h 的 BUG-006 说明).
 * 本函数只提供缓冲区并重置退出状态;__builtin_setjmp 由调用方
 * (pe_service_run 本体)直接执行,确保 setjmp 帧在 PE 执行全程存活. */
void **pe_shim_exit_jmpbuf(void) {
    g_exit_jmp_set = 1;
    g_exit_code = 0;
    return g_exit_jmp;
}

void pe_shim_exit_process(u64 exit_code) {
    g_exit_code = exit_code;
    if (g_exit_jmp_set) {
        __builtin_longjmp(g_exit_jmp, 1);
    }
    /* 无 jmpbuf:直接返回 */
}

int pe_shim_is_exit_call(u64 target) {
    /* 64位 ExitProcess shim 地址 */
    extern u64 pe_shim_exitprocess64_addr(void);
    if (target == pe_shim_exitprocess64_addr()) return 1;
    /* 32位合成地址中 ExitProcess 的标记 */
    return 0;
}

const char *pe_shim_get_cmdline(void) { return g_cmdline; }

u64 pe_shim_unimpl_stub(void) {
    extern u64 pe_shim_unimpl64_addr(void);
    return pe_shim_unimpl64_addr();
}

/* ===== 串口/帧缓冲输出(shim 内部复用) ===== */
/* 单次输出长度上限:防御 PE 侧传野 (buf,len)(如 panic 递归中损坏的
 * String)把半个地址空间倒进串口并踩穿未映射页(#PF).正常日志远远
 * 低于此值;截断时留锚点便于定位. */
#define SHIM_OUTPUT_MAX (1ULL * 1024ULL * 1024ULL)

static void shim_output(const char *s, u64 len) {
    if (len > SHIM_OUTPUT_MAX) {
        log_error("[shim] output len clamped");
        log_hex64("[shim] output buf=", (u64)s);
        log_hex64("[shim] output len=", len);
        len = SHIM_OUTPUT_MAX;
    }
    for (u64 i = 0; i < len; i++) {
        serial_putc(s[i]);
    }
}

static void shim_output_str(const char *s) {
    serial_write(s);
}

/* ===== kernel32.dll shims (64位, ms_abi) ===== */

/* 线程本地 last-error(简化全局) */
static u64 g_last_error = 0;

/* 句柄定义 */
#define STD_INPUT_HANDLE  ((u64)0xFFFFFFF6)
#define STD_OUTPUT_HANDLE ((u64)0xFFFFFFF5)
#define STD_ERROR_HANDLE  ((u64)0xFFFFFFF4)
#define SHIM_STDOUT_HANDLE ((u64)0x10000001)
#define SHIM_STDERR_HANDLE ((u64)0x10000002)
#define SHIM_STDIN_HANDLE  ((u64)0x10000003)

static u64 __attribute__((ms_abi)) shim_GetStdHandle(u64 n_std_handle) {
    if (n_std_handle == STD_OUTPUT_HANDLE) return SHIM_STDOUT_HANDLE;
    if (n_std_handle == STD_ERROR_HANDLE) return SHIM_STDERR_HANDLE;
    if (n_std_handle == STD_INPUT_HANDLE) return SHIM_STDIN_HANDLE;
    return 0;
}

static u64 __attribute__((ms_abi)) shim_WriteFile(u64 handle, u64 buffer, u64 length,
                                                   u64 bytes_written_ptr, u64 overlapped) {
    (void)overlapped;
    if (handle == SHIM_STDOUT_HANDLE || handle == SHIM_STDERR_HANDLE) {
        const char *buf = (const char *)gp(buffer);
        shim_output(buf, length);
        /* lpNumberOfBytesWritten 是 DWORD*(4 字节),不能按 u64 写 */
        u64 bwp = gp(bytes_written_ptr);
        if (bwp) *(u32 *)bwp = (u32)length;
        return 1;  /* TRUE */
    }
    return 0;
}

static u64 __attribute__((ms_abi)) shim_ReadFile(u64 handle, u64 buffer, u64 max_length,
                                                  u64 bytes_read_ptr, u64 overlapped) {
    (void)handle; (void)buffer; (void)max_length; (void)overlapped;
    /* 暂不支持输入读取;lpNumberOfBytesRead 同为 DWORD*(4 字节) */
    u64 brp = gp(bytes_read_ptr);
    if (brp) *(u32 *)brp = 0;
    return 1;
}

static u64 __attribute__((ms_abi)) shim_ExitProcess(u64 exit_code) {
    pe_shim_exit_process(exit_code);
    return 0;  /* 不返回 */
}

static u64 __attribute__((ms_abi)) shim_GetCommandLineA(void) {
    return (u64)g_cmdline;
}

static u64 __attribute__((ms_abi)) shim_GetCommandLineW(void) {
    return (u64)g_cmdline;  /* 简化:返回 ASCII */
}

static u64 __attribute__((ms_abi)) shim_GetLastError(void) { return g_last_error; }
static u64 __attribute__((ms_abi)) shim_SetLastError(u64 code) { g_last_error = code; return 0; }

static u64 __attribute__((ms_abi)) shim_HeapCreate(u64 fl, u64 init, u64 max) {
    (void)fl; (void)init; (void)max;
    return 0x10000010;  /* 假堆句柄 */
}
static u64 __attribute__((ms_abi)) shim_HeapAlloc(u64 heap, u64 flags, u64 size) {
    (void)heap;
    void *p = shim_heap_alloc(size);
    if (p && (flags & 8)) {  /* HEAP_ZERO_MEMORY */
        u8 *z = (u8 *)p;
        for (u64 i = 0; i < size; i++) z[i] = 0;
    }
    return (u64)p;
}
static u64 __attribute__((ms_abi)) shim_HeapFree(u64 heap, u64 flags, u64 ptr) {
    (void)heap; (void)flags;
    /* gp():PE32 野指针收束进 guest 沙盒,避免低地址 host 解引用 */
    shim_heap_free((void *)gp(ptr));
    return 1;
}
static u64 __attribute__((ms_abi)) shim_HeapReAlloc(u64 heap, u64 flags, u64 ptr, u64 size) {
    (void)heap; (void)flags;
    return (u64)shim_heap_realloc((void *)gp(ptr), size);
}
static u64 __attribute__((ms_abi)) shim_GetProcessHeap(void) {
    return 0x10000010;
}
static u64 __attribute__((ms_abi)) shim_VirtualAlloc(u64 addr, u64 size, u64 alloc_type, u64 protect) {
    (void)addr; (void)alloc_type; (void)protect;
    return (u64)kmem_alloc(size);
}
static u64 __attribute__((ms_abi)) shim_VirtualFree(u64 addr, u64 size, u64 free_type) {
    (void)addr; (void)size; (void)free_type;
    return 1;
}
static u64 __attribute__((ms_abi)) shim_GetModuleHandleA(u64 module_name) {
    (void)module_name;
    return 0x10000000;  /* 假模块基址 */
}
static u64 __attribute__((ms_abi)) shim_GetModuleHandleW(u64 module_name) {
    (void)module_name;
    return 0x10000000;
}
static u64 __attribute__((ms_abi)) shim_GetEnvironmentVariableA(u64 name, u64 buf, u64 size) {
    (void)name; (void)buf; (void)size;
    return 0;
}
static u64 __attribute__((ms_abi)) shim_CreateFileA(u64 filename, u64 access, u64 share,
                                                     u64 sa, u64 disp, u64 flags, u64 template) {
    (void)filename; (void)access; (void)share; (void)sa; (void)disp; (void)flags; (void)template;
    return 0;  /* 暂不支持文件 */
}
static u64 __attribute__((ms_abi)) shim_CloseHandle(u64 handle) { (void)handle; return 1; }
static u64 __attribute__((ms_abi)) shim_GetFileSize(u64 handle, u64 high) {
    (void)handle; (void)high; return 0;
}
static u64 __attribute__((ms_abi)) shim_SetFilePointer(u64 handle, i64 dist, u64 high_ptr, u64 method) {
    (void)handle; (void)dist; (void)high_ptr; (void)method; return 0;
}

/* ===== msvcrt.dll shims (64位, ms_abi) ===== */

static void shim_msvcrt_output(const char *s) {
    shim_output_str(s);
}

/* printf — 简化版,只支持 %s %d %c %x %u %% */
static u64 __attribute__((ms_abi)) shim_printf(u64 fmt, u64 a1, u64 a2, u64 a3) {
    /* 可变参数:MS x64 ABI 下 a1-a3 在 RDX/R8/R9,更多参数在栈上.
     * 简化处理:只处理前 3 个参数. */
    const char *f = (const char *)gp(fmt);
    u64 args[3] = {a1, a2, a3};
    int argi = 0;
    char outbuf[512];
    int oi = 0;
    for (int i = 0; f[i] && oi < 510; i++) {
        if (f[i] == '%' && f[i+1]) {
            i++;
            switch (f[i]) {
            case 's': {
                const char *s = (const char *)gp(args[argi++ & 3]);
                if (s) while (*s && oi < 510) outbuf[oi++] = *s++;
                break;
            }
            case 'd': {
                i64 v = (i64)args[argi++ & 3];
                if (v < 0) { outbuf[oi++] = '-'; v = -v; }
                char tmp[24]; int ti = 0;
                if (v == 0) tmp[ti++] = '0';
                while (v > 0 && ti < 23) { tmp[ti++] = '0' + (v % 10); v /= 10; }
                while (ti > 0 && oi < 510) outbuf[oi++] = tmp[--ti];
                break;
            }
            case 'u': {
                u64 v = args[argi++ & 3];
                char tmp[24]; int ti = 0;
                if (v == 0) tmp[ti++] = '0';
                while (v > 0 && ti < 23) { tmp[ti++] = '0' + (v % 10); v /= 10; }
                while (ti > 0 && oi < 510) outbuf[oi++] = tmp[--ti];
                break;
            }
            case 'x': {
                u64 v = args[argi++ & 3];
                char tmp[24]; int ti = 0;
                if (v == 0) tmp[ti++] = '0';
                while (v > 0 && ti < 23) { int d = (int)(v & 0xF); tmp[ti++] = d < 10 ? '0'+d : 'a'+d-10; v >>= 4; }
                while (ti > 0 && oi < 510) outbuf[oi++] = tmp[--ti];
                break;
            }
            case 'c': {
                outbuf[oi++] = (char)(args[argi++ & 3] & 0xFF);
                break;
            }
            case '%': outbuf[oi++] = '%'; break;
            default: outbuf[oi++] = '%'; outbuf[oi++] = f[i]; break;
            }
        } else {
            outbuf[oi++] = f[i];
        }
    }
    outbuf[oi] = 0;
    shim_msvcrt_output(outbuf);
    return (u64)oi;
}

static u64 __attribute__((ms_abi)) shim_puts(u64 str) {
    const char *s = (const char *)gp(str);
    shim_msvcrt_output(s);
    shim_msvcrt_output("\n");
    return 0;
}

static u64 __attribute__((ms_abi)) shim_putchar(u64 ch) {
    serial_putc((char)(ch & 0xFF));
    return ch & 0xFF;
}

static u64 __attribute__((ms_abi)) shim_malloc(u64 size) {
    return (u64)shim_heap_alloc(size);
}
static u64 __attribute__((ms_abi)) shim_calloc(u64 count, u64 size) {
    u64 total = count * size;
    u8 *p = shim_heap_alloc(total);
    if (p) for (u64 i = 0; i < total; i++) p[i] = 0;
    return (u64)p;
}
static u64 __attribute__((ms_abi)) shim_free(u64 ptr) { shim_heap_free((void *)gp(ptr)); return 0; }
static u64 __attribute__((ms_abi)) shim_realloc(u64 ptr, u64 size) {
    return (u64)shim_heap_realloc((void *)gp(ptr), size);
}
static u64 __attribute__((ms_abi)) shim_exit(u64 code) {
    pe_shim_exit_process(code);
    return 0;
}

static u64 __attribute__((ms_abi)) shim_memcpy(u64 dst, u64 src, u64 n) {
    u8 *d = (u8 *)gp(dst); const u8 *s = (const u8 *)gp(src);
    for (u64 i = 0; i < n; i++) d[i] = s[i];
    return dst;
}
static u64 __attribute__((ms_abi)) shim_memset(u64 dst, u64 val, u64 n) {
    u8 *d = (u8 *)gp(dst); u8 v = (u8)(val & 0xFF);
    for (u64 i = 0; i < n; i++) d[i] = v;
    return dst;
}
static u64 __attribute__((ms_abi)) shim_memmove(u64 dst, u64 src, u64 n) {
    u8 *d = (u8 *)gp(dst); const u8 *s = (const u8 *)gp(src);
    if (d < s) { for (u64 i = 0; i < n; i++) d[i] = s[i]; }
    else { for (u64 i = n; i > 0; i--) d[i-1] = s[i-1]; }
    return dst;
}
static u64 __attribute__((ms_abi)) shim_strlen(u64 str) {
    const char *s = (const char *)gp(str);
    u64 n = 0; while (s[n]) n++; return n;
}
static u64 __attribute__((ms_abi)) shim_strcmp(u64 a, u64 b) {
    const char *pa = (const char *)gp(a); const char *pb = (const char *)gp(b);
    while (*pa && *pa == *pb) { pa++; pb++; }
    return (u64)(i64)(*pa - *pb);
}
static u64 __attribute__((ms_abi)) shim_strcpy(u64 dst, u64 src) {
    char *d = (char *)gp(dst); const char *s = (const char *)gp(src);
    while (*s) *d++ = *s++;
    *d = 0;
    return dst;
}
static u64 __attribute__((ms_abi)) shim_strncmp(u64 a, u64 b, u64 n) {
    const char *pa = (const char *)gp(a); const char *pb = (const char *)gp(b);
    for (u64 i = 0; i < n; i++) {
        if (pa[i] != pb[i]) return (u64)(i64)(pa[i] - pb[i]);
        if (pa[i] == 0) return 0;
    }
    return 0;
}
static u64 __attribute__((ms_abi)) shim_fwrite(u64 ptr, u64 size, u64 nmemb, u64 stream) {
    (void)stream;
    u64 total = size * nmemb;
    shim_output((const char *)gp(ptr), total);
    return nmemb;
}
static u64 __attribute__((ms_abi)) shim_fputs(u64 str, u64 stream) {
    (void)stream;
    shim_output_str((const char *)gp(str));
    return 0;
}
static u64 __attribute__((ms_abi)) shim_fputc(u64 ch, u64 stream) {
    (void)stream;
    serial_putc((char)(ch & 0xFF));
    return ch & 0xFF;
}
/* _initterm / _initterm_e — CRT 初始化,遍历函数指针表调用.
 * 简化:直接跳过(返回 0). */
static u64 __attribute__((ms_abi)) shim_initterm(u64 start, u64 end) {
    (void)start; (void)end;
    return 0;
}
static u64 __attribute__((ms_abi)) shim_initterm_e(u64 start, u64 end) {
    (void)start; (void)end;
    return 0;
}

/* ============================================================
 *  用户态堆(free-list,支持真正 free/realloc)
 *
 *  Rust std 的 GlobalAlloc 走 GetProcessHeap/HeapAlloc/HeapReAlloc/
 *  HeapFree.arena 只增不减,GUI 主循环每帧的 Vec 分配会耗尽内存,
 *  因此 shim 从 arena carve 一块 32MB 区域自行管理:
 *  32B 块头 {size,used,next_free,pad},首次适应 + 分割.
 *
 *  合并策略(2026-08-02 修复):空闲链表按地址升序维护,free 时做
 *  前向 + 后向双向合并.旧实现是 LIFO 头插 + 仅前向合并,egui 每帧
 *  内部存在非 LIFO 的 alloc/free 序列,物理相邻的空闲块永远无法
 *  后向合并,堆逐渐碎成小片,实测 ~27KB/帧的速度劣化,第 ~1200 帧
 *  (BitBlt frames=0x4b0)连 0x2a80 的连续块都分不出而 OOM,随后
 *  Rust panic 递归损坏 String,NtWriteFile 野性转储 55MB 踩穿
 *  未映射页 #PF(rip=shim_NtWriteFile+0x30).地址有序空闲链表使
 *  "物理相邻"等价于"链相邻",前后向合并即可完备回收.
 *
 *  (egui 字体 + 每帧 tessellation 缓冲实测 8MB 会在第 3 帧 OOM,扩到 32MB;
 *   arena 总 64MB,PE 镜像~3MB + 栈 1MB + DIB ~6MB,余量充足.)
 * ========================================================== */

#define SHIM_HEAP_BYTES (32ULL * 1024ULL * 1024ULL)
#define HEAP_HDR 32

typedef struct heap_blk {
    u64 size;        /* 数据区字节数(不含头) */
    u64 used;        /* 1=已分配 */
    u64 next_free;   /* 空闲链表(heap_blk*,0=尾) */
    u64 pad;
} heap_blk;

static u8 *g_heap_region;
static heap_blk *g_heap_free_head;
static u64 g_heap_live;   /* 已分配字节合计(诊断:区分真泄漏与碎片化) */

static void shim_heap_init_once(void) {
    if (g_heap_region) return;
    g_heap_region = (u8 *)kmem_alloc_aligned(SHIM_HEAP_BYTES, 16);
    if (!g_heap_region) {
        log_error("[shim] heap region alloc failed");
        return;
    }
    heap_blk *b = (heap_blk *)g_heap_region;
    b->size = SHIM_HEAP_BYTES - HEAP_HDR;
    b->used = 0;
    b->next_free = 0;
    b->pad = 0;
    g_heap_free_head = b;
}

/* 大/小块阈值:egui 长寿命缓存块多为几十字节,每帧 tessellation 等
 * 瞬态缓冲为 10-30KB.大块专取地址最高的适配块,使顶部连续区域免于
 * 被小长寿块钉碎(实测无此策略时 2400 帧后 29.9MB 空闲碎成 6 万个
 * 平均 500B 的片,free_max 只剩 10.2KB,10.8KB 请求即 OOM). */
#define SHIM_HEAP_LARGE 2048ULL

static void *shim_heap_alloc(u64 size) {
    shim_heap_init_once();
    if (!g_heap_region) return 0;
    if (size == 0) size = 16;
    size = (size + 15) & ~15ULL;
    heap_blk *prev = 0, *b = g_heap_free_head;
    heap_blk *cand = 0, *cand_prev = 0;
    if (size < SHIM_HEAP_LARGE) {
        /* 小块:首次适应(低地址优先)填碎洞,保护顶部大区域 */
        while (b) {
            if (b->size >= size) { cand = b; cand_prev = prev; break; }
            prev = b; b = (heap_blk *)b->next_free;
        }
    } else {
        /* 大块:取地址最高的适配块(地址有序链,最后一个适配者) */
        while (b) {
            if (b->size >= size) { cand = b; cand_prev = prev; }
            prev = b; b = (heap_blk *)b->next_free;
        }
    }
    b = cand; prev = cand_prev;
    if (b) {
        /* 摘除 b;分割时余量 nb 占据 b 原链位即可保持地址有序
         * (nb=b+HEAP_HDR+size < 原后继:后继不与 b 重叠 => 后继>=b 块尾 > nb) */
        if (prev) prev->next_free = b->next_free;
        else g_heap_free_head = (heap_blk *)b->next_free;
        if (b->size >= size + HEAP_HDR + 16) {
            heap_blk *nb = (heap_blk *)((u8 *)b + HEAP_HDR + size);
            nb->size = b->size - size - HEAP_HDR;
            nb->used = 0;
            nb->pad = 0;
            nb->next_free = b->next_free;
            if (prev) prev->next_free = (u64)nb;
            else g_heap_free_head = nb;
            b->size = size;
        }
        b->used = 1;
        b->next_free = 0;
        g_heap_live += b->size;
        return (u8 *)b + HEAP_HDR;
    }
    /* OOM 诊断:live 持续增长=真泄漏;live 平稳而 free_max 萎缩=碎片化 */
    u64 ft = 0, fm = 0, fc = 0;
    for (heap_blk *f = g_heap_free_head; f; f = (heap_blk *)f->next_free) {
        ft += f->size; if (f->size > fm) fm = f->size; fc++;
    }
    log_error("[shim] heap OOM");
    log_hex64("[shim] heap req=", size);
    log_hex64("[shim] heap live=", g_heap_live);
    log_hex64("[shim] heap free_total=", ft);
    log_hex64("[shim] heap free_max=", fm);
    log_hex64("[shim] heap free_blocks=", fc);
    return 0;
}

static void shim_heap_free(void *ptr) {
    if (!ptr) return;
    heap_blk *b = (heap_blk *)((u8 *)ptr - HEAP_HDR);
    g_heap_live -= b->size;
    b->used = 0;
    /* 地址升序插入空闲链表:prev < b < x(x 可为 0 表尾) */
    heap_blk *prev = 0, *x = g_heap_free_head;
    while (x && x < b) { prev = x; x = (heap_blk *)x->next_free; }
    b->next_free = (u64)x;
    if (prev) prev->next_free = (u64)b;
    else g_heap_free_head = b;
    /* 前向合并:x 与 b 物理相邻则吸收(地址有序 => 物理相邻即链相邻) */
    if (x && (u8 *)b + HEAP_HDR + b->size == (u8 *)x) {
        b->size += HEAP_HDR + x->size;
        b->next_free = x->next_free;
    }
    /* 后向合并:prev 与 b 物理相邻则把 b 并入 prev */
    if (prev && (u8 *)prev + HEAP_HDR + prev->size == (u8 *)b) {
        prev->size += HEAP_HDR + b->size;
        prev->next_free = b->next_free;
    }
}

static void *shim_heap_realloc(void *ptr, u64 size) {
    if (!ptr) return shim_heap_alloc(size);
    heap_blk *b = (heap_blk *)((u8 *)ptr - HEAP_HDR);
    u64 old = b->size;
    if (size <= old) return ptr;
    u8 *np = (u8 *)shim_heap_alloc(size);
    if (!np) return 0;
    const u8 *s = (const u8 *)ptr;
    for (u64 i = 0; i < old; i++) np[i] = s[i];
    shim_heap_free(ptr);
    return np;
}

/* ============================================================
 *  TSC 计时(QueryPerformanceCounter/Frequency、GetTickCount、Sleep)
 *  PIT channel 0 一次性校准(与 cmd.elf 同款手法).
 * ========================================================== */

static __inline__ u64 shim_rdtsc(void) {
    u32 lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((u64)hi << 32) | lo;
}

static u64 g_tsc_per_ms = 0;

static void shim_tsc_init(void) {
    if (g_tsc_per_ms) return;
    outb(0x43, 0x30);
    outb(0x40, 0x7c);
    outb(0x40, 0x2e);
    u64 t0 = shim_rdtsc();
    u16 prev = 0; u64 loops = 0;
    for (;;) {
        outb(0x43, 0x00);
        u16 cur = (u16)inb(0x40) | ((u16)inb(0x40) << 8);
        if (cur > prev && loops > 10) break;
        prev = cur; loops++;
    }
    u64 t1 = shim_rdtsc();
    g_tsc_per_ms = (t1 - t0) / 10;
    if (!g_tsc_per_ms) g_tsc_per_ms = 1000000;  /* 兜底 1GHz */
}

static u64 __attribute__((ms_abi)) shim_QueryPerformanceFrequency(u64 freq_ptr) {
    shim_tsc_init();
    freq_ptr = gp(freq_ptr);
    if (freq_ptr) *(u64 *)freq_ptr = g_tsc_per_ms * 1000;
    return 1;
}
static u64 __attribute__((ms_abi)) shim_QueryPerformanceCounter(u64 count_ptr) {
    shim_tsc_init();
    count_ptr = gp(count_ptr);
    if (count_ptr) *(u64 *)count_ptr = shim_rdtsc();
    return 1;
}
static u64 __attribute__((ms_abi)) shim_GetTickCount(void) {
    shim_tsc_init();
    return shim_rdtsc() / g_tsc_per_ms;
}
static u64 __attribute__((ms_abi)) shim_Sleep(u64 ms) {
    shim_tsc_init();
    u64 end = shim_rdtsc() + ms * g_tsc_per_ms;
    while (shim_rdtsc() < end) __asm__ volatile("pause");
    return 0;
}

/* SYSTEM_INFO(x64,48 字节) */
static u64 __attribute__((ms_abi)) shim_GetSystemInfo(u64 si_p) {
    si_p = gp(si_p);
    if (!si_p) return 0;
    u8 *p = (u8 *)si_p;
    for (int i = 0; i < 48; i++) p[i] = 0;
    *(u32 *)(p + 0)  = 9;        /* PROCESSOR_ARCHITECTURE_AMD64 */
    *(u32 *)(p + 4)  = 4096;     /* dwPageSize */
    *(u32 *)(p + 20) = 1;        /* dwNumberOfProcessors */
    return 0;
}

/* ============================================================
 *  kernel32 增量:控制台/目录/环境/进程/同步/Rtl/编码转换
 * ========================================================== */

static u64 __attribute__((ms_abi)) shim_GetConsoleMode(u64 handle, u64 mode_ptr) {
    (void)handle; (void)mode_ptr;
    g_last_error = 6;  /* ERROR_INVALID_HANDLE -> std 回退 WriteFile */
    return 0;
}
static u64 __attribute__((ms_abi)) shim_GetConsoleOutputCP(void) { return 65001; }

static u64 __attribute__((ms_abi)) shim_WriteConsoleW(u64 handle, u64 buf, u64 nchars,
                                                       u64 written_ptr, u64 reserved) {
    (void)reserved;
    if (handle != SHIM_STDOUT_HANDLE && handle != SHIM_STDERR_HANDLE) return 0;
    const u16 *s = (const u16 *)gp(buf);
    for (u64 i = 0; i < nchars; i++) {
        u32 c = s[i];
        if (c < 0x80) {
            serial_putc((char)c);
        } else if (c < 0x800) {
            serial_putc((char)(0xC0 | (c >> 6)));
            serial_putc((char)(0x80 | (c & 0x3F)));
        } else {
            serial_putc((char)(0xE0 | (c >> 12)));
            serial_putc((char)(0x80 | ((c >> 6) & 0x3F)));
            serial_putc((char)(0x80 | (c & 0x3F)));
        }
    }
    written_ptr = gp(written_ptr);
    if (written_ptr) *(u32 *)written_ptr = (u32)nchars;
    return 1;
}

static u64 __attribute__((ms_abi)) shim_GetCurrentDirectoryW(u64 buflen, u64 buf) {
    static const u16 cwd[] = { 'C', ':', '\\', 0 };
    buf = gp(buf);
    if (!buf || buflen < 4) return 4;  /* 需要 4 个字符(含 NUL) */
    u16 *out = (u16 *)buf;
    for (int i = 0; i < 4; i++) out[i] = cwd[i];
    return 3;  /* 不含 NUL 的字符数 */
}

static u64 __attribute__((ms_abi)) shim_GetEnvironmentVariableW(u64 name, u64 buf, u64 size) {
    (void)name; (void)buf; (void)size;
    g_last_error = 203;  /* ERROR_ENVVAR_NOT_FOUND */
    return 0;
}

static u64 __attribute__((ms_abi)) shim_GetCurrentProcess(void) { return (u64)-1; }
static u64 __attribute__((ms_abi)) shim_GetCurrentProcessId(void) { return 1; }
static u64 __attribute__((ms_abi)) shim_GetCurrentThreadId(void) { return 1; }

static u64 __attribute__((ms_abi)) shim_CreateMutexA(u64 sa, u64 initial, u64 name) {
    (void)sa; (void)initial; (void)name;
    return 0x10000030;
}
static u64 __attribute__((ms_abi)) shim_ReleaseMutex(u64 h) { (void)h; return 1; }
static u64 __attribute__((ms_abi)) shim_WaitForSingleObject(u64 h, u64 ms) {
    (void)h; (void)ms; return 0;  /* WAIT_OBJECT_0 */
}
static u64 __attribute__((ms_abi)) shim_WaitForSingleObjectEx(u64 h, u64 ms, u64 alert) {
    (void)h; (void)ms; (void)alert; return 0;
}

static u64 __attribute__((ms_abi)) shim_FormatMessageW(u64 flags, u64 src, u64 msgid,
                                                        u64 lang, u64 buf, u64 size, u64 args) {
    (void)flags; (void)src; (void)msgid; (void)lang; (void)buf; (void)size; (void)args;
    return 0;
}

static u64 __attribute__((ms_abi)) shim_LoadLibraryA(u64 name_p) {
    name_p = gp(name_p);
    if (!name_p) return 0;
    const char *n = (const char *)name_p;
    /* 已知 shim DLL 返回伪句柄(大小写不敏感子串匹配) */
    static const char *known[] = { "kernel32", "ntdll", "user32", "gdi32", "msvcrt" };
    for (int k = 0; k < 5; k++) {
        const char *pat = known[k];
        for (const char *p = n; *p; p++) {
            int i = 0;
            while (pat[i] && p[i]) {
                char a = p[i], b = pat[i];
                if (a >= 'A' && a <= 'Z') a += 32;
                if (a != b) break;
                i++;
            }
            if (pat[i] == 0) return PE_SHIM_FAKE_MODULE;
        }
    }
    /* 未知 DLL：尝试从 SYSTEM/lib 加载真实 DLL（句柄=镜像基址） */
    if (g_pe_dll_manager_ready) {
        char nb[64];
        u32 i = 0;
        while (n[i] && i + 1 < sizeof(nb)) { nb[i] = n[i]; i++; }
        nb[i] = 0;
        u64 base = 0;
        if (pe_dll_load(nb, &base) == 0 && base != 0) {
            log_info("[shim] LoadLibrary real DLL ok");
            serial_write("  -> ");
            serial_write(nb);
            serial_write("\n");
            return base;
        }
    }
    g_last_error = 126;  /* ERROR_MOD_NOT_FOUND */
    return 0;
}

static u64 __attribute__((ms_abi)) shim_lstrlenW(u64 str_p) {
    const u16 *s = (const u16 *)gp(str_p);
    u64 n = 0;
    if (s) while (s[n]) n++;
    return n;
}

/* GetProcAddress — 伪句柄在 shim 表内按函数名全局查找(不限定 DLL);
 * 真实 DLL 句柄(pe_dll_load 返回的镜像基址)在该 DLL 导出表内解析。
 * lpProcName 高 16 位为 0 时是 ordinal。 */
static u64 __attribute__((ms_abi)) shim_GetProcAddress(u64 module, u64 name_p) {
    name_p = gp(name_p);
    if (!name_p) return 0;
    /* 真实 DLL 模块：按基址解析（支持名称与序号） */
    if (module && module != PE_SHIM_FAKE_MODULE && g_pe_dll_manager_ready) {
        u32 ord = ((name_p >> 16) == 0) ? (u32)(name_p & 0xFFFF) : 0;
        u64 addr = pe_dll_resolve_in_base(module,
                        ord ? 0 : (const char *)name_p, ord);
        if (addr) return addr;
        g_last_error = 127;             /* ERROR_PROC_NOT_FOUND */
        return 0;
    }
    (void)module;
    if ((name_p >> 16) == 0) {          /* ordinal 导入 */
        g_last_error = 127;             /* ERROR_PROC_NOT_FOUND */
        return 0;
    }
    const char *name = (const char *)name_p;
    int n = pe_shim_count();
    for (int i = 0; i < n; i++) {
        const pe_shim_entry *e = pe_shim_get(i);
        if (!e || !e->func || !e->fn64) continue;
        const char *a = e->func, *b = name;
        while (*a && *b && *a == *b) { a++; b++; }
        if (*a == *b) return (u64)e->fn64;
    }
    log_info("[shim] GetProcAddress miss");
    serial_write("  -> ");
    serial_write(name);
    serial_write("\n");
    g_last_error = 127;
    return 0;
}

/* Rtl* — Rust panic/unwind 机制引用(panic=abort 下不会真正走到) */
static u64 __attribute__((ms_abi)) shim_RtlCaptureContext(u64 ctx_p) {
    u8 *p = (u8 *)gp(ctx_p);
    if (p) for (int i = 0; i < 1232; i++) p[i] = 0;  /* x64 CONTEXT ≈ 1232B */
    return 0;
}
static u64 __attribute__((ms_abi)) shim_RtlLookupFunctionEntry(u64 rip, u64 base_p, u64 table_p) {
    (void)rip;
    base_p = gp(base_p); table_p = gp(table_p);
    if (base_p) *(u64 *)base_p = 0;
    if (table_p) *(u64 *)table_p = 0;
    return 0;
}
static u64 __attribute__((ms_abi)) shim_RtlVirtualUnwind(u64 a, u64 b, u64 c, u64 d,
                                                          u64 e, u64 f, u64 g, u64 h, u64 i2) {
    (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g; (void)h; (void)i2;
    return 0;
}

/* ---- MultiByteToWideChar / WideCharToMultiByte(CP_UTF8 + Latin-1)---- */
static u64 __attribute__((ms_abi)) shim_MultiByteToWideChar(u64 cp, u64 flags, u64 mbs_p,
                                                             u64 cbmb, u64 wbuf_p, u64 cchw) {
    (void)flags;
    const u8 *s = (const u8 *)gp(mbs_p);
    u16 *out = (u16 *)gp(wbuf_p);
    i64 remain = (i64)cbmb;  /* -1 = NUL 结尾 */
    u64 need = 0;
    if (!s) return 0;
    for (;;) {
        if (remain == 0) break;
        u8 b0 = s[0];
        if (b0 == 0) {
            if (remain < 0) {  /* -1 模式把 NUL 计入 */
                if (out && cchw > need) out[need] = 0;
                need++;
            }
            break;
        }
        u32 cp32; u64 adv;
        if (cp == 65001 && b0 >= 0xC2 && b0 < 0xE0 && (remain < 0 || remain >= 2)
            && (s[1] & 0xC0) == 0x80) {
            cp32 = ((u32)(b0 & 0x1F) << 6) | (s[1] & 0x3F); adv = 2;
        } else if (cp == 65001 && b0 >= 0xE0 && b0 < 0xF0 && (remain < 0 || remain >= 3)
                   && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
            cp32 = ((u32)(b0 & 0x0F) << 12) | ((u32)(s[1] & 0x3F) << 6) | (s[2] & 0x3F); adv = 3;
        } else if (cp == 65001 && b0 >= 0xF0 && b0 < 0xF5 && (remain < 0 || remain >= 4)
                   && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80) {
            cp32 = ((u32)(b0 & 0x07) << 18) | ((u32)(s[1] & 0x3F) << 12)
                 | ((u32)(s[2] & 0x3F) << 6) | (s[3] & 0x3F); adv = 4;
        } else {
            cp32 = b0; adv = 1;  /* Latin-1 / 非法序列按字节 */
        }
        if (cp32 > 0xFFFF) {
            if (out && cchw > need + 1) {
                cp32 -= 0x10000;
                out[need] = (u16)(0xD800 + (cp32 >> 10));
                out[need + 1] = (u16)(0xDC00 + (cp32 & 0x3FF));
            }
            need += 2;
        } else {
            if (out && cchw > need) out[need] = (u16)cp32;
            need++;
        }
        s += adv;
        if (remain > 0) remain -= (i64)adv;
    }
    return need;
}

static u64 __attribute__((ms_abi)) shim_WideCharToMultiByte(u64 cp, u64 flags, u64 ws_p,
                                                             u64 cchw, u64 mbs_p, u64 cbmb,
                                                             u64 defc, u64 used_p) {
    (void)flags; (void)defc;
    const u16 *s = (const u16 *)gp(ws_p);
    u8 *out = (u8 *)gp(mbs_p);
    i64 remain = (i64)cchw;  /* -1 = NUL 结尾 */
    u64 need = 0;
    used_p = gp(used_p);
    if (used_p) *(u32 *)used_p = 0;
    if (!s) return 0;
    for (;;) {
        if (remain == 0) break;
        u16 w = s[0];
        if (w == 0) {
            if (remain < 0) {
                if (out && cbmb > need) out[need] = 0;
                need++;
            }
            break;
        }
        u32 cp32 = w; u64 adv = 1;
        if (w >= 0xD800 && w < 0xDC00 && (remain < 0 || remain >= 2)
            && s[1] >= 0xDC00 && s[1] < 0xE000) {
            cp32 = 0x10000 + (((u32)(w - 0xD800) << 10) | (s[1] - 0xDC00));
            adv = 2;
        }
        u8 enc[4]; u64 elen;
        if (cp == 65001) {
            if (cp32 < 0x80) { enc[0] = (u8)cp32; elen = 1; }
            else if (cp32 < 0x800) {
                enc[0] = (u8)(0xC0 | (cp32 >> 6)); enc[1] = (u8)(0x80 | (cp32 & 0x3F)); elen = 2;
            } else if (cp32 < 0x10000) {
                enc[0] = (u8)(0xE0 | (cp32 >> 12)); enc[1] = (u8)(0x80 | ((cp32 >> 6) & 0x3F));
                enc[2] = (u8)(0x80 | (cp32 & 0x3F)); elen = 3;
            } else {
                enc[0] = (u8)(0xF0 | (cp32 >> 18)); enc[1] = (u8)(0x80 | ((cp32 >> 12) & 0x3F));
                enc[2] = (u8)(0x80 | ((cp32 >> 6) & 0x3F)); enc[3] = (u8)(0x80 | (cp32 & 0x3F));
                elen = 4;
            }
        } else {
            enc[0] = (u8)(cp32 < 256 ? cp32 : '?'); elen = 1;
        }
        if (out) for (u64 i = 0; i < elen; i++) { if (cbmb > need + i) out[need + i] = enc[i]; }
        need += elen;
        s += adv;
        if (remain > 0) remain -= (i64)adv;
    }
    return need;
}

/* ============================================================
 *  ntdll.dll / bcryptprimitives.dll / api-ms-win-core-synch
 * ========================================================== */

static u64 __attribute__((ms_abi)) shim_NtWriteFile(u64 fh, u64 ev, u64 apc_r, u64 apc_c,
                                                     u64 iosb, u64 buf, u64 len,
                                                     u64 off, u64 key) {
    (void)ev; (void)apc_r; (void)apc_c; (void)off; (void)key;
    if (fh == SHIM_STDOUT_HANDLE || fh == SHIM_STDERR_HANDLE) {
        shim_output((const char *)gp(buf), len);
    }
    iosb = gp(iosb);
    if (iosb) {  /* IO_STATUS_BLOCK: {NTSTATUS Status; ULONG_PTR Information} */
        ((u64 *)iosb)[0] = 0;
        ((u64 *)iosb)[1] = len;
    }
    return 0;  /* STATUS_SUCCESS */
}
static u64 __attribute__((ms_abi)) shim_RtlNtStatusToDosError(u64 status) {
    return status & 0xFFFFFFFF;
}

static u64 __attribute__((ms_abi)) shim_ProcessPrng(u64 buf, u64 len) {
    u64 s = shim_rdtsc() ^ 0x9E3779B97F4A7C15ULL;
    u8 *p = (u8 *)gp(buf);
    for (u64 i = 0; i < len; i++) {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;  /* xorshift64 */
        p[i] = (u8)(s & 0xFF);
    }
    return 1;
}

/* WaitOnAddress 允许虚假唤醒(调用者自旋重检),立即返回 TRUE 合法 */
static u64 __attribute__((ms_abi)) shim_WaitOnAddress(u64 addr, u64 expected, u64 size, u64 timeout) {
    (void)addr; (void)expected; (void)size; (void)timeout;
    return 1;
}
static u64 __attribute__((ms_abi)) shim_WakeByAddress(u64 addr) { (void)addr; return 0; }

/* ============================================================
 *  user32.dll — 单窗口 + 消息泵(PS/2 键盘 + AUX 鼠标轮询)
 * ========================================================== */

#define SHIM_HWND      0x20000001ULL
#define SHIM_HDC_WND   0x20000002ULL
#define SHIM_HCURSOR   0x20000003ULL

#define SHIM_WM_CLOSE     0x0010u
#define SHIM_WM_QUIT      0x0012u
#define SHIM_WM_KEYDOWN   0x0100u
#define SHIM_WM_KEYUP     0x0101u
#define SHIM_WM_CHAR      0x0102u
#define SHIM_WM_MOUSEMOVE    0x0200u
#define SHIM_WM_LBUTTONDOWN  0x0201u
#define SHIM_WM_LBUTTONUP    0x0202u
#define SHIM_WM_RBUTTONDOWN  0x0204u
#define SHIM_WM_RBUTTONUP    0x0205u
#define SHIM_WM_MBUTTONDOWN  0x0207u
#define SHIM_WM_MBUTTONUP    0x0208u
/* wparam MK_* 修饰位 */
#define SHIM_MK_LBUTTON   0x0001u
#define SHIM_MK_RBUTTON   0x0002u
#define SHIM_MK_SHIFT     0x0004u
#define SHIM_MK_CONTROL   0x0008u
#define SHIM_MK_MBUTTON   0x0010u

static u64 g_wnd_proc = 0;
static int g_wnd_created = 0;
static i32 g_wnd_x = 0, g_wnd_y = 0, g_wnd_w = 0, g_wnd_h = 0;

/* MSG 布局与 PE 侧 #[repr(C)] MSG 一致(48 字节) */
typedef struct {
    u64 hwnd;
    u32 message; u32 pad0;
    u64 wparam;
    u64 lparam;
    u32 time; u32 pad1;
    i32 ptx, pty;
} shim_msg_t;

#define SHIM_MSGQ_CAP 64
static shim_msg_t g_msgq[SHIM_MSGQ_CAP];
static int g_mq_head = 0, g_mq_count = 0;

static u32 g_mq_drops = 0;  /* 队列满丢弃计数(诊断) */

static void shim_msg_push_pt(u32 msg, u64 w, u64 l, i32 ptx, i32 pty) {
    if (g_mq_count >= SHIM_MSGQ_CAP) {
        if (g_mq_drops < 4) { g_mq_drops++; log_hex64("[shim] msgq drop msg=", msg); }
        return;
    }
    int idx = (g_mq_head + g_mq_count) % SHIM_MSGQ_CAP;
    g_msgq[idx].hwnd = SHIM_HWND;
    g_msgq[idx].message = msg;
    g_msgq[idx].pad0 = 0;
    g_msgq[idx].wparam = w;
    g_msgq[idx].lparam = l;
    g_msgq[idx].time = 0;
    g_msgq[idx].pad1 = 0;
    g_msgq[idx].ptx = ptx;
    g_msgq[idx].pty = pty;
    g_mq_count++;
}

static void shim_msg_push(u32 msg, u64 w, u64 l) {
    shim_msg_push_pt(msg, w, l, 0, 0);
}

/* PS/2 键盘状态 */
static int s_kbd_shift = 0, s_kbd_ctrl = 0, s_kbd_alt = 0, s_kbd_e0 = 0;

static char shim_scan_ascii(u8 sc, int shift) {
    static const char normal[58] = {
        0, 27, '1','2','3','4','5','6','7','8','9','0','-','=', 8, '\t',
        'q','w','e','r','t','y','u','i','o','p','[',']','\n',0,
        'a','s','d','f','g','h','j','k','l',';','\'', '`',0,'\\',
        'z','x','c','v','b','n','m',',','.','/',0,'*',0,' '
    };
    static const char shifted[58] = {
        0, 27, '!','@','#','$','%','^','&','*','(',')','_','+', 8, '\t',
        'Q','W','E','R','T','Y','U','I','O','P','{','}','\n',0,
        'A','S','D','F','G','H','J','K','L',':','"','~',0,'|',
        'Z','X','C','V','B','N','M','<','>','?',0,'*',0,' '
    };
    if (sc >= 58) return 0;
    return shift ? shifted[sc] : normal[sc];
}

/* scan code set 1 -> Win32 VK */
static u32 shim_scan_vk(u8 sc, int e0) {
    if (e0) {
        switch (sc) {
        case 0x4B: return 0x25;  /* VK_LEFT */
        case 0x48: return 0x26;  /* VK_UP */
        case 0x4D: return 0x27;  /* VK_RIGHT */
        case 0x50: return 0x28;  /* VK_DOWN */
        case 0x53: return 0x2E;  /* VK_DELETE */
        case 0x1D: return 0x11;  /* VK_CONTROL(右) */
        case 0x38: return 0x12;  /* VK_MENU(右) */
        default: return 0;
        }
    }
    /* 字母三行:scancode 顺序与 ASCII 序不一致(QWERTY 布局),且 'Z'+1='['
     * 在 ASCII 中不连续,必须用显式表,不能算术偏移(此前 shift-b 出 0x5E). */
    static const u8 vk_row_q[10] = { 'Q','W','E','R','T','Y','U','I','O','P' };
    static const u8 vk_row_a[9]  = { 'A','S','D','F','G','H','J','K','L' };
    static const u8 vk_row_z[7]  = { 'Z','X','C','V','B','N','M' };
    if (sc >= 0x10 && sc <= 0x19) return vk_row_q[sc - 0x10];
    if (sc >= 0x1E && sc <= 0x26) return vk_row_a[sc - 0x1E];
    if (sc >= 0x2C && sc <= 0x32) return vk_row_z[sc - 0x2C];
    /* 数字行 scancode 与 ASCII 均连续,算术偏移正确 */
    if (sc >= 0x02 && sc <= 0x0A) return '1' + (sc - 0x02);
    if (sc == 0x0B) return '0';
    switch (sc) {
    case 0x1C: return 0x0D;  /* VK_RETURN */
    case 0x0E: return 0x08;  /* VK_BACK */
    case 0x39: return 0x20;  /* VK_SPACE */
    case 0x01: return 0x1B;  /* VK_ESCAPE */
    case 0x0F: return 0x09;  /* VK_TAB */
    case 0x2A: case 0x36: return 0x10;  /* VK_SHIFT */
    case 0x1D: return 0x11;  /* VK_CONTROL */
    case 0x38: return 0x12;  /* VK_MENU */
    default: return 0;
    }
}

/* 前向声明(shim_fb 定义在下方 RegisterClassExW 之后) */
static const struct limine_framebuffer *shim_fb(void);

/* ============================================================
 *  PS/2 AUX 鼠标(轮询模式,无 IRQ12)
 *
 *  初始化:on-demand(首次消息泵轮询时执行),启用 AUX 端口 +
 *  清配置字节 bit5(启用 AUX 时钟) + 发 0xF4 启用数据报告.
 *  全程超时保护:任何单步失败只记日志并永久禁用鼠标,绝不阻塞
 *  GUI 主循环.包解码:3 字节(同步位 bit3 校验,9 位有符号位移,
 *  溢出位丢弃位移),坐标钳位到帧缓冲,边沿合成鼠标消息.
 * ========================================================== */

static int s_mouse_inited = 0;
static int s_mouse_ok = 0;
static i32 s_mouse_x = 512, s_mouse_y = 384;  /* 屏幕坐标 */
static u8 s_mouse_btn = 0;                     /* bit0=L bit1=R bit2=M */
static u8 s_mpkt[3];
static int s_mpkt_n = 0;
static int s_dbg_aux = 0;   /* 调试:前 16 个 AUX 字节计数 */
static int s_dbg_pkt = 0;   /* 调试:前 8 个完整包计数 */
static int s_dbg_key = 0;   /* 调试:前 16 个键盘消息计数(KEYDOWN/KEYUP/CHAR) */

static int ps2_wait_ibf_clear(u32 spins) {
    while (spins--) { if (!(inb(0x64) & 2)) return 0; }
    return -1;
}
static int ps2_wait_obf_set(u32 spins) {
    while (spins--) { if (inb(0x64) & 1) return 0; }
    return -1;
}
/* 经 0xD4 转发命令到鼠标并等 ACK(0xFA) */
static int ps2_aux_cmd(u8 v) {
    if (ps2_wait_ibf_clear(100000) < 0) return -1;
    outb(0x64, 0xD4);
    if (ps2_wait_ibf_clear(100000) < 0) return -1;
    outb(0x60, v);
    if (ps2_wait_obf_set(100000) < 0) return -1;
    return inb(0x60) == 0xFA ? 0 : -1;
}

static void shim_mouse_init_once(void) {
    if (s_mouse_inited) return;
    s_mouse_inited = 1;
    /* 排空控制器残留输出字节 */
    for (int i = 0; i < 16 && (inb(0x64) & 1); i++) (void)inb(0x60);
    /* 启用 AUX 端口 */
    if (ps2_wait_ibf_clear(100000) < 0) { log_warn("[shim] mouse: ctrl busy"); return; }
    outb(0x64, 0xA8);
    /* 读配置字节:清 bit5(启用 AUX 时钟),置 bit1(IRQ12 允许,轮询下无害) */
    if (ps2_wait_ibf_clear(100000) < 0) { log_warn("[shim] mouse: ctrl busy2"); return; }
    outb(0x64, 0x20);
    if (ps2_wait_obf_set(100000) < 0) { log_warn("[shim] mouse: no cfg byte"); return; }
    u8 cfg = inb(0x60);
    cfg = (u8)((cfg & ~0x20u) | 0x02u);
    if (ps2_wait_ibf_clear(100000) < 0) { log_warn("[shim] mouse: cfg wbusy"); return; }
    outb(0x64, 0x60);
    if (ps2_wait_ibf_clear(100000) < 0) { log_warn("[shim] mouse: cfg wbusy2"); return; }
    outb(0x60, cfg);
    /* 启用流模式数据报告 */
    if (ps2_aux_cmd(0xF4) < 0) { log_warn("[shim] mouse: enable-report nack"); return; }
    s_mouse_ok = 1;
    log_info("[shim] mouse init ok (AUX streaming, polled)");
}

/* 鼠标 1 字节 -> 3 字节包 -> Win32 消息 */
static void shim_mouse_byte(u8 b) {
    if (s_mpkt_n == 0 && !(b & 0x08)) return;  /* byte0 同步位(bit3)必须为 1,否则丢弃重同步 */
    s_mpkt[s_mpkt_n++] = b;
    if (s_mpkt_n < 3) return;
    s_mpkt_n = 0;

    u8 f = s_mpkt[0];
    i32 dx = (i32)s_mpkt[1] - ((f & 0x10) ? 0x100 : 0);  /* 9 位有符号位移 */
    i32 dy = (i32)s_mpkt[2] - ((f & 0x20) ? 0x100 : 0);
    if (f & 0x40) dx = 0;  /* X 溢出:位移不可信,只取按钮 */
    if (f & 0x80) dy = 0;  /* Y 溢出 */
    if (s_dbg_pkt < 8) {
        s_dbg_pkt++;
        log_hex64("[shim] mpkt f=", f);
        log_hex64("[shim] mpkt dx=", (u64)(i64)dx);
        log_hex64("[shim] mpkt dy=", (u64)(i64)dy);
    }

    const struct limine_framebuffer *fb = shim_fb();
    i32 fw = fb ? (i32)fb->width : 1024;
    i32 fh = fb ? (i32)fb->height : 768;
    i32 nx = s_mouse_x + dx;
    i32 ny = s_mouse_y - dy;  /* PS/2 Y 向前(屏幕上方向)为正 -> 屏幕坐标取反 */
    if (nx < 0) nx = 0; else if (nx >= fw) nx = fw - 1;
    if (ny < 0) ny = 0; else if (ny >= fh) ny = fh - 1;

    u8 nbtn = (u8)(f & 0x07);
    i32 cx = nx - g_wnd_x, cy = ny - g_wnd_y;  /* 客户区坐标(消息 lparam) */
    u64 lp = (u64)(u32)(((u32)(u16)cy << 16) | (u32)(u16)cx);
    u64 mk = 0;
    if (nbtn & 1) mk |= SHIM_MK_LBUTTON;
    if (nbtn & 2) mk |= SHIM_MK_RBUTTON;
    if (nbtn & 4) mk |= SHIM_MK_MBUTTON;
    if (s_kbd_shift) mk |= SHIM_MK_SHIFT;
    if (s_kbd_ctrl) mk |= SHIM_MK_CONTROL;

    if (nx != s_mouse_x || ny != s_mouse_y) {
        s_mouse_x = nx; s_mouse_y = ny;
        shim_msg_push_pt(SHIM_WM_MOUSEMOVE, mk, lp, nx, ny);
    }
    /* 按钮边沿(按钮事件罕见,逐一打点:msg/客户区坐标) */
    if ((nbtn & 1) && !(s_mouse_btn & 1)) { shim_msg_push_pt(SHIM_WM_LBUTTONDOWN, mk, lp, nx, ny); log_hex64("[shim] LDOWN lp=", lp); }
    if (!(nbtn & 1) && (s_mouse_btn & 1)) { shim_msg_push_pt(SHIM_WM_LBUTTONUP, mk, lp, nx, ny); log_hex64("[shim] LUP   lp=", lp); }
    if ((nbtn & 2) && !(s_mouse_btn & 2)) shim_msg_push_pt(SHIM_WM_RBUTTONDOWN, mk, lp, nx, ny);
    if (!(nbtn & 2) && (s_mouse_btn & 2)) shim_msg_push_pt(SHIM_WM_RBUTTONUP, mk, lp, nx, ny);
    if ((nbtn & 4) && !(s_mouse_btn & 4)) shim_msg_push_pt(SHIM_WM_MBUTTONDOWN, mk, lp, nx, ny);
    if (!(nbtn & 4) && (s_mouse_btn & 4)) shim_msg_push_pt(SHIM_WM_MBUTTONUP, mk, lp, nx, ny);
    s_mouse_btn = nbtn;
}

/* 单个 set-1 扫描码 -> Win32 消息入队（P5 抽出：全屏 PS/2 轮询与
 * 窗口模式 host 注入共用同一份 vk 映射/修饰键跟踪/WM_CHAR 合成）。
 * sc: 原始扫描码（bit7=release）；e0: 1=E0 扩展键。 */
static void shim_kbd_inject(u8 sc, int e0) {
    if (!e0) {
        if (sc == 0x2A || sc == 0x36) { s_kbd_shift = 1; shim_msg_push(SHIM_WM_KEYDOWN, 0x10, sc); return; }
        if (sc == 0xAA || sc == 0xB6) { s_kbd_shift = 0; shim_msg_push(SHIM_WM_KEYUP, 0x10, sc); return; }
        if (sc == 0x1D) { s_kbd_ctrl = 1; shim_msg_push(SHIM_WM_KEYDOWN, 0x11, sc); return; }
        if (sc == 0x9D) { s_kbd_ctrl = 0; shim_msg_push(SHIM_WM_KEYUP, 0x11, sc); return; }
        if (sc == 0x38) { s_kbd_alt = 1; shim_msg_push(SHIM_WM_KEYDOWN, 0x12, sc); return; }
        if (sc == 0xB8) { s_kbd_alt = 0; shim_msg_push(SHIM_WM_KEYUP, 0x12, sc); return; }
    }
    if (sc & 0x80) {
        u32 vk = shim_scan_vk(sc & 0x7F, e0);
        if (vk) {
            shim_msg_push(SHIM_WM_KEYUP, vk, sc);
            if (s_dbg_key < 16) { s_dbg_key++; log_hex64("[shim] KEYUP vk=", vk); }
        }
        return;
    }
    u32 vk = shim_scan_vk(sc, e0);
    if (vk) {
        shim_msg_push(SHIM_WM_KEYDOWN, vk, sc);
        if (s_dbg_key < 16) { s_dbg_key++; log_hex64("[shim] KEYDOWN vk=", vk); }
        /* Esc -> 额外合成 WM_CLOSE:无鼠标环境下关闭窗口的唯一途径 */
        if (vk == 0x1B && !e0) shim_msg_push(SHIM_WM_CLOSE, 0, 0);
    }
    if (!e0) {
        char c = shim_scan_ascii(sc, s_kbd_shift);
        if (c >= ' ' && c < 0x7F) {
            shim_msg_push(SHIM_WM_CHAR, (u64)(u8)c, sc);
            if (s_dbg_key < 16) { s_dbg_key++; log_hex64("[shim] CHAR ch=", (u64)(u8)c); }
        }
    }
}

/* 排空 PS/2 输出缓冲,转成 Win32 消息入队。
 * P5 窗口模式：输入权归 desktop（经 inject_* 入队），此函数 no-op，
 * 绝不触碰 PS/2 端口（避免与 desktop 的输入采集竞争）。 */
static void shim_kbd_poll(void) {
    if (s_wsurf) return;  /* 窗口模式：host 注入，不轮询端口 */
    shim_mouse_init_once();
    for (;;) {
        u8 st = inb(0x64);
        if (!(st & 1)) break;
        u8 sc = inb(0x60);
        if (st & 0x20) {
            if (s_dbg_aux < 16) { s_dbg_aux++; log_hex64("[shim] aux byte=", sc); }
            if (s_mouse_ok) shim_mouse_byte(sc);
            continue;
        }
        if (sc == 0xE0) { s_kbd_e0 = 1; continue; }
        int e0 = s_kbd_e0;
        s_kbd_e0 = 0;
        shim_kbd_inject(sc, e0);
    }
}

/* ===== P5 窗口模式输入注入（pe_service 经 ABI 转发到这里）===== */

int pe_shim_inject_scancode(u32 scancode, u32 e0_prefix) {
    if (!s_wsurf) return -1;
    shim_kbd_inject((u8)scancode, e0_prefix ? 1 : 0);
    return 0;
}

int pe_shim_inject_pointer(i32 x, i32 y, u32 buttons) {
    if (!s_wsurf) return -1;
    /* 钳位到 surface 范围 */
    if (x < 0) x = 0; else if (x >= (i32)s_wsurf_w) x = (i32)s_wsurf_w - 1;
    if (y < 0) y = 0; else if (y >= (i32)s_wsurf_h) y = (i32)s_wsurf_h - 1;

    u8 nbtn = (u8)(buttons & 0x07);
    i32 cx = x - g_wnd_x, cy = y - g_wnd_y;  /* 客户区坐标(消息 lparam) */
    u64 lp = (u64)(u32)(((u32)(u16)cy << 16) | (u32)(u16)cx);
    u64 mk = 0;
    if (nbtn & 1) mk |= SHIM_MK_LBUTTON;
    if (nbtn & 2) mk |= SHIM_MK_RBUTTON;
    if (nbtn & 4) mk |= SHIM_MK_MBUTTON;
    if (s_kbd_shift) mk |= SHIM_MK_SHIFT;
    if (s_kbd_ctrl) mk |= SHIM_MK_CONTROL;

    if (x != s_mouse_x || y != s_mouse_y) {
        s_mouse_x = x; s_mouse_y = y;
        shim_msg_push_pt(SHIM_WM_MOUSEMOVE, mk, lp, x, y);
    }
    /* 按钮边沿 */
    if ((nbtn & 1) && !(s_mouse_btn & 1)) shim_msg_push_pt(SHIM_WM_LBUTTONDOWN, mk, lp, x, y);
    if (!(nbtn & 1) && (s_mouse_btn & 1)) shim_msg_push_pt(SHIM_WM_LBUTTONUP, mk, lp, x, y);
    if ((nbtn & 2) && !(s_mouse_btn & 2)) shim_msg_push_pt(SHIM_WM_RBUTTONDOWN, mk, lp, x, y);
    if (!(nbtn & 2) && (s_mouse_btn & 2)) shim_msg_push_pt(SHIM_WM_RBUTTONUP, mk, lp, x, y);
    if ((nbtn & 4) && !(s_mouse_btn & 4)) shim_msg_push_pt(SHIM_WM_MBUTTONDOWN, mk, lp, x, y);
    if (!(nbtn & 4) && (s_mouse_btn & 4)) shim_msg_push_pt(SHIM_WM_MBUTTONUP, mk, lp, x, y);
    s_mouse_btn = nbtn;
    return 0;
}

int pe_shim_inject_input(u32 msg, u64 wparam, u64 lparam, i32 ptx, i32 pty) {
    if (!s_wsurf) return -1;
    shim_msg_push_pt(msg, wparam, lparam, ptx, pty);
    return 0;
}

/* 消息空转让出点：窗口模式回调 host 泵（desktop 帧），全屏 pause。
 * pump 返回非 0 = host 请求退出 → 投递 WM_CLOSE（PE 自愿退出路径）。 */
static void shim_msg_yield(void) {
    if (s_host_pump) {
        if (s_host_pump(s_host_ud))
            shim_msg_push(SHIM_WM_CLOSE, 0, 0);
    } else {
        __asm__ volatile("pause");
    }
}

static u64 __attribute__((ms_abi)) shim_RegisterClassExW(u64 wc_p) {
    if (!wc_p) return 0;
    g_wnd_proc = *(u64 *)(wc_p + 8);  /* cbSize(4)+style(4) -> lpfnWndProc */
    log_info("[shim] RegisterClassExW ok");
    return 0xC000;
}

static const struct limine_framebuffer *shim_fb(void) {
    /* P5 窗口模式：渲染目标是 host 提供的内存 surface（伪造 fb） */
    if (s_wsurf) return &s_wsurf_fb;
    if (!g_fb_request.response || g_fb_request.response->framebuffer_count == 0) return 0;
    return g_fb_request.response->framebuffers[0];
}

static u64 __attribute__((ms_abi)) shim_CreateWindowExW(
    u64 exstyle, u64 class_name, u64 wnd_name, u64 style,
    u64 x, u64 y, u64 w, u64 h,
    u64 parent, u64 menu, u64 hinst, u64 param) {
    (void)exstyle; (void)class_name; (void)wnd_name; (void)style;
    (void)parent; (void)menu; (void)hinst; (void)param;
    i32 ix = (i32)(u32)x, iy = (i32)(u32)y;
    i32 iw = (i32)(u32)w, ih = (i32)(u32)h;
    if (iw <= 0) iw = 800;
    if (ih <= 0) ih = 600;
    const struct limine_framebuffer *fb = shim_fb();
    i32 fw = fb ? (i32)fb->width : 0;
    i32 fh = fb ? (i32)fb->height : 0;
    if (fw && iw > fw) iw = fw;
    if (fh && ih > fh) ih = fh;
    /* CW_USEDEFAULT(0x80000000) -> 居中 */
    if (ix == (i32)0x80000000 || ix < 0) ix = fw ? (fw - iw) / 2 : 0;
    if (iy == (i32)0x80000000 || iy < 0) iy = fh ? (fh - ih) / 2 : 0;
    g_wnd_x = ix; g_wnd_y = iy; g_wnd_w = iw; g_wnd_h = ih;
    g_wnd_created = 1;
    log_info("[shim] CreateWindowExW ok");
    log_hex64("[shim] wnd x=", (u64)(i64)ix);
    log_hex64("[shim] wnd y=", (u64)(i64)iy);
    log_hex64("[shim] wnd w=", (u64)(i64)iw);
    log_hex64("[shim] wnd h=", (u64)(i64)ih);
    return SHIM_HWND;
}

static u64 __attribute__((ms_abi)) shim_ShowWindow(u64 hwnd, u64 cmd) {
    (void)hwnd; (void)cmd; return 1;
}
static u64 __attribute__((ms_abi)) shim_UpdateWindow(u64 hwnd) {
    (void)hwnd; return 1;
}
static u64 __attribute__((ms_abi)) shim_DestroyWindow(u64 hwnd) {
    (void)hwnd; g_wnd_created = 0; return 1;
}
static u64 __attribute__((ms_abi)) shim_LoadCursorW(u64 hinst, u64 name) {
    (void)hinst; (void)name; return SHIM_HCURSOR;
}
static u64 __attribute__((ms_abi)) shim_SetCursor(u64 cur) {
    (void)cur; return SHIM_HCURSOR;
}
static u64 __attribute__((ms_abi)) shim_GetCursorPos(u64 pt_p) {
    if (pt_p) { ((i32 *)pt_p)[0] = s_mouse_x; ((i32 *)pt_p)[1] = s_mouse_y; }
    return 1;
}
static u64 __attribute__((ms_abi)) shim_ScreenToClient(u64 hwnd, u64 pt_p) {
    (void)hwnd; (void)pt_p; return 1;  /* 窗口即全屏坐标系,无需转换 */
}
static u64 __attribute__((ms_abi)) shim_GetAsyncKeyState(u64 vk) {
    (void)vk; return 0;  /* 修饰键状态已由消息流维护 */
}
static u64 __attribute__((ms_abi)) shim_SetTimer(u64 hwnd, u64 id, u64 elapse, u64 cb) {
    (void)hwnd; (void)elapse; (void)cb; return id ? id : 1;
}
static u64 __attribute__((ms_abi)) shim_KillTimer(u64 hwnd, u64 id) {
    (void)hwnd; (void)id; return 1;
}
static u64 __attribute__((ms_abi)) shim_MessageBeep(u64 t) { (void)t; return 1; }
static u64 __attribute__((ms_abi)) shim_GetClientRect(u64 hwnd, u64 rect_p) {
    (void)hwnd;
    if (!rect_p) return 0;
    i32 *r = (i32 *)rect_p;
    r[0] = 0; r[1] = 0; r[2] = g_wnd_w; r[3] = g_wnd_h;
    return 1;
}
static u64 __attribute__((ms_abi)) shim_InvalidateRect(u64 hwnd, u64 rect_p, u64 erase) {
    (void)hwnd; (void)rect_p; (void)erase; return 1;  /* 主循环每帧全量重绘 */
}
/* PAINTSTRUCT: {hdc(8), fErase(4), rcPaint(16), fRestore(4), fIncUpdate(4), rgb(32)} */
static u64 __attribute__((ms_abi)) shim_BeginPaint(u64 hwnd, u64 ps_p) {
    (void)hwnd;
    if (!ps_p) return 0;
    u8 *p = (u8 *)ps_p;
    for (int i = 0; i < 72; i++) p[i] = 0;
    *(u64 *)p = SHIM_HDC_WND;
    return SHIM_HDC_WND;
}
static u64 __attribute__((ms_abi)) shim_EndPaint(u64 hwnd, u64 ps_p) {
    (void)hwnd; (void)ps_p; return 1;
}
static u64 __attribute__((ms_abi)) shim_ReleaseDC(u64 hwnd, u64 hdc) {
    (void)hwnd; (void)hdc; return 1;
}
static u64 __attribute__((ms_abi)) shim_GetSystemMetrics(u64 idx) {
    const struct limine_framebuffer *fb = shim_fb();
    if (!fb) return 0;
    if (idx == 0) return fb->width;   /* SM_CXSCREEN */
    if (idx == 1) return fb->height;  /* SM_CYSCREEN */
    return 0;
}
static u64 __attribute__((ms_abi)) shim_DefWindowProcW(u64 hwnd, u64 msg, u64 w, u64 l) {
    (void)hwnd; (void)msg; (void)w; (void)l; return 0;
}
static u64 __attribute__((ms_abi)) shim_TranslateMessage(u64 msg_p) {
    (void)msg_p; return 0;  /* WM_CHAR 已在键盘轮询时合成 */
}
static u64 __attribute__((ms_abi)) shim_PeekMessageW(u64 msg_p, u64 hwnd,
                                                      u64 fmin, u64 fmax, u64 remove) {
    (void)hwnd; (void)fmin; (void)fmax;
    shim_kbd_poll();
    /* P5 窗口模式：PeekMessage 驱动的渲染循环（Deaicup 型）下 PE 几乎不
     * 调 GetMessage，desktop 只能靠这里的空转让出获得 CPU 跑帧。 */
    if (g_mq_count == 0 && s_host_pump) shim_msg_yield();
    if (g_mq_count == 0) return 0;
    shim_msg_t *m = &g_msgq[g_mq_head];
    if (msg_p) *(shim_msg_t *)msg_p = *m;
    if (remove & 1) {  /* PM_REMOVE */
        g_mq_head = (g_mq_head + 1) % SHIM_MSGQ_CAP;
        g_mq_count--;
    }
    return 1;
}
static u64 __attribute__((ms_abi)) shim_GetMessageW(u64 msg_p, u64 hwnd,
                                                     u64 fmin, u64 fmax) {
    (void)hwnd; (void)fmin; (void)fmax;
    for (;;) {  /* 阻塞语义:轮询键盘直到有消息 */
        shim_kbd_poll();
        if (g_mq_count > 0) break;
        shim_msg_yield();  /* P5: 窗口模式泵 desktop，全屏 pause */
    }
    shim_msg_t *m = &g_msgq[g_mq_head];
    if (msg_p) *(shim_msg_t *)msg_p = *m;
    g_mq_head = (g_mq_head + 1) % SHIM_MSGQ_CAP;
    g_mq_count--;
    return (m->message == SHIM_WM_QUIT) ? 0 : 1;
}
static u64 __attribute__((ms_abi)) shim_PostQuitMessage(u64 exit_code) {
    shim_msg_push(SHIM_WM_QUIT, exit_code, 0);
    return 0;
}

typedef u64 (__attribute__((ms_abi)) *shim_wndproc_t)(u64, u64, u64, u64);

static u64 __attribute__((ms_abi)) shim_DispatchMessageW(u64 msg_p) {
    if (!msg_p || !g_wnd_proc) return 0;
    const shim_msg_t *m = (const shim_msg_t *)msg_p;
    return ((shim_wndproc_t)g_wnd_proc)(m->hwnd, m->message, m->wparam, m->lparam);
}

/* ============================================================
 *  gdi32.dll — 内存 DC / DIBSection / BitBlt -> 帧缓冲
 * ========================================================== */

typedef struct { u64 bmp; } shim_dc_t;              /* 当前选中位图(0=无) */
typedef struct { i32 w, h; u32 *bits; } shim_bmp_t; /* 32bpp 0xAARRGGBB 位图 */

static u64 __attribute__((ms_abi)) shim_GetDC(u64 hwnd) {
    (void)hwnd; return SHIM_HDC_WND;
}
static u64 __attribute__((ms_abi)) shim_CreateCompatibleDC(u64 hdc) {
    (void)hdc;
    shim_dc_t *dc = (shim_dc_t *)kmem_alloc(sizeof(shim_dc_t));
    if (dc) dc->bmp = 0;
    return (u64)dc;
}
static u64 __attribute__((ms_abi)) shim_CreateDIBSection(u64 hdc, u64 bmi_p, u64 usage,
                                                          u64 ppv, u64 hsec, u64 off) {
    (void)hdc; (void)usage; (void)hsec; (void)off;
    if (!bmi_p || !ppv) return 0;
    const u8 *bmi = (const u8 *)bmi_p;
    i32 w = *(const i32 *)(bmi + 4);    /* biWidth */
    i32 h = *(const i32 *)(bmi + 8);    /* biHeight(负=top-down) */
    u16 bpp = *(const u16 *)(bmi + 14); /* biBitCount */
    if (h < 0) h = -h;
    if (w <= 0 || h <= 0 || bpp != 32) return 0;
    u64 npix = (u64)w * (u64)h;
    u32 *bits = (u32 *)kmem_alloc(npix * 4);
    if (!bits) return 0;
    for (u64 i = 0; i < npix; i++) bits[i] = 0;
    shim_bmp_t *b = (shim_bmp_t *)kmem_alloc(sizeof(shim_bmp_t));
    if (!b) return 0;
    b->w = w; b->h = h; b->bits = bits;
    *(u64 *)ppv = (u64)bits;
    log_info("[shim] CreateDIBSection ok");
    log_hex64("[shim] dib w=", (u64)(i64)w);
    log_hex64("[shim] dib h=", (u64)(i64)h);
    log_hex64("[shim] dib bits=", (u64)bits);
    return (u64)b;
}
static u64 __attribute__((ms_abi)) shim_SelectObject(u64 hdc, u64 obj) {
    shim_dc_t *dc = (shim_dc_t *)hdc;
    if (!dc) return 0;
    u64 prev = dc->bmp;
    dc->bmp = obj;
    return prev;
}
static u64 __attribute__((ms_abi)) shim_DeleteDC(u64 hdc) {
    if (hdc == SHIM_HDC_WND) return 1;  /* 窗口 DC 不释放 */
    shim_dc_t *dc = (shim_dc_t *)hdc;
    /* kmem arena 不支持 free,小对象泄漏可接受 */
    (void)dc;
    return 1;
}
static u64 __attribute__((ms_abi)) shim_DeleteObject(u64 obj) {
    (void)obj; return 1;  /* 同上:arena 无 free,位图随进程退出回收 */
}
static u64 __attribute__((ms_abi)) shim_GetStockObject(u64 i) {
    (void)i; return 0x20000010;  /* 伪 GDI 对象句柄 */
}
static u64 __attribute__((ms_abi)) shim_GdiFlush(void) { return 1; }

static u64 g_blit_count = 0;

/* 12x12 箭头软件光标:'X'=黑边 '#'=白心 '.'=透明.
 * 设计:BitBlt 上屏后叠画;egui 每帧全量重绘,下一帧 BitBlt 自动覆盖旧光标,
 * 无需保存/恢复背景.只在窗口矩形内绘制(越界裁剪),避免屏外残影. */
static const char *const g_cursor_bmp[12] = {
    "X...........",
    "XX..........",
    "X#X.........",
    "X##X........",
    "X###X.......",
    "X####X......",
    "X#####X.....",
    "X######X....",
    "X#######X...",
    "X########X..",
    "X####XXXX...",
    "XXXX........",
};

static void shim_draw_cursor(const struct limine_framebuffer *fb) {
    if (s_wsurf) return;  /* P5 窗口模式：光标由 desktop 统一绘制 */
    if (!s_mouse_ok || !g_wnd_created) return;
    i32 wx1 = g_wnd_x + g_wnd_w, wy1 = g_wnd_y + g_wnd_h;
    if (s_mouse_x < g_wnd_x || s_mouse_y < g_wnd_y) return;
    if (s_mouse_x >= wx1 || s_mouse_y >= wy1) return;
    u8 *fbb = (u8 *)fb->address;
    for (i32 r = 0; r < 12; r++) {
        i32 fy = s_mouse_y + r;
        if (fy >= wy1 || fy >= (i32)fb->height) break;
        u8 *rowb = fbb + (u64)fy * fb->pitch;
        for (i32 c = 0; c < 12; c++) {
            char ch = g_cursor_bmp[r][c];
            if (ch != 'X' && ch != '#') continue;
            i32 fx = s_mouse_x + c;
            if (fx >= wx1 || fx >= (i32)fb->width) break;
            if (fb->bpp == 32) {
                ((u32 *)rowb)[fx] = (ch == '#') ? 0xFFFFFFFFu : 0xFF000000u;
            } else {
                u8 v = (ch == '#') ? 0xFF : 0x00;
                u8 *p = rowb + (u64)fx * 3;
                p[0] = v; p[1] = v; p[2] = v;
            }
        }
    }
}

static u64 __attribute__((ms_abi)) shim_BitBlt(u64 dstdc, u64 x, u64 y, u64 cx, u64 cy,
                                                u64 srcdc, u64 x1, u64 y1, u64 rop) {
    (void)rop;
    if (dstdc != SHIM_HDC_WND) return 1;  /* 仅支持窗口 DC 目标 */
    const struct limine_framebuffer *fb = shim_fb();
    if (!fb || !fb->address) return 0;
    if (fb->bpp != 32 && fb->bpp != 24) return 0;
    shim_dc_t *sdc = (shim_dc_t *)srcdc;
    if (!sdc || !sdc->bmp) return 0;
    shim_bmp_t *bmp = (shim_bmp_t *)sdc->bmp;

    i32 dx = (i32)(u32)x + g_wnd_x;
    i32 dy = (i32)(u32)y + g_wnd_y;
    i32 sx0 = (i32)(u32)x1, sy0 = (i32)(u32)y1;
    i32 w = (i32)(u32)cx, h = (i32)(u32)cy;

    u8 *fbb = (u8 *)fb->address;
    u32 rsh = fb->red_mask_shift, gsh = fb->green_mask_shift, bsh = fb->blue_mask_shift;
    u32 rsz = fb->red_mask_size ? fb->red_mask_size : 8;
    u32 gsz = fb->green_mask_size ? fb->green_mask_size : 8;
    u32 bsz = fb->blue_mask_size ? fb->blue_mask_size : 8;
    int fast = (fb->bpp == 32 && rsh == 16 && gsh == 8 && bsh == 0
                && rsz == 8 && gsz == 8 && bsz == 8);

    for (i32 row = 0; row < h; row++) {
        i32 fy = dy + row, sy = sy0 + row;
        if (fy < 0 || fy >= (i32)fb->height) continue;
        if (sy < 0 || sy >= bmp->h) continue;
        u8 *dst_row = fbb + (u64)fy * fb->pitch;
        const u32 *src = bmp->bits + (u64)sy * (u64)bmp->w;
        for (i32 col = 0; col < w; col++) {
            i32 fx = dx + col, sx = sx0 + col;
            if (fx < 0 || fx >= (i32)fb->width) continue;
            if (sx < 0 || sx >= bmp->w) continue;
            u32 px = src[sx];
            if (fast) {
                ((u32 *)dst_row)[fx] = px;
            } else {
                u32 r = (px >> 16) & 0xFF, g = (px >> 8) & 0xFF, bl = px & 0xFF;
                u32 v = ((r >> (8 - rsz)) << rsh)
                      | ((g >> (8 - gsz)) << gsh)
                      | ((bl >> (8 - bsz)) << bsh);
                if (fb->bpp == 32) {
                    ((u32 *)dst_row)[fx] = v;
                } else {
                    u8 *p = dst_row + (u64)fx * 3;
                    p[0] = (u8)(v & 0xFF);
                    p[1] = (u8)((v >> 8) & 0xFF);
                    p[2] = (u8)((v >> 16) & 0xFF);
                }
            }
        }
    }
    shim_draw_cursor(fb);
    g_blit_count++;
    if (g_blit_count == 1) log_info("[shim] BitBlt first frame on screen");
    if ((g_blit_count % 600) == 0) {
        log_hex64("[shim] BitBlt frames=", g_blit_count);
        /* 堆健康度:live 逐周期线性增长=真泄漏;free_max 持续萎缩=碎片化 */
        u64 fm = 0;
        for (heap_blk *f = g_heap_free_head; f; f = (heap_blk *)f->next_free) {
            if (f->size > fm) fm = f->size;
        }
        log_hex64("[shim] heap live=", g_heap_live);
        log_hex64("[shim] heap free_max=", fm);
    }
    return 1;
}

/* SetDIBitsToDevice: 不经内存 DC,直接把 DIB 位块贴上窗口 DC.
 * 语义(MSDN): (xSrc,ySrc) 是 DIB 源区域左下角(DIB 坐标系 y 向上),
 * startScan/numLines 描述 lpvBits 缓冲覆盖的扫描线区间,
 * (xDest,yDest,w,h) 是客户区目标矩形(y 向下).
 * bottom-up DIB: 缓冲行 0 = DIB 扫描线 startScan(从底数);
 * top-down DIB : 缓冲行 0 = 图像顶行 + startScan.
 * 仅支持 32bpp BI_RGB;返回实际拷贝的扫描线数. */
static u64 __attribute__((ms_abi)) shim_SetDIBitsToDevice(
    u64 hdc, i32 xDest, i32 yDest, u32 w, u32 h,
    i32 xSrc, i32 ySrc, u32 startScan, u32 numLines,
    u64 bits_p, u64 bmi_p, u64 colorUse) {
    (void)colorUse;
    if (hdc != SHIM_HDC_WND) return 0;  /* 仅支持窗口 DC 目标 */
    const struct limine_framebuffer *fb = shim_fb();
    if (!fb || !fb->address) return 0;
    if (fb->bpp != 32 && fb->bpp != 24) return 0;
    if (!bits_p || !bmi_p || w == 0 || h == 0) return 0;
    const u8 *bmi = (const u8 *)bmi_p;
    i32 bw = *(const i32 *)(bmi + 4);     /* biWidth */
    i32 bh = *(const i32 *)(bmi + 8);     /* biHeight(负=top-down) */
    u16 bpp = *(const u16 *)(bmi + 14);   /* biBitCount */
    u32 comp = *(const u32 *)(bmi + 16);  /* biCompression */
    if (bw <= 0 || bh == 0 || bpp != 32 || comp != 0 /* BI_RGB */) return 0;
    int topdown = 0;
    i32 H = bh;
    if (H < 0) { H = -H; topdown = 1; }

    const u32 *bits = (const u32 *)bits_p;
    i32 dx0 = xDest + g_wnd_x;
    i32 dy0 = yDest + g_wnd_y;

    u8 *fbb = (u8 *)fb->address;
    u32 rsh = fb->red_mask_shift, gsh = fb->green_mask_shift, bsh = fb->blue_mask_shift;
    u32 rsz = fb->red_mask_size ? fb->red_mask_size : 8;
    u32 gsz = fb->green_mask_size ? fb->green_mask_size : 8;
    u32 bsz = fb->blue_mask_size ? fb->blue_mask_size : 8;
    int fast = (fb->bpp == 32 && rsh == 16 && gsh == 8 && bsh == 0
                && rsz == 8 && gsz == 8 && bsz == 8);

    u32 copied = 0;
    for (u32 row = 0; row < h; row++) {
        /* 目标顶行 row=0 ← DIB 坐标 y=ySrc+h-1(源区域顶边) */
        i32 sy = ySrc + (i32)h - 1 - (i32)row;
        if (sy < 0 || sy >= H) continue;
        /* DIB 扫描线 sy → lpvBits 缓冲行 */
        i64 br = topdown ? ((i64)(H - 1 - sy) - (i32)startScan)
                         : ((i64)sy - (i32)startScan);
        if (br < 0 || br >= (i64)numLines) continue;
        i32 fy = dy0 + (i32)row;
        if (fy < 0 || fy >= (i32)fb->height) continue;
        const u32 *src = bits + (u64)br * (u64)bw;
        u8 *dst_row = fbb + (u64)fy * fb->pitch;
        copied++;
        for (u32 col = 0; col < w; col++) {
            i32 sx = xSrc + (i32)col;
            if (sx < 0 || sx >= bw) continue;
            i32 fx = dx0 + (i32)col;
            if (fx < 0 || fx >= (i32)fb->width) continue;
            u32 px = src[sx];
            if (fast) {
                ((u32 *)dst_row)[fx] = px;
            } else {
                u32 r = (px >> 16) & 0xFF, g = (px >> 8) & 0xFF, bl = px & 0xFF;
                u32 v = ((r >> (8 - rsz)) << rsh)
                      | ((g >> (8 - gsz)) << gsh)
                      | ((bl >> (8 - bsz)) << bsh);
                if (fb->bpp == 32) {
                    ((u32 *)dst_row)[fx] = v;
                } else {
                    u8 *p = dst_row + (u64)fx * 3;
                    p[0] = (u8)(v & 0xFF);
                    p[1] = (u8)((v >> 8) & 0xFF);
                    p[2] = (u8)((v >> 16) & 0xFF);
                }
            }
        }
    }
    shim_draw_cursor(fb);
    return copied;
}

/* 未实现 stub(64位)——严格错误策略插桩：PE 应用调到 shim 表未覆盖的
 * API = 未实现路径，按零降级原则 panic（PE-E01），不再告警返回 0。
 * shim 表没有的 API 一旦被调用，PE 程序的语义已经是错的。 */
static u64 __attribute__((ms_abi)) shim_unimpl64(u64 a0, u64 a1, u64 a2, u64 a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    log_error("[PE] unimplemented API called");
    log_hex64("[PE]   caller rip=", (u64)__builtin_return_address(0));
    panic_full("PE-E01 UNSHIMMED API CALLED",
               "PE app imported an API outside the shim table: not implemented", 0);
    return 0;
}

/* 提供给 pe_shim.h 的地址获取函数 */
u64 pe_shim_unimpl64_addr(void) { return (u64)shim_unimpl64; }
u64 pe_shim_exitprocess64_addr(void) { return (u64)shim_ExitProcess; }

/* 获取退出码 */
u64 pe_shim_get_exit_code(void) { return g_exit_code; }

/* ===== shim 表 ===== */
static const pe_shim_entry g_shim_table[] = {
    /* kernel32.dll */
    {"kernel32.dll", "GetStdHandle",          (void*)shim_GetStdHandle,        0},
    {"kernel32.dll", "WriteFile",             (void*)shim_WriteFile,           0},
    {"kernel32.dll", "ReadFile",              (void*)shim_ReadFile,            0},
    {"kernel32.dll", "ExitProcess",           (void*)shim_ExitProcess,         0},
    {"kernel32.dll", "GetCommandLineA",       (void*)shim_GetCommandLineA,     0},
    {"kernel32.dll", "GetCommandLineW",       (void*)shim_GetCommandLineW,     0},
    {"kernel32.dll", "GetLastError",          (void*)shim_GetLastError,        0},
    {"kernel32.dll", "SetLastError",          (void*)shim_SetLastError,        0},
    {"kernel32.dll", "HeapCreate",            (void*)shim_HeapCreate,          0},
    {"kernel32.dll", "HeapAlloc",             (void*)shim_HeapAlloc,           0},
    {"kernel32.dll", "HeapFree",              (void*)shim_HeapFree,            0},
    {"kernel32.dll", "VirtualAlloc",          (void*)shim_VirtualAlloc,        0},
    {"kernel32.dll", "VirtualFree",           (void*)shim_VirtualFree,         0},
    {"kernel32.dll", "GetModuleHandleA",      (void*)shim_GetModuleHandleA,    0},
    {"kernel32.dll", "GetModuleHandleW",      (void*)shim_GetModuleHandleW,    0},
    {"kernel32.dll", "GetEnvironmentVariableA",(void*)shim_GetEnvironmentVariableA, 0},
    {"kernel32.dll", "CreateFileA",           (void*)shim_CreateFileA,         0},
    {"kernel32.dll", "CloseHandle",           (void*)shim_CloseHandle,         0},
    {"kernel32.dll", "GetFileSize",           (void*)shim_GetFileSize,         0},
    {"kernel32.dll", "SetFilePointer",        (void*)shim_SetFilePointer,      0},
    /* kernel32.dll — Deaicup GUI 增量 */
    {"kernel32.dll", "HeapReAlloc",           (void*)shim_HeapReAlloc,         0},
    {"kernel32.dll", "GetProcessHeap",        (void*)shim_GetProcessHeap,      0},
    {"kernel32.dll", "QueryPerformanceFrequency", (void*)shim_QueryPerformanceFrequency, 0},
    {"kernel32.dll", "QueryPerformanceCounter",  (void*)shim_QueryPerformanceCounter, 0},
    {"kernel32.dll", "GetTickCount",          (void*)shim_GetTickCount,        0},
    {"kernel32.dll", "Sleep",                 (void*)shim_Sleep,               0},
    {"kernel32.dll", "GetSystemInfo",         (void*)shim_GetSystemInfo,       0},
    {"kernel32.dll", "GetConsoleMode",        (void*)shim_GetConsoleMode,      0},
    {"kernel32.dll", "GetConsoleOutputCP",    (void*)shim_GetConsoleOutputCP,  0},
    {"kernel32.dll", "WriteConsoleW",         (void*)shim_WriteConsoleW,       0},
    {"kernel32.dll", "GetCurrentDirectoryW",  (void*)shim_GetCurrentDirectoryW, 0},
    {"kernel32.dll", "GetEnvironmentVariableW", (void*)shim_GetEnvironmentVariableW, 0},
    {"kernel32.dll", "GetCurrentProcess",     (void*)shim_GetCurrentProcess,   0},
    {"kernel32.dll", "GetCurrentProcessId",   (void*)shim_GetCurrentProcessId, 0},
    {"kernel32.dll", "GetCurrentThreadId",    (void*)shim_GetCurrentThreadId,  0},
    {"kernel32.dll", "CreateMutexA",          (void*)shim_CreateMutexA,        0},
    {"kernel32.dll", "ReleaseMutex",          (void*)shim_ReleaseMutex,        0},
    {"kernel32.dll", "WaitForSingleObject",   (void*)shim_WaitForSingleObject, 0},
    {"kernel32.dll", "WaitForSingleObjectEx", (void*)shim_WaitForSingleObjectEx, 0},
    {"kernel32.dll", "FormatMessageW",        (void*)shim_FormatMessageW,      0},
    {"kernel32.dll", "LoadLibraryA",          (void*)shim_LoadLibraryA,        0},
    {"kernel32.dll", "GetProcAddress",        (void*)shim_GetProcAddress,      0},
    {"kernel32.dll", "lstrlenW",              (void*)shim_lstrlenW,            0},
    {"kernel32.dll", "MultiByteToWideChar",   (void*)shim_MultiByteToWideChar, 0},
    {"kernel32.dll", "WideCharToMultiByte",   (void*)shim_WideCharToMultiByte, 0},
    /* kernel32.dll 也转发导出这三个 Rtl*(Rust std 从 kernel32 静态导入) */
    {"kernel32.dll", "RtlCaptureContext",     (void*)shim_RtlCaptureContext,   0},
    {"kernel32.dll", "RtlLookupFunctionEntry",(void*)shim_RtlLookupFunctionEntry, 0},
    {"kernel32.dll", "RtlVirtualUnwind",      (void*)shim_RtlVirtualUnwind,    0},
    /* ntdll.dll */
    {"ntdll.dll",    "RtlCaptureContext",     (void*)shim_RtlCaptureContext,   0},
    {"ntdll.dll",    "RtlLookupFunctionEntry",(void*)shim_RtlLookupFunctionEntry, 0},
    {"ntdll.dll",    "RtlVirtualUnwind",      (void*)shim_RtlVirtualUnwind,    0},
    {"ntdll.dll",    "NtWriteFile",           (void*)shim_NtWriteFile,         0},
    {"ntdll.dll",    "RtlNtStatusToDosError", (void*)shim_RtlNtStatusToDosError, 0},
    /* bcryptprimitives.dll */
    {"bcryptprimitives.dll", "ProcessPrng",   (void*)shim_ProcessPrng,         0},
    /* api-ms-win-core-synch-l1-2-0.dll */
    {"api-ms-win-core-synch-l1-2-0.dll", "WaitOnAddress",      (void*)shim_WaitOnAddress, 0},
    {"api-ms-win-core-synch-l1-2-0.dll", "WakeByAddressSingle",(void*)shim_WakeByAddress, 0},
    {"api-ms-win-core-synch-l1-2-0.dll", "WakeByAddressAll",   (void*)shim_WakeByAddress, 0},
    /* user32.dll */
    {"user32.dll",   "RegisterClassExW",      (void*)shim_RegisterClassExW,    0},
    {"user32.dll",   "CreateWindowExW",       (void*)shim_CreateWindowExW,     0},
    {"user32.dll",   "DestroyWindow",         (void*)shim_DestroyWindow,       0},
    {"user32.dll",   "ShowWindow",            (void*)shim_ShowWindow,          0},
    {"user32.dll",   "UpdateWindow",          (void*)shim_UpdateWindow,        0},
    {"user32.dll",   "GetMessageW",           (void*)shim_GetMessageW,         0},
    {"user32.dll",   "PeekMessageW",          (void*)shim_PeekMessageW,        0},
    {"user32.dll",   "TranslateMessage",      (void*)shim_TranslateMessage,    0},
    {"user32.dll",   "DispatchMessageW",      (void*)shim_DispatchMessageW,    0},
    {"user32.dll",   "PostQuitMessage",       (void*)shim_PostQuitMessage,     0},
    {"user32.dll",   "DefWindowProcW",        (void*)shim_DefWindowProcW,      0},
    {"user32.dll",   "GetClientRect",         (void*)shim_GetClientRect,       0},
    {"user32.dll",   "InvalidateRect",        (void*)shim_InvalidateRect,      0},
    {"user32.dll",   "BeginPaint",            (void*)shim_BeginPaint,          0},
    {"user32.dll",   "EndPaint",              (void*)shim_EndPaint,            0},
    {"user32.dll",   "GetDC",                 (void*)shim_GetDC,               0},
    {"user32.dll",   "ReleaseDC",             (void*)shim_ReleaseDC,           0},
    {"user32.dll",   "LoadCursorW",           (void*)shim_LoadCursorW,         0},
    {"user32.dll",   "SetCursor",             (void*)shim_SetCursor,           0},
    {"user32.dll",   "GetCursorPos",          (void*)shim_GetCursorPos,        0},
    {"user32.dll",   "ScreenToClient",        (void*)shim_ScreenToClient,      0},
    {"user32.dll",   "GetAsyncKeyState",      (void*)shim_GetAsyncKeyState,    0},
    {"user32.dll",   "SetTimer",              (void*)shim_SetTimer,            0},
    {"user32.dll",   "KillTimer",             (void*)shim_KillTimer,           0},
    {"user32.dll",   "MessageBeep",           (void*)shim_MessageBeep,         0},
    {"user32.dll",   "GetSystemMetrics",      (void*)shim_GetSystemMetrics,    0},
    /* gdi32.dll */
    {"gdi32.dll",    "CreateCompatibleDC",    (void*)shim_CreateCompatibleDC,  0},
    {"gdi32.dll",    "CreateDIBSection",      (void*)shim_CreateDIBSection,    0},
    {"gdi32.dll",    "SelectObject",          (void*)shim_SelectObject,        0},
    {"gdi32.dll",    "DeleteObject",          (void*)shim_DeleteObject,        0},
    {"gdi32.dll",    "DeleteDC",              (void*)shim_DeleteDC,            0},
    {"gdi32.dll",    "BitBlt",                (void*)shim_BitBlt,              0},
    {"gdi32.dll",    "SetDIBitsToDevice",     (void*)shim_SetDIBitsToDevice,   0},
    {"gdi32.dll",    "GetStockObject",        (void*)shim_GetStockObject,      0},
    {"gdi32.dll",    "GdiFlush",              (void*)shim_GdiFlush,            0},
    /* msvcrt.dll */
    {"msvcrt.dll",   "printf",                (void*)shim_printf,              0},
    {"msvcrt.dll",   "puts",                  (void*)shim_puts,                0},
    {"msvcrt.dll",   "putchar",               (void*)shim_putchar,             0},
    {"msvcrt.dll",   "malloc",                (void*)shim_malloc,              0},
    {"msvcrt.dll",   "calloc",                (void*)shim_calloc,              0},
    {"msvcrt.dll",   "free",                  (void*)shim_free,                0},
    {"msvcrt.dll",   "realloc",               (void*)shim_realloc,             0},
    {"msvcrt.dll",   "exit",                  (void*)shim_exit,                0},
    {"msvcrt.dll",   "memcpy",                (void*)shim_memcpy,              0},
    {"msvcrt.dll",   "memset",                (void*)shim_memset,              0},
    {"msvcrt.dll",   "memmove",               (void*)shim_memmove,             0},
    {"msvcrt.dll",   "strlen",                (void*)shim_strlen,              0},
    {"msvcrt.dll",   "strcmp",                (void*)shim_strcmp,              0},
    {"msvcrt.dll",   "strcpy",                (void*)shim_strcpy,              0},
    {"msvcrt.dll",   "strncmp",               (void*)shim_strncmp,             0},
    {"msvcrt.dll",   "fwrite",                (void*)shim_fwrite,              0},
    {"msvcrt.dll",   "fputs",                 (void*)shim_fputs,               0},
    {"msvcrt.dll",   "fputc",                 (void*)shim_fputc,               0},
    {"msvcrt.dll",   "_initterm",             (void*)shim_initterm,            0},
    {"msvcrt.dll",   "_initterm_e",           (void*)shim_initterm_e,          0},
};

int pe_shim_lookup(const char *dll, const char *func) {
    if (!dll || !func) return -1;
    for (int i = 0; i < (int)(sizeof(g_shim_table)/sizeof(g_shim_table[0])); i++) {
        /* 大小写不敏感比较 DLL 名 */
        const char *a = g_shim_table[i].dll;
        const char *b = dll;
        int match = 1;
        while (*a && *b) {
            char ca = *a, cb = *b;
            if (ca >= 'A' && ca <= 'Z') ca += 32;
            if (cb >= 'A' && cb <= 'Z') cb += 32;
            if (ca != cb) { match = 0; break; }
            a++; b++;
        }
        if (!match || *a != *b) continue;
        /* 精确比较函数名 */
        const char *fa = g_shim_table[i].func;
        const char *fb = func;
        while (*fa && *fb) { if (*fa != *fb) break; fa++; fb++; }
        if (*fa == *fb) return i;
    }
    return -1;
}

const pe_shim_entry *pe_shim_get(int index) {
    if (index < 0 || index >= (int)(sizeof(g_shim_table)/sizeof(g_shim_table[0]))) return 0;
    return &g_shim_table[index];
}

int pe_shim_count(void) {
    return (int)(sizeof(g_shim_table)/sizeof(g_shim_table[0]));
}
