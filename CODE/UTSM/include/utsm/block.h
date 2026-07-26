#ifndef UTSM_BLOCK_H
#define UTSM_BLOCK_H

#include <utsm/types.h>

#define DKM_BLOCK_MAX_DEVICES 8u

typedef int (*dkm_block_read_fn)(void *ctx, u64 lba, u32 count, void *buffer);
typedef int (*dkm_block_write_fn)(void *ctx, u64 lba, u32 count, const void *buffer);

typedef struct dkm_block_device_desc {
    const char *name;
    u64 sector_size;
    u64 sector_count;
    void *ctx;
    dkm_block_read_fn read;
    dkm_block_write_fn write;
} dkm_block_device_desc;

typedef struct dkm_block_api {
    int (*register_device)(const dkm_block_device_desc *desc);
    u32 (*device_count)(void);
    int (*read)(u32 index, u64 lba, u32 count, void *buffer);
    int (*write)(u32 index, u64 lba, u32 count, const void *buffer);
    u64 (*sector_size)(u32 index);
    const char *(*device_name)(u32 index);
    int (*set_write_fn)(u32 index, dkm_block_write_fn fn);
} dkm_block_api;

void block_init(void);
const dkm_block_api *block_get_api(void);

#endif
