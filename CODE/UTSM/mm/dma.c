#include <utsm/dma.h>
#include <utsm/log.h>
#include <utsm/types.h>
#include <utsm/panic.h>
#include "../arch/x86_64/limine.h"

#define PAGE_SIZE 4096ULL
#define DMA_LOW_MAX_PHYS 0x100000000ULL          /* 只在低 4G 物理内存做 DMA 分配 */
#define DMA_BITMAP_PAGES (DMA_LOW_MAX_PHYS / PAGE_SIZE)   /* 1,048,576 页 */
#define DMA_BITMAP_WORDS (DMA_BITMAP_PAGES / 64ULL)       /* 每 word 覆盖 64 页 */

extern volatile struct limine_memmap_request g_memmap_request;
extern volatile struct limine_hhdm_request g_hhdm_request;

/*
 * 真实物理页分配器：
 * - 以 memmap 中低 4G 内的 USABLE 区域构建 page bitmap（bit=1 表示可用）。
 * - Limine 保证 USABLE 区域不含 kernel/modules/framebuffer/bootloader 结构，
 *   因此无需手动避让，标记为可用的页一定是安全的独立物理页。
 * - alloc 返回的 virt 使用 HHDM 直映射（hhdm_offset + phys），
 *   保证 CPU 访问的虚拟地址与设备 bus-master 使用的物理地址指向同一页。
 */
static u64 g_hhdm_offset;
static u64 g_bitmap[DMA_BITMAP_WORDS];   /* 1 bit/页，1=free */
static u64 g_free_pages;
static u64 g_lowest_free_index;          /* 分配起点提示，减少扫描 */
static int g_dma_ready;

static u64 align_up(u64 value, u64 alignment) {
    if (alignment < PAGE_SIZE) alignment = PAGE_SIZE;
    return (value + alignment - 1ULL) & ~(alignment - 1ULL);
}

static void bitmap_mark_free(u64 index) {
    g_bitmap[index >> 6] |= (1ULL << (index & 63ULL));
}

static void bitmap_mark_used(u64 index) {
    g_bitmap[index >> 6] &= ~(1ULL << (index & 63ULL));
}

static int bitmap_is_free(u64 index) {
    return (g_bitmap[index >> 6] >> (index & 63ULL)) & 1ULL;
}

void dma_init(void) {
    g_hhdm_offset = 0;
    g_free_pages = 0;
    g_lowest_free_index = DMA_BITMAP_PAGES;
    g_dma_ready = 0;
    for (u64 i = 0; i < DMA_BITMAP_WORDS; i++) g_bitmap[i] = 0;

    if (g_hhdm_request.response) {
        g_hhdm_offset = g_hhdm_request.response->offset;
    }
    if (!g_hhdm_offset || !g_memmap_request.response) {
        log_warn("[DMA] missing HHDM or memmap response");
        panic_full("MM-E20 MISSING HHDM OR MEMMAP",
                   "dma_init: HHDM or memmap response missing", 0);
        return;
    }

    struct limine_memmap_response *rsp = g_memmap_request.response;
    for (u64 i = 0; i < rsp->entry_count; i++) {
        struct limine_memmap_entry *e = rsp->entries[i];
        if (!e || e->type != LIMINE_MEMMAP_USABLE) continue;

        u64 base = align_up(e->base, PAGE_SIZE);
        u64 end = e->base + e->length;
        if (end <= base) continue;

        /* 保留最低 1MiB，避免 legacy/实模式结构；限制在低 4G 内。 */
        if (base < 0x100000ULL) base = 0x100000ULL;
        if (end > DMA_LOW_MAX_PHYS) end = DMA_LOW_MAX_PHYS;
        if (end <= base) continue;

        u64 start_index = base / PAGE_SIZE;
        u64 end_index = end / PAGE_SIZE;
        for (u64 idx = start_index; idx < end_index; idx++) {
            if (!bitmap_is_free(idx)) {
                bitmap_mark_free(idx);
                g_free_pages++;
                if (idx < g_lowest_free_index) g_lowest_free_index = idx;
            }
        }
    }

    if (g_free_pages == 0) {
        log_warn("[DMA] no usable low pages found");
        panic_full("MM-E21 NO USABLE LOW PAGES",
                   "dma_init: no usable low-4G pages in memmap", 0);
        return;
    }

    g_dma_ready = 1;
    log_info("[DMA] init ok");
    log_hex64("[DMA] free pages=", g_free_pages);
    log_hex64("[DMA] first free phys=", g_lowest_free_index * PAGE_SIZE);
}

int dma_alloc_pages(u64 page_count, u64 alignment, u64 max_phys, dkm_dma_buffer *out) {
    if (!out || page_count == 0 || !g_dma_ready) return -1;
    if (page_count > DMA_BITMAP_PAGES) return -3;
    /* 真机安全: max_phys 参数上限为低 4G (DMA bitmap 只覆盖低 4G)。
     * 调用方若传 0 表示"无限制"，按低 4G 上限处理。
     * 对 NVMe/AHCI 等需要 32-bit DMA 的设备, max_phys 传 0x100000000。
     * 若未来需要 >4G DMA, 需要扩展 bitmap 或使用 IOMMU/SWIOTLB。 */
    if (max_phys == 0 || max_phys > DMA_LOW_MAX_PHYS) max_phys = DMA_LOW_MAX_PHYS;

    if (alignment < PAGE_SIZE) alignment = PAGE_SIZE;
    u64 align_pages = alignment / PAGE_SIZE;
    if (align_pages == 0) align_pages = 1;

    u64 max_index = max_phys / PAGE_SIZE;
    u64 start = g_lowest_free_index;
    start = (start / align_pages) * align_pages;

    for (u64 base = start; base + page_count <= max_index; base += align_pages) {
        int ok = 1;
        for (u64 j = 0; j < page_count; j++) {
            if (!bitmap_is_free(base + j)) { ok = 0; break; }
        }
        if (!ok) continue;

        for (u64 j = 0; j < page_count; j++) bitmap_mark_used(base + j);
        g_free_pages -= page_count;

        u64 phys = base * PAGE_SIZE;
        out->phys = phys;
        out->virt = (void *)(g_hhdm_offset + phys);
        out->size = page_count * PAGE_SIZE;

        u8 *p = (u8 *)out->virt;
        for (u64 i = 0; i < out->size; i++) p[i] = 0;
        return 0;
    }

    log_warn("[DMA] alloc failed");
    log_hex64("[DMA] req pages=", page_count);
    log_hex64("[DMA] free pages=", g_free_pages);
    panic_full("MM-E22 DMA ALLOC FAILED",
               "dma_alloc_pages: no free pages satisfying request", 0);
    return -2;
}

static const dkm_dma_api g_dma_api = {
    .alloc_pages = dma_alloc_pages
};

const dkm_dma_api *dma_get_api(void) {
    return &g_dma_api;
}
