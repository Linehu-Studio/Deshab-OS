#ifndef UTSM_LIMINE_H
#define UTSM_LIMINE_H

#include <utsm/types.h>

#define LIMINE_COMMON_MAGIC 0xc7b1dd30df4c8b88ULL, 0x0a82e883a194f07bULL

struct limine_hhdm_response {
    u64 revision;
    u64 offset;
};

struct limine_hhdm_request {
    u64 id[4];
    u64 revision;
    struct limine_hhdm_response *response;
};

struct limine_kernel_address_response {
    u64 revision;
    u64 physical_base;
    u64 virtual_base;
};

struct limine_kernel_address_request {
    u64 id[4];
    u64 revision;
    struct limine_kernel_address_response *response;
};

#define LIMINE_KERNEL_ADDRESS_REQUEST_ID { LIMINE_COMMON_MAGIC, 0x71ba76863cc55f63ULL, 0xb2644a48c516a487ULL }

struct limine_file {
    u64 revision;
    void *address;
    u64 size;
    char *path;
    char *cmdline;
    u32 media_type;
    u32 unused;
    u32 tftp_ip;
    u32 tftp_port;
    u32 partition_index;
    u32 mbr_disk_id;
    u8 gpt_disk_uuid[16];
    u8 gpt_part_uuid[16];
    u8 part_uuid[16];
};

struct limine_module_response {
    u64 revision;
    u64 module_count;
    struct limine_file **modules;
};

struct limine_module_request {
    u64 id[4];
    u64 revision;
    struct limine_module_response *response;
};

#define LIMINE_MODULE_REQUEST_ID { LIMINE_COMMON_MAGIC, 0x3e7e279702be32afULL, 0xca1c4f3bd1280ceeULL }

struct limine_rsdp_response {
    u64 revision;
    void *address;
};

struct limine_rsdp_request {
    u64 id[4];
    u64 revision;
    struct limine_rsdp_response *response;
};

#define LIMINE_RSDP_REQUEST_ID { LIMINE_COMMON_MAGIC, 0xc5e77b6b397e7b43ULL, 0x27637845accdcf3cULL }

struct limine_framebuffer {
    void *address;
    u64 width;
    u64 height;
    u64 pitch;
    u16 bpp;
    u8 memory_model;
    u8 red_mask_size;
    u8 red_mask_shift;
    u8 green_mask_size;
    u8 green_mask_shift;
    u8 blue_mask_size;
    u8 blue_mask_shift;
    u8 unused[7];
    u64 edid_size;
    void *edid;
    u64 mode_count;
    struct limine_video_mode **modes;
};

struct limine_video_mode {
    u64 pitch;
    u64 width;
    u64 height;
    u16 bpp;
    u8 memory_model;
    u8 red_mask_size;
    u8 red_mask_shift;
    u8 green_mask_size;
    u8 green_mask_shift;
    u8 blue_mask_size;
    u8 blue_mask_shift;
};

struct limine_framebuffer_response {
    u64 revision;
    u64 framebuffer_count;
    struct limine_framebuffer **framebuffers;
};

struct limine_framebuffer_request {
    u64 id[4];
    u64 revision;
    struct limine_framebuffer_response *response;
};

#define LIMINE_FRAMEBUFFER_REQUEST_ID { LIMINE_COMMON_MAGIC, 0x9d5827dcd881dd75ULL, 0xa3148604f6fab11bULL }

#define LIMINE_MEMMAP_USABLE                 0
#define LIMINE_MEMMAP_RESERVED               1
#define LIMINE_MEMMAP_ACPI_RECLAIMABLE       2
#define LIMINE_MEMMAP_ACPI_NVS               3
#define LIMINE_MEMMAP_BAD_MEMORY             4
#define LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE 5
#define LIMINE_MEMMAP_KERNEL_AND_MODULES     6
#define LIMINE_MEMMAP_FRAMEBUFFER            7

struct limine_memmap_entry {
    u64 base;
    u64 length;
    u64 type;
};

struct limine_memmap_response {
    u64 revision;
    u64 entry_count;
    struct limine_memmap_entry **entries;
};

struct limine_memmap_request {
    u64 id[4];
    u64 revision;
    struct limine_memmap_response *response;
};

#define LIMINE_MEMMAP_REQUEST_ID { LIMINE_COMMON_MAGIC, 0x67cf3d9d378a806fULL, 0xe304acdfc50c3c62ULL }

#endif
