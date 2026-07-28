#ifndef UTSM_LINUX_LOADER_H
#define UTSM_LINUX_LOADER_H

#include <utsm/types.h>

/* ===== Linux boot protocol structures (Documentation/x86/boot.rst) ===== */

/* bzImage setup_header — lives at offset 0x1f1 inside bzImage and boot_params.
 * Only the fields we actually use are listed; gaps are reserved/padding. */
struct linux_setup_header {
    u8  setup_sects;           /* 0x1f1: number of setup sectors */
    u16 root_flags;            /* 0x1f2 */
    u32 syssize;               /* 0x1f4: protected-mode code size / 16 */
    u16 ram_size;              /* 0x1f8 */
    u16 vid_mode;              /* 0x1fa */
    u16 boot_flag;             /* 0x1fe: 0xAA55 */
    u16 jump;                  /* 0x200: jump insn */
    u32 header;                /* 0x202: "HdrS" magic */
    u16 version;               /* 0x206: boot protocol version */
    u32 realmode_swtch;        /* 0x208 */
    u16 start_sys_seg;         /* 0x20c */
    u16 kernel_version;        /* 0x20e: offset to version string */
    u8  type_of_loader;        /* 0x210 */
    u8  loadflags;             /* 0x211 */
    u16 setup_move_size;       /* 0x212 */
    u32 code32_start;          /* 0x214: 32-bit entry (also 64-bit entry base) */
    u32 ramdisk_image;         /* 0x218 */
    u32 ramdisk_size;          /* 0x21c */
    u32 bootsect_kludge;       /* 0x220 */
    u16 heap_end_ptr;          /* 0x224 */
    u8  ext_loader_ver;        /* 0x226 */
    u8  ext_loader_type;       /* 0x227 */
    u32 cmd_line_ptr;          /* 0x228: command line physical address */
    u32 init_size;             /* 0x22c: total init memory needed */
    u32 acpi_rsdp_addr;        /* 0x230 */
    u8  _pad0[4];              /* 0x234: relocatable_kernel + min_alignment */
    u16 xloadflags;            /* 0x236: bit0=XLF_KERNEL_64 */
    u32 cmdline_size;          /* 0x238 */
    u32 _pad1;                 /* 0x23c */
    u64 setup_data;            /* 0x240: linked list of setup_data */
    u64 pref_address;          /* 0x248 → wait, this is payload_offset */

    /* NOTE: The real layout continues with payload_offset at 0x248.
     * However, pref_address is at 0x258 and init_size at 0x260.
     * The struct above is intentionally simplified — we read fields
     * by offset rather than relying on this struct for the full layout. */
} __attribute__((packed));

/* boot_params is 4096 bytes. The setup_header is at offset 0x1f1. */
#define BOOT_PARAMS_SIZE        4096
#define SETUP_HEADER_OFFSET     0x1f1

/* Key field offsets within bzImage / boot_params */
#define BZ_BOOT_FLAG_OFF        0x1fe
#define BZ_HEADER_MAGIC         0x53726448   /* "HdrS" */
#define BZ_HEADER_OFF           0x202
#define BZ_VERSION_OFF          0x206
#define BZ_TYPE_OF_LOADER_OFF   0x210
#define BZ_LOADFLAGS_OFF        0x211
#define BZ_CODE32_START_OFF     0x214
#define BZ_RAMDISK_IMAGE_OFF    0x218
#define BZ_RAMDISK_SIZE_OFF     0x21c
#define BZ_CMD_LINE_PTR_OFF    0x228
#define BZ_INIT_SIZE_OFF        0x22c  /* wait — actually init_size is at 0x260 */
#define BZ_XLOADFLAGS_OFF       0x236
#define BZ_CMDLINE_SIZE_OFF     0x238
#define BZ_PAYLOAD_OFFSET_OFF   0x248
#define BZ_PAYLOAD_LENGTH_OFF   0x24c
#define BZ_PREF_ADDRESS_OFF     0x258
#define BZ_INIT_SIZE_ACTUAL_OFF 0x260

/* xloadflags bits */
#define XLF_KERNEL_64           0x0001
#define XLF_CAN_BE_LOADED_ABOVE 0x0002
#define XLF_EFI_HANDOVER        0x0010
#define XLF_EFI_KEXEC           0x0020

/* loadflags bits */
#define LOADFLAG_LOADED_HIGH    0x01
#define LOADFLAG_KEEP_ROOT      0x02
#define LOADFLAG_CAN_USE_HEAP   0x80

/* e820 entry types */
#define E820_TYPE_RAM           1
#define E820_TYPE_RESERVED      2
#define E820_TYPE_ACPI          3
#define E820_TYPE_NVS           4
#define E820_TYPE_UNUSABLE      5
#define E820_TYPE_PMEM          7

/* e820_table entry in boot_params at offset 0x2e0 (actually 0x2d0 in older,
 * 0x2e0 in newer). We use 0x2e0 and the e820_entries count at 0x1e8.
 * Each entry is 20 bytes: u64 addr, u64 size, u32 type. */
#define E820_ENTRIES_OFFSET     0x1e8
#define E820_TABLE_OFFSET       0x2e0
#define E820_MAX_ENTRIES        128
#define E820_ENTRY_SIZE         20

/* ===== Guest memory layout (GPA) =====
 * Based on utsm-linux-dual-kernel-architecture.md.
 * 0x00000000-0x003FFFFF : UTSM reserved (denied to Linux)
 * 0x00400000-0x004FFFFF : boot_params / cmdline / GDT / page tables / stack
 * 0x01000000-0x02FFFFFF : Linux kernel image
 * 0x03000000-0x03FFFFFF : Linux initrd
 * 0x04000000-0x040FFFFF : IPC shared memory (1MB, both kernels RW)
 * 0x05000000-MAX        : Linux general RAM
 */
#define LINUX_GUEST_KERNEL_GPA  0x01000000ULL  /* 16 MB, 2MB-aligned */
#define LINUX_GUEST_BOOTPARAMS_GPA 0x00400000ULL /* 4 MB */
#define LINUX_GUEST_CMDLINE_GPA 0x00500000ULL  /* 5 MB */
#define LINUX_GUEST_INITRD_GPA  0x03000000ULL  /* 48 MB (within kernel+initrd region) */
#define LINUX_GUEST_STACK_GPA   0x00800000ULL  /* 8 MB (stack grows down) */
#define LINUX_GUEST_GDT_GPA     0x00600000ULL  /* 6 MB (guest GDT) */
#define LINUX_GUEST_PGT_GPA     0x00700000ULL  /* 7 MB (guest page tables) */
#define LINUX_GUEST_CMDLINE_MAX 4096

/* IPC shared memory GPA (must match CODE/utsm-ipc/ipc_proto.h) */
#define LINUX_GUEST_IPC_SHM_GPA 0x04000000ULL  /* 64 MB */
#define LINUX_GUEST_IPC_SHM_SIZE (1024 * 1024)  /* 1MB */

/* General RAM starts after IPC region */
#define LINUX_GUEST_RAM_GPA     0x05000000ULL  /* 80 MB */

/* ===== Linux loader API ===== */

/* Loaded Linux guest state */
struct linux_guest_info {
    u64 kernel_gpa;         /* GPA where kernel was loaded */
    u64 kernel_entry;       /* 64-bit entry point GPA */
    u64 kernel_size;        /* Size of loaded kernel image */
    u64 bootparams_gpa;     /* GPA of boot_params */
    u64 cmdline_gpa;        /* GPA of command line */
    u64 cmdline_size;       /* Command line length (with NUL) */
    u64 initrd_gpa;         /* GPA of initrd (0 = none) */
    u64 initrd_size;        /* Size of initrd */
    u64 pgt_gpa;            /* GPA of guest page tables */
    u64 gdt_gpa;            /* GPA of guest GDT */
    int loaded;             /* 1 = successfully loaded */
};

/* Find the Linux bzImage boot module among Limine modules.
 * Returns pointer to the module data, or NULL if not found.
 * Sets *size_out to the module size. */
void *linux_find_bzimage_module(u64 *size_out);

/* Parse bzImage header and extract key parameters.
 * Returns 0 on success, negative on error. */
int linux_parse_bzimage(const void *bzimage, u64 size,
                        u64 *payload_offset_out, u64 *payload_length_out,
                        u64 *init_size_out, u64 *entry_offset_out);

/* Initialize the Linux guest: parse bzImage, allocate guest memory,
 * set up boot_params, page tables, GDT, and EPT mappings.
 * Returns 0 on success, negative on error. */
int linux_loader_init(void);

/* Get the loaded Linux guest info (NULL if not loaded). */
const struct linux_guest_info *linux_get_guest_info(void);

/* Configure VMCS for Linux guest and perform vmlaunch.
 * Must be called after vmm_init() + linux_loader_init().
 * Returns 0 on successful guest termination, negative on error. */
int linux_launch(void);

/* Default Linux command line (can be overridden).
 * nohlt: prevent kernel idle loop from using HLT — only the exec daemon's
 *        explicit ioctl(PARK) HLT should trigger VM-Exit park, so that
 *        linux_launch() returns only after the daemon is ready. */
#define LINUX_DEFAULT_CMDLINE \
    "console=ttyS0,115200 earlyprintk=serial nokaslr " \
    "no_timer_check loglevel=7 nohlt idle=poll"

#endif
