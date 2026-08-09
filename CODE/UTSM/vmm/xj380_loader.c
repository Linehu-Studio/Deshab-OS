/* xj380_loader.c — OpenXJ380 kernel.krl loader for UTSM VMX non-root guest.
 *
 * 参照 linux_loader.c 的既有模式实现 XJ380 第二 guest：
 *   1. 在 Limine boot module 中查找 kernel.krl（路径含 "xj380"）
 *   2. 解析 ELF64 PT_LOAD 段（固定链接 0xFFFFFFFF80000000）
 *   3. 分配 host 物理内存：boot area / framebuffer / kernel / RAM
 *   4. 构造 guest 初始页表（恒等 0-4GB + HHDM + 内核区三区布局）
 *   5. 构造 guest GDT（64 位 code/data/TSS）
 *   6. 构造 ACPI 桩表 MADT/FADT/MCFG/HPET（幽灵设备地址，EPT 按需分配）
 *   7. 构造 EFI 格式内存图（单一 Conventional 区 = guest RAM）
 *   8. 构造 BOOT_CONFIG / FrameBufferConfig / 伪 EFI_SYSTEM_TABLE
 *   9. EPT 映射所有 guest 区域
 *
 * VMCS 配置与 vmlaunch 见 xj380_boot.c。
 */

#include <utsm/xj380_loader.h>
#include <utsm/vmx.h>
#include <utsm/ept.h>
#include <utsm/log.h>
#include <utsm/types.h>
#include <utsm/dma.h>
#include "../arch/x86_64/limine.h"

extern volatile struct limine_module_request g_module_request;
extern volatile struct limine_hhdm_request g_hhdm_request;

/* ===== 运行期辅助 ===== */

static void mem_copy(void *dst, const void *src, u64 n) {
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    for (u64 i = 0; i < n; i++) d[i] = s[i];
}

static void mem_zero(void *dst, u64 n) {
    u8 *d = (u8 *)dst;
    for (u64 i = 0; i < n; i++) d[i] = 0;
}

static u64 hhdm(void) {
    return g_hhdm_request.response ? g_hhdm_request.response->offset : 0;
}

static void *to_virt(u64 hpa) {
    return (void *)(hpa + hhdm());
}

/* ===== ELF64 解析（kernel.krl 为 ET_EXEC） ===== */

#define ELF_MAGIC   0x464C457FULL   /* "\x7fELF" */
#define ELF_ET_EXEC 2
#define ELF_PT_LOAD 1

typedef struct __attribute__((packed)) {
    u8  e_ident[16];
    u16 e_type;
    u16 e_machine;
    u32 e_version;
    u64 e_entry;
    u64 e_phoff;
    u64 e_shoff;
    u32 e_flags;
    u16 e_ehsize;
    u16 e_phentsize;
    u16 e_phnum;
    u16 e_shentsize;
    u16 e_shnum;
    u16 e_shstrndx;
} xj_elf64_ehdr;

typedef struct __attribute__((packed)) {
    u32 p_type;
    u32 p_flags;
    u64 p_offset;
    u64 p_vaddr;
    u64 p_paddr;
    u64 p_filesz;
    u64 p_memsz;
    u64 p_align;
} xj_elf64_phdr;

/* 解析 ELF：校验头、遍历 PT_LOAD 计算内核跨度，记录入口。
 * 返回 0 成功；负值失败。 */
static int xj380_parse_elf(const void *img, u64 size,
                           u64 *entry_out, u64 *span_out) {
    if (size < sizeof(xj_elf64_ehdr)) {
        log_error("[XJ380] kernel.krl too small");
        return -1;
    }
    const xj_elf64_ehdr *eh = (const xj_elf64_ehdr *)img;
    if (*(u32 *)eh->e_ident != (u32)ELF_MAGIC) {
        log_error("[XJ380] bad ELF magic");
        return -2;
    }
    if (eh->e_ident[4] != 2) {          /* ELFCLASS64 */
        log_error("[XJ380] not a 64-bit ELF");
        return -3;
    }
    if (eh->e_type != ELF_ET_EXEC) {
        log_error("[XJ380] kernel.krl not ET_EXEC");
        return -4;
    }
    if (eh->e_phnum == 0 || eh->e_phentsize != sizeof(xj_elf64_phdr) ||
        eh->e_phoff + (u64)eh->e_phnum * sizeof(xj_elf64_phdr) > size) {
        log_error("[XJ380] bad program header table");
        return -5;
    }

    u64 span = 0;
    const xj_elf64_phdr *ph = (const xj_elf64_phdr *)((const u8 *)img + eh->e_phoff);
    for (u16 i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != ELF_PT_LOAD) continue;
        if (ph[i].p_vaddr < XJ380_KERNEL_VADDR) {
            log_error("[XJ380] PT_LOAD below kernel base");
            log_hex64("[XJ380] vaddr=", ph[i].p_vaddr);
            return -6;
        }
        u64 off = ph[i].p_vaddr - XJ380_KERNEL_VADDR;
        if (off + ph[i].p_memsz > XJ380_GUEST_KERNEL_MAX) {
            log_error("[XJ380] kernel exceeds reserved window (32MB)");
            return -7;
        }
        if (ph[i].p_offset + ph[i].p_filesz > size) {
            log_error("[XJ380] PT_LOAD filesz beyond image");
            return -8;
        }
        if (off + ph[i].p_memsz > span) span = off + ph[i].p_memsz;
    }
    if (span == 0) {
        log_error("[XJ380] no PT_LOAD segments");
        return -9;
    }

    if (entry_out) *entry_out = eh->e_entry;
    if (span_out)  *span_out  = span;
    log_hex64("[XJ380] ELF entry=", eh->e_entry);
    log_hex64("[XJ380] kernel span=", span);
    return 0;
}

/* ===== Guest 状态 ===== */

static struct xj380_guest_info g_guest;

/* ===== Guest 页表（三区布局，同 XJ380 引导器契约） =====
 *
 * PML4  @ pgt_gpa + 0x0000
 *   [0]   → PDPT0       恒等映射 0-4GB（早期直读 MADT 等，内核自清低半）
 *   [256] → PDPT0       HHDM 映射 0xFFFF800000000000 + [0,4GB)
 *   [511] → PDPT_K      内核区 0xFFFFFF8000000000+
 * PDPT0 @ pgt_gpa + 0x1000，[0..3] → PD0..PD3（每页覆盖 1GB，2MB 大页）
 * PD0-3 @ pgt_gpa + 0x2000..0x5000
 * PDPT_K@ pgt_gpa + 0x6000，[480] → PD_K（0xFFFFFFFF80000000 区间）
 * PD_K  @ pgt_gpa + 0x7000，[0..n] 2MB 大页映射内核 GPA
 */
#define XJ_PGT_PDPT0_OFF  0x1000
#define XJ_PGT_PD0_OFF    0x2000
#define XJ_PGT_PDPTK_OFF  0x6000
#define XJ_PGT_PDK_OFF    0x7000

/* x86-64 页表项位 */
#define XJ_PTE_P   (1ULL << 0)
#define XJ_PTE_RW  (1ULL << 1)
#define XJ_PTE_PS  (1ULL << 7)

static int xj380_build_page_tables(u64 pgt_hpa, u64 kernel_gpa, u64 kernel_span) {
    u64 *pml4 = (u64 *)to_virt(pgt_hpa);
    mem_zero(pml4, XJ380_PGT_SIZE);

    u64 pdpt0_gpa = g_guest.pgt_gpa + XJ_PGT_PDPT0_OFF;
    u64 pdptk_gpa = g_guest.pgt_gpa + XJ_PGT_PDPTK_OFF;

    /* PML4[0] 恒等 + PML4[256] HHDM 指向同一个 PDPT0 */
    pml4[0]   = pdpt0_gpa | XJ_PTE_P | XJ_PTE_RW;
    pml4[256] = pdpt0_gpa | XJ_PTE_P | XJ_PTE_RW;
    /* PML4[511] → PDPT_K */
    pml4[511] = pdptk_gpa | XJ_PTE_P | XJ_PTE_RW;

    /* PDPT0[0..3] → PD0..PD3（覆盖 0-4GB） */
    u64 *pdpt0 = (u64 *)((u8 *)pml4 + XJ_PGT_PDPT0_OFF);
    for (int i = 0; i < 4; i++) {
        u64 pd_gpa = g_guest.pgt_gpa + XJ_PGT_PD0_OFF + (u64)i * 4096;
        pdpt0[i] = pd_gpa | XJ_PTE_P | XJ_PTE_RW;
    }
    /* 每个 PD 512 项，每项 2MB 大页：GPA = i*1GB + j*2MB */
    for (int i = 0; i < 4; i++) {
        u64 *pd = (u64 *)((u8 *)pml4 + XJ_PGT_PD0_OFF + (u64)i * 4096);
        for (int j = 0; j < 512; j++) {
            u64 page_gpa = (u64)i * (1ULL << 30) + (u64)j * (2ULL << 20);
            pd[j] = page_gpa | XJ_PTE_P | XJ_PTE_RW | XJ_PTE_PS;
        }
    }

    /* PDPT_K[480] → PD_K（虚拟 0xFFFFFFFF80000000 落在 PML4[511] 第 480 个 1GB） */
    u64 *pdptk = (u64 *)((u8 *)pml4 + XJ_PGT_PDPTK_OFF);
    u64 pdk_gpa = g_guest.pgt_gpa + XJ_PGT_PDK_OFF;
    pdptk[480] = pdk_gpa | XJ_PTE_P | XJ_PTE_RW;

    /* PD_K[i]：虚拟 0xFFFFFFFF80000000 + i*2MB → GPA kernel_gpa + i*2MB */
    u64 *pdk = (u64 *)((u8 *)pml4 + XJ_PGT_PDK_OFF);
    u64 pages = (kernel_span + (2ULL << 20) - 1) >> 21;
    for (u64 i = 0; i < pages; i++) {
        u64 gpa = kernel_gpa + (i << 21);
        pdk[i] = gpa | XJ_PTE_P | XJ_PTE_RW | XJ_PTE_PS;
    }
    return 0;
}

/* ===== Guest GDT（与 linux_loader 相同的 64 位布局） ===== */

typedef struct __attribute__((packed)) {
    u16 limit_low;
    u16 base_low;
    u8  base_mid;
    u8  access;
    u8  flags_limit_high;
    u8  base_high;
} xj_gdt_entry64;

typedef struct __attribute__((packed)) {
    u32 limit_low_base_low;
    u8  base_mid;
    u8  access;
    u8  flags_limit_high;
    u8  base_high;
    u32 base_upper;
    u32 reserved;
} xj_tss_desc64;

static int xj380_build_gdt(u64 gdt_hpa) {
    u8 *gdt_page = (u8 *)to_virt(gdt_hpa);
    mem_zero(gdt_page, 4096);

    xj_gdt_entry64 *gdt = (xj_gdt_entry64 *)gdt_page;
    gdt[1].limit_low = 0xFFFF;
    gdt[1].access = 0x9B;
    gdt[1].flags_limit_high = 0xAF;   /* G=1, L=1（64 位代码） */

    gdt[2].limit_low = 0xFFFF;
    gdt[2].access = 0x93;
    gdt[2].flags_limit_high = 0xCF;

    /* [0x18] 64 位 TSS 描述符，TSS 本体在同页 +0x100 */
    xj_tss_desc64 *tss_d = (xj_tss_desc64 *)&gdt_page[0x18];
    u64 tss_base = g_guest.gdt_gpa + 0x100;
    u32 tss_limit = 0x67;
    tss_d->limit_low_base_low = (tss_limit & 0xFFFF) | ((u32)(tss_base & 0xFFFFFF) << 16);
    tss_d->base_mid = (u8)((tss_base >> 24) & 0xFF);
    tss_d->access = 0x89;
    tss_d->flags_limit_high = (u8)((tss_limit >> 16) & 0x0F);
    tss_d->base_high = 0;
    tss_d->base_upper = 0;
    tss_d->reserved = 0;
    return 0;
}

/* ===== ACPI 桩表 =====
 *
 * XJ380 初始化需要 MADT（APIC/LAPIC/IOAPIC）、FADT（未使用）、MCFG（PCIe）、
 * HPET（时钟）。所有设备地址均为"幽灵地址"：EPT 未映射该区域时由
 * handle_ept_violation 按需分配真实页，guest 读写不崩溃。
 * LAPIC/IOAPIC/HPET/MMCONF 均落在低 4GB，恒等 + HHDM 双路可见。
 */

typedef struct __attribute__((packed)) {
    char signature[4];
    u32  length;
    u8   revision;
    u8   checksum;
    char oemid[6];
    char oemtableid[8];
    u32  oemrev;
    u32  creatorid;
    u32  creatorrev;
} xj_acpi_header;   /* 36 字节 */

static void acpi_set_header(xj_acpi_header *h, const char sig[4], u32 len) {
    mem_zero(h, len);
    h->signature[0] = sig[0];
    h->signature[1] = sig[1];
    h->signature[2] = sig[2];
    h->signature[3] = sig[3];
    h->length = len;
    h->revision = 1;
    mem_copy(h->oemid, "DESHAB", 6);
    mem_copy(h->oemtableid, "UTSMVMM", 8);
    h->oemrev = 1;
    mem_copy((char *)&h->creatorid, "UTSM", 4);
    h->creatorrev = 1;
    /* 校验和 = -sum（校验和字节置 0 后求和） */
    u32 sum = 0;
    u8 *p = (u8 *)h;
    for (u32 i = 0; i < len; i++) sum += p[i];
    h->checksum = (u8)(0 - (u8)sum);
}

/* MADT：36 头 + LAPIC 地址(4) + 标志(4) + LAPIC 项(8) + IOAPIC 项(12) = 64 字节。
 * LAPIC 项：type=0, len=8, uid=0, id=0, flags=1（仅 enabled，不触发 AP 启动）
 * IOAPIC 项：type=1, len=12, id=0, addr=0xFEC00000（幽灵）, gsib=0 */
static int xj380_build_madt(u64 hpa) {
    u8 *t = (u8 *)to_virt(hpa);
    acpi_set_header((xj_acpi_header *)t, "APIC", 64);
    u8 *p = t + 36;
    p[0] = 0x00; p[1] = 0x00; p[2] = 0x00; p[3] = 0xF0;   /* lapic addr = 0xFEE00000 */
    p[4] = 0x01; p[5] = 0x00; p[6] = 0x00; p[7] = 0x00;   /* flags = PCAT_COMPAT */
    p += 8;
    p[0] = 0x00;  /* type = LAPIC */
    p[1] = 0x08;  /* len */
    p[2] = 0x00;  /* ACPI processor uid */
    p[3] = 0x00;  /* lapic id */
    p[4] = 0x01; p[5] = 0x00; p[6] = 0x00; p[7] = 0x00;   /* flags = enabled only */
    p += 8;
    p[0] = 0x01;  /* type = IOAPIC */
    p[1] = 0x0C;  /* len */
    p[2] = 0x00;  /* ioapic id */
    p[3] = 0x00;  /* reserved */
    p[4] = 0x00; p[5] = 0x00; p[6] = 0xC0; p[7] = 0xFE;   /* ioapic addr = 0xFEC00000 */
    p[8] = 0x00; p[9] = 0x00; p[10] = 0x00; p[11] = 0x00; /* gsib */
    return 0;
}

/* FADT：最小桩表（XJ380 仅在未接线的 power_shutdown 路径使用，启动不读） */
static int xj380_build_fadt(u64 hpa) {
    u8 *t = (u8 *)to_virt(hpa);
    acpi_set_header((xj_acpi_header *)t, "FACP", 96);
    return 0;
}

/* MCFG：36 头 + reserved(8) + 1 项(16) = 60 字节。
 * base=0xF0000000（幽灵 MMCONF），1 个 segment、1 条总线（扫描结果为无设备） */
static int xj380_build_mcfg(u64 hpa) {
    u8 *t = (u8 *)to_virt(hpa);
    acpi_set_header((xj_acpi_header *)t, "MCFG", 60);
    u8 *p = t + 36 + 8;
    p[0] = 0x00; p[1] = 0x00; p[2] = 0x00; p[3] = 0x00;
    p[4] = 0x00; p[5] = 0x00; p[6] = 0xF0; p[7] = 0x0F;   /* base = 0xF0000000 */
    p[8] = 0x00; p[9] = 0x00;                               /* segment group 0 */
    p[10] = 0x00;                                           /* start bus 0 */
    p[11] = 0x00;                                           /* end bus 0 */
    p[12] = 0x00; p[13] = 0x00; p[14] = 0x00; p[15] = 0x00; /* reserved */
    return 0;
}

/* HPET：36 头 + event_block_id(4) + GAS(12) + tick_unit(2) + 属性(1) = 55 → 56。
 * base=0xFED00000（幽灵），generalCapabilities 读 0 → hpetPeriod=0，不挂起 */
static int xj380_build_hpet(u64 hpa) {
    u8 *t = (u8 *)to_virt(hpa);
    acpi_set_header((xj_acpi_header *)t, "HPET", 56);
    u8 *p = t + 36;
    p[0] = 0x00; p[1] = 0x00; p[2] = 0x00; p[3] = 0x00;   /* event_block_id */
    p[4] = 0x00;                                            /* address_space = 系统内存 */
    p[5] = 0x20;                                            /* bit_width = 64 */
    p[6] = 0x00;                                            /* bit_offset */
    p[7] = 0x00;                                            /* access_size */
    p[8] = 0x00; p[9] = 0x00; p[10] = 0x00; p[11] = 0x00;
    p[12] = 0x00; p[13] = 0xD0; p[14] = 0xFE; p[15] = 0x0F; /* base = 0xFED00000 */
    p[16] = 0x00; p[17] = 0x00;                             /* clock_tick_unit */
    p[18] = 0x00;                                           /* page_oem_flags */
    return 0;
}

/* ===== EFI 格式内存图 =====
 *
 * 仅一个 Conventional 区（guest RAM），其余区域隐式保留。
 * EfiMemoryDesc：Type(4)+pad(4)+PhysicalStart(8)+VirtualStart(8)+
 *                NumberOfPages(8)+Attribute(8)，16 字节对齐 → 48 字节。
 */
typedef struct __attribute__((aligned(16))) {
    u32 type;
    u32 pad;
    u64 physical_start;
    u64 virtual_start;
    u64 number_of_pages;
    u64 attribute;
} xj_efi_memdesc;

#define XJ_EFI_CONVENTIONAL   7
#define XJ_EFI_DESC_SIZE      (u64)sizeof(xj_efi_memdesc)
#define XJ_EFI_MEM_ATTR       0xFULL

static int xj380_build_memmap(u64 hpa) {
    xj_efi_memdesc *d = (xj_efi_memdesc *)to_virt(hpa);
    mem_zero(d, sizeof(*d));
    d->type = XJ_EFI_CONVENTIONAL;
    d->physical_start = g_guest.ram_gpa;
    d->virtual_start = 0;
    d->number_of_pages = g_guest.ram_size / 4096;
    d->attribute = XJ_EFI_MEM_ATTR;
    return 0;
}

/* ===== BOOT_CONFIG（布局必须与 OpenXJ380 boot/include/boot.h 一致） =====
 *
 * MEMORY_MAP 在 memory.h 中 #pragma pack(1)：36 字节；BOOT_CONFIG 默认对齐。
 * 关键：MemoryMap.Buffer / saved_mtrrs 是【物理地址】（内核自行 +HHDM）；
 * temp_stack[] 是【HHDM 虚拟地址】数组（仅拷贝，1 CPU 不 deref）。
 */

typedef struct __attribute__((packed)) {
    u64 map_size;
    u64 buffer;          /* 物理 GPA */
    u64 map_key;
    u64 descriptor_size;
    u32 descriptor_version;
} xj_memory_map;         /* 36 字节 */

typedef struct {
    xj_memory_map memory_map;
    u64  fadt;
    u64  madt;
    u64  hpet;
    u64  mcfg;
    u64 *saved_mtrrs;        /* 物理 GPA，内核 +HHDM */
    u64 *temp_stack[256];    /* HHDM 虚拟地址 */
    int  is_qemu;
    u64  boot_flags;
    u64  installer_root_pak;
    u64  installer_root_pak_size;
    u64  system_payload_pak;
    u64  system_payload_pak_size;
} xj_boot_config;

_Static_assert(sizeof(xj_memory_map) == 36, "xj_memory_map layout mismatch");
_Static_assert(sizeof(xj_boot_config) == 2176, "xj_boot_config layout mismatch");

/* FrameBufferConfig（boot/include/fbc.h，默认对齐，24 字节） */
typedef struct {
    u8  *frame_buffer;          /* HHDM 虚拟地址 */
    u32 pixels_per_scan_line;
    u32 horizontal_resolution;
    u32 vertical_resolution;
    u32 pixel_format;           /* 0=kRGBR, 1=kBGRR */
} xj_framebuffer_config;
_Static_assert(sizeof(xj_framebuffer_config) == 24, "fbc layout mismatch");

static int xj380_build_boot_config(u64 hpa, u64 mtrr_hpa, u64 stack_gpa, u64 fbc_gpa) {
    xj_boot_config *bc = (xj_boot_config *)to_virt(hpa);
    mem_zero(bc, sizeof(*bc));

    bc->memory_map.map_size = XJ_EFI_DESC_SIZE;          /* 单描述符 */
    bc->memory_map.buffer = XJ380_MEMMAP_GPA;            /* 物理 GPA */
    bc->memory_map.map_key = 0;
    bc->memory_map.descriptor_size = XJ_EFI_DESC_SIZE;
    bc->memory_map.descriptor_version = 1;

    bc->fadt = XJ380_FADT_GPA;
    bc->madt = XJ380_MADT_GPA;
    bc->hpet = XJ380_HPET_GPA;
    bc->mcfg = XJ380_MCFG_GPA;

    bc->saved_mtrrs = (u64 *)mtrr_hpa;                   /* 物理 GPA */

    u64 stack_hhdm = stack_gpa + XJ380_HHDM_OFFSET;
    for (int i = 0; i < 256; i++) bc->temp_stack[i] = (u64 *)stack_hhdm;

    bc->is_qemu = 1;
    bc->boot_flags = XJ380_BOOT_FLAG_DISABLE_KMOD | XJ380_BOOT_FLAG_SAFE_STORAGE_IO;
    /* installer paks 均为 0（安装器模式关闭） */

    /* FrameBufferConfig：frame_buffer 传 HHDM 虚拟地址，pixel_format=1(kBGRR) */
    xj_framebuffer_config *fbc = (xj_framebuffer_config *)to_virt(fbc_gpa);
    mem_zero(fbc, sizeof(*fbc));
    fbc->frame_buffer = (u8 *)(XJ380_GUEST_FB_GPA + XJ380_HHDM_OFFSET);
    fbc->pixels_per_scan_line = XJ380_FB_WIDTH;
    fbc->horizontal_resolution = XJ380_FB_WIDTH;
    fbc->vertical_resolution = XJ380_FB_HEIGHT;
    fbc->pixel_format = 1;   /* kBGRR */
    return 0;
}

/* ===== Module finder ===== */

void *xj380_find_kernel_module(u64 *size_out) {
    if (!g_module_request.response) {
        log_error("[XJ380] no Limine module response");
        return (void *)0;
    }

    struct limine_module_response *resp = g_module_request.response;
    for (u64 i = 0; i < resp->module_count; i++) {
        struct limine_file *file = resp->modules[i];
        if (!file || !file->path) continue;

        /* 路径含 "xj380"（大小写不敏感） */
        const char *p = file->path;
        int match = 0;
        for (const char *s = p; *s; s++) {
            if ((s[0] == 'x' || s[0] == 'X') &&
                (s[1] == 'j' || s[1] == 'J') &&
                (s[2] == '3' || s[2] == '3') &&
                (s[3] == '8' || s[3] == '8') &&
                (s[4] == '0' || s[4] == '0')) {
                match = 1;
                break;
            }
        }
        if (!match) continue;

        if (size_out) *size_out = file->size;
        log_info("[XJ380] found kernel module:");
        log_info(file->path);
        log_hex64("[XJ380] module size=", file->size);
        return file->address;
    }

    log_warn("[XJ380] no kernel.krl module found");
    return (void *)0;
}

/* ===== 主加载入口 ===== */

int xj380_loader_init(void) {
    log_info("[XJ380] loader init begin");

    /* 1. 查找 kernel.krl 模块 */
    u64 krl_size = 0;
    void *krl = xj380_find_kernel_module(&krl_size);
    if (!krl) {
        log_warn("[XJ380] no kernel.krl found, skipping XJ380 guest");
        return -1;
    }

    /* 2. 解析 ELF */
    u64 entry, span;
    if (xj380_parse_elf(krl, krl_size, &entry, &span) != 0) {
        log_error("[XJ380] kernel.krl parse failed");
        return -2;
    }
    if (entry < XJ380_KERNEL_VADDR || entry >= XJ380_KERNEL_VADDR + span) {
        log_error("[XJ380] entry outside kernel window");
        return -3;
    }

    /* 3. 分配 boot area 缓冲（按页） */
    dkm_dma_buffer pgt_buf, gdt_buf, madt_buf, fadt_buf, mcfg_buf, hpet_buf;
    dkm_dma_buffer memmap_buf, bootcfg_buf, mtrr_buf, stack_buf, fakeefi_buf, fbc_buf;
    if (dma_alloc_pages(8, 4096, 0, &pgt_buf)     != 0) { log_error("[XJ380] alloc pgt failed"); return -4; }
    if (dma_alloc_pages(1, 4096, 0, &gdt_buf)     != 0) { log_error("[XJ380] alloc gdt failed"); return -5; }
    if (dma_alloc_pages(1, 4096, 0, &madt_buf)    != 0) { log_error("[XJ380] alloc madt failed"); return -6; }
    if (dma_alloc_pages(1, 4096, 0, &fadt_buf)    != 0) { log_error("[XJ380] alloc fadt failed"); return -7; }
    if (dma_alloc_pages(1, 4096, 0, &mcfg_buf)    != 0) { log_error("[XJ380] alloc mcfg failed"); return -8; }
    if (dma_alloc_pages(1, 4096, 0, &hpet_buf)    != 0) { log_error("[XJ380] alloc hpet failed"); return -9; }
    if (dma_alloc_pages(1, 4096, 0, &memmap_buf)  != 0) { log_error("[XJ380] alloc memmap failed"); return -10; }
    if (dma_alloc_pages(1, 4096, 0, &bootcfg_buf) != 0) { log_error("[XJ380] alloc bootcfg failed"); return -11; }
    if (dma_alloc_pages(1, 4096, 0, &mtrr_buf)    != 0) { log_error("[XJ380] alloc mtrr failed"); return -12; }
    if (dma_alloc_pages(2, 4096, 0, &stack_buf)   != 0) { log_error("[XJ380] alloc stack failed"); return -13; }
    if (dma_alloc_pages(1, 4096, 0, &fakeefi_buf) != 0) { log_error("[XJ380] alloc fake efi failed"); return -14; }
    if (dma_alloc_pages(1, 4096, 0, &fbc_buf)     != 0) { log_error("[XJ380] alloc fbc failed"); return -15; }

    /* 4. 分配内核缓冲并装入 PT_LOAD 段 */
    u64 kernel_alloc = (span + 4095) & ~4095ULL;
    dkm_dma_buffer kernel_buf;
    if (dma_alloc_pages(kernel_alloc / 4096, 2 * 1024 * 1024, 0, &kernel_buf) != 0) {
        log_error("[XJ380] failed to alloc kernel memory");
        return -16;
    }
    {
        const xj_elf64_ehdr *eh = (const xj_elf64_ehdr *)krl;
        const xj_elf64_phdr *ph = (const xj_elf64_phdr *)((const u8 *)krl + eh->e_phoff);
        u8 *dst = (u8 *)kernel_buf.virt;
        for (u16 i = 0; i < eh->e_phnum; i++) {
            if (ph[i].p_type != ELF_PT_LOAD) continue;
            u64 off = ph[i].p_vaddr - XJ380_KERNEL_VADDR;
            mem_copy(dst + off, (const u8 *)krl + ph[i].p_offset, ph[i].p_filesz);
            if (ph[i].p_memsz > ph[i].p_filesz)
                mem_zero(dst + off + ph[i].p_filesz, ph[i].p_memsz - ph[i].p_filesz);
        }
        log_hex64("[XJ380] kernel loaded HPA=", kernel_buf.phys);
    }

    /* 5. 分配 framebuffer 并清零 */
    dkm_dma_buffer fb_buf;
    if (dma_alloc_pages((XJ380_FB_SIZE + 4095) / 4096, 4096, 0, &fb_buf) != 0) {
        log_error("[XJ380] failed to alloc framebuffer");
        return -17;
    }
    {
        u8 *fb = (u8 *)fb_buf.virt;
        for (u64 i = 0; i < XJ380_FB_SIZE; i++) fb[i] = 0;
    }

    /* 6. 分配 guest RAM（1GB，降级 512MB/256MB） */
    u64 ram_size = XJ380_GUEST_RAM_SIZE;
    dkm_dma_buffer ram_buf;
    if (dma_alloc_pages(ram_size / 4096, 2 * 1024 * 1024, 0, &ram_buf) != 0) {
        log_warn("[XJ380] failed to alloc guest RAM (1GB), trying 512MB");
        ram_size = 512 * 1024 * 1024;
        if (dma_alloc_pages(ram_size / 4096, 2 * 1024 * 1024, 0, &ram_buf) != 0) {
            log_warn("[XJ380] failed to alloc guest RAM (512MB), trying 256MB");
            ram_size = XJ380_GUEST_RAM_MIN;
            if (dma_alloc_pages(ram_size / 4096, 2 * 1024 * 1024, 0, &ram_buf) != 0) {
                log_error("[XJ380] failed to alloc guest RAM (256MB minimum)");
                return -18;
            }
        }
    }
    log_hex64("[XJ380] guest RAM HPA=", ram_buf.phys);
    log_hex64("[XJ380] guest RAM size=", ram_size);

    /* 7. 记录 guest 信息 */
    g_guest.pgt_gpa = XJ380_PGT_GPA;
    g_guest.gdt_gpa = XJ380_GDT_GPA;
    g_guest.kernel_gpa = XJ380_GUEST_KERNEL_GPA;
    g_guest.kernel_entry = entry;
    g_guest.kernel_size = span;
    g_guest.ram_gpa = XJ380_GUEST_RAM_GPA;
    g_guest.ram_size = ram_size;
    g_guest.fb_gpa = XJ380_GUEST_FB_GPA;

    /* 8. 构造 guest 页表 / GDT */
    xj380_build_page_tables(pgt_buf.phys, g_guest.kernel_gpa, span);
    xj380_build_gdt(gdt_buf.phys);
    log_hex64("[XJ380] page tables HPA=", pgt_buf.phys);
    log_hex64("[XJ380] GDT HPA=", gdt_buf.phys);

    /* 9. 构造 ACPI 桩表 */
    xj380_build_madt(madt_buf.phys);
    xj380_build_fadt(fadt_buf.phys);
    xj380_build_mcfg(mcfg_buf.phys);
    xj380_build_hpet(hpet_buf.phys);

    /* 10. 构造内存图 + BOOT_CONFIG（含 MTRR 缓冲清零、伪 EFI 表清零） */
    xj380_build_memmap(memmap_buf.phys);
    mem_zero(mtrr_buf.virt, 4096);
    mem_zero(fakeefi_buf.virt, 4096);
    xj380_build_boot_config(bootcfg_buf.phys, mtrr_buf.phys, XJ380_STACK_GPA, fbc_buf.phys);

    /* 11. EPT 映射所有 guest 区域。
     * boot area 各缓冲独立分配，逐个按自身 GPA 映射。 */
    if (ept_map_range(XJ380_PGT_GPA, pgt_buf.phys, XJ380_PGT_SIZE, EPT_READ | EPT_WRITE) != 0) {
        log_error("[XJ380] EPT map page tables failed");
        return -19;
    }
    if (ept_map_range(XJ380_GDT_GPA, gdt_buf.phys, 4096, EPT_READ | EPT_WRITE) != 0) {
        log_error("[XJ380] EPT map GDT failed");
        return -20;
    }
    if (ept_map_range(XJ380_MADT_GPA, madt_buf.phys, 4096, EPT_READ | EPT_WRITE) != 0) {
        log_error("[XJ380] EPT map MADT failed");
        return -21;
    }
    if (ept_map_range(XJ380_FADT_GPA, fadt_buf.phys, 4096, EPT_READ | EPT_WRITE) != 0) {
        log_error("[XJ380] EPT map FADT failed");
        return -22;
    }
    if (ept_map_range(XJ380_MCFG_GPA, mcfg_buf.phys, 4096, EPT_READ | EPT_WRITE) != 0) {
        log_error("[XJ380] EPT map MCFG failed");
        return -23;
    }
    if (ept_map_range(XJ380_HPET_GPA, hpet_buf.phys, 4096, EPT_READ | EPT_WRITE) != 0) {
        log_error("[XJ380] EPT map HPET failed");
        return -24;
    }
    if (ept_map_range(XJ380_MEMMAP_GPA, memmap_buf.phys, 4096, EPT_READ | EPT_WRITE) != 0) {
        log_error("[XJ380] EPT map memory map failed");
        return -25;
    }
    if (ept_map_range(XJ380_BOOTCFG_GPA, bootcfg_buf.phys, 4096, EPT_READ | EPT_WRITE) != 0) {
        log_error("[XJ380] EPT map BOOT_CONFIG failed");
        return -26;
    }
    if (ept_map_range(XJ380_MTRR_GPA, mtrr_buf.phys, 4096, EPT_READ | EPT_WRITE) != 0) {
        log_error("[XJ380] EPT map MTRR buffer failed");
        return -27;
    }
    if (ept_map_range(XJ380_STACK_GPA, stack_buf.phys, 8192, EPT_READ | EPT_WRITE) != 0) {
        log_error("[XJ380] EPT map stack failed");
        return -28;
    }
    if (ept_map_range(XJ380_FAKEEFI_GPA, fakeefi_buf.phys, 4096, EPT_READ | EPT_WRITE) != 0) {
        log_error("[XJ380] EPT map fake EFI table failed");
        return -29;
    }
    if (ept_map_range(XJ380_FBC_GPA, fbc_buf.phys, 4096, EPT_READ | EPT_WRITE) != 0) {
        log_error("[XJ380] EPT map FrameBufferConfig failed");
        return -30;
    }
    if (ept_map_range(g_guest.fb_gpa, fb_buf.phys, XJ380_FB_SIZE,
                      EPT_READ | EPT_WRITE) != 0) {
        log_error("[XJ380] EPT map framebuffer failed");
        return -31;
    }
    if (ept_map_range(g_guest.kernel_gpa, kernel_buf.phys, kernel_alloc,
                      EPT_RWX) != 0) {
        log_error("[XJ380] EPT map kernel failed");
        return -32;
    }
    if (ept_map_range(g_guest.ram_gpa, ram_buf.phys, ram_size,
                      EPT_RWX) != 0) {
        log_error("[XJ380] EPT map guest RAM failed");
        return -33;
    }
    /* 低 1MB 恒等映射（BIOS 区，与 linux_loader 一致；<1MB 无需按需分配） */
    if (ept_identity_map(0, 0x100000, EPT_RWX) != 0) {
        log_warn("[XJ380] EPT identity map first 1MB failed (non-critical)");
    }

    /* 12. 完成 */
    g_guest.loaded = 1;
    log_hex64("[XJ380] kernel GPA=", g_guest.kernel_gpa);
    log_hex64("[XJ380] kernel entry=", g_guest.kernel_entry);
    log_hex64("[XJ380] kernel size=", g_guest.kernel_size);
    log_hex64("[XJ380] RAM GPA=", g_guest.ram_gpa);
    log_hex64("[XJ380] RAM size=", g_guest.ram_size);
    log_info("[XJ380] loader init ok");
    return 0;
}

const struct xj380_guest_info *xj380_get_guest_info(void) {
    return g_guest.loaded ? &g_guest : (struct xj380_guest_info *)0;
}
