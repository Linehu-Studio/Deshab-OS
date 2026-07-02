#ifndef UTSM_DMA_H
#define UTSM_DMA_H

#include <utsm/types.h>

typedef struct dkm_dma_buffer {
    void *virt;
    u64 phys;
    u64 size;
} dkm_dma_buffer;

typedef struct dkm_dma_api {
    int (*alloc_pages)(u64 page_count, u64 alignment, u64 max_phys, dkm_dma_buffer *out);
} dkm_dma_api;

void dma_init(void);
int dma_alloc_pages(u64 page_count, u64 alignment, u64 max_phys, dkm_dma_buffer *out);
const dkm_dma_api *dma_get_api(void);

#endif
