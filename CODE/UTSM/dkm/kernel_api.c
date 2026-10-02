#include <utsm/dkm.h>
#include <utsm/log.h>
#include <utsm/idt.h>
#include <utsm/net.h>
#include <utsm/mm.h>
#include <utsm/dma.h>
#include <utsm/drr.h>
#include <utsm/utrw.h>
#include <utsm/panic.h>

static void dkm_api_info(const char *msg) {
    log_info(msg);
}

static void dkm_api_warn(const char *msg) {
    log_warn(msg);
}

static void dkm_api_error(const char *msg) {
    log_error(msg);
}

static void dkm_api_panic(const char *msg) {
    /* F1: 驱动 panic 统一走莲花崩溃屏（串口 dump + DRR 归档 + 帧缓冲） */
    panic_full("DKM-PANIC", msg, 0);
    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}

static const dkm_log_api g_log_api = {
    .info = dkm_api_info,
    .warn = dkm_api_warn,
    .error = dkm_api_error,
    .panic = dkm_api_panic
};

/* ---- D4: DKM 彩色日志——每个驱动有自己的脾气 ----
 * 加载器在 driver_init 前调用 dkm_log_set_driver(desc->name)：
 * 驱动名 FNV-1a hash → 8 色调色板（明亮色系，串口可读），
 * 同名驱动颜色确定性可复现。init 返回后由调用方清上下文。 */
void dkm_log_set_driver(const char *name) {
    static const char *palette[8] = {
        "\x1b[36m", "\x1b[32m", "\x1b[33m", "\x1b[35m",
        "\x1b[96m", "\x1b[92m", "\x1b[93m", "\x1b[95m"
    };
    if (!name || !*name) {
        log_set_color(0);
        return;
    }
    u32 h = 2166136261u;
    while (*name) {
        h ^= (u8)*name++;
        h *= 16777619u;
    }
    log_set_color(palette[h & 7]);
}

static dkm_kernel_api g_kernel_api = {
    .version = DKM_KERNEL_API_VERSION,
    .size = sizeof(dkm_kernel_api),
    .feature_bits = 0,
    .log = &g_log_api,
    .mem = 0,
    .utsm = 0,
    .irq = 0,
    .pci = 0,
    .dma = 0,
    .vfs = 0,
    .net = 0,
    .timer = 0,
    .drr = 0,
    .rsdp_address = 0,
    .fb_address = 0,
    .fb_width = 0,
    .fb_height = 0,
    .fb_pitch = 0,
    .fb_bpp = 0,
    .boot_modules_response = 0,
    .irq_register = 0,
    .hhdm_offset = 0,
    .block = 0,
    .mmio = 0,
    .mm_map_mmio = 0,
    .mm_unmap_mmio = 0,
    .register_apic_eoi = 0,
    .sched = 0,
    .kapi = 0
};

const dkm_kernel_api *dkm_get_kernel_api(void) {
    return &g_kernel_api;
}

/* ===================================================================
 *  D3: 按名导出表（kapi）——自由也要有地图
 * =================================================================== */

static const dkm_kapi_entry g_kapi_entries[] = {
    { "log.info",           (void *)dkm_api_info },
    { "log.warn",           (void *)dkm_api_warn },
    { "log.error",          (void *)dkm_api_error },
    { "log.panic",          (void *)dkm_api_panic },
    { "dma.alloc_pages",    (void *)dma_alloc_pages },
    { "irq.register",       (void *)irq_register },
    { "irq.vector_alloc",   (void *)irq_vector_alloc },
    { "irq.vector_free",    (void *)irq_vector_free },
    { "mm.map_mmio",        (void *)mm_map_mmio },
    { "mm.unmap_mmio",      (void *)mm_unmap_mmio },
    { "apic.register_eoi",  (void *)idt_register_apic_eoi },
    { "drr.snapshot_dirty", (void *)drr_ckpt_snapshot_dirty },
    { "drr.rollback_pages", (void *)drr_rollback_pages },
    { "utrw.debug_dump",    (void *)utrw_debug_dump_page },
};

static int kapi_lookup(const char *name, void **out_fn) {
    if (!name) return -1;
    for (u32 i = 0; i < sizeof(g_kapi_entries) / sizeof(g_kapi_entries[0]); i++) {
        const dkm_kapi_entry *e = &g_kapi_entries[i];
        const char *a = e->name, *b = name;
        while (*a && *a == *b) { a++; b++; }
        if (*a == 0 && *b == 0) {
            if (out_fn) *out_fn = e->fn;
            return 0;
        }
    }
    return -1;
}

static const dkm_kapi_table g_kapi_table = {
    .magic = DKM_KAPI_MAGIC,
    .version = DKM_KAPI_VERSION,
    .count = sizeof(g_kapi_entries) / sizeof(g_kapi_entries[0]),
    .entries = g_kapi_entries,
    .lookup = kapi_lookup,
};

#include "../arch/x86_64/limine.h"
extern volatile struct limine_rsdp_request g_rsdp_request;
extern volatile struct limine_framebuffer_request g_fb_request;
extern volatile struct limine_module_request g_module_request;
extern volatile struct limine_hhdm_request g_hhdm_request;

void dkm_fill_platform_info(void) {
    g_kernel_api.dma = dma_get_api();
    g_kernel_api.net = net_get_api();
    g_kernel_api.block = block_get_api();
    g_kernel_api.mmio = paging_get_api();
    /* M4/Phase7-8: 调度器与恢复根服务表（drr 为原占位字段，此处激活） */
    g_kernel_api.sched = utsm_sched_get_api();
    g_kernel_api.drr = drr_get_recovery_api();
    if (g_rsdp_request.response && g_rsdp_request.response->address) {
        g_kernel_api.rsdp_address = g_rsdp_request.response->address;
    }
    if (g_fb_request.response && g_fb_request.response->framebuffer_count > 0) {
        struct limine_framebuffer *fb = g_fb_request.response->framebuffers[0];
        g_kernel_api.fb_address = fb->address;
        g_kernel_api.fb_width = fb->width;
        g_kernel_api.fb_height = fb->height;
        g_kernel_api.fb_pitch = fb->pitch;
        g_kernel_api.fb_bpp = fb->bpp;
    }
    if (g_module_request.response) {
        g_kernel_api.boot_modules_response = g_module_request.response;
    }
    g_kernel_api.irq_register = (int (*)(u8,void*))((u64)irq_register);
    if (g_hhdm_request.response) {
        g_kernel_api.hhdm_offset = g_hhdm_request.response->offset;
    }
    g_kernel_api.mm_map_mmio = mm_map_mmio;
    g_kernel_api.mm_unmap_mmio = mm_unmap_mmio;
    /* B7 阶段2: 暴露 LAPIC EOI 钩子注册入口（apic.drv LAPIC 接管时调用） */
    g_kernel_api.register_apic_eoi = idt_register_apic_eoi;
    /* B7 阶段3: 暴露动态向量分配器（MSI/MSI-X 设备驱动使用） */
    g_kernel_api.irq_vector_alloc = irq_vector_alloc;
    g_kernel_api.irq_vector_free = irq_vector_free;
    /* D3: 按名导出表挂入 kernel_api（尾部追加，ABI 兼容） */
    g_kernel_api.kapi = &g_kapi_table;
}

/* ===================================================================
 *  严格错误策略插桩：设计已定但未实现的 DKM 能力，建立代码骨架，
 *  调用即莲花 panic（DKM-E 系列）。当前无调用方（骨架）。
 * =================================================================== */

/* D1: DKM 驱动热卸载（引用计数）。driver_desc 已有 NO_UNLOAD 标志位，
 * 但卸载路径本身（refcount + driver_exit + 段回收）未实现。 */
void dkm_unload_driver(u32 driver_index) {
    (void)driver_index;
    panic_full("DKM-E01 DRIVER HOT UNLOAD NOT IMPLEMENTED",
               "refcount unload path: designed (D1), not implemented", 0);
}

/* D2: 裸机器码加载器（无 ELF 头，直接映射 .text blob 到可执行页跳转）。
 * SAS-R0 单地址空间下合法；加载器未实现。 */
void *dkm_load_bare_code(const void *blob, u64 size) {
    (void)blob; (void)size;
    panic_full("DKM-E02 BARE CODE LOADER NOT IMPLEMENTED",
               "headerless machine-code loader: designed (D2), not implemented", 0);
    return 0;
}

/* 多核：SIPI 唤醒 AP（apic.drv 注释预留）。BSP-only 是当前既定边界，
 * 一旦被调用即 panic 而非静默单核继续。 */
void dkm_sipi_wake_ap(u32 lapic_id, u64 entry_eip) {
    (void)lapic_id; (void)entry_eip;
    panic_full("DKM-E03 SIPI AP WAKE NOT IMPLEMENTED",
               "AP startup framework: designed, not implemented (BSP-only)", 0);
}
