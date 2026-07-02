#include <utsm/dkm.h>
#include <utsm/log.h>
#include <utsm/types.h>
#include <utsm/arena.h>
#include "../arch/x86_64/limine.h"

#define ELF_MAGIC0 0x7f
#define ELF_MAGIC1 'E'
#define ELF_MAGIC2 'L'
#define ELF_MAGIC3 'F'
#define ELFCLASS64 2
#define ELFDATA2LSB 1
#define ET_REL 1
#define ET_DYN 3
#define ET_EXEC 2
#define EM_X86_64 62
#define SHT_SYMTAB 2
#define SHT_STRTAB 3
#define SHT_RELA 4
#define SHT_PROGBITS 1
#define SHT_NOBITS 8

#define SHF_ALLOC 0x2
#define SHF_WRITE 0x1
#define SHF_EXECINSTR 0x4

#define R_X86_64_64 1
#define R_X86_64_32 10
#define R_X86_64_32S 11
#define R_X86_64_PC32 2

void *memset(void *dst, int value, usize len);
void *memcpy(void *dst, const void *src, usize len);

__attribute__((used, section(".limine_requests")))
volatile struct limine_module_request g_module_request = {
    .id = LIMINE_MODULE_REQUEST_ID,
    .revision = 0,
    .response = 0
};

__attribute__((used, section(".limine_requests_start")))
static volatile u64 g_limine_requests_start[4] = { LIMINE_COMMON_MAGIC, 0xf6b8f4b39de7d1aeULL, 0xfab91a6940fcb9cfULL };

__attribute__((used, section(".limine_requests_end")))
static volatile u64 g_limine_requests_end[2] = { 0, 0 };

__attribute__((used, section(".limine_requests")))
volatile struct limine_rsdp_request g_rsdp_request = {
    .id = LIMINE_RSDP_REQUEST_ID,
    .revision = 0,
    .response = 0
};

__attribute__((used, section(".limine_requests")))
volatile struct limine_framebuffer_request g_fb_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST_ID,
    .revision = 0,
    .response = 0
};

__attribute__((used, section(".limine_requests")))
volatile struct limine_hhdm_request g_hhdm_request = {
    .id = { LIMINE_COMMON_MAGIC, 0x48dcf1cb8ad2b852ULL, 0x63984e959a98244bULL },
    .revision = 0,
    .response = 0
};

__attribute__((used, section(".limine_requests")))
volatile struct limine_kernel_address_request g_kernel_address_request = {
    .id = LIMINE_KERNEL_ADDRESS_REQUEST_ID,
    .revision = 0,
    .response = 0
};

__attribute__((used, section(".limine_requests")))
volatile struct limine_memmap_request g_memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST_ID,
    .revision = 0,
    .response = 0
};

typedef struct elf64_ehdr {
    u8 ident[16];
    u16 type;
    u16 machine;
    u32 version;
    u64 entry;
    u64 phoff;
    u64 shoff;
    u32 flags;
    u16 ehsize;
    u16 phentsize;
    u16 phnum;
    u16 shentsize;
    u16 shnum;
    u16 shstrndx;
} elf64_ehdr;

typedef struct elf64_shdr {
    u32 name;
    u32 type;
    u64 flags;
    u64 addr;
    u64 offset;
    u64 size;
    u32 link;
    u32 info;
    u64 addralign;
    u64 entsize;
} elf64_shdr;

typedef struct elf64_sym {
    u32 name;
    u8 info;
    u8 other;
    u16 shndx;
    u64 value;
    u64 size;
} elf64_sym;

typedef struct elf64_rela {
    u64 offset;
    u64 info;
    i64 addend;
} elf64_rela;

static int streq(const char *a, const char *b) {
    usize i = 0;
    while (a[i] && b[i]) {
        if (a[i] != b[i]) {
            return 0;
        }
        i++;
    }
    return a[i] == b[i];
}

int dkm_check_elf64(const void *address, u64 size) {
    if (!address || size < sizeof(elf64_ehdr)) {
        return -1;
    }

    const elf64_ehdr *ehdr = (const elf64_ehdr *)address;
    if (ehdr->ident[0] != ELF_MAGIC0 || ehdr->ident[1] != ELF_MAGIC1 || ehdr->ident[2] != ELF_MAGIC2 || ehdr->ident[3] != ELF_MAGIC3) {
        return -2;
    }
    if (ehdr->ident[4] != ELFCLASS64 || ehdr->ident[5] != ELFDATA2LSB) {
        return -3;
    }
    if (ehdr->machine != EM_X86_64) {
        return -4;
    }
    if (ehdr->type != ET_REL && ehdr->type != ET_DYN && ehdr->type != ET_EXEC) {
        return -5;
    }
    if (ehdr->ehsize < sizeof(elf64_ehdr)) {
        return -6;
    }
    return 0;
}

static int range_ok(u64 offset, u64 length, u64 size) {
    if (offset > size) {
        return 0;
    }
    if (length > size - offset) {
        return 0;
    }
    return 1;
}

int dkm_scan_symbols(const void *address, u64 size, struct dkm_symbol_scan *out) {
    const elf64_ehdr *ehdr = (const elf64_ehdr *)address;
    out->driver_desc = 0;
    out->driver_init = 0;
    out->driver_exit = 0;

    if (ehdr->shoff == 0 || ehdr->shnum == 0 || ehdr->shentsize < sizeof(elf64_shdr)) {
        log_error("[DKM] ELF has no section table");
        return -1;
    }
    if (!range_ok(ehdr->shoff, (u64)ehdr->shnum * ehdr->shentsize, size)) {
        log_error("[DKM] ELF section table out of range");
        return -1;
    }

    const elf64_shdr *sections = (const elf64_shdr *)((const u8 *)address + ehdr->shoff);
    const elf64_shdr *symtab = 0;
    const elf64_shdr *strtab = 0;

    for (u16 i = 0; i < ehdr->shnum; i++) {
        const elf64_shdr *section = (const elf64_shdr *)((const u8 *)sections + (u64)i * ehdr->shentsize);
        if (section->type == SHT_SYMTAB) {
            symtab = section;
            if (section->link < ehdr->shnum) {
                strtab = (const elf64_shdr *)((const u8 *)sections + (u64)section->link * ehdr->shentsize);
            }
            break;
        }
    }

    if (!symtab || !strtab || strtab->type != SHT_STRTAB) {
        log_error("[DKM] missing symtab/strtab");
        return -1;
    }
    if (symtab->entsize < sizeof(elf64_sym) || !range_ok(symtab->offset, symtab->size, size) || !range_ok(strtab->offset, strtab->size, size)) {
        log_error("[DKM] invalid symtab/strtab range");
        return -1;
    }

    log_info("[DKM] symtab found");
    log_hex64("[DKM] symbol count=", symtab->size / symtab->entsize);

    for (u64 off = 0; off + sizeof(elf64_sym) <= symtab->size; off += symtab->entsize) {
        const elf64_sym *sym = (const elf64_sym *)((const u8 *)address + symtab->offset + off);
        if (sym->name >= strtab->size) {
            continue;
        }
        const char *name = (const char *)address + strtab->offset + sym->name;
        if (streq(name, "driver_desc")) {
            out->driver_desc = sym;
            log_info("[DKM] found driver_desc");
            log_hex64("[DKM] driver_desc section=", sym->shndx);
            log_hex64("[DKM] driver_desc value=", sym->value);
        } else if (streq(name, "driver_init")) {
            out->driver_init = sym;
            log_info("[DKM] found driver_init");
            log_hex64("[DKM] driver_init section=", sym->shndx);
            log_hex64("[DKM] driver_init value=", sym->value);
        } else if (streq(name, "driver_exit")) {
            out->driver_exit = sym;
            log_info("[DKM] found driver_exit");
            log_hex64("[DKM] driver_exit section=", sym->shndx);
            log_hex64("[DKM] driver_exit value=", sym->value);
        }
    }

    if (out->driver_desc && out->driver_init && out->driver_exit) {
        log_info("[DKM] required driver symbols ok");
        return 0;
    }
    log_error("[DKM] required driver symbols missing");
    return -1;
}

int dkm_load_elf_rel(const void *address, u64 size, const struct dkm_symbol_scan *symbols) {
    const elf64_ehdr *ehdr = (const elf64_ehdr *)address;
    const elf64_shdr *sections = (const elf64_shdr *)((const u8 *)address + ehdr->shoff);
    u64 image_offset[64];
    u64 total_size = 0;
    u64 max_align = 16;
    u16 alloc_count = 0;

    for (u16 i = 1; i < ehdr->shnum && alloc_count < 64; i++) {
        const elf64_shdr *sh = (const elf64_shdr *)((const u8 *)sections + (u64)i * ehdr->shentsize);
        if (!(sh->flags & SHF_ALLOC)) {
            continue;
        }
        if (sh->addralign > max_align) {
            max_align = sh->addralign;
        }
        total_size = utsm_align_up_u64(total_size, sh->addralign);
        image_offset[i] = total_size;
        total_size += sh->size;
        alloc_count++;
    }

    if (total_size == 0) {
        log_error("[DKM] no allocatable sections");
        return -1;
    }

    u8 *image = (u8 *)kmem_alloc_aligned(total_size, max_align);
    if (!image) {
        log_error("[DKM] failed to allocate module memory");
        return -1;
    }
    memset(image, 0, total_size);

    for (u16 i = 1; i < ehdr->shnum; i++) {
        const elf64_shdr *sh = (const elf64_shdr *)((const u8 *)sections + (u64)i * ehdr->shentsize);
        if (!(sh->flags & SHF_ALLOC)) {
            continue;
        }
        if (sh->type == SHT_PROGBITS && sh->size > 0 && range_ok(sh->offset, sh->size, size)) {
            memcpy(image + image_offset[i], (const u8 *)address + sh->offset, sh->size);
        }
    }

    for (u16 i = 1; i < ehdr->shnum; i++) {
        const elf64_shdr *sh = (const elf64_shdr *)((const u8 *)sections + (u64)i * ehdr->shentsize);
        if (sh->type != SHT_RELA) {
            continue;
        }
        if (sh->entsize < sizeof(elf64_rela) || !range_ok(sh->offset, sh->size, size)) {
            continue;
        }
        if (sh->info >= ehdr->shnum) {
            continue;
        }
        const elf64_shdr *target = (const elf64_shdr *)((const u8 *)sections + (u64)sh->info * ehdr->shentsize);
        if (!(target->flags & SHF_ALLOC)) {
            continue;
        }
        u8 *target_base = image + image_offset[sh->info];

        const elf64_shdr *rela_symtab = 0;
        if (sh->link < ehdr->shnum) {
            rela_symtab = (const elf64_shdr *)((const u8 *)sections + (u64)sh->link * ehdr->shentsize);
        }
        const elf64_shdr *rela_strtab = 0;
        if (rela_symtab && rela_symtab->link < ehdr->shnum) {
            rela_strtab = (const elf64_shdr *)((const u8 *)sections + (u64)rela_symtab->link * ehdr->shentsize);
        }
        (void)rela_strtab;

        for (u64 off = 0; off + sizeof(elf64_rela) <= sh->size; off += sh->entsize) {
            const elf64_rela *rela = (const elf64_rela *)((const u8 *)address + sh->offset + off);
            u32 sym_idx = (u32)(rela->info >> 32);
            u32 reloc_type = (u32)(rela->info & 0xffffffff);

            u64 sym_value = 0;
            u16 sym_shndx = 0;
            if (rela_symtab && rela_strtab &&
                rela_symtab->entsize >= sizeof(elf64_sym) &&
                range_ok(rela_symtab->offset, rela_symtab->size, size)) {
                u64 sym_entry_off = (u64)sym_idx * rela_symtab->entsize;
                if (sym_entry_off + sizeof(elf64_sym) <= rela_symtab->size) {
                    const elf64_sym *sym = (const elf64_sym *)((const u8 *)address + rela_symtab->offset + sym_entry_off);
                    sym_value = sym->value;
                    sym_shndx = sym->shndx;
                }
            }

            u64 S = sym_value;
            if (sym_shndx > 0 && sym_shndx < 64) {
                S = (u64)image + image_offset[sym_shndx] + sym_value;
            }
            u64 P = (u64)image + image_offset[sh->info] + rela->offset;
            u64 A = rela->addend;

            u64 patch = 0;
            if (reloc_type == R_X86_64_64) {
                patch = S + A;
            } else if (reloc_type == R_X86_64_32 || reloc_type == R_X86_64_32S) {
                patch = (u64)((i64)(i32)(S + A));
            } else if (reloc_type == R_X86_64_PC32) {
                patch = (u64)((i64)(i32)(S + A - P));
            } else {
                log_error("[DKM] unsupported relocation type");
                log_hex64("[DKM] reloc type=", (u64)reloc_type);
                return -1;
            }

            if (rela->offset + 8 <= target->size) {
                if (reloc_type == R_X86_64_32 || reloc_type == R_X86_64_32S || reloc_type == R_X86_64_PC32) {
                    *(u32 *)(target_base + rela->offset) = (u32)patch;
                } else {
                    *(u64 *)(target_base + rela->offset) = patch;
                }
            }
        }
    }

    const elf64_sym *desc_sym = (const elf64_sym *)symbols->driver_desc;
    const elf64_sym *init_sym = (const elf64_sym *)symbols->driver_init;
    const dkm_driver_desc *desc = (const dkm_driver_desc *)(image + image_offset[desc_sym->shndx] + desc_sym->value);
    if (desc->magic != DKM_DRIVER_MAGIC) {
        log_error("[DKM] external driver_desc magic mismatch");
        return -1;
    }

    log_info("[DKM] external driver loaded");
    log_hex64("[DKM] desc ptr=", (u64)desc);
    log_hex64("[DKM] name ptr=", (u64)desc->name);
    log_info(desc->name);

    dkm_driver_init_fn init = (dkm_driver_init_fn)(image + image_offset[init_sym->shndx] + init_sym->value);

    dkm_driver_handle handle;
    handle.desc = desc;
    handle.state = DKM_STATE_INITING;
    handle.init_status = 0;
    handle.load_stage = desc->stage;
    handle.flags = desc->flags;

    int result = init(dkm_get_kernel_api(), &handle);
    handle.init_status = result;
    if (result != 0) {
        handle.state = DKM_STATE_FAILED;
        log_error("[DKM] external driver init failed");
        return result;
    }
    handle.state = DKM_STATE_ACTIVE;
    log_info("[DKM] external driver active");
    return 0;
}

void dkm_scan_boot_modules(void) {
    struct limine_module_response *response = g_module_request.response;
    if (!response) {
        log_warn("[DKM] no Limine module response");
        return;
    }

    log_info("[DKM] boot modules scan begin");
    log_hex64("[DKM] boot module count=", response->module_count);

    for (u64 i = 0; i < response->module_count; i++) {
        struct limine_file *file = response->modules[i];
        if (!file) {
            log_warn("[DKM] null module entry");
            continue;
        }

        log_info("[DKM] module path");
        if (file->path) {
            log_info(file->path);
        }
        if (file->cmdline) {
            log_info(file->cmdline);
        }
        log_hex64("[DKM] module size=", file->size);

        int status = dkm_check_elf64(file->address, file->size);
        if (status == 0) {
            log_info("[DKM] module ELF64 x86_64 ok");
            struct dkm_symbol_scan symbols;
            if (dkm_scan_symbols(file->address, file->size, &symbols) == 0) {
                log_info("[DKM] loading external ELF driver");
                int load_result = dkm_load_elf_rel(file->address, file->size, &symbols);
                if (load_result != 0) {
                    log_error("[DKM] external driver load failed");
                }
            }
        } else {
            log_error("[DKM] module ELF64 rejected");
            log_hex64("[DKM] elf status=", (u64)(i64)status);
        }
    }

    log_info("[DKM] boot modules scan end");
}
