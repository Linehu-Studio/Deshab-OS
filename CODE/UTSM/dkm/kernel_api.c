#include <utsm/dkm.h>
#include <utsm/log.h>
#include <utsm/idt.h>
#include <utsm/net.h>

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
    log_error(msg);
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
    .block = 0
};

const dkm_kernel_api *dkm_get_kernel_api(void) {
    return &g_kernel_api;
}

#include "../arch/x86_64/limine.h"
extern volatile struct limine_rsdp_request g_rsdp_request;
extern volatile struct limine_framebuffer_request g_fb_request;
extern volatile struct limine_module_request g_module_request;
extern volatile struct limine_hhdm_request g_hhdm_request;

void dkm_fill_platform_info(void) {
    g_kernel_api.dma = dma_get_api();
    g_kernel_api.net = net_get_api();
    g_kernel_api.block = block_get_api();
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
}
