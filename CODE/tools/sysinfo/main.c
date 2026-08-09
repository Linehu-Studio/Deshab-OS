/* sysinfo - Deshab 系统信息查看工具
 *
 * 展示 SDK 完整能力链:
 *   boot_context 解析 -> kernel_api 探测 -> block/net 设备枚举 -> 绘制 + 输入
 *
 * 构建: cd CODE/tools/sysinfo ; make
 * 输出: SYSTEM/system/deshab64/tools/sysinfo.elf
 */
#include "../../firstInit/ascii_bitmaps.c"
#include "../../sdk/include/deshab/deshab.h"
#include "../../sdk/include/deshab/font.h"

static dsb_app_context g_ac;

/* ---- 简易格式化（SDK 无 sprintf） ---- */
static char *fmt_u64_hex(u64 v, char *buf) {
    static const char hx[] = "0123456789ABCDEF";
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 0; i < 16; i++)
        buf[2 + i] = hx[(v >> ((15 - i) * 4)) & 0xF];
    buf[18] = 0;
    return buf;
}

static char *fmt_u64_dec(u64 v, char *buf) {
    char tmp[24]; int n = 0;
    if (!v) { buf[0] = '0'; buf[1] = 0; return buf; }
    while (v && n < 23) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    for (int i = 0; i < n; i++) buf[i] = tmp[n - 1 - i];
    buf[n] = 0;
    return buf;
}

/* ---- 信息行绘制 ---- */
#define INFO_X        12
#define INFO_Y_START  (DSB_TITLEBAR_H + 8)
#define LINE_H        20
#define COL_LABEL_W   200

static int g_line = 0;

static void info_line(const char *label, const char *value, u32 val_color) {
    i64 y = INFO_Y_START + (i64)g_line * LINE_H;
    dsb_draw_string(&g_ac, label, INFO_X, y, DSB_TEXT_DIM, DSB_BG_PRIMARY, 12);
    dsb_draw_string(&g_ac, value, INFO_X + COL_LABEL_W, y, val_color, DSB_BG_PRIMARY, 12);
    g_line++;
}

static void info_section(const char *title) {
    i64 y = INFO_Y_START + (i64)g_line * LINE_H;
    dsb_draw_string(&g_ac, title, INFO_X, y, DSB_ACCENT_LIGHT, DSB_BG_PRIMARY, 12);
    dsb_fill_rect(&g_ac, INFO_X, y + 16, (i64)g_ac.fb_w - INFO_X * 2, 1, DSB_BORDER);
    g_line += 2;
}

static void info_ptr(const char *label, const void *p) {
    info_line(label, p ? "present" : "NULL", p ? DSB_SUCCESS : DSB_ERROR);
}

void dsk_entry(const dsk_boot_context *ctx) {
    if (ctx->magic != DSK_BOOT_MAGIC) return;
    dsb_app_init(&g_ac, ctx);

    /* 清屏 + 标题栏 + 状态栏 */
    dsb_fill_bg(&g_ac, DSB_BG_PRIMARY);
    dsb_draw_titlebar(&g_ac, "SysInfo", (i64)g_ac.fb_w);
    dsb_draw_statusbar(&g_ac, "Esc to exit", (i64)g_ac.fb_w, (i64)g_ac.fb_h);

    char buf[32];

    /* ---- DSK Boot Context ---- */
    info_section("== DSK Boot Context ==");
    info_line("magic", fmt_u64_hex(ctx->magic, buf), DSB_TEXT_PRIMARY);
    info_line("abi_version", fmt_u64_dec(ctx->abi_version, buf), DSB_TEXT_PRIMARY);
    info_line("flags", fmt_u64_hex(ctx->flags, buf), DSB_TEXT_PRIMARY);

    /* ---- Framebuffer ---- */
    info_section("== Framebuffer ==");
    info_line("width", fmt_u64_dec(ctx->framebuffer_width, buf), DSB_TEXT_PRIMARY);
    info_line("height", fmt_u64_dec(ctx->framebuffer_height, buf), DSB_TEXT_PRIMARY);
    info_line("pitch", fmt_u64_dec(ctx->framebuffer_pitch, buf), DSB_TEXT_PRIMARY);
    info_line("bpp", fmt_u64_dec(ctx->framebuffer_bpp, buf), DSB_TEXT_PRIMARY);
    info_line("fb_addr", fmt_u64_hex(ctx->framebuffer_address, buf), DSB_TEXT_PRIMARY);

    /* ---- Memory Map ---- */
    info_section("== Memory Map ==");
    info_line("entry_count", fmt_u64_dec(ctx->memory_map_count, buf), DSB_TEXT_PRIMARY);
    info_line("entry_size", fmt_u64_dec(ctx->memory_map_entry_size, buf), DSB_TEXT_PRIMARY);
    info_line("hhdm_offset", fmt_u64_hex(ctx->hhdm_offset, buf), DSB_TEXT_PRIMARY);

    /* ---- Kernel API ---- */
    info_section("== Kernel API ==");
    const dkm_kernel_api *api = g_ac.api;
    if (!api) {
        info_line("kernel_api", "NULL", DSB_ERROR);
    } else {
        info_line("version", fmt_u64_dec(api->version, buf), DSB_TEXT_PRIMARY);
        info_line("feature_bits", fmt_u64_hex(api->feature_bits, buf), DSB_TEXT_PRIMARY);
        info_line("rsdp_addr", fmt_u64_hex((u64)api->rsdp_address, buf), DSB_TEXT_PRIMARY);
        info_ptr("log_api", api->log);
        info_ptr("dma_api", api->dma);
        info_ptr("net_api", api->net);
        info_ptr("block_api", api->block);
        info_ptr("mmio_api", api->mmio);
        info_ptr("irq_register", (const void *)api->irq_register);
        info_ptr("mm_map_mmio", (const void *)api->mm_map_mmio);
        info_ptr("register_apic_eoi", (const void *)api->register_apic_eoi);
        info_ptr("irq_vector_alloc", (const void *)api->irq_vector_alloc);

        /* ---- Block 设备探测 ---- */
        if (api->block && api->block->device_count) {
            info_section("== Block Devices ==");
            u32 dev_count = api->block->device_count();
            info_line("device_count", fmt_u64_dec(dev_count, buf), DSB_TEXT_PRIMARY);
            for (u32 i = 0; i < dev_count && i < 4; i++) {
                char label[8];
                label[0] = '['; label[1] = '0' + (char)i; label[2] = ']'; label[3] = 0;
                const char *name = api->block->device_name ? api->block->device_name(i) : "?";
                u64 sz = api->block->sector_size ? api->block->sector_size(i) : 0;
                /* 显示: "name  sector_size" */
                info_line(label, name ? name : "?", DSB_ACCENT_LIGHT);
                char sz_label[16];
                sz_label[0] = ' '; sz_label[1] = 's'; sz_label[2] = 'z'; sz_label[3] = '=';
                /* 拼接 size 到 sz_label... 简化: 单独行显示 */
                info_line("  sector_size", fmt_u64_dec(sz, buf), DSB_TEXT_DIM);
            }
        }

        /* ---- Net 设备探测 ---- */
        if (api->net && api->net->device_count) {
            info_section("== Net Devices ==");
            u32 dev_count = api->net->device_count();
            info_line("device_count", fmt_u64_dec(dev_count, buf), DSB_TEXT_PRIMARY);
        }
    }

    /* ---- SDK Block 便捷层 ---- */
    info_section("== SDK Block Layer ==");
    info_ptr("block.read", (const void *)g_ac.block.read);
    info_ptr("block.write", (const void *)g_ac.block.write);

    /* 等待 Esc 退出 */
    for (;;) {
        u8 st = dsb_inb(0x64);
        if (st & 1) {
            u8 sc = dsb_inb(0x60);
            if (sc == 1) break;  /* Esc make code */
        }
        __asm__ volatile("pause");
    }
}
