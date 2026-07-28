#ifndef PE_LOADER_H
#define PE_LOADER_H

/* pe_loader.h — PE32+/PE32 解析与加载器内部接口。 */

#include <utsm/types.h>
#include <utsm/pe.h>

/* ===== PE 常量 ===== */
#define IMAGE_DOS_SIGNATURE    0x5A4D     /* "MZ" */
#define IMAGE_NT_SIGNATURE     0x00004550 /* "PE\0\0" */
#define IMAGE_NT_OPTIONAL_HDR32_MAGIC 0x10b
#define IMAGE_NT_OPTIONAL_HDR64_MAGIC 0x20b

/* Section characteristics */
#define IMAGE_SCN_MEM_EXECUTE  0x20000000
#define IMAGE_SCN_MEM_READ     0x40000000
#define IMAGE_SCN_MEM_WRITE    0x80000000

/* Data directory indices */
#define IMAGE_DIRECTORY_ENTRY_EXPORT    0
#define IMAGE_DIRECTORY_ENTRY_IMPORT    1
#define IMAGE_DIRECTORY_ENTRY_RESOURCE  2
#define IMAGE_DIRECTORY_ENTRY_BASERELOC 5
#define IMAGE_DIRECTORY_ENTRY_IAT       12

/* Relocation types */
#define IMAGE_REL_BASED_ABSOLUTE  0
#define IMAGE_REL_BASED_HIGHLOW   3  /* 32位: base + offset 写 32位 */
#define IMAGE_REL_BASED_DIR64     10 /* 64位: base + offset 写 64位 */

/* ===== PE 结构体定义（packed） ===== */

typedef struct __attribute__((packed)) {
    u16 e_magic;      /* "MZ" */
    u16 e_cblp;
    u16 e_cp;
    u16 e_crlc;
    u16 e_cparhdr;
    u16 e_minalloc;
    u16 e_maxalloc;
    u16 e_ss;
    u16 e_sp;
    u16 e_csum;
    u16 e_ip;
    u16 e_cs;
    u16 e_lfarlc;
    u16 e_ovno;
    u16 e_res[4];
    u16 e_oemid;
    u16 e_oeminfo;
    u16 e_res2[10];
    u32 e_lfanew;     /* PE 头偏移 */
} image_dos_header;

typedef struct __attribute__((packed)) {
    u16 machine;
    u16 number_of_sections;
    u32 time_date_stamp;
    u32 pointer_to_symbol_table;
    u32 number_of_symbols;
    u16 size_of_optional_header;
    u16 characteristics;
} image_file_header;

typedef struct __attribute__((packed)) {
    u32 virtual_address;
    u32 size;
} image_data_directory;

typedef struct __attribute__((packed)) {
    u16 magic;
    u8  major_linker_version;
    u8  minor_linker_version;
    u32 size_of_code;
    u32 size_of_initialized_data;
    u32 size_of_uninitialized_data;
    u32 address_of_entry_point;
    u32 base_of_code;
    u32 base_of_data;              /* PE32 only */
    u32 image_base;                /* PE32: 32位 */
    u32 section_alignment;
    u32 file_alignment;
    u16 major_os_version;
    u16 minor_os_version;
    u16 major_image_version;
    u16 minor_image_version;
    u16 major_subsystem_version;
    u16 minor_subsystem_version;
    u32 win32_version_value;
    u32 size_of_image;
    u32 size_of_headers;
    u32 check_sum;
    u16 subsystem;
    u16 dll_characteristics;
    u32 size_of_stack_reserve;
    u32 size_of_stack_commit;
    u32 size_of_heap_reserve;
    u32 size_of_heap_commit;
    u32 loader_flags;
    u32 number_of_rva_and_sizes;
    image_data_directory data_directory[16];
} image_optional_header32;

typedef struct __attribute__((packed)) {
    u16 magic;
    u8  major_linker_version;
    u8  minor_linker_version;
    u32 size_of_code;
    u32 size_of_initialized_data;
    u32 size_of_uninitialized_data;
    u32 address_of_entry_point;
    u32 base_of_code;
    u64 image_base;                /* PE32+: 64位 */
    u32 section_alignment;
    u32 file_alignment;
    u16 major_os_version;
    u16 minor_os_version;
    u16 major_image_version;
    u16 minor_image_version;
    u16 major_subsystem_version;
    u16 minor_subsystem_version;
    u32 win32_version_value;
    u32 size_of_image;
    u32 size_of_headers;
    u32 check_sum;
    u16 subsystem;
    u16 dll_characteristics;
    u64 size_of_stack_reserve;
    u64 size_of_stack_commit;
    u64 size_of_heap_reserve;
    u64 size_of_heap_commit;
    u32 loader_flags;
    u32 number_of_rva_and_sizes;
    image_data_directory data_directory[16];
} image_optional_header64;

typedef struct __attribute__((packed)) {
    u8  name[8];
    u32 virtual_size;
    u32 virtual_address;
    u32 size_of_raw_data;
    u32 pointer_to_raw_data;
    u32 pointer_to_relocations;
    u32 pointer_to_linenumbers;
    u16 number_of_relocations;
    u16 number_of_linenumbers;
    u32 characteristics;
} image_section_header;

typedef struct __attribute__((packed)) {
    u32 original_first_thunk;  /* INT (Import Name Table) RVA */
    u32 time_date_stamp;
    u32 forwarder_chain;
    u32 name;                  /* DLL 名 RVA */
    u32 first_thunk;           /* IAT RVA */
} image_import_descriptor;

typedef struct __attribute__((packed)) {
    u32 original_first_thunk;  /* 指向 IMAGE_IMPORT_BY_NAME */
} image_import_by_name_ref;

/* ===== 加载器 API ===== */

/* 解析并加载 PE 到内存。
 * - 分配 SizeOfImage 字节 RWX 内存
 * - 复制 headers + 各节区
 * - 应用基址重定位
 * - 解析 imports（填 IAT 为 shim 函数指针）
 * 返回 0 成功，info 填充；负数失败。 */
int pe_load_image(const void *pe_data, u64 size, pe_image_info *out);

/* 释放 pe_load_image 分配的内存 */
void pe_unload_image(pe_image_info *info);

/* 获取 PE 内部信息（供 shim/解释器使用） */
u64 pe_get_image_base(const pe_image_info *info);
u64 pe_get_image_size(const pe_image_info *info);

/* 获取 IAT 条目表（供 32位解释器 CALL 拦截）。
 * pe_iat_entry 定义在 x86emu32.h。返回表指针，*count 填充条目数。 */
struct pe_iat_entry;
struct pe_iat_entry *pe_get_iat_entries(int *count);

#endif /* PE_LOADER_H */
