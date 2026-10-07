/* login.elf — Deshab login screen
 * Shown on non-first boot (firstInit=1) before loading desktop.
 * Reads USER.CONF via boot context (ctx->reserved[0/1]),
 * extracts username + password hash, prompts for password,
 * verifies via SHA256 comparison, sets ctx->reserved[2]=1 on success.
 *
 * 防篡改：对 .text 段做 SHA256 自校验，预期哈希由构建后补丁脚本
 * 写入 .hashseg 段（__expected_hash）。运行时若发现哈希不匹配，
 * 拒绝继续并触发串口告警。开发期哈希为全 0 时跳过校验。
 *
 * Returns to DSK in all cases (success, skip via Esc, or fatal error).
 * DSK loads desktop.elf after this returns.
 */

#include "../UTSM/include/utsm/dsk.h"

/* 防篡改：linker 导出的 .text 段范围与预期哈希存储位置。
 * hidden 可见性强制 PIE 下用 lea rip 取址（基址无关），禁止走 GOT——
 * GOT slot 若被 DSK 侧后续 IO 破坏（BUG：ibuf+0x1EAE10 被清零），
 * len 将计算成天文数字导致 integrity 误报 LOGIN-E01。 */
extern const u8 __text_start[] __attribute__((visibility("hidden")));
extern const u8 __text_end[] __attribute__((visibility("hidden")));
extern const u8 __expected_hash[] __attribute__((visibility("hidden")));
extern const u8 __expected_hash_end[] __attribute__((visibility("hidden")));

typedef signed char        i8;
typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;
typedef long long          i64;

#define COM1 0x3F8

static __inline__ void outb(u16 port, u8 value) { __asm__ volatile("outb %0,%1"::"a"(value),"Nd"(port)); }
static __inline__ u8 inb(u16 port) { u8 v; __asm__ volatile("inb %1,%0":"=a"(v):"Nd"(port)); return v; }

/* ---- TSC-based timing (PIT-calibrated) ---- */
static u64 g_tsc_per_ms = 0;

static __inline__ u64 rdtsc_fi(void) {
    u32 lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((u64)hi << 32) | lo;
}

static void tsc_calibrate_fi(void) {
    outb(0x43, 0x30);       /* ch0, lo+hi, mode 0, binary */
    outb(0x40, 0x7c);       /* 11932 low = ~10ms */
    outb(0x40, 0x2e);       /* 11932 high */
    u64 tsc_start = rdtsc_fi();
    u16 prev = 0; u64 loops = 0;
    for (;;) {
        outb(0x43, 0x00);
        u16 cur = (u16)inb(0x40) | ((u16)inb(0x40) << 8);
        if (cur > prev && loops > 10) break;
        prev = cur; loops++;
    }
    u64 tsc_end = rdtsc_fi();
    g_tsc_per_ms = (tsc_end - tsc_start) / 10;
}

static void delay_ms_fi(u32 ms) {
    if (!g_tsc_per_ms) {
        for (volatile u32 i = 0; i < 100000 * ms; i++) __asm__ volatile("pause");
        return;
    }
    u64 target = g_tsc_per_ms * ms;
    u64 start = rdtsc_fi();
    while (rdtsc_fi() - start < target) __asm__ volatile("pause");
}

static void delay_frame(void) { delay_ms_fi(33); }

/* ---- Serial logging (COM1) ---- */
static void sputc(char c) {
    for (unsigned int i=0; i<100000; i++) { if (inb(COM1+5)&0x20) break; }
    outb(COM1, (unsigned char)c);
}
static void swrite(const char *s) { while(*s) { if(*s=='\n')sputc('\r'); sputc(*s++); } }
static void logl(const char *s) { swrite(s); swrite("\n"); }

/* ---- Colors (dark theme matching FirstInit) ---- */
#define BG_TOP    0xFF2D2D30u
#define BG_BOTTOM 0xFF1A1A1Eu
#define CARD_BG   0xFF353539u
#define INPUT_BG  0xFF2A2A2Eu
#define TEXT_FG   0xFFE0E0E0u
#define ACCENT    0xFF5A8AC8u
#define ERROR_FG  0xFFE05050u
#define DIM_FG    0xFF808088u

static u64 fb_a, fb_w, fb_h, fb_p;

static u32 blend(u32 c1, u32 c2, u32 a) {
    u32 na=256-a;
    u32 r=((c1&0xFF)*na+(c2&0xFF)*a)>>8;
    u32 g=(((c1>>8)&0xFF)*na+((c2>>8)&0xFF)*a)>>8;
    u32 b=(((c1>>16)&0xFF)*na+((c2>>16)&0xFF)*a)>>8;
    return 0xFF000000|(b<<16)|(g<<8)|r;
}

/* ---- ASCII font (Consolas 18px grayscale) — shared from firstInit ---- */
#include "../firstInit/ascii_bitmaps.c"
#define ASCII_W  11
#define ASCII_H  18
#define ASCII_STEP 11

static void fb_char(u32 *fb, u32 ch, i64 x, i64 y, u32 fg, u32 bg) {
    if (ch < ' ' || ch > '~') ch = ' ';
    u32 idx = (u32)(ch - ' ');
    const u8 *g = g_ascii[idx];
    for (i64 r=0; r<ASCII_H; r++) {
        i64 yy = y + r;
        if (yy < 0 || (u64)yy >= fb_h) continue;
        u32 *line = (u32 *)((u8 *)fb + (u64)yy * fb_p);
        for (i64 c=0; c<ASCII_W; c++) {
            u32 a = g[r * ASCII_W + c];
            if (a == 0) continue;
            i64 xx = x + c;
            if (xx < 0 || (u64)xx >= fb_w) continue;
            line[(u64)xx] = (a == 255) ? fg : blend(bg, fg, a);
        }
    }
}

static void fb_text(u32 *fb, const char *s, i64 x, i64 y, u32 fg, u32 bg) {
    for (i64 i=0; s[i]; i++) fb_char(fb, (u32)(u8)s[i], x+i*ASCII_STEP, y, fg, bg);
}

static void fill_rect(u32 *fb, i64 x, i64 y, i64 w, i64 h, u32 color);
static void fill_gradient_rect(u32 *fb, i64 x, i64 y, i64 w, i64 h);
static u64 lf_strlen(const char *s) { u64 n = 0; while (s[n]) n++; return n; }

/* ===================================================================
 *  严格错误策略插桩：任何错误（含未实现路径）不再降级、不再静默跳过，
 *  必须整屏 fatal 显示错误码后停机。"疯但好使" 的反面保障：
 *  错也错得有落款。每个插桩点一个唯一 symbol（LOGIN-E<nn>）。
 * =================================================================== */
static void login_fatal(const char *sym) {
    logl(sym);
    if (fb_a) {
        u32 *fb = (u32 *)(u64)fb_a;
        fill_rect(fb, 0, 0, (i64)fb_w, (i64)fb_h, 0xFF3A0E14u);
        i64 cx = ((i64)fb_w - (i64)lf_strlen(sym) * ASCII_STEP) / 2;
        fb_text(fb, sym, cx, (i64)fb_h / 2 - 40, 0xFFF2D8D8u, 0);
        fb_text(fb, "DESHAB LOGIN FATAL - SYSTEM HALTED",
                ((i64)fb_w - 36 * ASCII_STEP) / 2, (i64)fb_h / 2, 0xFFB08890u, 0);
    }
    for (;;) __asm__("cli; hlt");
}

/* ohMyLogo 数据保留（panic/其它界面可复用），登录界面不再显示 logo 开场，
 * logo_intro_screen/draw_logo_rgba 已随需求移除 */
#include "ohmylogo_data.c"

/* 模糊化的桌面壁纸（back.rgba 构建期盒模糊 x3 @1/4 分辨率），登录卡片背景 */
#include "back_blur_data.c"

/* 把模糊壁纸最近邻拉伸铺满整个 framebuffer */
static void draw_blur_wallpaper(u32 *fb) {
    for (u64 y = 0; y < fb_h; y++) {
        u32 *line = (u32 *)((u8 *)fb + y * fb_p);
        u64 sy = y * WALLPAPER_BLUR_H / fb_h;
        const unsigned char *srow = g_wall_blur_rgba + sy * WALLPAPER_BLUR_W * 4;
        for (u64 x = 0; x < fb_w; x++) {
            u64 sx = x * WALLPAPER_BLUR_W / fb_w;
            const unsigned char *px = srow + sx * 4;
            line[x] = 0xFF000000u | ((u32)px[0] << 16) | ((u32)px[1] << 8) | px[2];
        }
    }
}

/* 从模糊壁纸重采样一小块（spinner 每帧清除残迹用） */
static void restore_wallpatch(u32 *fb, i64 cx, i64 cy, i64 r) {
    for (i64 y = cy - r; y <= cy + r; y++) {
        if (y < 0 || (u64)y >= fb_h) continue;
        u32 *line = (u32 *)((u8 *)fb + (u64)y * fb_p);
        u64 sy = (u64)y * WALLPAPER_BLUR_H / fb_h;
        const unsigned char *srow = g_wall_blur_rgba + sy * WALLPAPER_BLUR_W * 4;
        for (i64 x = cx - r; x <= cx + r; x++) {
            if (x < 0 || (u64)x >= fb_w) continue;
            u64 sx = (u64)x * WALLPAPER_BLUR_W / fb_w;
            const unsigned char *px = srow + sx * 4;
            line[x] = 0xFF000000u | ((u32)px[0] << 16) | ((u32)px[1] << 8) | px[2];
        }
    }
}

/* 登录成功后的加载圆圈：卡片下方 8 段拖尾 spinner（16 分圆整数查表，禁 SSE） */
static const i32 SIN16[17] = {0,383,707,924,1000,924,707,383,0,-383,-707,-924,-1000,-924,-707,-383,0};
static void draw_spinner(u32 *fb, i64 cx, i64 cy, i64 frame) {
    const i64 R = 22;
    for (int seg = 0; seg < 8; seg++) {
        int idx = (int)((frame + seg) & 15);
        u32 alpha = 255 - seg * 28;
        u32 color = 0xFF000000u
                  | (u32)(0xE0 * alpha / 255) << 16
                  | (u32)(0xE0 * alpha / 255) << 8
                  | (u32)(0xE0 * alpha / 255);
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++) {
                i64 px = cx + SIN16[(idx + 4) & 15] * R / 1000 + dx;
                i64 py = cy + SIN16[idx] * R / 1000 + dy;
                if (px < 0 || (u64)px >= fb_w || py < 0 || (u64)py >= fb_h) continue;
                *(u32 *)((u8 *)fb + (u64)py * fb_p + (u64)px * 4) = color;
            }
    }
}

static void fill_rect(u32 *fb, i64 x, i64 y, i64 w, i64 h, u32 color) {
    for (i64 r=0; r<h; r++) {
        i64 yy = y + r;
        if (yy < 0 || (u64)yy >= fb_h) continue;
        u32 *line = (u32 *)((u8 *)fb + (u64)yy * fb_p);
        for (i64 c=0; c<w; c++) {
            i64 xx = x + c;
            if (xx < 0 || (u64)xx >= fb_w) continue;
            line[(u64)xx] = color;
        }
    }
}

static u32 bg_at_y(i64 y) {
    if (fb_h <= 1) return BG_TOP;
    if (y < 0) y = 0;
    if ((u64)y >= fb_h) y = (i64)fb_h - 1;
    return blend(BG_TOP, BG_BOTTOM, (u32)(((u64)y * 255ULL) / (fb_h - 1)));
}

static void fill_gradient_rect(u32 *fb, i64 x, i64 y, i64 w, i64 h) {
    for (i64 r=0; r<h; r++) {
        i64 yy = y + r;
        if (yy < 0 || (u64)yy >= fb_h) continue;
        u32 color = bg_at_y(yy);
        u32 *line = (u32 *)((u8 *)fb + (u64)yy * fb_p);
        for (i64 c=0; c<w; c++) {
            i64 xx = x + c;
            if (xx < 0 || (u64)xx >= fb_w) continue;
            line[(u64)xx] = color;
        }
    }
}

static int rounded_rect_contains(i64 px, i64 py, i64 x, i64 y, i64 w, i64 h, i64 radius) {
    if (px < x || py < y || px >= x + w || py >= y + h) return 0;
    if (radius <= 0) return 1;
    if (radius * 2 > w) radius = w / 2;
    if (radius * 2 > h) radius = h / 2;
    i64 left = x + radius;
    i64 right = x + w - radius - 1;
    i64 top = y + radius;
    i64 bottom = y + h - radius - 1;
    if (px >= left && px <= right) return 1;
    if (py >= top && py <= bottom) return 1;
    i64 cx = px < left ? left : right;
    i64 cy = py < top ? top : bottom;
    i64 dx = px - cx;
    i64 dy = py - cy;
    return dx * dx + dy * dy <= radius * radius;
}

static void fill_rounded_rect(u32 *fb, i64 x, i64 y, i64 w, i64 h, i64 radius, u32 color) {
    if (w <= 0 || h <= 0) return;
    for (i64 r=0; r<h; r++) {
        i64 yy = y + r;
        if (yy < 0 || (u64)yy >= fb_h) continue;
        u32 *line = (u32 *)((u8 *)fb + (u64)yy * fb_p);
        for (i64 c=0; c<w; c++) {
            i64 xx = x + c;
            if (xx < 0 || (u64)xx >= fb_w) continue;
            if (rounded_rect_contains(xx, yy, x, y, w, h, radius)) line[(u64)xx] = color;
        }
    }
}

static void stroke_rounded_rect(u32 *fb, i64 x, i64 y, i64 w, i64 h, i64 radius, i64 thickness, u32 color) {
    if (w <= 0 || h <= 0 || thickness <= 0) return;
    for (i64 r=0; r<h; r++) {
        i64 yy = y + r;
        if (yy < 0 || (u64)yy >= fb_h) continue;
        u32 *line = (u32 *)((u8 *)fb + (u64)yy * fb_p);
        for (i64 c=0; c<w; c++) {
            i64 xx = x + c;
            if (xx < 0 || (u64)xx >= fb_w) continue;
            int outer = rounded_rect_contains(xx, yy, x, y, w, h, radius);
            int inner = rounded_rect_contains(xx, yy, x + thickness, y + thickness, w - thickness * 2, h - thickness * 2, radius - thickness);
            if (outer && !inner) line[(u64)xx] = color;
        }
    }
}

static void draw_card(u32 *fb, i64 x, i64 y, i64 w, i64 h, u32 card) {
    fill_rounded_rect(fb, x, y, w, h, 4, card);
}

static void draw_rounded_input(u32 *fb, i64 x, i64 y, i64 w, i64 h, const char *value, int mask, u32 fg, u32 card_bg, int active) {
    (void)card_bg;
    u32 fill = active ? 0xFF45454Au : INPUT_BG;
    fill_rounded_rect(fb, x, y, w, h, 2, fill);
    stroke_rounded_rect(fb, x, y, w, h, 2, 1, active ? 0xFF6C6C72 : 0xFF404044);
    char out[80]; int i=0;
    while(value[i] && i<76) { out[i] = mask ? '*' : value[i]; i++; }
    out[i]=0;
    if (out[0]) fb_text(fb, out, x+14, y+(h-ASCII_H)/2, fg, fill);
    if (active) {
        i64 cx2 = x + 14 + i * ASCII_STEP;
        fill_rect(fb, cx2, y+10, 2, h-20, fg);
    }
}

/* ---- PS/2 鼠标光标 ---- */
#define CURSZ 16
static int g_mouse_x = 400, g_mouse_y = 300;
static int g_mouse_btn = 0;
static u32 g_cursor_bg[CURSZ * CURSZ];
static int g_cursor_saved = 0;
static int g_cursor_old_x = -1, g_cursor_old_y = -1;
static u8 g_mouse_buf[3];
static int g_mouse_idx = 0;

/* 16×16 箭头光标（与 desktop 一致） */
static const u8 cursor_shape[16][16] = {
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0},
    {1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0},
    {1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0},
    {1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0},
    {1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0},
    {1,1,1,1,1,1,1,1,1,0,0,0,0,0,0,0},
    {1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0},
    {1,1,1,1,1,0,1,1,0,0,0,0,0,0,0,0},
    {1,1,1,0,0,0,1,1,0,0,0,0,0,0,0,0},
    {1,1,0,0,0,0,0,1,1,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,1,1,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
};

static void cursor_save_bg(u32 *fb, int mx, int my) {
    for (int r = 0; r < CURSZ; r++) {
        for (int c = 0; c < CURSZ; c++) {
            int x = mx + c, y = my + r;
            if (x >= 0 && (u64)x < fb_w && y >= 0 && (u64)y < fb_h) {
                u32 *line = (u32 *)((u8 *)fb + (u64)y * fb_p);
                g_cursor_bg[r * CURSZ + c] = line[x];
            } else {
                g_cursor_bg[r * CURSZ + c] = 0;
            }
        }
    }
    g_cursor_saved = 1;
    g_cursor_old_x = mx;
    g_cursor_old_y = my;
}

static void cursor_restore_bg(u32 *fb) {
    if (!g_cursor_saved) return;
    for (int r = 0; r < CURSZ; r++) {
        for (int c = 0; c < CURSZ; c++) {
            int x = g_cursor_old_x + c, y = g_cursor_old_y + r;
            if (x >= 0 && (u64)x < fb_w && y >= 0 && (u64)y < fb_h) {
                u32 *line = (u32 *)((u8 *)fb + (u64)y * fb_p);
                line[x] = g_cursor_bg[r * CURSZ + c];
            }
        }
    }
    g_cursor_saved = 0;
}

static void cursor_draw(u32 *fb, int mx, int my) {
    u32 fg = 0xFFFFFFFF;   /* 白色指针 */
    /* 黑色描边：空心像素且 8 邻域有实心 */
    for (int r = 0; r < CURSZ; r++) {
        for (int c = 0; c < CURSZ; c++) {
            if (cursor_shape[r][c]) continue;
            int near = 0;
            for (int dy = -1; dy <= 1 && !near; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    int rr = r + dy, cc = c + dx;
                    if (rr < 0 || rr >= CURSZ || cc < 0 || cc >= CURSZ) continue;
                    if (cursor_shape[rr][cc]) { near = 1; break; }
                }
            }
            if (near) {
                int x = mx + c, y = my + r;
                if (x >= 0 && (u64)x < fb_w && y >= 0 && (u64)y < fb_h) {
                    u32 *line = (u32 *)((u8 *)fb + (u64)y * fb_p);
                    line[x] = 0xFF000000u;
                }
            }
        }
    }
    for (int r = 0; r < CURSZ; r++) {
        for (int c = 0; c < CURSZ; c++) {
            if (!cursor_shape[r][c]) continue;
            int x = mx + c, y = my + r;
            if (x >= 0 && (u64)x < fb_w && y >= 0 && (u64)y < fb_h) {
                u32 *line = (u32 *)((u8 *)fb + (u64)y * fb_p);
                line[x] = fg;
            }
        }
    }
}

static int ps2_mouse_poll(void) {
    u8 st = inb(0x64);
    if (!(st & 1)) return 0;
    if (!(st & 0x20)) return 0;
    u8 data = inb(0x60);
    g_mouse_buf[g_mouse_idx++] = data;
    if (g_mouse_idx < 3) return 0;
    g_mouse_idx = 0;
    if (!(g_mouse_buf[0] & 0x08)) return 0;
    int dx = (int)(i8)g_mouse_buf[1];
    int dy = -(int)(i8)g_mouse_buf[2];
    g_mouse_x += dx;
    g_mouse_y += dy;
    if (g_mouse_x < 0) g_mouse_x = 0;
    if (g_mouse_y < 0) g_mouse_y = 0;
    if (g_mouse_x >= (int)fb_w - CURSZ) g_mouse_x = (int)fb_w - CURSZ;
    if (g_mouse_y >= (int)fb_h - CURSZ) g_mouse_y = (int)fb_h - CURSZ;
    int old_btn = g_mouse_btn;
    g_mouse_btn = g_mouse_buf[0] & 0x03;
    if (dx == 0 && dy == 0 && g_mouse_btn == old_btn) return 0;
    return 1;
}

/* ---- PS/2 scan code set 1 → ASCII ---- */
static char scan_to_ascii(u8 sc, int shift) {
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

/* ---- SHA-256 (compact, copied from FirstInit) ---- */
#define ROR(x,n) (((x)>>(n))|((x)<<(32-(n))))
static const u32 K256[64] = {0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
static void sha256(const char *msg, u8 out[32]) {
    u32 h[8]={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    u8 block[64]; u64 len=0; while(msg[len]) len++;
    for(int i=0;i<64;i++) block[i]=0;
    for(u64 i=0;i<len && i<55;i++) block[i]=(u8)msg[i];
    block[len]=0x80; u64 bit=len*8;
    for(int i=0;i<8;i++) block[63-i]=(u8)(bit>>(i*8));
    u32 w[64];
    for(int i=0;i<16;i++) w[i]=((u32)block[i*4]<<24)|((u32)block[i*4+1]<<16)|((u32)block[i*4+2]<<8)|block[i*4+3];
    for(int i=16;i<64;i++){u32 s0=ROR(w[i-15],7)^ROR(w[i-15],18)^(w[i-15]>>3);u32 s1=ROR(w[i-2],17)^ROR(w[i-2],19)^(w[i-2]>>10);w[i]=w[i-16]+s0+w[i-7]+s1;}
    u32 a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
    for(int i=0;i<64;i++){u32 S1=ROR(e,6)^ROR(e,11)^ROR(e,25);u32 ch=(e&f)^((~e)&g);u32 t1=hh+S1+ch+K256[i]+w[i];u32 S0=ROR(a,2)^ROR(a,13)^ROR(a,22);u32 maj=(a&b)^(a&c)^(b&c);u32 t2=S0+maj;hh=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;}
    h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=hh;
    for(int i=0;i<8;i++){out[i*4]=(u8)(h[i]>>24);out[i*4+1]=(u8)(h[i]>>16);out[i*4+2]=(u8)(h[i]>>8);out[i*4+3]=(u8)h[i];}
}

/* ---- 防篡改：对任意字节缓冲计算 SHA256（分块处理，支持 >55 字节） ---- */
static void sha256_buf(const u8 *msg, u64 len, u8 out[32]) {
    u32 h[8]={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    u8 block[64];
    u64 processed = 0;
    while (processed + 64 <= len) {
        for (int i = 0; i < 64; i++) block[i] = msg[processed + i];
        u32 w[64];
        for(int i=0;i<16;i++) w[i]=((u32)block[i*4]<<24)|((u32)block[i*4+1]<<16)|((u32)block[i*4+2]<<8)|block[i*4+3];
        for(int i=16;i<64;i++){u32 s0=ROR(w[i-15],7)^ROR(w[i-15],18)^(w[i-15]>>3);u32 s1=ROR(w[i-2],17)^ROR(w[i-2],19)^(w[i-2]>>10);w[i]=w[i-16]+s0+w[i-7]+s1;}
        u32 a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
        for(int i=0;i<64;i++){u32 S1=ROR(e,6)^ROR(e,11)^ROR(e,25);u32 ch=(e&f)^((~e)&g);u32 t1=hh+S1+ch+K256[i]+w[i];u32 S0=ROR(a,2)^ROR(a,13)^ROR(a,22);u32 maj=(a&b)^(a&c)^(b&c);u32 t2=S0+maj;hh=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;}
        h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=hh;
        processed += 64;
    }
    /* 最终块：剩余数据 + 0x80 + 长度（64 位大端） */
    u32 rem = (u32)(len - processed);
    for (u32 i = 0; i < 64; i++) block[i] = 0;
    for (u32 i = 0; i < rem; i++) block[i] = msg[processed + i];
    block[rem] = 0x80;
    u64 bit = len * 8;
    for (int i = 0; i < 8; i++) block[63 - i] = (u8)(bit >> (i * 8));
    u32 w[64];
    for(int i=0;i<16;i++) w[i]=((u32)block[i*4]<<24)|((u32)block[i*4+1]<<16)|((u32)block[i*4+2]<<8)|block[i*4+3];
    for(int i=16;i<64;i++){u32 s0=ROR(w[i-15],7)^ROR(w[i-15],18)^(w[i-15]>>3);u32 s1=ROR(w[i-2],17)^ROR(w[i-2],19)^(w[i-2]>>10);w[i]=w[i-16]+s0+w[i-7]+s1;}
    u32 a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
    for(int i=0;i<64;i++){u32 S1=ROR(e,6)^ROR(e,11)^ROR(e,25);u32 ch=(e&f)^((~e)&g);u32 t1=hh+S1+ch+K256[i]+w[i];u32 S0=ROR(a,2)^ROR(a,13)^ROR(a,22);u32 maj=(a&b)^(a&c)^(b&c);u32 t2=S0+maj;hh=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;}
    h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=hh;
    for(int i=0;i<8;i++){out[i*4]=(u8)(h[i]>>24);out[i*4+1]=(u8)(h[i]>>16);out[i*4+2]=(u8)(h[i]>>8);out[i*4+3]=(u8)h[i];}
}

/* ---- 防篡改：运行时 .text 段完整性校验 ----
 * 计算 __text_start ~ __text_end 的 SHA256，与 __expected_hash 对比。
 * 返回 1=校验通过（或开发期哈希为空跳过），0=校验失败（被篡改） */
static int login_verify_integrity(void) {
    const u8 *start = __text_start;
    const u8 *end = __text_end;
    const u8 *expected = __expected_hash;
    u64 len = (u64)(end - start);
    if (len == 0) return 0;

    /* 开发期便利：预期哈希全 0 时跳过校验 */
    int all_zero = 1;
    for (int i = 0; i < 32; i++) if (expected[i] != 0) { all_zero = 0; break; }
    if (all_zero) {
        logl("[login] integrity: expected hash empty (dev mode), skip");
        return 1;
    }

    u8 actual[32];
    sha256_buf(start, len, actual);

    /* 常量时间比较 */
    u8 diff = 0;
    for (int i = 0; i < 32; i++) diff |= actual[i] ^ expected[i];
    if (diff == 0) {
        logl("[login] integrity: OK");
        return 1;
    }
    logl("[login] integrity: FAIL — code tampered!");
    return 0;
}

/* ---- conf parsing ----
 * USER.CONF format (built by FirstInit's build_user_conf, but stored unencrypted
 * on disk for login.elf to parse):
 *   DESHAB_USERCONF_V1
 *   computer=<pc>
 *   username=<user>
 *   passwordSha256=<64 hex chars>
 *   ...
 *
 * If the buffer does NOT start with "DESHAB_USERCONF_V1", it may be XOR-encrypted
 * with the password hash stream. In that case, the user's entered password is
 * used to decrypt: SHA256(password) → hash, then buf[i] ^= hash[i & 31].
 * If the decrypted buffer starts with the magic, the password is correct.
 */

static int streq_n(const char *a, const char *b, u32 n) {
    for (u32 i = 0; i < n; i++) {
        if (a[i] != b[i]) return 0;
    }
    return 1;
}

/* Find "key=" in buf, copy value (up to newline) into out (NUL-terminated).
 * Returns 0 on success, -1 if key not found. */
static int conf_get_value(const u8 *buf, u32 size, const char *key, char *out, u32 out_cap) {
    u32 klen = 0; while (key[klen]) klen++;
    for (u32 i = 0; i + klen + 1 < size; i++) {
        /* Match key at start of line (or start of buffer) */
        if (i > 0 && buf[i-1] != '\n') continue;
        if (buf[i + klen] != '=') continue;
        if (!streq_n((const char *)buf + i, key, klen)) continue;
        /* Found key=, copy value until newline or end */
        u32 vstart = i + klen + 1;
        u32 j = 0;
        while (vstart + j < size && buf[vstart + j] != '\n' && buf[vstart + j] != '\r' && j + 1 < out_cap) {
            out[j] = (char)buf[vstart + j];
            j++;
        }
        out[j] = 0;
        return 0;
    }
    return -1;
}

static int starts_with(const u8 *buf, u32 size, const char *prefix) {
    u32 i = 0;
    while (prefix[i]) {
        if (i >= size) return 0;
        if (buf[i] != (u8)prefix[i]) return 0;
        i++;
    }
    return 1;
}

/* ---- hex string comparison ---- */
static int hexeq(const u8 *hash32, const char *hex64) {
    static const char hx[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        if (hex64[i*2] != hx[hash32[i] >> 4]) return 0;
        if (hex64[i*2+1] != hx[hash32[i] & 0xf]) return 0;
    }
    return 1;
}

/* ---- login state ---- */
static char g_username[64];
static char g_password_hash[80];   /* 64 hex chars + NUL */
static int  g_has_user;            /* 1 if username extracted */
static int  g_has_hash;            /* 1 if passwordSha256 extracted */
static int  g_is_encrypted;        /* 1 if conf buffer is XOR-encrypted */

/* ---- UI layout ---- */
#define CARD_W  560
#define CARD_H  400

static void draw_login_card(u32 *fb, i64 card_x, i64 card_y, const char *pass, int active,
                            const char *msg, u32 msg_color, u32 fg) {
    u32 card = CARD_BG;
    u32 sub_fg = 0xFF9098A0;
    /* Background + card */
    cursor_restore_bg(fb);
    fill_gradient_rect(fb, card_x - 24, card_y - 24, CARD_W + 48, CARD_H + 48);
    draw_card(fb, card_x, card_y, CARD_W, CARD_H, card);
    /* Title */
    fb_text(fb, "Deshab Login", card_x + 40, card_y + 40, fg, card);
    fb_text(fb, "Enter your password to continue", card_x + 40, card_y + 70, sub_fg, card);
    /* Username field */
    fb_text(fb, "Username", card_x + 40, card_y + 120, sub_fg, card);
    draw_rounded_input(fb, card_x + 40, card_y + 150, CARD_W - 80, 44,
                       g_has_user ? g_username : "(unknown)", 0, fg, card, 0);
    /* Password field */
    fb_text(fb, "Password", card_x + 40, card_y + 210, sub_fg, card);
    draw_rounded_input(fb, card_x + 40, card_y + 240, CARD_W - 80, 44, pass, 1, fg, card, active);
    /* Login button */
    {
        u32 btn_bg = ACCENT;
        fill_rounded_rect(fb, card_x + 40, card_y + 310, 160, 44, 3, btn_bg);
        fb_text(fb, "Login", card_x + 40 + 56, card_y + 310 + (44 - ASCII_H) / 2,
                0xFFFFFFFFu, btn_bg);
    }
    /* Status / error message */
    if (msg && msg[0]) {
        fb_text(fb, msg, card_x + 220, card_y + 322, msg_color, card);
    }
    /* Bottom hint */
    fb_text(fb, "Enter=login  Esc=skip  Backspace=delete",
            card_x + 40, card_y + CARD_H - 28, DIM_FG, card);
    cursor_save_bg(fb, g_mouse_x, g_mouse_y);
    cursor_draw(fb, g_mouse_x, g_mouse_y);
}

/* Redraw only the password input field (avoids full card redraw on each keystroke) */
static void redraw_password_only(u32 *fb, i64 card_x, i64 card_y, const char *pass, u32 fg) {
    u32 card = CARD_BG;
    i64 fy = card_y + 240;
    cursor_restore_bg(fb);
    fill_rect(fb, card_x + 40, fy, CARD_W - 80, 44, card);
    draw_rounded_input(fb, card_x + 40, fy, CARD_W - 80, 44, pass, 1, fg, card, 1);
    cursor_save_bg(fb, g_mouse_x, g_mouse_y);
    cursor_draw(fb, g_mouse_x, g_mouse_y);
}

/* ---- verify password against stored hash or encrypted conf ----
 * Returns 1 if password is correct, 0 otherwise.
 * For encrypted conf: XOR-decrypt with SHA256(password); if magic matches, correct.
 * For unencrypted conf: SHA256(password) == stored passwordSha256 hex string.
 */
static int verify_password(const u8 *conf_buf, u32 conf_size, const char *password) {
    if (g_is_encrypted) {
        /* Decrypt a copy and check magic */
        u8 hash[32];
        sha256(password, hash);
        static u8 scratch[2048];
        u32 sz = conf_size;
        if (sz > sizeof(scratch)) sz = sizeof(scratch);
        for (u32 i = 0; i < sz; i++) scratch[i] = conf_buf[i] ^ hash[i & 31];
        if (starts_with(scratch, sz, "DESHAB_USERCONF_V1")) {
            /* Also extract username from decrypted buffer for display */
            if (!g_has_user) {
                conf_get_value(scratch, sz, "username", g_username, sizeof(g_username));
                if (g_username[0]) g_has_user = 1;
            }
            return 1;
        }
        return 0;
    } else if (g_has_hash) {
        u8 hash[32];
        sha256(password, hash);
        return hexeq(hash, g_password_hash);
    }
    return 0;
}

/* ---- P8.2 Fix: PS/2 鼠标初始化（login 进入前 mouseInit 可能未运行） ----
 * release 版 is_first=0 路径中 DSK 不加载 mouseInit，login 需自行初始化鼠标
 * 端口轮询方能工作。与 mouseInit 同样的命令序列，但去掉 IRQ12 注册
 * （login 全程 cli，无需中断驱动）。单步失败只记日志，不阻塞登录。 */
#define PS2_DATA    0x60
#define PS2_STATUS  0x64
#define PS2_CMD     0x64
#define PS2_ST_OBF  0x01
#define PS2_ST_IBF  0x02
#define PS2_CMD_READ_CFG   0x20
#define PS2_CMD_WRITE_CFG  0x60
#define PS2_CMD_ENABLE_AUX 0xA8
#define PS2_CMD_TO_MOUSE   0xD4
#define MOUSE_CMD_RESET    0xFF
#define MOUSE_CMD_DEFAULTS 0xF6
#define MOUSE_CMD_RATE     0xF3
#define MOUSE_CMD_RESOL    0xE8
#define MOUSE_CMD_ENABLE   0xF4
#define MOUSE_ACK          0xFA
#define PS2_TIMEOUT 100000u

static int ps2_wait_write(void) {
    for (u32 t = 0; t < PS2_TIMEOUT; t++) {
        if (!(inb(PS2_STATUS) & PS2_ST_IBF)) return 0;
        __asm__ volatile("pause");
    }
    return -1;
}
static int ps2_wait_read(void) {
    for (u32 t = 0; t < PS2_TIMEOUT; t++) {
        if (inb(PS2_STATUS) & PS2_ST_OBF) return 0;
        __asm__ volatile("pause");
    }
    return -1;
}
static void ps2_drain(void) {
    for (int i = 0; i < 16; i++) {
        if (!(inb(PS2_STATUS) & PS2_ST_OBF)) break;
        inb(PS2_DATA);
    }
}
static int ps2_cmd(u8 cmd) {
    if (ps2_wait_write() != 0) return -1;
    outb(PS2_CMD, cmd);
    return 0;
}
static int mouse_cmd(u8 cmd) {
    if (ps2_cmd(PS2_CMD_TO_MOUSE) != 0) return -1;
    if (ps2_wait_write() != 0) return -2;
    outb(PS2_DATA, cmd);
    if (ps2_wait_read() != 0) return -3;
    return inb(PS2_DATA) == MOUSE_ACK ? 0 : -4;
}
static int mouse_cmd_param(u8 cmd, u8 param) {
    int rc = mouse_cmd(cmd);
    if (rc != 0) return rc;
    if (ps2_cmd(PS2_CMD_TO_MOUSE) != 0) return -5;
    if (ps2_wait_write() != 0) return -6;
    outb(PS2_DATA, param);
    if (ps2_wait_read() != 0) return -7;
    return inb(PS2_DATA) == MOUSE_ACK ? 0 : -8;
}
static void login_mouse_init(void) {
    logl("[login] mouse init begin");

    /* 1. 冲刷残留 */
    ps2_drain();

    /* 2. 启用 AUX 端口 */
    {
        u8 cfg = 0;
        if (ps2_cmd(PS2_CMD_READ_CFG) == 0 && ps2_wait_read() == 0) {
            cfg = inb(PS2_DATA);
            cfg &= (u8)~0x20u;                 /* bit5=0: 启用 AUX 时钟 */
            cfg |= 0x02u | 0x40u;              /* bit1=AUX IRQ, bit6=翻译 */
            if (ps2_cmd(PS2_CMD_WRITE_CFG) == 0 && ps2_wait_write() == 0) {
                outb(PS2_DATA, cfg);
                logl("[login] cfg byte written");
            } else {
                logl("[login] cfg write failed, continue");
            }
        } else {
            logl("[login] cfg read failed, continue");
        }
    }

    /* 3. 启用 AUX 端口 */
    if (ps2_cmd(PS2_CMD_ENABLE_AUX) != 0)
        logl("[login] enable AUX timeout, continue");

    /* 4. 复位鼠标 + drain 应答 */
    {
        int rc = mouse_cmd(MOUSE_CMD_RESET);
        if (rc != 0) logl("[login] reset failed, continue");
        for (int i = 0; i < 8; i++) {
            if (ps2_wait_read() != 0) break;
            inb(PS2_DATA);
        }
    }

    /* 5-8. Set Defaults -> 采样率 100 -> 分辨率 2 -> 启用数据报告 */
    {
        int rc;
        rc = mouse_cmd(MOUSE_CMD_DEFAULTS);
        if (rc != 0) logl("[login] set defaults failed, continue");
        rc = mouse_cmd_param(MOUSE_CMD_RATE, 100);
        if (rc != 0) logl("[login] sample rate failed, continue");
        rc = mouse_cmd_param(MOUSE_CMD_RESOL, 2);
        if (rc != 0) logl("[login] resolution failed, continue");
        rc = mouse_cmd(MOUSE_CMD_ENABLE);
        if (rc != 0) logl("[login] enable reporting failed, continue");
        else logl("[login] mouse data reporting enabled");
    }

    logl("[login] mouse init complete");
}

__attribute__((visibility("default")))
/* [DSK-E70] 原布局 guard 已删除：内嵌模糊壁纸（~1.8MB 数据）将雷区
 * （image+0x1EAE10 附近）整体吸收进壁纸像素区，写零不再可见/有害。 */

void dsk_entry(const dsk_boot_context *ctx) {
    __asm__ volatile("cli");  /* prevent IRQ1 (ps2kbd) from racing with our polling */
    logl("[login] boot");

    /* 防篡改：第一时间校验 .text 段完整性。失败 = 系统被篡改，严格 panic。 */
    if (!login_verify_integrity()) {
        login_fatal("LOGIN-E01-INTEGRITY-FAIL");
    }

    logl("[login] calibrating TSC");
    tsc_calibrate_fi();

    /* P8.2 Fix: 确保 PS/2 鼠标已初始化（非首次启动时 DSK 不加载 mouseInit） */
    login_mouse_init();

    if (!ctx || ctx->magic != 0x44534B31424F4F54ULL) {
        login_fatal("LOGIN-E02-BADCONTEXT");
    }

    fb_a = ctx->framebuffer_address;
    fb_w = ctx->framebuffer_width;
    fb_h = ctx->framebuffer_height;
    fb_p = ctx->framebuffer_pitch;

    logl("[login] framebuffer ready");

    /* Get conf data from boot context (passed by DSK) */
    const u8 *conf_buf = (const u8 *)(u64)ctx->reserved[0];
    u32 conf_size = (u32)ctx->reserved[1];
    if (!conf_buf || conf_size == 0) {
        login_fatal("LOGIN-E03-NOUSERCONF");
    }
    logl("[login] conf data received");

    /* Initialize state */
    g_username[0] = 0;
    g_password_hash[0] = 0;
    g_has_user = 0;
    g_has_hash = 0;
    g_is_encrypted = 0;

    /* Check if conf is unencrypted (starts with magic) or encrypted */
    if (starts_with(conf_buf, conf_size, "DESHAB_USERCONF_V1")) {
        logl("[login] conf is plaintext, parsing");
        g_is_encrypted = 0;
        if (conf_get_value(conf_buf, conf_size, "username", g_username, sizeof(g_username)) == 0) {
            g_has_user = 1;
            logl("[login] username found");
        } else {
            login_fatal("LOGIN-E04-NO-USERNAME");
        }
        if (conf_get_value(conf_buf, conf_size, "passwordSha256", g_password_hash, sizeof(g_password_hash)) == 0) {
            g_has_hash = 1;
            logl("[login] password hash found");
        } else {
            login_fatal("LOGIN-E05-NO-PASSWORD-HASH");
        }
    } else {
        logl("[login] conf appears encrypted, will verify by password decryption");
        g_is_encrypted = 1;
        /* Username will be extracted after successful decryption */
    }

    u32 fg = TEXT_FG;
    u32 *fb = (u32 *)(u64)fb_a;

    /* 背景：模糊化的桌面壁纸铺满全屏（卡片浮于其上） */
    draw_blur_wallpaper(fb);

    /* Card position (centered) */
    i64 card_x = ((i64)fb_w - CARD_W) / 2;
    i64 card_y = ((i64)fb_h - CARD_H) / 2;

    /* Initial message */
    const char *msg = "Enter password and press Enter";
    u32 msg_color = DIM_FG;

    /* Password input buffer */
    char pass[64];
    pass[0] = 0;
    int plen = 0;

    /* Draw initial UI（draw_login_card 内部已含光标 save+draw，
     * 此处不得重复 save——否则会把光标像素当背景存入，移动后留残影） */
    draw_login_card(fb, card_x, card_y, pass, 1, msg, msg_color, fg);

    /* Input loop */
    int shift = 0;
    int release = 0;
    for (;;) {
        /* 鼠标轮询：处理鼠标移动并更新光标 */
        if (ps2_mouse_poll()) {
            cursor_restore_bg(fb);
            cursor_save_bg(fb, g_mouse_x, g_mouse_y);
            cursor_draw(fb, g_mouse_x, g_mouse_y);
        }

        u8 st = inb(0x64);
        if (!(st & 1)) { __asm__("pause"); continue; }
        /* 鼠标数据留给 ps2_mouse_poll 读取，不消费不丢弃 */
        if (st & 0x20) { __asm__("pause"); continue; }
        u8 data = inb(0x60);

        u8 sc = data;
        /* Handle extended (E0) sequences — ignore (no arrow keys needed for login) */
        if (sc == 0xE0) { continue; }
        /* Handle release (F0 in Set 2, or high bit in Set 1) */
        if (sc == 0xF0) { release = 1; continue; }
        if (release) { release = 0; /* track shift release below */ }

        /* Shift press/release */
        if (sc == 0x2A || sc == 0x36) { shift = 1; continue; }
        if (sc == 0xAA || sc == 0xB6) { shift = 0; continue; }

        /* Ignore high-bit (release in Set 1) */
        if (sc & 0x80) continue;

        /* Escape (sc 0x01) — skip login */
        if (sc == 0x01) {
            logl("[login] Esc pressed, skipping login");
            ((dsk_boot_context *)ctx)->reserved[2] = 0;
            return;
        }

        char c = scan_to_ascii(sc, shift);
        if (!c) continue;

        if (c == '\n') {
            /* Submit password */
            if (plen == 0) {
                /* Empty password — show hint, don't verify */
                msg = "Password is empty";
                msg_color = ERROR_FG;
                draw_login_card(fb, card_x, card_y, pass, 1, msg, msg_color, fg);
                continue;
            }
            logl("[login] verifying password");
            /* 先给用户反馈再算：verify_password 是 SHA256 派生 + 解密比对，
             * 慢环境下要几百毫秒，不提示会像卡死。
             * 浮在登录卡片正中的独立提示层（不占用卡片本身的文案区）。
             * 关键：先作废光标背景备份，否则光标下次 cursor_restore_bg()
             * 会把浮层画上去之前的旧背景贴回来，在浮层上挖出一个洞。 */
            {
                g_cursor_saved = 0;
                const char *tip = "Logging in...";
                i64 tw = (i64)(sizeof("Logging in...") - 1) * ASCII_STEP;  /* 12 字符 */
                i64 bw = tw + 56, bh = 56;
                i64 bx = card_x + (CARD_W - bw) / 2;
                i64 by = card_y + (CARD_H - bh) / 2;               /* 卡片正中 */
                /* 半透明遮罩压暗底下的卡片（含阴影外扩 24px），再画提示框 */
                for (i64 r = -24; r < CARD_H + 24; r++) {
                    i64 yy = card_y + r;
                    if (yy < 0 || (u64)yy >= fb_h) continue;
                    u32 *line = (u32 *)((u8 *)fb + (u64)yy * fb_p);
                    for (i64 c = -24; c < CARD_W + 24; c++) {
                        i64 xx = card_x + c;
                        if (xx < 0 || (u64)xx >= fb_w) continue;
                        u32 px = line[(u64)xx];
                        /* 压暗到 25%：每通道取高 6 位后右移 2 位 */
                        u32 dim = ((px >> 2) & 0x3F3F3Fu) | 0xFF000000u;
                        line[(u64)xx] = dim;
                    }
                }
                fill_rounded_rect(fb, bx, by, bw, bh, 10, 0xFF30303Au);
                stroke_rounded_rect(fb, bx, by, bw, bh, 10, 1, 0xFF6A6A78u);
                fb_text(fb, tip, bx + 28, by + (bh - ASCII_H) / 2, 0xFFE8E8F0, 0xFF30303Au);
                g_cursor_old_x = -100;   /* 让下次光标重画到当前位置 */
                g_cursor_old_y = -100;
            }
            int ok = verify_password(conf_buf, conf_size, pass);
            if (ok) {
                logl("[login] password correct");
                ((dsk_boot_context *)ctx)->reserved[2] = 1;
                /* "Login successful" + 卡片下方旋转加载圆圈：
                 * DSK 在 login 返回后加载 desktop，圆圈给出加载反馈 */
                draw_login_card(fb, card_x, card_y, pass, 0,
                                "Login successful", 0xFF5FAF6Fu, fg);
                i64 scx = card_x + CARD_W / 2;
                i64 scy = card_y + CARD_H + 40;
                if (scy > (i64)fb_h - 30) scy = (i64)fb_h - 30;
                for (int f = 0; f < 90; f++) {   /* ~3s，转 90/8 圈 */
                    restore_wallpatch(fb, scx, scy, 30);  /* 清上一帧残迹 */
                    draw_spinner(fb, scx, scy, f);
                    delay_frame();
                }
                restore_wallpatch(fb, scx, scy, 30);      /* 停转后清干净 */
                return;
            } else {
                logl("[login] password wrong");
                msg = "Wrong password, try again";
                msg_color = ERROR_FG;
                /* Clear password */
                pass[0] = 0;
                plen = 0;
                draw_login_card(fb, card_x, card_y, pass, 1, msg, msg_color, fg);
                continue;
            }
        }

        if (c == 8) {
            /* Backspace */
            if (plen > 0) {
                pass[--plen] = 0;
                redraw_password_only(fb, card_x, card_y, pass, fg);
            }
            continue;
        }

        /* Regular character */
        if (plen < 62 && c >= 32 && c <= 126) {
            pass[plen++] = c;
            pass[plen] = 0;
            redraw_password_only(fb, card_x, card_y, pass, fg);
        }
    }
}
