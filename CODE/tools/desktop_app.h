/* desktop_app.h — Deshab 独立工具应用公共头文件
 *
 * 独立工具应用运行在 DSK 加载的 .elf 中，全屏独占 framebuffer。
 * 每个 .elf 实现 dsk_entry(dsk_boot_context *ctx) 入口点。
 * 通过 kernel_api 访问块设备、网络等内核服务。
 * PS/2 键盘/鼠标通过端口 I/O 直接轮询。
 */

#ifndef DESHAB_DESKTOP_APP_H
#define DESHAB_DESKTOP_APP_H

/* 基础类型 */
typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;
typedef long long          i64;

/* 端口 I/O（宿主已自带 outb/inb 时，包含前 #define DA_NO_PORTIO 跳过本段） */
#ifndef DA_NO_PORTIO
static inline void outb(u16 port, u8 val) { __asm__ volatile("outb %0,%1"::"a"(val),"Nd"(port)); }
static inline u8 inb(u16 port) { u8 v; __asm__ volatile("inb %1,%0":"=a"(v):"Nd"(port)); return v; }
#endif

/* COM1 串口日志 */
#define DA_COM1 0x3F8
static inline void da_sputc(char c) { for(unsigned i=0;i<100000;i++){if(inb(DA_COM1+5)&0x20)break;} outb(DA_COM1,(u8)c); }
static inline void da_swrite(const char *s) { while(*s){if(*s=='\n')da_sputc('\r');da_sputc(*s++);} }
static inline void da_slog(const char *tag, const char *msg) { da_swrite("["); da_swrite(tag); da_swrite("] "); da_swrite(msg); da_swrite("\n"); }

/* dsk_boot_context 定义（与 dsk.h 一致，自包含避免依赖） */
#define DA_BOOT_MAGIC 0x44534B31424F4F54ULL
typedef struct {
    u64 magic; u32 abi_version; u32 size; u64 flags;
    u64 hhdm_offset; u64 rsdp_address;
    u64 framebuffer_address; u64 framebuffer_width; u64 framebuffer_height;
    u64 framebuffer_pitch; u32 framebuffer_bpp; u32 reserved_fb;
    u64 boot_modules_response;
    u64 dkm_kernel_api; u64 dkm_driver_table; u64 dkm_driver_count;
    u64 utsm_state; u64 drr_state;
    u64 memory_map; u64 memory_map_count; u64 memory_map_entry_size;
    u64 kernel_stack_top; u64 reserved[8];
} da_boot_context;

/* block 设备函数类型（与 shell/fat32_io.h 一致：u32 index 固定 0） */
typedef int (*da_block_read_fn)(u32 index, u64 lba, u32 count, void *buffer);
typedef int (*da_block_write_fn)(u32 index, u64 lba, u32 count, const void *buffer);

/* 应用上下文 — 从 dsk_boot_context 提取的常用数据 */
typedef struct {
    u32 *fb;           /* framebuffer 基址 */
    u64 fb_w;          /* 宽度 */
    u64 fb_h;          /* 高度 */
    u64 fb_pitch;      /* 行宽(字节) */
    da_block_read_fn  block_read;
    da_block_write_fn block_write;
    const void *kernel_api;
} da_app_context;

/* 从 dsk_boot_context 初始化 da_app_context。
 * block API 从 kernel_api + 0xA8 获取：read @ +0x10，write @ +0x18。 */
static inline void da_init(da_app_context *ac, const da_boot_context *ctx) {
    ac->fb = (u32 *)ctx->framebuffer_address;
    ac->fb_w = ctx->framebuffer_width;
    ac->fb_h = ctx->framebuffer_height;
    ac->fb_pitch = ctx->framebuffer_pitch;
    ac->kernel_api = (const void *)ctx->dkm_kernel_api;
    ac->block_read = 0;
    ac->block_write = 0;
    u64 api = ctx->dkm_kernel_api;
    u64 blk_api = *(u64 *)(api + 0xA8);
    if (blk_api) {
        ac->block_read  = (da_block_read_fn)*(u64 *)(blk_api + 0x10);
        ac->block_write = (da_block_write_fn)*(u64 *)(blk_api + 0x18);
    }
}

/* ========== 帧缓冲绘制 ========== */

static inline void da_pixel(da_app_context *ac, i64 x, i64 y, u32 color) {
    if (x < 0 || (u64)x >= ac->fb_w || y < 0 || (u64)y >= ac->fb_h) return;
    u32 *line = (u32 *)((u8 *)ac->fb + (u64)y * ac->fb_pitch);
    line[x] = color;
}

static inline u32 da_pixel_read(da_app_context *ac, i64 x, i64 y) {
    if (x < 0 || (u64)x >= ac->fb_w || y < 0 || (u64)y >= ac->fb_h) return 0;
    u32 *line = (u32 *)((u8 *)ac->fb + (u64)y * ac->fb_pitch);
    return line[x];
}

static inline u32 da_blend(u32 bg, u32 fg, u32 alpha) {
    u32 na = 256 - alpha;
    u32 r = ((bg & 0xFF) * na + (fg & 0xFF) * alpha) >> 8;
    u32 g = (((bg >> 8) & 0xFF) * na + ((fg >> 8) & 0xFF) * alpha) >> 8;
    u32 b = (((bg >> 16) & 0xFF) * na + ((fg >> 16) & 0xFF) * alpha) >> 8;
    return 0xFF000000u | (b << 16) | (g << 8) | r;
}

static inline void da_fill_rect(da_app_context *ac, i64 x, i64 y, i64 w, i64 h, u32 color) {
    for (i64 r = 0; r < h; r++) {
        i64 yy = y + r;
        if (yy < 0 || (u64)yy >= ac->fb_h) continue;
        u32 *line = (u32 *)((u8 *)ac->fb + (u64)yy * ac->fb_pitch);
        for (i64 c = 0; c < w; c++) {
            i64 xx = x + c;
            if (xx < 0 || (u64)xx >= ac->fb_w) continue;
            line[xx] = color;
        }
    }
}

static inline void da_fill_bg(da_app_context *ac, u32 color) {
    da_fill_rect(ac, 0, 0, (i64)ac->fb_w, (i64)ac->fb_h, color);
}

/* 圆角矩形 */
static inline void da_fill_rounded_rect(da_app_context *ac, i64 x, i64 y, i64 w, i64 h, u32 color, i64 radius) {
    i64 r = radius;
    if (r > w/2) r = w/2;
    if (r > h/2) r = h/2;
    da_fill_rect(ac, x+r, y, w-2*r, h, color);
    da_fill_rect(ac, x, y+r, r, h-2*r, color);
    da_fill_rect(ac, x+w-r, y+r, r, h-2*r, color);
    for (i64 dy = 0; dy < r; dy++)
        for (i64 dx = 0; dx < r; dx++)
            if (dx*dx+dy*dy <= r*r) {
                da_pixel(ac, x+r-1-dx, y+r-1-dy, color);
                da_pixel(ac, x+w-r+dx, y+r-1-dy, color);
                da_pixel(ac, x+r-1-dx, y+h-r+dy, color);
                da_pixel(ac, x+w-r+dx, y+h-r+dy, color);
            }
}

static inline void da_rect_outline(da_app_context *ac, i64 x, i64 y, i64 w, i64 h, u32 color, i64 radius) {
    i64 r = radius;
    if (r > w/2) r = w/2;
    if (r > h/2) r = h/2;
    for (i64 c = r; c < w-r; c++) { da_pixel(ac, x+c, y, color); da_pixel(ac, x+c, y+h-1, color); }
    for (i64 rr = r; rr < h-r; rr++) { da_pixel(ac, x, y+rr, color); da_pixel(ac, x+w-1, y+rr, color); }
    for (i64 dy = 0; dy <= r; dy++)
        for (i64 dx = 0; dx <= r; dx++) {
            i64 d2 = dx*dx+dy*dy;
            if (d2 <= r*r && d2 > (r-1)*(r-1)) {
                da_pixel(ac, x+r-dx, y+r-dy, color);
                da_pixel(ac, x+w-1-r+dx, y+r-dy, color);
                da_pixel(ac, x+r-dx, y+h-1-r+dy, color);
                da_pixel(ac, x+w-1-r+dx, y+h-1-r+dy, color);
            }
        }
}

/* ========== ASCII 字体渲染 ==========
 *
 * 使用方法：先 #include ascii_bitmaps.c（定义 static g_ascii/g_ascii_w/g_ascii_h），
 * 再 #include desktop_app.h。da_draw_char/da_draw_string 直接引用 g_ascii。
 * 与 deshab_ui.h 使用相同的包含顺序约定。
 */

static inline void da_draw_char(da_app_context *ac, char ch, i64 x, i64 y, u32 fg, u32 bg) {
    u32 idx = (u32)(ch - ' ');
    if (idx > 94) idx = 0;
    const u8 *glyph = g_ascii[idx];
    i64 gw = g_ascii_w, gh = g_ascii_h;
    for (i64 r = 0; r < gh; r++) {
        i64 yy = y + r;
        if (yy < 0 || (u64)yy >= ac->fb_h) continue;
        u32 *line = (u32 *)((u8 *)ac->fb + (u64)yy * ac->fb_pitch);
        for (i64 c = 0; c < gw; c++) {
            u8 a = glyph[r * gw + c];
            if (a == 0) continue;
            i64 xx = x + c;
            if (xx < 0 || (u64)xx >= ac->fb_w) continue;
            line[xx] = (a == 255) ? fg : da_blend(bg, fg, a);
        }
    }
}

static inline void da_draw_string(da_app_context *ac, const char *s, i64 x, i64 y, u32 fg, u32 bg, i64 step) {
    i64 cx = x;
    while (*s) { da_draw_char(ac, *s, cx, y, fg, bg); cx += step; s++; }
}

/* ========== 鼠标光标 ========== */
#define DA_CURSOR_SIZE 24

static const u8 da_cursor_shape[24][24] = {
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {1,1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {1,1,0,0,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {1,1,0,0,0,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {1,0,0,0,0,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
};

typedef struct {
    u32 save[DA_CURSOR_SIZE * DA_CURSOR_SIZE];
    int saved;
    int old_x, old_y;
    int mx, my;
    int btn;
} da_cursor;

static inline void da_cursor_init(da_cursor *c, i64 fb_w, i64 fb_h) {
    for (int i = 0; i < DA_CURSOR_SIZE*DA_CURSOR_SIZE; i++) c->save[i] = 0;
    c->saved = 0; c->old_x = -1; c->old_y = -1;
    c->mx = (int)(fb_w / 2); c->my = (int)(fb_h / 2); c->btn = 0;
}

static inline void da_cursor_save(da_app_context *ac, da_cursor *c) {
    for (int r = 0; r < DA_CURSOR_SIZE; r++)
        for (int col = 0; col < DA_CURSOR_SIZE; col++) {
            int x = c->mx + col, y = c->my + r;
            c->save[r * DA_CURSOR_SIZE + col] = da_pixel_read(ac, x, y);
        }
    c->saved = 1; c->old_x = c->mx; c->old_y = c->my;
}

static inline void da_cursor_restore(da_app_context *ac, da_cursor *c) {
    if (!c->saved) return;
    for (int r = 0; r < DA_CURSOR_SIZE; r++)
        for (int col = 0; col < DA_CURSOR_SIZE; col++) {
            int x = c->old_x + col, y = c->old_y + r;
            da_pixel(ac, x, y, c->save[r * DA_CURSOR_SIZE + col]);
        }
    c->saved = 0;
}

static inline void da_cursor_draw(da_app_context *ac, da_cursor *c, u32 color) {
    for (int r = 0; r < DA_CURSOR_SIZE; r++)
        for (int col = 0; col < DA_CURSOR_SIZE; col++)
            if (da_cursor_shape[r][col])
                da_pixel(ac, c->mx + col, c->my + r, color);
}

/* ========== PS/2 鼠标 ========== */
typedef struct {
    u8 buf[3];
    int idx;
    int has_pkt;
} da_mouse;

/* 冲刷 PS/2 控制器输出缓冲（最多 16 字节） */
static inline void da_ps2_drain(void) {
    for (int i = 0; i < 16; i++) {
        if (!(inb(0x64) & 1)) break;
        inb(0x60);
    }
}

static inline void da_mouse_init(void) {
    /* 必须先冲刷输出缓冲再发 0x20 读配置字：shell 启动本应用时最后键入的
     * 回车 break code(0x9C) 或鼠标流字节若残留在输出缓冲，会被误读为"配置字"
     * 并写回控制器 —— 残留字节 bit4(0x10)=1 会置位 KBD 时钟禁用位，键盘被
     * 控制器级禁用且应用退出后仍不恢复（BUG-20260801-008）。
     * 复位/使能后的响应字节也必须全部读完（0xFF 返回 FA AA 00 共 3 字节），
     * 否则残留字节同样会污染后续配置字读写。
     * 序列与 desktop/main.c:ps2_mouse_init 保持一致。 */
    da_ps2_drain();
    for (int t = 0; t < 100000; t++) { if (!(inb(0x64) & 2)) break; }
    outb(0x64, 0xA8);
    for (int t = 0; t < 100000; t++) { if (!(inb(0x64) & 2)) break; }
    outb(0x64, 0x20);
    for (int t = 0; t < 100000; t++) { if (inb(0x64) & 1) break; }
    u8 cfg = inb(0x60);
    cfg &= ~0x20; cfg |= 0x02; cfg |= 0x40; /* AUX 时钟启用 + AUX IRQ + 键盘翻译(集1, 与 desktop 一致) */
    for (int t = 0; t < 100000; t++) { if (!(inb(0x64) & 2)) break; }
    outb(0x64, 0x60);
    for (int t = 0; t < 100000; t++) { if (!(inb(0x64) & 2)) break; }
    outb(0x60, cfg);
    for (int t = 0; t < 100000; t++) { if (!(inb(0x64) & 2)) break; }
    outb(0x64, 0xD4);
    for (int t = 0; t < 100000; t++) { if (!(inb(0x64) & 2)) break; }
    outb(0x60, 0xFF);
    for (int i = 0; i < 8; i++) {           /* 读完复位全部响应(FA AA 00) */
        int got = 0;
        for (int t = 0; t < 100000; t++) { if (inb(0x64) & 1) { got = 1; break; } }
        if (!got) break;
        inb(0x60);
    }
    for (int t = 0; t < 100000; t++) { if (!(inb(0x64) & 2)) break; }
    outb(0x64, 0xD4);
    for (int t = 0; t < 100000; t++) { if (!(inb(0x64) & 2)) break; }
    outb(0x60, 0xF4);
    for (int i = 0; i < 4; i++) {           /* 读完使能 ACK */
        int got = 0;
        for (int t = 0; t < 100000; t++) { if (inb(0x64) & 1) { got = 1; break; } }
        if (!got) break;
        inb(0x60);
    }
    da_ps2_drain();
}

static inline int da_mouse_poll(da_mouse *m, da_cursor *c, i64 fb_w, i64 fb_h) {
    u8 st = inb(0x64);
    if (!(st & 1)) return 0;
    /* 必须先查 AUX 位(bit5)再读数据端口：键盘字节(st&0x20)==0 属于键盘路径，
     * 此处若先 inb(0x60) 会把键盘扫描码吞掉（BUG-20260801-003）。
     * 与 desktop/main.c:ps2_mouse_poll 顺序保持一致。 */
    if (!(st & 0x20)) return 0;
    u8 data = inb(0x60);
    m->buf[m->idx++] = data;
    if (m->idx < 3) return 0;
    m->idx = 0;
    if (!(m->buf[0] & 0x08)) return 0;
    /* PS/2 鼠标包：buf[1]/buf[2] 已是 9 位补码的低 8 位，
     * (signed char) 强转即可正确符号扩展，无需再依据 buf[0] 的 sign bit 反转。
     * X 直接相加；Y 需翻转一次（PS/2 Y 向上为正，屏幕 Y 向下为正）。
     * 与 desktop/main.c:ps2_mouse_poll 保持一致。 */
    int dx = (int)(signed char)m->buf[1];
    int dy = (int)(signed char)m->buf[2];
    dy = -dy;
    c->mx += dx; c->my += dy;
    if (c->mx < 0) c->mx = 0;
    if (c->my < 0) c->my = 0;
    if (c->mx >= (int)fb_w - DA_CURSOR_SIZE) c->mx = (int)fb_w - DA_CURSOR_SIZE;
    if (c->my >= (int)fb_h - DA_CURSOR_SIZE) c->my = (int)fb_h - DA_CURSOR_SIZE;
    c->btn = (m->buf[0] & 0x01) ? 1 : 0;
    m->has_pkt = 1;
    return 1;
}

/* ========== 键盘 ========== */
static inline char da_scan_to_ascii(u8 sc, int shift) {
    static const char normal[58] = {
        0, 27, '1','2','3','4','5','6','7','8','9','0','-','=', 8, '\t',
        'q','w','e','r','t','y','u','i','o','p','[',']','\n',0,
        'a','s','d','f','g','h','j','k','l',';','\'', '`',0,'\\',
        'z','x','c','v','b','n','m',',','.','/',0,'*',0,' '
    };
    static const char shifted[58] = {
        0, 27, '!','@','#','$','%','^','&','*','(',')','_','+', 8, '\t',
        'Q','W','E','R','T','Y','U','I','O','P','{','}','\n',0,
        'A','S','D','F','G','H','J','K','L',':','"','~',0,'|',
        'Z','X','C','V','B','N','M','<','>','?',0,'*',0,' '
    };
    if (sc >= 58) return 0;
    return shift ? shifted[sc] : normal[sc];
}

/* ========== 窗口概念 ========== */
/* 独立工具全屏运行时的窗口装饰：标题栏 + 关闭按钮 */

#define DA_TITLEBAR_H 28
#define DA_BORDER_W   2
#define DA_STATUSBAR_H 20

/* 色板引用（与 deshab_ui.h 一致） */
#define DA_BG_PRIMARY    0xFF0A1428u   /* DP_ABYSS_900 */
#define DA_BG_SECONDARY  0xFF0F1E38u   /* DP_ABYSS_800 */
#define DA_BG_TERTIARY   0xFF162848u   /* DP_ABYSS_700 */
#define DA_TEXT_PRIMARY   0xFFE8E8F0u   /* DP_NEUTRAL_700 */
#define DA_TEXT_DIM       0xFF6A6A88u   /* DP_NEUTRAL_300 */
#define DA_ACCENT         0xFF00A8CCu   /* DP_SEAL_500 */
#define DA_ACCENT_LIGHT   0xFF44CCF0u   /* DP_SEAL_300 */
#define DA_BORDER         0xFF284888u   /* DP_ABYSS_500 */
#define DA_BORDER_FOCUS   0xFF44CCF0u   /* DP_SEAL_300 */
#define DA_CURSOR_COLOR   0xFF00A8CCu   /* DP_SEAL_500 */
#define DA_PROMPT_COLOR   0xFF44CCF0u   /* DP_SEAL_300 */
#define DA_SUCCESS        0xFF40C880u   /* DP_SUCCESS */
#define DA_ERROR          0xFFFF4466u   /* DP_ERROR */
#define DA_WARNING        0xFFF0A030u   /* DP_WARNING */

static inline void da_draw_titlebar(da_app_context *ac, const char *title, i64 w) {
    da_fill_rect(ac, 0, 0, w, DA_TITLEBAR_H, DA_BG_SECONDARY);
    da_draw_string(ac, title, 8, (DA_TITLEBAR_H - 18) / 2, DA_TEXT_PRIMARY, DA_BG_SECONDARY, 12);
    /* 关闭按钮 */
    da_fill_rounded_rect(ac, w - 28, 4, 24, 20, DA_ERROR, 4);
    da_draw_string(ac, "X", w - 20, 5, 0xFF0A1428u, DA_ERROR, 12);
    /* 底部分隔线 */
    da_fill_rect(ac, 0, DA_TITLEBAR_H, w, 1, DA_ACCENT);
}

static inline void da_draw_statusbar(da_app_context *ac, const char *text, i64 w, i64 h) {
    i64 y = h - DA_STATUSBAR_H;
    da_fill_rect(ac, 0, y, w, DA_STATUSBAR_H, DA_BG_SECONDARY);
    da_draw_string(ac, text, 4, y + 1, DA_TEXT_DIM, DA_BG_SECONDARY, 12);
    /* 顶部分隔线 */
    da_fill_rect(ac, 0, y, w, 1, DA_BORDER);
}

/* ========== RTC 时钟 ========== */
static inline void da_rtc_time(u8 *h, u8 *m) {
    outb(0x70, 4); *h = inb(0x71);
    outb(0x70, 2); *m = inb(0x71);
    *h = (u8)((*h >> 4) * 10 + (*h & 0xF));
    *m = (u8)((*m >> 4) * 10 + (*m & 0xF));
}

/* ========== memset/memcpy ========== */
static inline void *da_memset(void *d, int c, u64 n) { u8 *p=(u8*)d; while(n--)*p++=(u8)c; return d; }
static inline void *da_memcpy(void *d, const void *s, u64 n) { u8 *dd=(u8*)d; const u8 *ss=(const u8*)s; while(n--)*dd++=*ss++; return d; }

/* ========== Linux 桌面应用配置契约（Phase 5） ==========
 *
 * desktop.elf 启动时（Linux 兼容层服务可用时）从 FAT32 根目录读取
 * LINUXAPP.CNF（8.3 名 LINUXAPPCNF），每行注册一个 Linux 应用图标：
 *
 *   显示名|linux命令
 *
 * 示例：
 *   NEOFETCH|/usr/bin/neofetch
 *   HTOP|/usr/bin/htop
 *   LS|/bin/ls -la /
 *
 * 规则：
 *   - 空行与 # 开头的行被忽略；显示名与命令两侧空白被修剪
 *   - 显示名 <= 11 字符（桌面图标 label），命令 <= 63 字符
 *   - 命令按空格分词：第一个 token 为程序路径（建议绝对路径；
 *     裸命令由调用方补 /bin/ 前缀），其余 token 作为 argv 参数，
 *     不支持引号转义
 *   - 文件不存在或无有效行时，调用方回退到内置默认列表
 */

#define DA_LINUXAPP_CNF_83   "LINUXAPPCNF"  /* FAT32 8.3 名（即 LINUXAPP.CNF） */
#define DA_LINUXAPP_MAX      8              /* Linux 应用注册上限 */
#define DA_LINUXAPP_NAME_CAP 12             /* 显示名缓冲（含 NUL） */
#define DA_LINUXAPP_CMD_CAP  64             /* 命令行缓冲（含 NUL） */

typedef struct {
    char name[DA_LINUXAPP_NAME_CAP];
    char cmd[DA_LINUXAPP_CMD_CAP];
} da_linux_app;

/* 解析 LINUXAPP.CNF 文本（允许非 NUL 结尾，按 size 截断）。
 * 返回有效条目数（<= max）。 */
static inline int da_linuxapp_parse(const char *text, u32 size, da_linux_app *out, int max) {
    int count = 0;
    u32 pos = 0;
    while (pos < size && count < max) {
        /* 取出一行（吞掉 \r，超长部分截断） */
        char line[96];
        int ll = 0;
        while (pos < size && text[pos] != '\n') {
            char c = text[pos++];
            if (c == '\r') continue;
            if (ll < 95) line[ll++] = c;
        }
        if (pos < size && text[pos] == '\n') pos++;
        line[ll] = 0;

        /* 行尾空白修剪 */
        while (ll > 0 && (line[ll-1] == ' ' || line[ll-1] == '\t')) line[--ll] = 0;

        /* 跳过空行与注释行 */
        int s = 0;
        while (line[s] == ' ' || line[s] == '\t') s++;
        if (line[s] == 0 || line[s] == '#') continue;

        /* 分隔符 '|' */
        int bar = s;
        while (line[bar] && line[bar] != '|') bar++;
        if (line[bar] != '|') continue;

        /* 显示名 [s, bar) 去尾空白 */
        int ne = bar;
        while (ne > s && (line[ne-1] == ' ' || line[ne-1] == '\t')) ne--;
        if (ne <= s) continue;

        /* 命令 (bar, end) 去头空白 */
        int cs = bar + 1;
        while (line[cs] == ' ' || line[cs] == '\t') cs++;
        if (line[cs] == 0) continue;

        int ni = 0;
        for (int k = s; k < ne && ni < DA_LINUXAPP_NAME_CAP - 1; k++)
            out[count].name[ni++] = line[k];
        out[count].name[ni] = 0;

        int ci = 0;
        for (int k = cs; line[k] && ci < DA_LINUXAPP_CMD_CAP - 1; k++)
            out[count].cmd[ci++] = line[k];
        out[count].cmd[ci] = 0;

        count++;
    }
    return count;
}

#endif /* DESHAB_DESKTOP_APP_H */
