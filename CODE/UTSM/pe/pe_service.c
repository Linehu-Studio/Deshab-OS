/* pe_service.c — PE 兼容层服务装配。
 *
 * 将 pe_loader + pe_shim + x86emu32 整合为统一的 pe_service，
 * 通过 pe_get_service() 暴露给 DSK/cmd.elf（经 dsk_boot_context.reserved[4]）。
 *
 * 执行策略：
 *   PE32+ (64位)：pe_load_image 加载（actual_base 重定位），
 *                 IAT 填 ms_abi shim 函数地址，Ring0 原生执行入口。
 *                 ExitProcess 经 __builtin_longjmp 跳回 pe_service_run。
 *   PE32  (32位)：直接以 preferred_base 加载到一块含栈/堆的大缓冲区，
 *                 无需重定位。IAT 填合成地址（0x00010000 | shim_idx），
 *                 x86emu32 解释执行，CALL 合成地址时拦截分发到 shim32。
 */

#include "pe_loader.h"
#include "pe_shim.h"
#include "x86emu32.h"
#include "../include/utsm/pe.h"
#include <utsm/arena.h>
#include <utsm/log.h>
#include <utsm/types.h>

/* 32位 PE 用的栈与堆 */
#define PE32_STACK_SIZE  (64 * 1024)   /* 64KB 栈 */
#define PE32_HEAP_SIZE   (256 * 1024)  /* 256KB 堆 */
#define PE32_MAX_IAT     256

/* 合成 shim 地址范围（与 pe_loader.c 一致） */
#define PE32_SHIM_ADDR_BASE 0x00010000u
#define PE32_SHIM_UNIMPL    0x0001FFFFu

/* 前向声明（服务表引用） */
int  pe_service_load(const void *pe_data, u64 size, pe_image_info *out);
int  pe_service_run(const void *pe_data, u64 size, const char *cmdline, u64 *exit_code);
void pe_service_unload(pe_image_info *info);

/* 全局服务表实例 */
static const pe_service g_pe_service = {
    PE_SERVICE_MAGIC,
    pe_service_load,
    pe_service_run,
    pe_service_unload,
};

const pe_service *pe_get_service(void) {
    return &g_pe_service;
}

/* ===== 加载（不执行）===== */
int pe_service_load(const void *pe_data, u64 size, pe_image_info *out) {
    if (!pe_data || !out) return -1;
    return pe_load_image(pe_data, size, out);
}

void pe_service_unload(pe_image_info *info) {
    pe_unload_image(info);
}

/* ===== PE32 专用加载器（加载到指定缓冲区，base=preferred_base，无重定位）===== */
typedef struct {
    u8 *mem;                /* 大缓冲区起始 */
    u32 mem_size;           /* 缓冲区总大小 */
    u32 image_base;         /* = preferred_base */
    u32 image_size;
    u32 entry_point;        /* 绝对入口 = image_base + entry_rva */
    pe_iat_entry iat[PE32_MAX_IAT];
    int iat_count;
} pe32_interp_image;

static int pe32_load_into(const void *pe_data, u64 size, pe32_interp_image *img) {
    const u8 *base = (const u8 *)pe_data;
    if (size < sizeof(image_dos_header)) return -1;
    const image_dos_header *dos = (const image_dos_header *)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return -2;
    if ((u64)dos->e_lfanew + 4 + sizeof(image_file_header) > size) return -3;

    const u8 *nt = base + dos->e_lfanew;
    if (*(const u32 *)nt != IMAGE_NT_SIGNATURE) return -4;
    const image_file_header *fh = (const image_file_header *)(nt + 4);
    const void *opt = (const void *)(nt + 4 + sizeof(image_file_header));
    u16 magic = *(const u16 *)opt;
    if (magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) return -5;  /* 仅 PE32 */

    const image_optional_header32 *oh = (const image_optional_header32 *)opt;
    u32 preferred_base = oh->image_base;
    u32 image_size = oh->size_of_image;
    u32 entry_rva = oh->address_of_entry_point;
    if (image_size == 0 || image_size > 64 * 1024 * 1024) return -6;
    if (preferred_base > 0x10000000u) return -7;  /* 解释器地址空间限制 */

    int nsec = fh->number_of_sections;
    if (nsec <= 0 || nsec > 96) return -8;
    const image_section_header *sections = (const image_section_header *)
        ((const u8 *)opt + fh->size_of_optional_header);

    /* 分配大缓冲区：[0 .. preferred_base + image_size + stack + heap] */
    u32 total = preferred_base + image_size + PE32_STACK_SIZE + PE32_HEAP_SIZE;
    u8 *mem = (u8 *)kmem_alloc_aligned((u64)total, 0x1000);
    if (!mem) return -9;
    for (u32 i = 0; i < total; i++) mem[i] = 0;

    /* 复制 headers */
    u32 hdr_copy = oh->size_of_headers;
    if (hdr_copy > size) hdr_copy = (u32)size;
    if (hdr_copy > image_size) hdr_copy = image_size;
    u8 *img_start = mem + preferred_base;
    for (u32 i = 0; i < hdr_copy; i++) img_start[i] = base[i];

    /* 复制各节区 */
    for (int i = 0; i < nsec; i++) {
        u32 va = sections[i].virtual_address;
        u32 raw_size = sections[i].size_of_raw_data;
        u32 raw_off = sections[i].pointer_to_raw_data;
        if (va >= image_size) continue;
        u32 copy_size = raw_size;
        if (va + copy_size > image_size) copy_size = image_size - va;
        if ((u64)raw_off + copy_size > size) copy_size = (u32)(size - raw_off);
        for (u32 j = 0; j < copy_size; j++) img_start[va + j] = base[raw_off + j];
    }

    /* 解析 imports，填充 IAT（合成地址） */
    img->iat_count = 0;
    const image_data_directory *import_dir = &oh->data_directory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (import_dir->virtual_address != 0 && import_dir->size != 0) {
        /* RVA→offset 辅助 */
        for (int dll_idx = 0; ; dll_idx++) {
            u32 imp_rva = import_dir->virtual_address + (u32)dll_idx * sizeof(image_import_descriptor);
            /* imp_rva 在 image 内 */
            if (imp_rva + sizeof(image_import_descriptor) > image_size) break;
            const image_import_descriptor *desc = (const image_import_descriptor *)(img_start + imp_rva);
            if (desc->name == 0 && desc->first_thunk == 0) break;

            /* DLL 名 */
            u32 name_rva = desc->name;
            const char *dll_name = 0;
            if (name_rva < image_size) dll_name = (const char *)(img_start + name_rva);

            u32 iat_rva = desc->first_thunk;
            u32 int_rva = desc->original_first_thunk;
            if (int_rva == 0) int_rva = iat_rva;

            for (int fn_idx = 0; ; fn_idx++) {
                u32 int_entry_rva = int_rva + (u32)fn_idx * 4;
                if (int_entry_rva + 4 > image_size) break;
                u32 thunk = *(const u32 *)(img_start + int_entry_rva);
                if (thunk == 0) break;

                const char *func_name = 0;
                if (thunk & 0x80000000u) {
                    func_name = "#ordinal";
                } else {
                    u32 hint_rva = thunk;
                    if (hint_rva + 2 < image_size) {
                        func_name = (const char *)(img_start + hint_rva + 2);
                    }
                }

                int shim_idx = -1;
                if (func_name && func_name[0] != '#') {
                    shim_idx = pe_shim_lookup(dll_name, func_name);
                }

                u32 synth_addr;
                if (shim_idx >= 0) {
                    synth_addr = PE32_SHIM_ADDR_BASE | (u32)(shim_idx & 0xFFFF);
                } else {
                    synth_addr = PE32_SHIM_UNIMPL;
                }

                u32 iat_entry_rva = iat_rva + (u32)fn_idx * 4;
                if (iat_entry_rva + 4 <= image_size) {
                    *(u32 *)(img_start + iat_entry_rva) = synth_addr;
                }

                if (img->iat_count < PE32_MAX_IAT) {
                    img->iat[img->iat_count].iat_rva = iat_entry_rva;
                    img->iat[img->iat_count].shim_index = shim_idx;
                    img->iat[img->iat_count].target_addr = synth_addr;
                    img->iat_count++;
                }

                if (shim_idx < 0 && func_name) {
                    log_info("[PE32] unimplemented import");
                    if (dll_name) serial_write(dll_name);
                    serial_write("!");
                    serial_write(func_name);
                    serial_write("\n");
                }
            }
        }
    }

    img->mem = mem;
    img->mem_size = total;
    img->image_base = preferred_base;
    img->image_size = image_size;
    img->entry_point = preferred_base + entry_rva;
    return 0;
}

/* ===== 运行 =====
 * 加载 + 执行 + 卸载。返回 0 成功，*exit_code 为进程退出码。 */
int pe_service_run(const void *pe_data, u64 size, const char *cmdline, u64 *exit_code) {
    if (!pe_data || size == 0) return -1;

    /* 初始化 shim 状态（cmdline for GetCommandLineA） */
    pe_shim_init(cmdline ? cmdline : "");

    /* 先解析 PE 判断 32/64 位 */
    if (size < sizeof(image_dos_header)) return -2;
    const image_dos_header *dos = (const image_dos_header *)pe_data;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return -3;
    const u8 *nt_base = (const u8 *)pe_data + dos->e_lfanew;
    if (*(const u32 *)nt_base != IMAGE_NT_SIGNATURE) return -4;
    const void *opt = (const void *)(nt_base + 4 + sizeof(image_file_header));
    u16 opt_magic = *(const u16 *)opt;

    if (opt_magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        /* ===== PE32+ : Ring0 原生执行 ===== */
        pe_image_info info;
        int rc = pe_load_image(pe_data, size, &info);
        if (rc != 0) {
            log_error("[PE] PE32+ load failed");
            return -5;
        }
        log_info("[PE] running PE32+ natively");

        u64 code = 0;
        int jumped = pe_shim_setup_exit();
        if (jumped == 0) {
            typedef void (*pe_entry_fn)(void);
            pe_entry_fn entry = (pe_entry_fn)info.entry_point;
            log_hex64("[PE] entry=", (u64)entry);
            entry();
            code = 0;
        } else {
            code = pe_shim_get_exit_code();
            log_hex64("[PE] ExitProcess code=", code);
        }

        pe_unload_image(&info);
        if (exit_code) *exit_code = code;
        return 0;

    } else if (opt_magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
        /* ===== PE32 : x86-32 解释器执行 ===== */
        pe32_interp_image img;
        int rc = pe32_load_into(pe_data, size, &img);
        if (rc != 0) {
            log_error("[PE] PE32 interp load failed");
            log_hex64("[PE] rc=", (u64)(i64)rc);
            return -6;
        }
        log_info("[PE] running PE32 via x86emu32");

        u32 stack_top = img.image_base + img.image_size + PE32_STACK_SIZE - 16;

        x86emu_state emu;
        int irc = x86emu32_init(&emu, img.mem, img.mem_size,
                                img.image_base, img.image_size,
                                img.iat, img.iat_count, stack_top);
        if (irc != 0) {
            log_error("[PE32] emu init failed");
            return -7;
        }
        emu.eip = img.entry_point;

        u64 code = 0;
        int jumped = pe_shim_setup_exit();
        if (jumped == 0) {
            int rrc = x86emu32_run(&emu, &code);
            if (rrc != 0) {
                log_error("[PE32] emulator error");
                code = 0xFFFFFFFFULL;
            }
        } else {
            code = pe_shim_get_exit_code();
            log_hex64("[PE32] ExitProcess code=", code);
        }

        if (emu.error && !emu.exited) {
            log_warn("[PE32] emulator stopped with error");
        }

        if (exit_code) *exit_code = code;
        return 0;
    }

    return -8;  /* 未知 PE magic */
}
