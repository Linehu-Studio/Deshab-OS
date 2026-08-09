/* hello-app - Deshab SDK 最小应用示例
 *
 * 展示完整的应用入口契约：校验 magic -> 初始化上下文 -> 绘制 UI -> 等待退出。
 * 构建产物：SYSTEM/system/deshab64/tools/hello.elf
 */
#include "../../../firstInit/ascii_bitmaps.c"
#include "deshab/deshab.h"
#include "deshab/font.h"

static dsb_app_context g_ac;

void dsk_entry(const dsk_boot_context *ctx) {
    /* 1. 校验启动上下文 magic */
    if (ctx->magic != DSK_BOOT_MAGIC) return;

    /* 2. 初始化应用上下文（framebuffer + block + kernel_api） */
    dsb_app_init(&g_ac, ctx);

    /* 3. 清屏 */
    dsb_fill_bg(&g_ac, DSB_BG_PRIMARY);

    /* 4. 标题栏 + 状态栏 */
    dsb_draw_titlebar(&g_ac, "HelloApp", (i64)g_ac.fb_w);
    dsb_draw_statusbar(&g_ac, "Esc to exit", (i64)g_ac.fb_w, (i64)g_ac.fb_h);

    /* 5. 居中显示欢迎文字 */
    const char *msg = "Hello, Deshab!";
    i64 msg_w = (i64)dsb_strlen(msg) * 12;
    i64 x = ((i64)g_ac.fb_w - msg_w) / 2;
    i64 y = ((i64)g_ac.fb_h) / 2;
    dsb_draw_string(&g_ac, msg, x, y, DSB_TEXT_PRIMARY, DSB_BG_PRIMARY, 12);

    /* 6. 等待 Esc 键退出 */
    for (;;) {
        u8 st = dsb_inb(0x64);
        if (st & 1) {
            u8 sc = dsb_inb(0x60);
            if (sc == 1) break;  /* Esc make code */
        }
        __asm__ volatile("pause");
    }
}
