#ifndef UTSM_DSK_H
#define UTSM_DSK_H

#include <utsm/types.h>

#define DSK_BOOT_MAGIC 0x44534B31424F4F54ULL /* "DSK1BOOT" */
#define DSK_BOOT_ABI_VERSION 1u

#define DSK_BOOT_FLAG_FROM_UTSM       (1ULL << 0)
#define DSK_BOOT_FLAG_DKM_READY       (1ULL << 1)
#define DSK_BOOT_FLAG_FAT32_PATH      (1ULL << 2)

typedef struct dsk_boot_context {
    u64 magic;
    u32 abi_version;
    u32 size;

    u64 flags;

    u64 hhdm_offset;
    u64 rsdp_address;

    u64 framebuffer_address;
    u64 framebuffer_width;
    u64 framebuffer_height;
    u64 framebuffer_pitch;
    u32 framebuffer_bpp;
    u32 reserved_fb;

    u64 boot_modules_response;

    u64 dkm_kernel_api;
    u64 dkm_driver_table;
    u64 dkm_driver_count;

    u64 utsm_state;
    u64 drr_state;

    u64 memory_map;
    u64 memory_map_count;
    u64 memory_map_entry_size;

    u64 kernel_stack_top;
    u64 reserved[8];
} dsk_boot_context;

typedef void (*dsk_entry_fn)(const dsk_boot_context *context);

int dsk_load_and_jump(void);

#endif
