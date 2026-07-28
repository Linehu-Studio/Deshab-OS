/* pe_shim.c — Windows API shim 实现。
 *
 * 所有 64位 shim 用 __attribute__((ms_abi)) 声明，匹配 PE32+ 调用约定。
 * 32位 shim 从模拟栈读参数，供 x86emu32 解释器调用。
 *
 * 支持 API：kernel32.dll（GetStdHandle/WriteFile/ReadFile/ExitProcess/
 *   HeapAlloc/HeapFree/VirtualAlloc/VirtualFree/GetCommandLineA/...）
 *   msvcrt.dll（printf/puts/putchar/malloc/free/exit/memcpy/memset/
 *   strlen/strcmp/strcpy/_initterm/_initterm_e）
 */

#include "pe_shim.h"
#include <utsm/arena.h>
#include <utsm/log.h>
#include <utsm/types.h>

/* setjmp/longjmp for ExitProcess — 使用 __builtin_setjmp/__builtin_longjmp
 * （clang/gcc 内置，freestanding 可用，无需 libc）。
 * __builtin_setjmp 接受 void** 缓冲区，至少需要 8 个指针槽。 */
static void *g_exit_jmp[24];
static int g_exit_jmp_set;
static u64 g_exit_code;
static const char *g_cmdline = "";

void pe_shim_init(const char *cmdline) {
    g_cmdline = cmdline ? cmdline : "";
    g_exit_jmp_set = 0;
    g_exit_code = 0;
}

/* pe_run 调用：保存上下文，返回 0 = 首次（继续执行），1 = ExitProcess 触发 */
int pe_shim_setup_exit(void) {
    g_exit_jmp_set = 1;
    return __builtin_setjmp(g_exit_jmp);
}

void pe_shim_exit_process(u64 exit_code) {
    g_exit_code = exit_code;
    if (g_exit_jmp_set) {
        __builtin_longjmp(g_exit_jmp, 1);
    }
    /* 无 jmpbuf：直接返回 */
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

/* ===== 串口/帧缓冲输出（shim 内部复用） ===== */
static void shim_output(const char *s, u64 len) {
    for (u64 i = 0; i < len; i++) {
        serial_putc(s[i]);
    }
}

static void shim_output_str(const char *s) {
    serial_write(s);
}

/* ===== kernel32.dll shims (64位, ms_abi) ===== */

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
        const char *buf = (const char *)buffer;
        shim_output(buf, length);
        if (bytes_written_ptr) *(u64 *)bytes_written_ptr = length;
        return 1;  /* TRUE */
    }
    return 0;
}

static u64 __attribute__((ms_abi)) shim_ReadFile(u64 handle, u64 buffer, u64 max_length,
                                                  u64 bytes_read_ptr, u64 overlapped) {
    (void)handle; (void)buffer; (void)max_length; (void)overlapped;
    /* 暂不支持输入读取 */
    if (bytes_read_ptr) *(u64 *)bytes_read_ptr = 0;
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
    return (u64)g_cmdline;  /* 简化：返回 ASCII */
}

static u64 __attribute__((ms_abi)) shim_GetLastError(void) { return 0; }
static u64 __attribute__((ms_abi)) shim_SetLastError(u64 code) { (void)code; return 0; }

static u64 __attribute__((ms_abi)) shim_HeapCreate(u64 fl, u64 init, u64 max) {
    (void)fl; (void)init; (void)max;
    return 0x10000010;  /* 假堆句柄 */
}
static u64 __attribute__((ms_abi)) shim_HeapAlloc(u64 heap, u64 flags, u64 size) {
    (void)heap; (void)flags;
    return (u64)kmem_alloc(size);
}
static u64 __attribute__((ms_abi)) shim_HeapFree(u64 heap, u64 flags, u64 ptr) {
    (void)heap; (void)flags; (void)ptr;
    return 1;  /* arena 无 free，返回 TRUE */
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

/* printf — 简化版，只支持 %s %d %c %x %u %% */
static u64 __attribute__((ms_abi)) shim_printf(u64 fmt, u64 a1, u64 a2, u64 a3) {
    /* 可变参数：MS x64 ABI 下 a1-a3 在 RDX/R8/R9，更多参数在栈上。
     * 简化处理：只处理前 3 个参数。 */
    const char *f = (const char *)fmt;
    u64 args[3] = {a1, a2, a3};
    int argi = 0;
    char outbuf[512];
    int oi = 0;
    for (int i = 0; f[i] && oi < 510; i++) {
        if (f[i] == '%' && f[i+1]) {
            i++;
            switch (f[i]) {
            case 's': {
                const char *s = (const char *)(args[argi++ & 3]);
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
    const char *s = (const char *)str;
    shim_msvcrt_output(s);
    shim_msvcrt_output("\n");
    return 0;
}

static u64 __attribute__((ms_abi)) shim_putchar(u64 ch) {
    serial_putc((char)(ch & 0xFF));
    return ch & 0xFF;
}

static u64 __attribute__((ms_abi)) shim_malloc(u64 size) {
    return (u64)kmem_alloc(size);
}
static u64 __attribute__((ms_abi)) shim_calloc(u64 count, u64 size) {
    u64 total = count * size;
    u8 *p = kmem_alloc(total);
    if (p) for (u64 i = 0; i < total; i++) p[i] = 0;
    return (u64)p;
}
static u64 __attribute__((ms_abi)) shim_free(u64 ptr) { (void)ptr; return 0; }
static u64 __attribute__((ms_abi)) shim_realloc(u64 ptr, u64 size) {
    (void)ptr; return (u64)kmem_alloc(size);
}
static u64 __attribute__((ms_abi)) shim_exit(u64 code) {
    pe_shim_exit_process(code);
    return 0;
}

static u64 __attribute__((ms_abi)) shim_memcpy(u64 dst, u64 src, u64 n) {
    u8 *d = (u8 *)dst; const u8 *s = (const u8 *)src;
    for (u64 i = 0; i < n; i++) d[i] = s[i];
    return dst;
}
static u64 __attribute__((ms_abi)) shim_memset(u64 dst, u64 val, u64 n) {
    u8 *d = (u8 *)dst; u8 v = (u8)(val & 0xFF);
    for (u64 i = 0; i < n; i++) d[i] = v;
    return dst;
}
static u64 __attribute__((ms_abi)) shim_memmove(u64 dst, u64 src, u64 n) {
    u8 *d = (u8 *)dst; const u8 *s = (const u8 *)src;
    if (d < s) { for (u64 i = 0; i < n; i++) d[i] = s[i]; }
    else { for (u64 i = n; i > 0; i--) d[i-1] = s[i-1]; }
    return dst;
}
static u64 __attribute__((ms_abi)) shim_strlen(u64 str) {
    const char *s = (const char *)str;
    u64 n = 0; while (s[n]) n++; return n;
}
static u64 __attribute__((ms_abi)) shim_strcmp(u64 a, u64 b) {
    const char *pa = (const char *)a; const char *pb = (const char *)b;
    while (*pa && *pa == *pb) { pa++; pb++; }
    return (u64)(i64)(*pa - *pb);
}
static u64 __attribute__((ms_abi)) shim_strcpy(u64 dst, u64 src) {
    char *d = (char *)dst; const char *s = (const char *)src;
    while (*s) *d++ = *s++;
    *d = 0;
    return dst;
}
static u64 __attribute__((ms_abi)) shim_strncmp(u64 a, u64 b, u64 n) {
    const char *pa = (const char *)a; const char *pb = (const char *)b;
    for (u64 i = 0; i < n; i++) {
        if (pa[i] != pb[i]) return (u64)(i64)(pa[i] - pb[i]);
        if (pa[i] == 0) return 0;
    }
    return 0;
}
static u64 __attribute__((ms_abi)) shim_fwrite(u64 ptr, u64 size, u64 nmemb, u64 stream) {
    (void)stream;
    u64 total = size * nmemb;
    shim_output((const char *)ptr, total);
    return nmemb;
}
static u64 __attribute__((ms_abi)) shim_fputs(u64 str, u64 stream) {
    (void)stream;
    shim_output_str((const char *)str);
    return 0;
}
static u64 __attribute__((ms_abi)) shim_fputc(u64 ch, u64 stream) {
    (void)stream;
    serial_putc((char)(ch & 0xFF));
    return ch & 0xFF;
}
/* _initterm / _initterm_e — CRT 初始化，遍历函数指针表调用。
 * 简化：直接跳过（返回 0）。 */
static u64 __attribute__((ms_abi)) shim_initterm(u64 start, u64 end) {
    (void)start; (void)end;
    return 0;
}
static u64 __attribute__((ms_abi)) shim_initterm_e(u64 start, u64 end) {
    (void)start; (void)end;
    return 0;
}

/* 未实现 stub（64位）— 日志告警并返回 0 */
static u64 __attribute__((ms_abi)) shim_unimpl64(u64 a0, u64 a1, u64 a2, u64 a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    log_warn("[PE] unimplemented API called");
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
