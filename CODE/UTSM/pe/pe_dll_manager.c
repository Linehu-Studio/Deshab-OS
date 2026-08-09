/* pe_dll_manager.c — PE 真实 DLL 加载管理器实现。
 *
 * 加载流程：
 *   1. pe_dll_resolve(dll, func) 被 resolve_imports 调用（shim miss 后）
 *   2. 若 DLL 未缓存，pe_dll_load 读取 DLL 文件 → 复制到 arena → pe_load_image
 *   3. pe_load_image 内部 resolve_imports 递归解析该 DLL 的依赖
 *   4. DLL 加载完成后，解析导出表按名称查找函数地址
 *
 * 内存安全：fat32_read_path 返回 static 缓冲（g_dsk_fat32_filedata），
 * 递归加载会覆盖该缓冲。因此 pe_dll_load 先将文件数据复制到 arena，
 * 再调 pe_load_image，确保递归不破坏外层 pe_data。
 */

#include "pe_dll_manager.h"
#include "pe_loader.h"
#include "pe_shim.h"
#include <utsm/arena.h>
#include <utsm/log.h>
#include <utsm/types.h>
#include <utsm/pe.h>

/* ---- PE 导出目录结构（packed，与 pe_loader.h 风格一致） ---- */
typedef struct __attribute__((packed)) {
    u32 characteristics;
    u32 time_date_stamp;
    u16 major_version;
    u16 minor_version;
    u32 name_rva;
    u32 ordinal_base;
    u32 number_of_functions;
    u32 number_of_names;
    u32 address_of_functions;     /* EAT RVA */
    u32 address_of_names;         /* ENPT RVA */
    u32 address_of_name_ordinals; /* EOT RVA */
} image_export_directory;

/* ---- DLL 缓存 ---- */
#define PE_DLL_CACHE_MAX 32
#define PE_DLL_NAME_MAX  64

enum {
    DLL_STATE_NONE    = 0,
    DLL_STATE_LOADING = 1,
    DLL_STATE_LOADED  = 2,
};

typedef struct {
    char name[PE_DLL_NAME_MAX];  /* 小写 DLL 名 */
    u64  image_base;             /* pe_load_image 返回基址 */
    u32  image_size;             /* SizeOfImage */
    u32  export_dir_rva;         /* 导出表 RVA（0=无导出） */
    u32  export_dir_size;
    int  state;                  /* DLL_STATE_* */
} pe_dll_entry;

static pe_dll_entry g_dll_cache[PE_DLL_CACHE_MAX];

/* ===== 延迟 DllMain 队列 =====
 * pe_load_image 触发递归 pe_dll_load 时，PE32+ 原生执行环境尚未就绪：
 * SSE 未启用（pe_enable_sse 在 load 之后才调用）、仍跑在 16KB 内核栈上。
 * 真实 DLL 的 CRT 初始化含 SSE 指令且栈需求大，立即调用必 #UD/溢栈。
 * 故加载阶段只登记 DllMain，由 pe_service_run 在启用 SSE 并切换到
 * 1MB PE 大栈后调 pe_dll_run_pending_dllmains() 统一执行。 */
#define PE_DLLMAIN_PENDING_MAX 32
typedef struct {
    u64 image_base;
    u64 entry_point;
    char name[PE_DLL_NAME_MAX];
} pe_dllmain_pending;
static pe_dllmain_pending g_dllmain_pending[PE_DLLMAIN_PENDING_MAX];
static int g_dllmain_pending_count = 0;
static int g_dllmain_defer = 0;

/* enable=1 开启延迟模式并清空队列（每次 pe_service_run 加载前调用）；
 * enable=0 恢复立即调用（兜底路径用）。 */
void pe_dll_defer_dllmain(int enable) {
    g_dllmain_defer = enable;
    if (enable) g_dllmain_pending_count = 0;
}

/* 在大栈 + SSE 环境下执行全部已登记的 DllMain(ATTACH)。
 * 执行完毕清空队列并关闭延迟模式。 */
void pe_dll_run_pending_dllmains(void) {
    for (int i = 0; i < g_dllmain_pending_count; i++) {
        pe_dllmain_pending *p = &g_dllmain_pending[i];
        typedef int (__attribute__((ms_abi)) *dllmain_fn)(u64, u32, u64);
        dllmain_fn dm = (dllmain_fn)p->entry_point;
        int ok = dm(p->image_base, 1 /* DLL_PROCESS_ATTACH */, 0);
        if (!ok) {
            log_warn("[PE] DllMain returned FALSE (init failed)");
            log_info(p->name);
        } else {
            log_info("[PE] DllMain ATTACH ok");
        }
    }
    g_dllmain_pending_count = 0;
    g_dllmain_defer = 0;
}
static pe_dll_reader_fn g_dll_reader = 0;
int g_pe_dll_manager_ready = 0;

/* 转发导出递归深度上限（A->B->C 链） */
#define PE_DLL_FWD_DEPTH_MAX 8

/* ---- API set 重定向 ----
 * Windows 10+ 的 api-ms-win-crt-*.dll 是 API set 虚拟 DLL，
 * 物理上不存在文件，由 loader 按 schema 重定向到 ucrtbase.dll。
 * 此处实现等价映射：api-ms-win-crt-* -> ucrtbase.dll。
 * 返回目标 DLL 名（静态字符串），或 NULL 表示非 API set。 */
static const char *apiset_redirect(const char *dll_name) {
    if (!dll_name) return 0;
    /* 大小写不敏感前缀匹配 "api-ms-win-crt-" */
    static const char prefix[] = "api-ms-win-crt-";
    int i = 0;
    while (prefix[i]) {
        char c = dll_name[i];
        if (!c) return 0;
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        if (c != prefix[i]) return 0;
        i++;
    }
    return "ucrtbase.dll";
}

/* ---- 辅助函数 ---- */

/* ASCII 大小写不敏感字符串比较（freestanding，无 strcmp） */
static int dll_streq_ci(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'a' && ca <= 'z') ca = (char)(ca - 32);
        if (cb >= 'a' && cb <= 'z') cb = (char)(cb - 32);
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == *b;
}

/* 将 DLL 名转为小写到 dst（最多 n-1 字符 + NUL） */
static void dll_name_lower(const char *src, char *dst, int n) {
    int i = 0;
    while (src[i] && i < n - 1) {
        char c = src[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        dst[i] = c;
        i++;
    }
    dst[i] = 0;
}

/* 在缓存中查找 DLL 名（大小写不敏感）。返回索引，-1 未找到。 */
static int dll_cache_find(const char *dll_name) {
    char lower[PE_DLL_NAME_MAX];
    dll_name_lower(dll_name, lower, sizeof(lower));
    for (int i = 0; i < PE_DLL_CACHE_MAX; i++) {
        if (g_dll_cache[i].state == DLL_STATE_NONE) continue;
        if (dll_streq_ci(g_dll_cache[i].name, lower)) return i;
    }
    return -1;
}

/* 在缓存中分配一个空闲槽。返回索引，-1 缓存满。 */
static int dll_cache_alloc(void) {
    for (int i = 0; i < PE_DLL_CACHE_MAX; i++) {
        if (g_dll_cache[i].state == DLL_STATE_NONE) return i;
    }
    return -1;
}

/* 从已加载的 PE 镜像中提取导出表 RVA 和 size。
 * image 指向 pe_load_image 分配的镜像基址（headers 已映射）。 */
static void extract_export_dir(u64 image_base, u32 *out_rva, u32 *out_size) {
    *out_rva = 0;
    *out_size = 0;
    const u8 *img = (const u8 *)image_base;

    /* DOS header */
    const image_dos_header *dos = (const image_dos_header *)img;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;

    /* NT headers */
    const u8 *nt = img + dos->e_lfanew;
    if (*(const u32 *)nt != IMAGE_NT_SIGNATURE) return;
    const image_file_header *fh = (const image_file_header *)(nt + 4);
    const void *opt = (const void *)(fh + 1);

    u16 magic = *(const u16 *)opt;
    const image_data_directory *data_dir;
    if (magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        const image_optional_header64 *oh = (const image_optional_header64 *)opt;
        data_dir = oh->data_directory;
    } else {
        const image_optional_header32 *oh = (const image_optional_header32 *)opt;
        data_dir = oh->data_directory;
    }

    *out_rva = data_dir[IMAGE_DIRECTORY_ENTRY_EXPORT].virtual_address;
    *out_size = data_dir[IMAGE_DIRECTORY_ENTRY_EXPORT].size;
}

/* 按序号在已加载 DLL 的导出表中查找函数（转发串 "#123" 用）。
 * 返回绝对地址，0=未找到/越界。 */
static u64 resolve_export_by_ordinal(u64 image_base, u32 export_dir_rva,
                                      u32 ordinal) {
    if (export_dir_rva == 0) return 0;
    const u8 *img = (const u8 *)image_base;
    const image_export_directory *exp =
        (const image_export_directory *)(img + export_dir_rva);
    if (ordinal < exp->ordinal_base) return 0;
    u32 idx = ordinal - exp->ordinal_base;
    if (idx >= exp->number_of_functions) return 0;
    const u32 *functions = (const u32 *)(img + exp->address_of_functions);
    u32 func_rva = functions[idx];
    if (func_rva == 0) return 0;
    return image_base + func_rva;
}

/* 解析转发导出串（"DLL.Func" 或 "DLL.#123"）。
 * fwd_str 位于 image_base+func_rva 处（导出目录数据区内）。
 * depth 防止 A→B→A 循环转发。返回绝对地址，0=失败。 */
static u64 resolve_forwarded_export(u64 image_base, u32 func_rva, int depth);

/* 在已加载 DLL 的导出表中按名称查找函数。
 * 返回绝对地址（image_base + func_rva），0=未找到。
 * 转发导出自动跟随解析（PE 规范：func_rva 落在导出目录数据区内即为转发串）。 */
static u64 resolve_export_by_name(u64 image_base, u32 export_dir_rva,
                                   u32 export_dir_size, const char *func_name,
                                   int depth) {
    if (export_dir_rva == 0) return 0;
    if (depth > PE_DLL_FWD_DEPTH_MAX) return 0;
    const u8 *img = (const u8 *)image_base;
    const image_export_directory *exp =
        (const image_export_directory *)(img + export_dir_rva);
    if (exp->number_of_names == 0) return 0;

    const u32 *names = (const u32 *)(img + exp->address_of_names);
    const u16 *ordinals = (const u16 *)(img + exp->address_of_name_ordinals);
    const u32 *functions = (const u32 *)(img + exp->address_of_functions);

    for (u32 i = 0; i < exp->number_of_names; i++) {
        const char *name = (const char *)(img + names[i]);
        if (dll_streq_ci(name, func_name)) {
            u16 eat_index = ordinals[i];
            if (eat_index >= exp->number_of_functions) return 0;
            u32 func_rva = functions[eat_index];

            /* 转发导出（PE 规范）：func_rva 落在导出目录数据区
             * [export_dir_rva, export_dir_rva+export_dir_size) 内 →
             * 该处是 "DLL.Func" 转发字符串而非代码地址 */
            if (func_rva >= export_dir_rva &&
                func_rva < export_dir_rva + export_dir_size) {
                return resolve_forwarded_export(image_base, func_rva, depth + 1);
            }
            return image_base + func_rva;
        }
    }
    return 0;
}

/* 转发串解析："NTDLL.RtlInitUnicodeString" 或 "KERNEL32.#123"。
 * DLL 名通常不带 .dll 后缀（如 "UCRTBASE"），需补全。 */
static u64 resolve_forwarded_export(u64 image_base, u32 func_rva, int depth) {
    if (depth > PE_DLL_FWD_DEPTH_MAX) {
        log_warn("[PE] forwarded export chain too deep");
        return 0;
    }
    const char *fwd = (const char *)(image_base + func_rva);

    /* 拷贝到本地缓冲并找分隔点 '.'（转发串最大 ~64 字符，防御截断） */
    char buf[96];
    u32 i = 0;
    while (i + 1 < sizeof(buf) && fwd[i] && fwd[i] != ' ') {
        buf[i] = fwd[i];
        i++;
    }
    buf[i] = 0;

    u32 dot = 0;
    while (buf[dot] && buf[dot] != '.') dot++;
    if (buf[dot] != '.' || dot == 0 || buf[dot + 1] == 0) {
        log_warn("[PE] malformed forwarder string");
        return 0;
    }
    buf[dot] = 0;
    const char *target_func = buf + dot + 1;

    /* 目标 DLL 名：小写化 + 补 ".dll" 后缀 */
    char tdll[PE_DLL_NAME_MAX];
    dll_name_lower(buf, tdll, sizeof(tdll));
    int tl = 0;
    while (tdll[tl]) tl++;
    if (tl + 4 < (int)sizeof(tdll)) {
        /* 已有 ".dll" 后缀则不重复添加 */
        if (!(tl >= 4 && tdll[tl-4] == '.' &&
              (tdll[tl-3] == 'd') && (tdll[tl-2] == 'l') && (tdll[tl-1] == 'l'))) {
            tdll[tl++] = '.'; tdll[tl++] = 'd';
            tdll[tl++] = 'l'; tdll[tl++] = 'l';
            tdll[tl] = 0;
        }
    }

    /* 序号转发："#123" */
    if (target_func[0] == '#') {
        u32 ord = 0;
        const char *p = target_func + 1;
        if (*p == 0) return 0;
        while (*p >= '0' && *p <= '9') {
            ord = ord * 10 + (u32)(*p - '0');
            p++;
        }
        if (ord == 0 || ord > 0xFFFF) return 0;
        /* 需先确保目标 DLL 已加载 */
        u64 base = 0;
        if (pe_dll_load(tdll, &base) != 0) return 0;
        int idx = dll_cache_find(tdll);
        if (idx < 0) return 0;
        u64 addr = resolve_export_by_ordinal(base, g_dll_cache[idx].export_dir_rva, ord);
        if (addr == 0) {
            log_warn("[PE] ordinal forward target not found");
        }
        return addr;
    }

    /* 名称转发：递归走 pe_dll_resolve（带深度计） */
    return pe_dll_resolve_ex(tdll, target_func, depth);
}

/* ---- 公共接口 ---- */

void pe_dll_manager_init(pe_dll_reader_fn reader) {
    g_dll_reader = reader;
    for (int i = 0; i < PE_DLL_CACHE_MAX; i++) {
        g_dll_cache[i].state = DLL_STATE_NONE;
    }
    g_pe_dll_manager_ready = 1;
    log_info("[PE] DLL manager initialized (SYSTEM/lib)");
}

int pe_dll_load(const char *dll_name, u64 *out_base) {
    if (!g_pe_dll_manager_ready || !g_dll_reader || !dll_name) {
        if (out_base) *out_base = 0;
        return -1;
    }

    /* API set 重定向：api-ms-win-crt-* -> ucrtbase.dll */
    const char *redirected = apiset_redirect(dll_name);
    if (redirected) dll_name = redirected;

    /* 查缓存 */
    int idx = dll_cache_find(dll_name);
    if (idx >= 0) {
        pe_dll_entry *e = &g_dll_cache[idx];
        if (e->state == DLL_STATE_LOADED) {
            if (out_base) *out_base = e->image_base;
            return 0;
        }
        if (e->state == DLL_STATE_LOADING) {
            /* 循环依赖：返回 -1，调用方填 unimpl stub */
            if (out_base) *out_base = 0;
            return -1;
        }
    }

    /* 分配缓存槽 */
    idx = dll_cache_alloc();
    if (idx < 0) {
        log_warn("[PE] DLL cache full, cannot load");
        if (out_base) *out_base = 0;
        return -2;
    }

    pe_dll_entry *entry = &g_dll_cache[idx];
    dll_name_lower(dll_name, entry->name, PE_DLL_NAME_MAX);
    entry->state = DLL_STATE_LOADING;
    entry->image_base = 0;
    entry->export_dir_rva = 0;
    entry->export_dir_size = 0;

    /* 读取 DLL 文件 */
    u8 *file_data = 0;
    u32 file_size = 0;
    if (g_dll_reader(dll_name, &file_data, &file_size) != 0 || !file_data || file_size == 0) {
        entry->state = DLL_STATE_NONE;  /* 回收槽 */
        if (out_base) *out_base = 0;
        return -3;
    }

    /* 复制文件数据到 arena（防止递归加载覆盖 fat32_read_path 的 static 缓冲） */
    u8 *file_copy = (u8 *)kmem_alloc_aligned(file_size, 16);
    if (!file_copy) {
        entry->state = DLL_STATE_NONE;
        if (out_base) *out_base = 0;
        return -4;
    }
    for (u32 i = 0; i < file_size; i++) file_copy[i] = file_data[i];

    /* 加载 PE 镜像（内部会递归 resolve_imports → 可能再次 pe_dll_load） */
    pe_image_info info;
    if (pe_load_image(file_copy, file_size, &info) != 0) {
        log_warn("[PE] DLL load failed: pe_load_image error");
        log_info(dll_name);
        entry->state = DLL_STATE_NONE;
        if (out_base) *out_base = 0;
        return -5;
    }

    /* 提取导出表信息 */
    extract_export_dir(info.image_base, &entry->export_dir_rva,
                       &entry->export_dir_size);
    entry->image_base = info.image_base;
    entry->image_size = (u32)info.image_size;
    entry->state = DLL_STATE_LOADED;

    log_info("[PE] loaded DLL:");
    log_info(dll_name);
    log_hex64("[PE] DLL base=", info.image_base);

    /* 调用 DllMain(hinst, DLL_PROCESS_ATTACH=1, NULL)。
     * 真实 DLL（如 ucrtbase）依赖入口完成 CRT/TLS 初始化。
     * 无入口点（AddressOfEntryPoint==0 → entry==image_base）则跳过。
     * 返回 0（初始化失败）只记告警不阻断：无状态函数仍可用。
     * 延迟模式（pe_service_run 加载阶段）：只登记不调用，待 SSE 启用
     * 且切换到 1MB PE 大栈后由 pe_dll_run_pending_dllmains 统一执行。 */
    if (info.entry_point != info.image_base) {
        if (g_dllmain_defer) {
            if (g_dllmain_pending_count < PE_DLLMAIN_PENDING_MAX) {
                pe_dllmain_pending *p = &g_dllmain_pending[g_dllmain_pending_count++];
                p->image_base = info.image_base;
                p->entry_point = info.entry_point;
                int i = 0;
                for (; i < PE_DLL_NAME_MAX - 1 && entry->name[i]; i++) p->name[i] = entry->name[i];
                p->name[i] = 0;
                log_info("[PE] DllMain deferred");
            } else {
                log_warn("[PE] DllMain pending queue full, skipped");
            }
        } else {
            typedef int (__attribute__((ms_abi)) *dllmain_fn)(u64, u32, u64);
            dllmain_fn dm = (dllmain_fn)info.entry_point;
            int ok = dm(info.image_base, 1 /* DLL_PROCESS_ATTACH */, 0);
            if (!ok) {
                log_warn("[PE] DllMain returned FALSE (init failed)");
                log_info(dll_name);
            } else {
                log_info("[PE] DllMain ATTACH ok");
            }
        }
    }

    if (out_base) *out_base = info.image_base;
    return 0;
}

u64 pe_dll_resolve_ex(const char *dll_name, const char *func_name, int depth) {
    if (!g_pe_dll_manager_ready || !dll_name || !func_name) return 0;

    /* API set 重定向：api-ms-win-crt-* -> ucrtbase.dll */
    const char *redirected = apiset_redirect(dll_name);
    if (redirected) dll_name = redirected;

    /* 确保 DLL 已加载 */
    int idx = dll_cache_find(dll_name);
    if (idx < 0 || g_dll_cache[idx].state != DLL_STATE_LOADED) {
        u64 base = 0;
        if (pe_dll_load(dll_name, &base) != 0) return 0;
        idx = dll_cache_find(dll_name);
        if (idx < 0 || g_dll_cache[idx].state != DLL_STATE_LOADED) return 0;
    }

    pe_dll_entry *e = &g_dll_cache[idx];
    return resolve_export_by_name(e->image_base, e->export_dir_rva,
                                  e->export_dir_size, func_name, depth);
}

u64 pe_dll_resolve(const char *dll_name, const char *func_name) {
    return pe_dll_resolve_ex(dll_name, func_name, 0);
}

u64 pe_dll_resolve_in_base(u64 image_base, const char *func_name, u32 ordinal) {
    if (!g_pe_dll_manager_ready || image_base == 0) return 0;
    for (int i = 0; i < PE_DLL_CACHE_MAX; i++) {
        pe_dll_entry *e = &g_dll_cache[i];
        if (e->state != DLL_STATE_LOADED || e->image_base != image_base) continue;
        if (ordinal != 0)
            return resolve_export_by_ordinal(e->image_base, e->export_dir_rva, ordinal);
        if (func_name)
            return resolve_export_by_name(e->image_base, e->export_dir_rva,
                                          e->export_dir_size, func_name, 0);
        return 0;
    }
    return 0;
}
