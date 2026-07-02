#include <utsm/dsk.h>
#include <utsm/arena.h>
#include <utsm/dkm.h>
#include <utsm/log.h>
#include <utsm/types.h>
#include "../arch/x86_64/limine.h"

#define DSK_PATH "/system/deshab64/deshab.elf"
#define DSK_CMDLINE "dsk:main"

#define ELF_MAGIC0 0x7f
#define ELF_MAGIC1 'E'
#define ELF_MAGIC2 'L'
#define ELF_MAGIC3 'F'
#define ELFCLASS64 2
#define ELFDATA2LSB 1
#define ET_DYN 3
#define ET_EXEC 2
#define EM_X86_64 62
#define PT_LOAD 1

void *memset(void *dst, int value, usize len);
void *memcpy(void *dst, const void *src, usize len);

extern volatile struct limine_module_request g_module_request;
extern volatile struct limine_rsdp_request g_rsdp_request;
extern volatile struct limine_framebuffer_request g_fb_request;
extern volatile struct limine_hhdm_request g_hhdm_request;

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

typedef struct elf64_phdr {
    u32 type;
    u32 flags;
    u64 offset;
    u64 vaddr;
    u64 paddr;
    u64 filesz;
    u64 memsz;
    u64 align;
} elf64_phdr;

/* ---- minimal FAT32 parser ---- */
typedef struct __attribute__((packed)) {
    u8  jmp[3];
    char oem[8];
    u16 bytes_per_sector;
    u8  sectors_per_cluster;
    u16 reserved_sector_count;
    u8  fat_count;
    u16 root_entry_count;
    u16 total_sectors_16;
    u8  media;
    u16 sectors_per_fat_16;
    u16 sectors_per_track;
    u16 heads;
    u32 hidden_sectors;
    u32 total_sectors_32;
    u32 sectors_per_fat;
    u16 flags;
    u16 version;
    u32 root_cluster;
    u16 fsinfo_sector;
    u16 backup_boot;
    u8  reserved[12];
    u8  drive;
    u8  nt_flags;
    u8  signature;
    u32 serial;
    char label[11];
    char type[8];
    u8  code[420];
    u16 boot_sig;
} fat32_bpb;

typedef struct __attribute__((packed)) {
    char name[11];
    u8  attr;
    u8  nt_reserved;
    u8  creation_tenth;
    u16 creation_time;
    u16 creation_date;
    u16 access_date;
    u16 cluster_high;
    u16 write_time;
    u16 write_date;
    u16 cluster_low;
    u32 file_size;
} fat32_dir_entry;

static u32 fat32_read_u32(const u8 *p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static u16 fat32_read_u16(const u8 *p) {
    return (u16)p[0] | ((u16)p[1] << 8);
}

static int fat32_name11_eq(const char *n, const char *s) {
    for (int i = 0; i < 11; i++) {
        if (n[i] != s[i]) return 0;
    }
    return 1;
}

/* core helpers reused from earlier loader */
static int streq(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        if (*a != *b) return 0;
        a++;
        b++;
    }
    return *a == *b;
}

static int range_ok(u64 offset, u64 length, u64 size) {
    if (offset > size) return 0;
    if (length > size - offset) return 0;
    return 1;
}

static struct limine_file *dsk_find_module(void) {
    struct limine_module_response *rsp = g_module_request.response;
    if (!rsp) return 0;

    for (u64 i = 0; i < rsp->module_count; i++) {
        struct limine_file *file = rsp->modules[i];
        if (!file) continue;
        if (streq(file->path, DSK_PATH) || streq(file->cmdline, DSK_CMDLINE)) {
            return file;
        }
    }
    return 0;
}

/* ---- FAT32 block-provider file reader ---- */
static u8 g_dsk_fat32_disk[65536];  /* 128-sector BPB+FAT+root dir buffer */
static u8 g_dsk_fat32_cluster[4096]; /* 8-sector cluster buffer */
static u8 g_dsk_fat32_filedata[65536]; /* file data buffer */

static int dsk_load_from_block_fat32(const void **out_addr, u64 *out_size) {
    const dkm_kernel_api *api = dkm_get_kernel_api();
    if (!api || !api->block || !api->block->device_count) return -1;
    u32 count = api->block->device_count();
    if (count == 0 || api->block->sector_size(0) != 512) return -1;

    /* Read first 128 sectors: BPB + FAT + root dir */
    u8 *disk = g_dsk_fat32_disk;
    int st = api->block->read(0, 0, 128, disk);
    if (st != 0) return -1;

    const fat32_bpb *bpb = (const fat32_bpb *)disk;
    if (bpb->boot_sig != 0xAA55 || bpb->bytes_per_sector != 512) return -1;

    u32 fat_start = bpb->reserved_sector_count * 512;
    u32 data_start_sec = bpb->reserved_sector_count + (u32)bpb->fat_count * bpb->sectors_per_fat;

    u32 clus = bpb->root_cluster;
    u32 found_clus = 0;
    u32 found_size = 0;
    const char *target = "DESHAB  ELF";

    while (clus >= 2 && clus < 0x0FFFFFF8) {
        u32 clus_lba = data_start_sec + (clus - 2) * bpb->sectors_per_cluster;
        u32 cls_sec = bpb->sectors_per_cluster;
        u8 *cb = g_dsk_fat32_cluster;
        st = api->block->read(0, clus_lba, cls_sec, cb);
        if (st != 0) return -1;

        const fat32_dir_entry *dir = (const fat32_dir_entry *)cb;
        for (u32 e = 0; e * 32 < cls_sec * 512; e++) {
            if (dir[e].name[0] == 0) break;
            if ((u8)dir[e].name[0] == 0xE5) continue;
            if (dir[e].attr == 0x0F) continue;
            if (dir[e].attr & 0x08) continue;
            if (fat32_name11_eq(dir[e].name, target)) {
                found_size = dir[e].file_size;
                found_clus = fat32_read_u16((const u8 *)&dir[e].cluster_low);
                break;
            }
        }
        if (found_clus) break;
        u32 fat_ent = fat_start + clus * 4;
        if (fat_ent + 4 > 65536) break;
        clus = fat32_read_u32(disk + fat_ent) & 0x0FFFFFFF;
    }

    if (!found_clus || found_size == 0) return -1;

    if (found_size > sizeof(g_dsk_fat32_filedata)) return -1;
    u8 *dst = g_dsk_fat32_filedata;
    u32 remaining = found_size;
    u32 fc = found_clus;

    while (fc >= 2 && fc < 0x0FFFFFF8 && remaining > 0) {
        u32 fc_lba = data_start_sec + (fc - 2) * bpb->sectors_per_cluster;
        u32 fc_sec = bpb->sectors_per_cluster;
        u32 fc_bytes = fc_sec * 512;
        if (fc_bytes > remaining) fc_bytes = remaining;

        u8 *chunk = g_dsk_fat32_cluster;
        st = api->block->read(0, fc_lba, fc_sec, chunk);
        if (st != 0) return -1;
        for (u32 b = 0; b < fc_bytes; b++) dst[b] = chunk[b];
        dst += fc_bytes;
        remaining -= fc_bytes;

        u32 fat_ent = fat_start + fc * 4;
        if (fat_ent + 4 > 65536) { fc = 0x0FFFFFFF; break; }
        fc = fat32_read_u32(disk + fat_ent) & 0x0FFFFFFF;
    }

    /* Copy to arena for ELF loader */
    u8 *file_buf = (u8 *)kmem_alloc_aligned(found_size, 16);
    if (!file_buf) return -1;
    for (u32 i = 0; i < found_size; i++) file_buf[i] = g_dsk_fat32_filedata[i];

    *out_addr = file_buf;
    *out_size = found_size;
    log_hex64("[UTSM] FAT32 file size=", found_size);
    return 0;
}

/* ---- ELF checks ---- */
static int dsk_check_elf(const void *address, u64 size) {
    if (!address || size < sizeof(elf64_ehdr)) return -1;
    const elf64_ehdr *eh = (const elf64_ehdr *)address;
    if (eh->ident[0] != ELF_MAGIC0 || eh->ident[1] != ELF_MAGIC1 ||
        eh->ident[2] != ELF_MAGIC2 || eh->ident[3] != ELF_MAGIC3) return -2;
    if (eh->ident[4] != ELFCLASS64 || eh->ident[5] != ELFDATA2LSB) return -3;
    if (eh->machine != EM_X86_64) return -4;
    if (eh->type != ET_DYN && eh->type != ET_EXEC) return -5;
    if (eh->phoff == 0 || eh->phnum == 0 || eh->phentsize < sizeof(elf64_phdr)) return -6;
    if (!range_ok(eh->phoff, (u64)eh->phnum * eh->phentsize, size)) return -7;
    return 0;
}

static int dsk_load_elf_image(const void *address, u64 size, dsk_entry_fn *entry_out) {
    const elf64_ehdr *eh = (const elf64_ehdr *)address;
    const u8 *base = (const u8 *)address;

    u64 min_vaddr = ~0ULL;
    u64 max_vaddr = 0;
    u64 max_align = 0x1000;
    u32 load_count = 0;

    for (u16 i = 0; i < eh->phnum; i++) {
        const elf64_phdr *ph = (const elf64_phdr *)(base + eh->phoff + (u64)i * eh->phentsize);
        if (ph->type != PT_LOAD) continue;
        if (ph->filesz > ph->memsz) return -1;
        if (!range_ok(ph->offset, ph->filesz, size)) return -2;
        if (ph->vaddr < min_vaddr) min_vaddr = ph->vaddr;
        if (ph->vaddr + ph->memsz > max_vaddr) max_vaddr = ph->vaddr + ph->memsz;
        if (ph->align > max_align) max_align = ph->align;
        load_count++;
    }

    if (load_count == 0 || min_vaddr == ~0ULL || max_vaddr <= min_vaddr) return -3;
    if (eh->entry < min_vaddr || eh->entry >= max_vaddr) return -4;

    u64 image_size = utsm_align_up_u64(max_vaddr - min_vaddr, 0x1000);
    u8 *image = (u8 *)kmem_alloc_aligned(image_size, max_align);
    if (!image) return -5;
    memset(image, 0, image_size);

    for (u16 i = 0; i < eh->phnum; i++) {
        const elf64_phdr *ph = (const elf64_phdr *)(base + eh->phoff + (u64)i * eh->phentsize);
        if (ph->type != PT_LOAD) continue;
        u64 dst_off = ph->vaddr - min_vaddr;
        if (dst_off + ph->memsz > image_size) return -6;
        memcpy(image + dst_off, base + ph->offset, ph->filesz);
    }

    *entry_out = (dsk_entry_fn)(image + (eh->entry - min_vaddr));
    log_hex64("[UTSM] DSK image base=", (u64)image);
    log_hex64("[UTSM] DSK image size=", image_size);
    log_hex64("[UTSM] DSK entry=", (u64)*entry_out);
    return 0;
}

static void dsk_fill_boot_context(dsk_boot_context *ctx) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->magic = DSK_BOOT_MAGIC;
    ctx->abi_version = DSK_BOOT_ABI_VERSION;
    ctx->size = sizeof(*ctx);
    ctx->flags = DSK_BOOT_FLAG_FROM_UTSM | DSK_BOOT_FLAG_DKM_READY | DSK_BOOT_FLAG_FAT32_PATH;

    if (g_hhdm_request.response) {
        ctx->hhdm_offset = g_hhdm_request.response->offset;
    }
    if (g_rsdp_request.response) {
        ctx->rsdp_address = (u64)g_rsdp_request.response->address;
    }
    if (g_fb_request.response && g_fb_request.response->framebuffer_count > 0) {
        struct limine_framebuffer *fb = g_fb_request.response->framebuffers[0];
        ctx->framebuffer_address = (u64)fb->address;
        ctx->framebuffer_width = fb->width;
        ctx->framebuffer_height = fb->height;
        ctx->framebuffer_pitch = fb->pitch;
        ctx->framebuffer_bpp = fb->bpp;
    }
    ctx->boot_modules_response = (u64)g_module_request.response;
    ctx->dkm_kernel_api = (u64)dkm_get_kernel_api();

    u64 rsp;
    __asm__ volatile ("movq %%rsp, %0" : "=r"(rsp));
    ctx->kernel_stack_top = rsp;
}

int dsk_load_and_jump(void) {
    log_info("[UTSM] loading DSK");
    log_info(DSK_PATH);

    const void *data = 0;
    u64 data_size = 0;

    /* 1) try FAT32 block provider */
    int fat32_ok = dsk_load_from_block_fat32(&data, &data_size);
    if (fat32_ok == 0) {
        log_info("[UTSM] DSK loaded from FAT32 block provider");
    } else {
        /* 2) fallback: Limine boot module */
        log_info("[UTSM] FAT32 block path unavailable; trying Limine module");
        struct limine_file *file = dsk_find_module();
        if (!file) {
            log_error("[UTSM] DSK module not found (neither FAT32 nor Limine)");
            return -1;
        }
        data = file->address;
        data_size = file->size;
        log_hex64("[UTSM] DSK module size=", data_size);
    }

    int status = dsk_check_elf(data, data_size);
    if (status != 0) {
        log_error("[UTSM] DSK ELF rejected");
        log_hex64("[UTSM] DSK ELF status=", (u64)(i64)status);
        return status;
    }

    dsk_entry_fn entry = 0;
    status = dsk_load_elf_image(data, data_size, &entry);
    if (status != 0) {
        log_error("[UTSM] DSK load failed");
        log_hex64("[UTSM] DSK load status=", (u64)(i64)status);
        return status;
    }

    dsk_boot_context *ctx = (dsk_boot_context *)kmem_alloc_aligned(sizeof(dsk_boot_context), 16);
    if (!ctx) {
        log_error("[UTSM] DSK context allocation failed");
        return -1;
    }
    dsk_fill_boot_context(ctx);

    log_info("[UTSM] jumping to DSK");
    entry(ctx);

    log_error("[UTSM] DSK returned unexpectedly");
    return -1;
}
