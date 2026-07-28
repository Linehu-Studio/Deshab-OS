/* linux_loader.c — Linux 6.6 bzImage loader for UTSM VMX non-root guest.
 *
 * Responsibilities:
 *   1. Find the bzImage among Limine boot modules
 *   2. Parse the bzImage setup_header (Linux boot protocol)
 *   3. Allocate host physical memory for guest regions
 *   4. Load the protected-mode kernel into guest memory
 *   5. Set up boot_params (e820, cmdline, initrd pointers)
 *   6. Build guest page tables (identity-mapped 4GB, 2MB pages)
 *   7. Build guest GDT (64-bit code/data/TSS)
 *   8. EPT-map all guest regions (GPA → HPA)
 *
 * The actual VMCS configuration and vmlaunch happen in linux_boot.c.
 */

#include <utsm/linux_loader.h>
#include <utsm/vmx.h>
#include <utsm/ept.h>
#include <utsm/log.h>
#include <utsm/types.h>
#include <utsm/dma.h>
#include "../arch/x86_64/limine.h"

extern volatile struct limine_module_request g_module_request;
extern volatile struct limine_hhdm_request g_hhdm_request;
extern volatile struct limine_memmap_request g_memmap_request;

/* ===== Runtime helpers ===== */

static void mem_copy(void *dst, const void *src, u64 n) {
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    for (u64 i = 0; i < n; i++) d[i] = s[i];
}

static void mem_zero(void *dst, u64 n) {
    u8 *d = (u8 *)dst;
    for (u64 i = 0; i < n; i++) d[i] = 0;
}

static u64 str_len(const char *s) {
    u64 n = 0;
    while (s[n]) n++;
    return n;
}

/* ===== Guest state ===== */

static struct linux_guest_info g_guest;
static const char *g_default_cmdline = LINUX_DEFAULT_CMDLINE;

/* ===== Guest page table helpers =====
 *
 * Build identity-mapped 4-level page tables for the guest.
 * Maps the first 4GB of GPA space using 2MB large pages.
 *
 * Layout (all at known GPAs for guest CR3):
 *   PML4  at g_guest.pgt_gpa + 0x0000 (1 page)
 *   PDPT  at g_guest.pgt_gpa + 0x1000 (1 page)
 *   PD[0] at g_guest.pgt_gpa + 0x2000 (1 page, covers 0-1GB)
 *   PD[1] at g_guest.pgt_gpa + 0x3000 (1 page, covers 1-2GB)
 *   PD[2] at g_guest.pgt_gpa + 0x4000 (1 page, covers 2-3GB)
 *   PD[3] at g_guest.pgt_gpa + 0x5000 (1 page, covers 3-4GB)
 */

#define PGT_PML4_OFF    0x0000
#define PGT_PDPT_OFF    0x1000
#define PGT_PD_BASE     0x2000
#define PGT_PD_COUNT    4          /* 4 PD pages = 4GB */
#define PGT_TOTAL_PAGES (2 + PGT_PD_COUNT)
#define PGT_TOTAL_SIZE  (PGT_TOTAL_PAGES * 4096)

/* x86-64 page table entry bits */
#define PTE_P           (1ULL << 0)   /* Present */
#define PTE_RW          (1ULL << 1)   /* Read/Write */
#define PTE_US          (1ULL << 2)   /* User/Supervisor */
#define PTE_PS          (1ULL << 7)   /* Page Size (large page) */
#define PTE_G           (1ULL << 8)   /* Global */
#define PTE_NX          (1ULL << 63)  /* No Execute */

static int build_guest_page_tables(u64 pgt_hpa) {
    u64 *pml4 = (u64 *)(pgt_hpa + (g_hhdm_request.response ? g_hhdm_request.response->offset : 0));
    u64 *pdpt = (u64 *)((u8 *)pml4 + 4096);

    /* PML4[0] → PDPT */
    u64 pdpt_gpa = g_guest.pgt_gpa + PGT_PDPT_OFF;
    pml4[0] = pdpt_gpa | PTE_P | PTE_RW;

    /* PDPT[0..3] → PD pages (each covers 1GB) */
    for (int i = 0; i < PGT_PD_COUNT; i++) {
        u64 pd_gpa = g_guest.pgt_gpa + PGT_PD_BASE + (u64)i * 4096;
        pdpt[i] = pd_gpa | PTE_P | PTE_RW;
    }

    /* Each PD has 512 entries, each mapping a 2MB page (PS=1).
     * PD[i] entry j maps GPA = i*1GB + j*2MB. */
    for (int pd_idx = 0; pd_idx < PGT_PD_COUNT; pd_idx++) {
        u64 *pd = (u64 *)((u8 *)pml4 + PGT_PD_BASE + (u64)pd_idx * 4096);
        for (int j = 0; j < 512; j++) {
            u64 page_gpa = (u64)pd_idx * (1ULL << 30) + (u64)j * (2ULL << 20);
            pd[j] = page_gpa | PTE_P | PTE_RW | PTE_PS;
        }
    }

    return 0;
}

/* ===== Guest GDT builder =====
 *
 * GDT layout (at g_guest.gdt_gpa):
 *   [0x00] null
 *   [0x08] 64-bit code: base=0, limit=0xFFFFF, L=1, G=1, present, DPL0, type=0x9B
 *   [0x10] 64-bit data: base=0, limit=0xFFFFF, G=1, present, DPL0, type=0x93
 *   [0x18] 64-bit TSS descriptor (16 bytes)
 *
 * TSS itself is placed in the same 4KB page after the GDT.
 */

typedef struct __attribute__((packed)) {
    u16 limit_low;
    u16 base_low;
    u8  base_mid;
    u8  access;
    u8  flags_limit_high;
    u8  base_high;
} gdt_entry64;

typedef struct __attribute__((packed)) {
    u32 limit_low_base_low;
    u8  base_mid;
    u8  access;
    u8  flags_limit_high;
    u8  base_high;
    u32 base_upper;
    u32 reserved;
} tss_desc64;

static int build_guest_gdt(u64 gdt_hpa) {
    u8 *gdt_page = (u8 *)(gdt_hpa + (g_hhdm_request.response ? g_hhdm_request.response->offset : 0));
    mem_zero(gdt_page, 4096);

    gdt_entry64 *gdt = (gdt_entry64 *)gdt_page;

    /* [0x08] 64-bit code segment */
    gdt[1].limit_low = 0xFFFF;
    gdt[1].base_low = 0;
    gdt[1].base_mid = 0;
    gdt[1].access = 0x9B;           /* present, DPL0, code, exec/read */
    gdt[1].flags_limit_high = 0xAF; /* G=1, L=1 (64-bit), limit[19:16]=0xF */
    gdt[1].base_high = 0;

    /* [0x10] 64-bit data segment */
    gdt[2].limit_low = 0xFFFF;
    gdt[2].base_low = 0;
    gdt[2].base_mid = 0;
    gdt[2].access = 0x93;           /* present, DPL0, data, read/write */
    gdt[2].flags_limit_high = 0xCF; /* G=1, DB=1, limit[19:16]=0xF */
    gdt[2].base_high = 0;

    /* [0x18] 64-bit TSS descriptor (16 bytes).
     * TSS structure lives at offset 0x100 in the same page (GPA = gdt_gpa + 0x100). */
    tss_desc64 *tss_d = (tss_desc64 *)&gdt_page[0x18];
    u64 tss_base = g_guest.gdt_gpa + 0x100;
    u32 tss_limit = 0x67;           /* 104-byte minimum 64-bit TSS */
    tss_d->limit_low_base_low = (tss_limit & 0xFFFF) | ((u32)(tss_base & 0xFFFFFF) << 16);
    tss_d->base_mid = (u8)((tss_base >> 24) & 0xFF);
    tss_d->access = 0x89;           /* present, DPL0, 64-bit TSS (available) */
    tss_d->flags_limit_high = (u8)((tss_limit >> 16) & 0x0F);
    tss_d->base_high = 0;
    tss_d->base_upper = 0;
    tss_d->reserved = 0;

    return 0;
}

/* ===== e820 setup ===== */

struct __attribute__((packed)) e820_entry {
    u64 addr;
    u64 size;
    u32 type;
};

static int setup_e820(void *bootparams_virt) {
    u8 *bp = (u8 *)bootparams_virt;

    /* e820_entries count at offset 0x1e8 */
    u8 *e820_count = &bp[E820_ENTRIES_OFFSET];
    struct e820_entry *table = (struct e820_entry *)&bp[E820_TABLE_OFFSET];

    int n = 0;

    /* Entry 0: 0x00000000-0x0009FFFF: usable RAM (640KB) */
    table[n].addr = 0x00000000;
    table[n].size = 0x000A0000;
    table[n].type = E820_TYPE_RAM;
    n++;

    /* Entry 1: 0x000A0000-0x000FFFFF: reserved (VGA + BIOS) */
    table[n].addr = 0x000A0000;
    table[n].size = 0x00060000;
    table[n].type = E820_TYPE_RESERVED;
    n++;

    /* Entry 2: 0x00100000-0x003FFFFF: reserved (UTSM area) */
    table[n].addr = 0x00100000;
    table[n].size = 0x00300000;
    table[n].type = E820_TYPE_RESERVED;
    n++;

    /* Entry 3: 0x00400000-0x03FFFFFF: usable RAM (kernel + initrd + low RAM) */
    table[n].addr = 0x00400000;
    table[n].size = 0x03C00000;  /* 60MB */
    table[n].type = E820_TYPE_RAM;
    n++;

    /* Entry 4: 0x04000000-0x040FFFFF: reserved (IPC shared memory, 1MB) */
    table[n].addr = 0x04000000;
    table[n].size = 0x00100000;  /* 1MB */
    table[n].type = E820_TYPE_RESERVED;
    n++;

    /* Entry 5: 0x04100000-0x0FFFFFFF: usable RAM (general RAM, ~188MB) */
    table[n].addr = 0x04100000;
    table[n].size = 0x0BF00000;  /* 191MB */
    table[n].type = E820_TYPE_RAM;
    n++;

    *e820_count = (u8)n;
    return n;
}

/* ===== bzImage module finder ===== */

void *linux_find_bzimage_module(u64 *size_out) {
    if (!g_module_request.response) {
        log_error("[LINUX] no Limine module response");
        return (void *)0;
    }

    struct limine_module_response *resp = g_module_request.response;
    for (u64 i = 0; i < resp->module_count; i++) {
        struct limine_file *file = resp->modules[i];
        if (!file || !file->path) continue;

        /* Look for paths containing "linux" or "bzImage" */
        const char *p = file->path;
        int match = 0;
        /* Simple case-insensitive substring search for "linux" */
        for (const char *s = p; *s; s++) {
            if ((s[0] == 'l' || s[0] == 'L') &&
                (s[1] == 'i' || s[1] == 'I') &&
                (s[2] == 'n' || s[2] == 'N') &&
                (s[3] == 'u' || s[3] == 'U') &&
                (s[4] == 'x' || s[4] == 'X')) {
                match = 1;
                break;
            }
        }
        if (!match) continue;

        if (size_out) *size_out = file->size;
        log_info("[LINUX] found bzImage module:");
        log_info(file->path);
        log_hex64("[LINUX] module size=", file->size);
        return file->address;
    }

    log_warn("[LINUX] no bzImage module found");
    return (void *)0;
}

/* ===== initrd module finder =====
 *
 * 在 Limine boot module 中查找 initramfs（路径含 "initrd" 或 "initramfs"）。
 * 返回模块数据指针，*size_out 为大小；未找到返回 NULL。 */
static void *linux_find_initrd_module(u64 *size_out) {
    if (!g_module_request.response) return (void *)0;

    struct limine_module_response *resp = g_module_request.response;
    for (u64 i = 0; i < resp->module_count; i++) {
        struct limine_file *file = resp->modules[i];
        if (!file || !file->path) continue;

        const char *p = file->path;
        int match = 0;
        for (const char *s = p; *s; s++) {
            /* 匹配 "initrd"（如 linux-initrd.img） */
            if ((s[0] == 'i' || s[0] == 'I') &&
                (s[1] == 'n' || s[1] == 'N') &&
                (s[2] == 'i' || s[2] == 'I') &&
                (s[3] == 't' || s[3] == 'T') &&
                (s[4] == 'r' || s[4] == 'R') &&
                (s[5] == 'd' || s[5] == 'D')) {
                match = 1;
                break;
            }
        }
        if (!match) continue;

        if (size_out) *size_out = file->size;
        log_info("[LINUX] found initrd module:");
        log_info(file->path);
        log_hex64("[LINUX] initrd size=", file->size);
        return file->address;
    }

    log_warn("[LINUX] no initrd module found");
    return (void *)0;
}

/* ===== bzImage parser ===== */

int linux_parse_bzimage(const void *bzimage, u64 size,
                        u64 *payload_offset_out, u64 *payload_length_out,
                        u64 *init_size_out, u64 *entry_offset_out) {
    const u8 *img = (const u8 *)bzimage;

    if (size < 0x300) {
        log_error("[LINUX] bzImage too small");
        return -1;
    }

    /* Check boot_flag at 0x1fe */
    u16 boot_flag = *(u16 *)&img[BZ_BOOT_FLAG_OFF];
    if (boot_flag != 0xAA55) {
        log_hex64("[LINUX] bad boot_flag=", boot_flag);
        return -2;
    }

    /* Check header magic "HdrS" at 0x202 */
    u32 magic = *(u32 *)&img[BZ_HEADER_OFF];
    if (magic != BZ_HEADER_MAGIC) {
        log_hex64("[LINUX] bad header magic=", magic);
        return -3;
    }

    /* Check protocol version */
    u16 version = *(u16 *)&img[BZ_VERSION_OFF];
    log_hex64("[LINUX] boot protocol version=", version);
    if (version < 0x020c) {
        log_error("[LINUX] boot protocol too old");
        return -4;
    }

    /* Check xloadflags for 64-bit kernel */
    u16 xloadflags = *(u16 *)&img[BZ_XLOADFLAGS_OFF];
    log_hex64("[LINUX] xloadflags=", xloadflags);
    if (!(xloadflags & XLF_KERNEL_64)) {
        log_error("[LINUX] not a 64-bit kernel");
        return -5;
    }

    /* Payload offset and length */
    u32 payload_offset = *(u32 *)&img[BZ_PAYLOAD_OFFSET_OFF];
    u32 payload_length = *(u32 *)&img[BZ_PAYLOAD_LENGTH_OFF];
    log_hex64("[LINUX] payload_offset=", payload_offset);
    log_hex64("[LINUX] payload_length=", payload_length);

    if (payload_offset + payload_length > size) {
        log_error("[LINUX] payload exceeds image size");
        return -6;
    }

    /* Init size (total memory needed for kernel init) */
    u32 init_size = *(u32 *)&img[BZ_INIT_SIZE_ACTUAL_OFF];
    if (init_size == 0) init_size = payload_length;
    log_hex64("[LINUX] init_size=", init_size);

    /* The 64-bit entry point is at offset 0x200 from the payload start.
     * This is the "startup_64" symbol. */
    u64 entry_offset = payload_offset + 0x200;
    log_hex64("[LINUX] entry_offset=", entry_offset);

    /* Setup sectors (for real-mode header size) */
    u8 setup_sects = img[0x1f1];
    if (setup_sects == 0) setup_sects = 4;
    u64 setup_size = (setup_sects + 1) * 512;
    log_hex64("[LINUX] setup_sects=", setup_sects);
    log_hex64("[LINUX] setup_size=", setup_size);

    if (payload_offset_out) *payload_offset_out = payload_offset;
    if (payload_length_out) *payload_length_out = payload_length;
    if (init_size_out) *init_size_out = init_size;
    if (entry_offset_out) *entry_offset_out = entry_offset;

    return 0;
}

/* ===== Boot params setup ===== */

static int setup_boot_params(void *bp_virt, u64 bp_gpa,
                             u64 kernel_entry_gpa,
                             u64 cmdline_gpa, u64 cmdline_len,
                             u64 initrd_gpa, u64 initrd_size,
                             const void *bzimage) {
    (void)bp_gpa;  /* GPA is used by EPT mapping, not needed here */
    const u8 *img = (const u8 *)bzimage;
    u8 *bp = (u8 *)bp_virt;

    /* Zero entire boot_params */
    mem_zero(bp, BOOT_PARAMS_SIZE);

    /* Copy setup_header from bzImage (offset 0x1f1, length 0x1ef = 0x200 - 0x11) */
    /* The setup_header in boot_params starts at 0x1f1 and goes to 0x2xx.
     * We copy from bzImage offset 0x1f1 to boot_params offset 0x1f1. */
    u64 hdr_copy_len = 0x210;  /* copy a generous chunk */
    if (hdr_copy_len > BOOT_PARAMS_SIZE - SETUP_HEADER_OFFSET) {
        hdr_copy_len = BOOT_PARAMS_SIZE - SETUP_HEADER_OFFSET;
    }
    mem_copy(&bp[SETUP_HEADER_OFFSET], &img[SETUP_HEADER_OFFSET], hdr_copy_len);

    /* Override key fields */
    bp[BZ_TYPE_OF_LOADER_OFF] = 0xFF;   /* custom bootloader */

    /* loadflags: LOADED_HIGH | CAN_USE_HEAP */
    u8 loadflags = bp[BZ_LOADFLAGS_OFF];
    loadflags |= LOADFLAG_LOADED_HIGH | LOADFLAG_CAN_USE_HEAP;
    bp[BZ_LOADFLAGS_OFF] = loadflags;

    /* Command line pointer and size */
    *(u32 *)&bp[BZ_CMD_LINE_PTR_OFF] = (u32)cmdline_gpa;
    *(u32 *)&bp[BZ_CMDLINE_SIZE_OFF] = (u32)cmdline_len;

    /* Initrd */
    if (initrd_size > 0) {
        *(u32 *)&bp[BZ_RAMDISK_IMAGE_OFF] = (u32)initrd_gpa;
        *(u32 *)&bp[BZ_RAMDISK_SIZE_OFF] = (u32)initrd_size;
    }

    /* code32_start: set to kernel entry (informational) */
    *(u32 *)&bp[BZ_CODE32_START_OFF] = (u32)kernel_entry_gpa;

    /* Heap end pointer (offset 0x224): end of real-mode heap.
     * Not critical for 64-bit boot, but set a reasonable value. */
    *(u16 *)&bp[0x224] = 0xFE00;  /* heap ends at 0xFE00 */

    /* Set up e820 memory map */
    int e820_count = setup_e820(bp_virt);
    log_hex64("[LINUX] e820 entries=", (u64)e820_count);

    return 0;
}

/* ===== Main loader entry ===== */

int linux_loader_init(void) {
    log_info("[LINUX] loader init begin");

    /* 1. Find bzImage module */
    u64 bzimage_size = 0;
    void *bzimage = linux_find_bzimage_module(&bzimage_size);
    if (!bzimage) {
        log_warn("[LINUX] no bzImage found, skipping Linux guest");
        return -1;
    }

    /* 2. Parse bzImage header */
    u64 payload_offset, payload_length, init_size, entry_offset;
    if (linux_parse_bzimage(bzimage, bzimage_size,
                            &payload_offset, &payload_length,
                            &init_size, &entry_offset) != 0) {
        log_error("[LINUX] bzImage parse failed");
        return -2;
    }

    /* 3. Allocate host physical memory for guest regions */

    /* Kernel: round up init_size to page boundary, align to 2MB */
    u64 kernel_alloc_size = ((init_size + 0xFFFFF) & ~0xFFFFFULL);
    if (kernel_alloc_size < payload_length) kernel_alloc_size = payload_length;
    dkm_dma_buffer kernel_buf;
    if (dma_alloc_pages(kernel_alloc_size / 4096, 2 * 1024 * 1024, 0, &kernel_buf) != 0) {
        log_error("[LINUX] failed to alloc kernel memory");
        return -3;
    }
    log_hex64("[LINUX] kernel HPA=", kernel_buf.phys);
    log_hex64("[LINUX] kernel alloc size=", kernel_alloc_size);

    /* Boot params: 1 page */
    dkm_dma_buffer bp_buf;
    if (dma_alloc_pages(1, 4096, 0, &bp_buf) != 0) {
        log_error("[LINUX] failed to alloc boot_params");
        return -4;
    }

    /* Page tables: PGT_TOTAL_PAGES pages */
    dkm_dma_buffer pgt_buf;
    if (dma_alloc_pages(PGT_TOTAL_PAGES, 4096, 0, &pgt_buf) != 0) {
        log_error("[LINUX] failed to alloc page tables");
        return -5;
    }

    /* GDT: 1 page */
    dkm_dma_buffer gdt_buf;
    if (dma_alloc_pages(1, 4096, 0, &gdt_buf) != 0) {
        log_error("[LINUX] failed to alloc GDT");
        return -6;
    }

    /* Stack: 1 page (stack grows down from top) */
    dkm_dma_buffer stack_buf;
    if (dma_alloc_pages(1, 4096, 0, &stack_buf) != 0) {
        log_error("[LINUX] failed to alloc stack");
        return -7;
    }

    /* Command line: 1 page */
    dkm_dma_buffer cmdline_buf;
    if (dma_alloc_pages(1, 4096, 0, &cmdline_buf) != 0) {
        log_error("[LINUX] failed to alloc cmdline");
        return -8;
    }

    /* General RAM: 64MB for Linux, mapped at GPA 0x05000000 (after IPC region) */
    u64 ram_size = 64 * 1024 * 1024;
    dkm_dma_buffer ram_buf;
    if (dma_alloc_pages(ram_size / 4096, 2 * 1024 * 1024, 0, &ram_buf) != 0) {
        log_error("[LINUX] failed to alloc guest RAM (64MB)");
        /* Try smaller: 32MB */
        ram_size = 32 * 1024 * 1024;
        if (dma_alloc_pages(ram_size / 4096, 2 * 1024 * 1024, 0, &ram_buf) != 0) {
            log_error("[LINUX] failed to alloc guest RAM (32MB)");
            return -9;
        }
    }
    log_hex64("[LINUX] guest RAM HPA=", ram_buf.phys);
    log_hex64("[LINUX] guest RAM size=", ram_size);

    /* 4. Load kernel image into allocated memory */
    const u8 *kernel_src = (const u8 *)bzimage + payload_offset;
    mem_copy(kernel_buf.virt, kernel_src, payload_length);
    log_hex64("[LINUX] kernel loaded, size=", payload_length);

    /* 5. Set up guest page tables */
    g_guest.pgt_gpa = LINUX_GUEST_PGT_GPA;
    build_guest_page_tables(pgt_buf.phys);
    log_hex64("[LINUX] page tables built at HPA=", pgt_buf.phys);

    /* 6. Set up guest GDT */
    g_guest.gdt_gpa = LINUX_GUEST_GDT_GPA;
    build_guest_gdt(gdt_buf.phys);
    log_hex64("[LINUX] GDT built at HPA=", gdt_buf.phys);

    /* 7. Set up command line */
    u64 cmdline_len = str_len(g_default_cmdline) + 1;  /* include NUL */
    mem_copy(cmdline_buf.virt, g_default_cmdline, cmdline_len);
    log_info("[LINUX] cmdline:");
    log_info(g_default_cmdline);

    /* 8. Set up boot_params */
    g_guest.bootparams_gpa = LINUX_GUEST_BOOTPARAMS_GPA;
    g_guest.kernel_gpa = LINUX_GUEST_KERNEL_GPA;
    g_guest.kernel_entry = LINUX_GUEST_KERNEL_GPA + entry_offset;
    g_guest.cmdline_gpa = LINUX_GUEST_CMDLINE_GPA;
    g_guest.cmdline_size = cmdline_len;

    /* 8.5. Load initrd (busybox initramfs) if present.
     * initrd 放在 GPA 0x03000000（initrd 区，上限 16MB 到 IPC 区 0x04000000）。 */
    g_guest.initrd_gpa = 0;
    g_guest.initrd_size = 0;
    {
        u64 initrd_size = 0;
        void *initrd = linux_find_initrd_module(&initrd_size);
        if (initrd && initrd_size > 0) {
            u64 initrd_max = LINUX_GUEST_IPC_SHM_GPA - LINUX_GUEST_INITRD_GPA; /* 16MB */
            if (initrd_size > initrd_max) {
                log_error("[LINUX] initrd too large (>16MB), skipped");
            } else {
                u64 initrd_pages = (initrd_size + 4095) / 4096;
                dkm_dma_buffer initrd_buf;
                if (dma_alloc_pages(initrd_pages, 4096, 0, &initrd_buf) != 0) {
                    log_error("[LINUX] failed to alloc initrd memory");
                    return -17;
                }
                mem_copy(initrd_buf.virt, initrd, initrd_size);
                if (ept_map_range(LINUX_GUEST_INITRD_GPA, initrd_buf.phys,
                                  initrd_pages * 4096, EPT_READ | EPT_WRITE) != 0) {
                    log_error("[LINUX] EPT map initrd failed");
                    return -18;
                }
                g_guest.initrd_gpa = LINUX_GUEST_INITRD_GPA;
                g_guest.initrd_size = initrd_size;
                log_hex64("[LINUX] initrd mapped GPA=", g_guest.initrd_gpa);
                log_hex64("[LINUX] initrd size=", g_guest.initrd_size);
            }
        } else {
            log_warn("[LINUX] booting without initrd (no userland!)");
        }
    }

    setup_boot_params(bp_buf.virt, g_guest.bootparams_gpa,
                      g_guest.kernel_entry,
                      g_guest.cmdline_gpa, cmdline_len,
                      g_guest.initrd_gpa, g_guest.initrd_size,
                      bzimage);
    log_hex64("[LINUX] boot_params at GPA=", g_guest.bootparams_gpa);

    /* 9. EPT-map all guest regions (GPA → HPA) */

    /* Kernel: GPA 0x01000000 → kernel HPA, RWX */
    if (ept_map_range(g_guest.kernel_gpa, kernel_buf.phys,
                      kernel_alloc_size, EPT_RWX) != 0) {
        log_error("[LINUX] EPT map kernel failed");
        return -10;
    }

    /* Boot params: GPA 0x00400000 → bp HPA, RW */
    if (ept_map_range(g_guest.bootparams_gpa, bp_buf.phys,
                      4096, EPT_READ | EPT_WRITE) != 0) {
        log_error("[LINUX] EPT map boot_params failed");
        return -11;
    }

    /* Page tables: GPA 0x00700000 → pgt HPA, RW */
    if (ept_map_range(g_guest.pgt_gpa, pgt_buf.phys,
                      PGT_TOTAL_SIZE, EPT_READ | EPT_WRITE) != 0) {
        log_error("[LINUX] EPT map page tables failed");
        return -12;
    }

    /* GDT: GPA 0x00600000 → gdt HPA, RW */
    if (ept_map_range(g_guest.gdt_gpa, gdt_buf.phys,
                      4096, EPT_READ | EPT_WRITE) != 0) {
        log_error("[LINUX] EPT map GDT failed");
        return -13;
    }

    /* Stack: GPA 0x00800000 → stack HPA, RW */
    if (ept_map_range(LINUX_GUEST_STACK_GPA, stack_buf.phys,
                      4096, EPT_READ | EPT_WRITE) != 0) {
        log_error("[LINUX] EPT map stack failed");
        return -14;
    }

    /* Command line: GPA 0x00500000 → cmdline HPA, R */
    if (ept_map_range(g_guest.cmdline_gpa, cmdline_buf.phys,
                      4096, EPT_READ) != 0) {
        log_error("[LINUX] EPT map cmdline failed");
        return -15;
    }

    /* General RAM: map the allocated RAM at GPA 0x05000000 (after IPC region).
     * GPA layout (matches e820):
     *   0x00400000-0x009FFFFF: fixed regions (boot_params, GDT, pgt, stack, cmdline)
     *   0x01000000-0x02FFFFFF: kernel
     *   0x03000000-0x03FFFFFF: initrd (if any)
     *   0x04000000-0x040FFFFF: IPC shared memory (mapped by ipc_shm_init)
     *   0x05000000-0x08FFFFFF: general RAM (64MB)
     * Linux sees this as usable RAM in e820. */
    u64 ram_gpa = LINUX_GUEST_RAM_GPA;
    if (ept_map_range(ram_gpa, ram_buf.phys, ram_size, EPT_RWX) != 0) {
        log_error("[LINUX] EPT map guest RAM failed");
        return -16;
    }
    log_hex64("[LINUX] guest RAM mapped GPA=", ram_gpa);

    /* Identity-map the first 1MB (BIOS area, needed by some Linux code paths) */
    if (ept_identity_map(0, 0x100000, EPT_RWX) != 0) {
        log_warn("[LINUX] EPT identity map first 1MB failed (non-critical)");
    }

    /* 10. Record guest info */
    g_guest.kernel_size = payload_length;
    g_guest.loaded = 1;

    log_hex64("[LINUX] kernel GPA=", g_guest.kernel_gpa);
    log_hex64("[LINUX] kernel entry=", g_guest.kernel_entry);
    log_hex64("[LINUX] kernel size=", g_guest.kernel_size);
    log_info("[LINUX] loader init ok");

    return 0;
}

const struct linux_guest_info *linux_get_guest_info(void) {
    return g_guest.loaded ? &g_guest : (struct linux_guest_info *)0;
}
