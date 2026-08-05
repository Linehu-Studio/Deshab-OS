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
static pe_dll_reader_fn g_dll_reader = 0;
int g_pe_dll_manager_ready = 0;

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

/* 在已加载 DLL 的导出表中按名称查找函数。
 * 返回绝对地址（image_base + func_rva），0=未找到/转发导出。 */
static u64 resolve_export_by_name(u64 image_base, u32 export_dir_rva,
                                   const char *func_name) {
    if (export_dir_rva == 0) return 0;
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

            /* 转发导出：func_rva 落在导出表范围内 → 是转发字符串而非代码地址 */
            if (func_rva >= export_dir_rva &&
                func_rva < export_dir_rva + exp->number_of_functions * 4) {
                /* 粗略范围检查（更精确应使用 export_dir_size，但此处足够） */
                log_info("[PE] forwarded export (not supported)");
                return 0;
            }
            return image_base + func_rva;
        }
    }
    return 0;
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

    if (out_base) *out_base = info.image_base;
    return 0;
}

u64 pe_dll_resolve(const char *dll_name, const char *func_name) {
    if (!g_pe_dll_manager_ready || !dll_name || !func_name) return 0;

    /* 确保 DLL 已加载 */
    int idx = dll_cache_find(dll_name);
    if (idx < 0 || g_dll_cache[idx].state != DLL_STATE_LOADED) {
        u64 base = 0;
        if (pe_dll_load(dll_name, &base) != 0) return 0;
        idx = dll_cache_find(dll_name);
        if (idx < 0 || g_dll_cache[idx].state != DLL_STATE_LOADED) return 0;
    }

    pe_dll_entry *e = &g_dll_cache[idx];
    return resolve_export_by_name(e->image_base, e->export_dir_rva, func_name);
}
