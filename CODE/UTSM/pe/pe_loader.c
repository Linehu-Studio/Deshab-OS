/* pe_loader.c — PE32+/PE32 解析、节区映射、重定位、import 解析。
 *
 * 加载流程：
 *   1. 验证 MZ + PE 签名
 *   2. 区分 PE32（OptionalHeader.Magic=0x10b）/ PE32+（0x20b）
 *   3. 分配 SizeOfImage 字节 RWX 内存（kmem_alloc_aligned）
 *   4. 复制 SizeOfHeaders + 各节区 raw data 到 virtual_address
 *   5. 应用基址重定位（.reloc 节区：DIR64 for PE32+，HIGHLOW for PE32）
 *   6. 解析 import 表，用 shim 函数指针填充 IAT
 *   7. 构建 IAT 条目表（供 32位解释器 CALL 拦截）
 */

#include "pe_loader.h"
#include "pe_shim.h"
#include "x86emu32.h"
#include "../include/utsm/pe.h"
#include <utsm/arena.h>
#include <utsm/log.h>
#include <utsm/types.h>

/* 合成 shim 地址范围（32位解释器用）：0x00010000..0x0001FFFF */
#define PE_SHIM_ADDR_BASE 0x00010000u
#define PE_SHIM_ADDR_END  0x00020000u
#define PE_SHIM_UNIMPL    0x0001FFFFu

/* 全局 IAT 表（pe_loader 填充，x86emu32 读取） */
static pe_iat_entry g_iat_entries[256];
static int g_iat_count;

/* ---- 辅助：RVA → 文件偏移 ---- */
static u32 rva_to_offset(const image_section_header *sections, int nsec,
                         u32 rva, u32 *out_size) {
    for (int i = 0; i < nsec; i++) {
        u32 va = sections[i].virtual_address;
        u32 vs = sections[i].virtual_size;
        if (vs == 0) vs = sections[i].size_of_raw_data;
        if (rva >= va && rva < va + vs) {
            if (out_size) *out_size = vs;
            return sections[i].pointer_to_raw_data + (rva - va);
        }
    }
    if (out_size) *out_size = 0;
    return 0xFFFFFFFF;
}

/* ---- 解析 NT 头 ---- */
typedef struct {
    const image_file_header *file_hdr;
    const void *optional_hdr;
    int is_pe32_plus;
    u64 image_base;        /* 链接时首选基址（PE32+ 为 64 位，如 0x140000000，必须全宽保留） */
    u32 size_of_image;
    u32 size_of_headers;
    u32 entry_rva;
    const image_data_directory *data_dir;
} pe_nt_info;

static int parse_nt_headers(const void *pe_data, u64 size, pe_nt_info *out) {
    if (size < sizeof(image_dos_header)) return -1;
    const image_dos_header *dos = (const image_dos_header *)pe_data;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return -2;
    if (dos->e_lfanew + 4 + sizeof(image_file_header) > size) return -3;

    const u8 *nt_base = (const u8 *)pe_data + dos->e_lfanew;
    u32 sig = *(const u32 *)nt_base;
    if (sig != IMAGE_NT_SIGNATURE) return -4;

    const image_file_header *fh = (const image_file_header *)(nt_base + 4);
    out->file_hdr = fh;

    const void *opt = (const void *)(nt_base + 4 + sizeof(image_file_header));
    out->optional_hdr = opt;

    u16 magic = *(const u16 *)opt;
    if (magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        const image_optional_header64 *oh = (const image_optional_header64 *)opt;
        out->is_pe32_plus = 1;
        out->image_base = oh->image_base;   /* PE32+: u64 全宽，禁止截断（重定位 delta 依赖高位） */
        out->size_of_image = oh->size_of_image;
        out->size_of_headers = oh->size_of_headers;
        out->entry_rva = oh->address_of_entry_point;
        out->data_dir = oh->data_directory;
    } else if (magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
        const image_optional_header32 *oh = (const image_optional_header32 *)opt;
        out->is_pe32_plus = 0;
        out->image_base = oh->image_base;
        out->size_of_image = oh->size_of_image;
        out->size_of_headers = oh->size_of_headers;
        out->entry_rva = oh->address_of_entry_point;
        out->data_dir = oh->data_directory;
    } else {
        return -5;
    }

    if (out->size_of_image == 0 || out->size_of_image > 64 * 1024 * 1024) return -6;
    if (out->entry_rva == 0 || out->entry_rva >= out->size_of_image) return -7;
    return 0;
}

/* ---- 应用基址重定位 ---- */
static void apply_relocations(u8 *image, u32 image_size, u64 image_base_actual, u64 image_base_preferred,
                              const image_data_directory *reloc_dir,
                              const image_section_header *sections, int nsec,
                              const void *pe_data, u64 pe_size, int is_pe32_plus) {
    if (reloc_dir->virtual_address == 0 || reloc_dir->size == 0) return;

    u32 block_rva = reloc_dir->virtual_address;
    u32 block_end = block_rva + reloc_dir->size;
    i64 delta = (i64)image_base_actual - (i64)image_base_preferred;
    if (delta == 0) return;  /* 加载到首选基址，无需重定位 */

    while (block_rva < block_end) {
        u32 dummy_size;
        u32 file_off = rva_to_offset(sections, nsec, block_rva, &dummy_size);
        if (file_off == 0xFFFFFFFF || file_off + 8 > pe_size) break;

        const u8 *block = (const u8 *)pe_data + file_off;
        u32 page_rva = *(const u32 *)block;
        u32 block_size = *(const u32 *)(block + 4);
        if (block_size < 8 || block_rva + block_size > block_end) break;

        u32 entry_count = (block_size - 8) / 2;
        const u16 *entries = (const u16 *)(block + 8);

        for (u32 i = 0; i < entry_count; i++) {
            u16 e = entries[i];
            u32 type = (e >> 12) & 0xF;
            u32 offset = e & 0xFFF;

            if (type == IMAGE_REL_BASED_ABSOLUTE) continue;

            u32 target_rva = page_rva + offset;
            /* image 缓冲区按 RVA 排列（image[0] = RVA 0） */
            if (target_rva >= (u32)image_size) continue;
            u8 *target = image + target_rva;
            if (type == IMAGE_REL_BASED_DIR64) {
                u64 val = *(u64 *)target;
                val += (u64)delta;
                *(u64 *)target = val;
            } else if (type == IMAGE_REL_BASED_HIGHLOW) {
                u32 val = *(u32 *)target;
                val += (u32)delta;
                *(u32 *)target = val;
            } else {
                /* 其他重定位类型暂不支持 */
            }
        }
        block_rva += block_size;
    }
    (void)is_pe32_plus;
}

/* ---- 解析 imports，填充 IAT ---- */
static void resolve_imports(u8 *image, u64 image_base_actual,
                            const image_data_directory *import_dir,
                            const image_section_header *sections, int nsec,
                            const void *pe_data, u64 pe_size, int is_pe32_plus) {
    (void)image_base_actual;  /* PE32 IAT 用合成地址，不依赖 actual_base */
    if (import_dir->virtual_address == 0 || import_dir->size == 0) return;

    g_iat_count = 0;
    u32 dummy;
    u32 imp_off = rva_to_offset(sections, nsec, import_dir->virtual_address, &dummy);
    if (imp_off == 0xFFFFFFFF) return;

    const image_import_descriptor *imp = (const image_import_descriptor *)
        ((const u8 *)pe_data + imp_off);

    for (int dll_idx = 0; ; dll_idx++) {
        const image_import_descriptor *desc = &imp[dll_idx];
        if (desc->name == 0 && desc->first_thunk == 0) break;

        /* DLL 名 */
        u32 name_off = rva_to_offset(sections, nsec, desc->name, &dummy);
        if (name_off == 0xFFFFFFFF || name_off >= pe_size) continue;
        const char *dll_name = (const char *)pe_data + name_off;

        /* IAT (first_thunk) 与 INT (original_first_thunk) */
        u32 iat_rva = desc->first_thunk;
        u32 int_rva = desc->original_first_thunk;
        if (int_rva == 0) int_rva = iat_rva;  /* 无 INT，直接用 IAT */

        u32 iat_off = rva_to_offset(sections, nsec, iat_rva, &dummy);
        u32 int_off = rva_to_offset(sections, nsec, int_rva, &dummy);

        for (int fn_idx = 0; ; fn_idx++) {
            u32 iat_entry_off = iat_off + (u32)fn_idx * (is_pe32_plus ? 8 : 4);
            if (iat_entry_off + (is_pe32_plus ? 8 : 4) > pe_size) break;

            u32 int_entry_off = int_off + (u32)fn_idx * (is_pe32_plus ? 8 : 4);
            if (int_entry_off + (is_pe32_plus ? 8 : 4) > pe_size) break;

            /* 读 INT 条目判断函数名/序号 */
            u64 thunk = 0;
            if (is_pe32_plus) {
                thunk = *(const u64 *)((const u8 *)pe_data + int_entry_off);
            } else {
                thunk = *(const u32 *)((const u8 *)pe_data + int_entry_off);
            }
            if (thunk == 0) break;  /* 结束 */

            const char *func_name = 0;
            if (thunk & 0x80000000ULL) {
                /* 序号导入：暂不支持，记为未实现 */
                func_name = "#ordinal";
            } else {
                /* 名称导入：thunk 是 IMAGE_IMPORT_BY_NAME 的 RVA */
                u32 hint_rva = (u32)thunk;
                u32 hint_off = rva_to_offset(sections, nsec, hint_rva, &dummy);
                if (hint_off != 0xFFFFFFFF && hint_off + 2 < pe_size) {
                    func_name = (const char *)pe_data + hint_off + 2;  /* 跳过 hint(2字节) */
                }
            }

            /* 查 shim 表 */
            int shim_idx = -1;
            if (func_name && func_name[0] != '#') {
                shim_idx = pe_shim_lookup(dll_name, func_name);
            }

            /* 填充 IAT（image 缓冲区内） */
            u8 *iat_ptr = image + iat_rva + (u32)fn_idx * (is_pe32_plus ? 8 : 4);
            if (is_pe32_plus) {
                u64 fn_addr;
                if (shim_idx >= 0) {
                    const pe_shim_entry *se = pe_shim_get(shim_idx);
                    fn_addr = (u64)se->fn64;
                } else {
                    fn_addr = pe_shim_unimpl_stub();
                }
                *(u64 *)iat_ptr = fn_addr;
            } else {
                /* 32位：填合成地址，解释器拦截 */
                u32 synth_addr;
                if (shim_idx >= 0) {
                    synth_addr = PE_SHIM_ADDR_BASE | (u32)(shim_idx & 0xFFFF);
                } else {
                    synth_addr = PE_SHIM_UNIMPL;
                }
                *(u32 *)iat_ptr = synth_addr;

                /* 记录 IAT 条目供解释器查询 */
                if (g_iat_count < 255) {
                    g_iat_entries[g_iat_count].iat_rva = iat_rva + (u32)fn_idx * 4;
                    g_iat_entries[g_iat_count].shim_index = shim_idx;
                    g_iat_entries[g_iat_count].target_addr = synth_addr;
                    g_iat_count++;
                }
            }

            if (shim_idx < 0 && func_name) {
                log_info("[PE] unimplemented import");
                serial_write(dll_name);
                serial_write("!");
                serial_write(func_name);
                serial_write("\n");
            }
        }
    }
}

/* ---- 主加载函数 ---- */
int pe_load_image(const void *pe_data, u64 size, pe_image_info *out) {
    if (!pe_data || !out || size < sizeof(image_dos_header)) return -1;

    pe_nt_info nt;
    int rc = parse_nt_headers(pe_data, size, &nt);
    if (rc != 0) {
        log_error("[PE] NT header parse failed");
        log_hex64("[PE] rc=", (u64)(i64)rc);
        return -2;
    }

    /* 节区头 */
    int nsec = nt.file_hdr->number_of_sections;
    if (nsec <= 0 || nsec > 96) return -3;
    usize opt_size = nt.file_hdr->size_of_optional_header;
    const image_section_header *sections = (const image_section_header *)
        ((const u8 *)nt.optional_hdr + opt_size);

    /* 分配 SizeOfImage RWX 内存 */
    u32 img_size = nt.size_of_image;
    u8 *image = (u8 *)kmem_alloc_aligned(img_size, 0x1000);
    if (!image) {
        log_error("[PE] alloc image failed");
        return -4;
    }
    /* 清零 */
    for (u32 i = 0; i < img_size; i++) image[i] = 0;

    /* 复制 headers */
    u32 hdr_copy = nt.size_of_headers;
    if (hdr_copy > size) hdr_copy = (u32)size;
    if (hdr_copy > img_size) hdr_copy = img_size;
    for (u32 i = 0; i < hdr_copy; i++) image[i] = ((const u8 *)pe_data)[i];

    /* 复制各节区 raw data */
    for (int i = 0; i < nsec; i++) {
        u32 va = sections[i].virtual_address;
        u32 raw_size = sections[i].size_of_raw_data;
        u32 raw_off = sections[i].pointer_to_raw_data;
        if (va >= img_size) continue;
        u32 copy_size = raw_size;
        if (va + copy_size > img_size) copy_size = img_size - va;
        if (raw_off + copy_size > size) {
            copy_size = (u32)(size - raw_off);
        }
        if (raw_off + copy_size > size) continue;
        for (u32 j = 0; j < copy_size; j++) {
            image[va + j] = ((const u8 *)pe_data)[raw_off + j];
        }
    }

    /* 基址重定位 */
    u64 actual_base = (u64)image;
    u64 preferred_base = nt.image_base;
    /* image 缓冲区按 RVA 排列（image[0]=RVA 0），重定位写入 image 内 */
    /* 注意：apply_relocations 直接操作 image 缓冲区，delta = actual_base - preferred_base */
    {
        const image_data_directory *reloc_dir = &nt.data_dir[IMAGE_DIRECTORY_ENTRY_BASERELOC];
        /* 重定位 target 是 RVA，写入 image[rva]。
         * 但 image 加载在 actual_base，代码中绝对地址 = actual_base + RVA。
         * 重定位修正量 = actual_base - preferred_base。
         * image[rva] 处的值原为 preferred_base + rva_offset（链接时假设），
         * 修正后应为 actual_base + rva_offset，即加 delta。 */
        apply_relocations(image, img_size, actual_base, preferred_base, reloc_dir,
                          sections, nsec, pe_data, size, nt.is_pe32_plus);
    }

    /* 解析 imports */
    {
        const image_data_directory *import_dir = &nt.data_dir[IMAGE_DIRECTORY_ENTRY_IMPORT];
        resolve_imports(image, actual_base, import_dir, sections, nsec,
                        pe_data, size, nt.is_pe32_plus);
    }

    out->image_base = actual_base;
    out->image_size = img_size;
    out->entry_point = actual_base + nt.entry_rva;
    out->is_pe32_plus = nt.is_pe32_plus;
    out->load_ok = 1;

    log_hex64("[PE] loaded base=", actual_base);
    log_hex64("[PE] image size=", img_size);
    log_hex64("[PE] entry=", out->entry_point);
    log_info(nt.is_pe32_plus ? "[PE] PE32+ (64-bit)" : "[PE] PE32 (32-bit)");
    return 0;
}

void pe_unload_image(pe_image_info *info) {
    if (!info || !info->load_ok) return;
    /* kmem_alloc_aligned 无对应 free（arena 分配器），标记为未加载即可 */
    info->load_ok = 0;
}

u64 pe_get_image_base(const pe_image_info *info) {
    return info ? info->image_base : 0;
}

u64 pe_get_image_size(const pe_image_info *info) {
    return info ? info->image_size : 0;
}

/* 供 x86emu32 获取 IAT 表 */
pe_iat_entry *pe_get_iat_entries(int *count) {
    if (count) *count = g_iat_count;
    return g_iat_entries;
}
