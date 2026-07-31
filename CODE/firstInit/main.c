/* FirstInit — first boot setup wizard
 * Fades out loading ring, shows welcome, 5s delay, transitions to setup guide.
 */

#include "../UTSM/include/utsm/dsk.h"

typedef signed char        i8;
typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;
typedef long long          i64;

#define COM1 0x3F8

static __inline__ void outb(u16 port, u8 value) { __asm__ volatile("outb %0,%1"::"a"(value),"Nd"(port)); }
static __inline__ u8 inb(u16 port) { u8 v; __asm__ volatile("inb %1,%0":"=a"(v):"Nd"(port)); return v; }

/* ---- TSC-based timing (实机要求: 用 CPU 频率计算, 不用循环) ---- */
static u64 g_tsc_per_ms = 0;

static __inline__ u64 rdtsc_fi(void) {
    u32 lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((u64)hi << 32) | lo;
}

/* 用 PIT (8254, 1.193182 MHz) 校准 TSC 频率 */
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

static void sputc(char c) {
    for (unsigned int i=0; i<100000; i++) { if (inb(COM1+5)&0x20) break; }
    outb(COM1, (unsigned char)c);
}
static void swrite(const char *s) { while(*s) { if(*s=='\n')sputc('\r'); sputc(*s++); } }
static void logl(const char *s) { swrite(s); swrite("\n"); }

/* [debug-point b6-io] */
/* B6 forensics: log every byte consumed from the i8042 in the input loops */
static void dbgb(const char *tag, u8 st, u8 d) {
    static const char hx[] = "0123456789abcdef";
    char b[48]; int p = 0;
    while (tag[p]) { b[p] = tag[p]; p++; }
    b[p++]=' '; b[p++]='s'; b[p++]='t'; b[p++]='=';
    b[p++]=hx[(st>>4)&15]; b[p++]=hx[st&15];
    b[p++]=' '; b[p++]='d'; b[p++]='=';
    b[p++]=hx[(d>>4)&15]; b[p++]=hx[d&15];
    b[p]=0;
    logl(b);
}
/* [/debug-point] */

/* ---- PS/2 controller / mouse helpers (timeout-protected, TSC-based) ---- */
static int ps2_wait_write(void) {
    u64 deadline = rdtsc_fi() + g_tsc_per_ms * 100;  /* 100ms 超时 */
    while (rdtsc_fi() < deadline) {
        if (!(inb(0x64) & 0x02)) return 0;  /* input buffer empty */
        __asm__ volatile("pause");
    }
    return -1;
}
static int ps2_wait_read(void) {
    u64 deadline = rdtsc_fi() + g_tsc_per_ms * 100;  /* 100ms 超时 */
    while (rdtsc_fi() < deadline) {
        if (inb(0x64) & 0x01) return 0;     /* output buffer full */
        __asm__ volatile("pause");
    }
    return -1;
}
static void ps2_cmd(u8 cmd) {
    if (ps2_wait_write() == 0) outb(0x64, cmd);
}
static void mouse_cmd(u8 cmd) {
    if (ps2_wait_write() == 0) outb(0x64, 0xD4);  /* next byte -> mouse */
    if (ps2_wait_write() == 0) outb(0x60, cmd);
}
static void mouse_init(void) {
    /* flush any pending data left by firmware */
    for (int i = 0; i < 16; i++) {
        if (!(inb(0x64) & 0x01)) break;
        inb(0x60);
    }
    logl("[mouse] enable AUX port");
    ps2_cmd(0xA8);        /* enable AUX (mouse) port */
    /* read controller config: enable AUX clock, keep scan-code translation enabled */
    ps2_cmd(0x20);
    u8 cfg = 0;
    if (ps2_wait_read() == 0) cfg = inb(0x60);
    logl("[mouse] old cfg read");
    cfg &= (u8)~(1 << 5);          /* bit5=1 disables AUX clock; clear it */
    cfg |= (1 << 1) | (1 << 6);    /* AUX IRQ + Set2→Set1 translation */
    ps2_cmd(0x60);
    if (ps2_wait_write() == 0) outb(0x60, cfg);
    logl("[mouse] new cfg written");
    /* reset mouse, drain ACK + self-test + device ID */
    logl("[mouse] reset");
    mouse_cmd(0xFF);
    for (int i = 0; i < 32; i++) {
        if (ps2_wait_read() != 0) break;
        u8 r = inb(0x60);
        if (r == 0xFA) logl("[mouse] ACK");
        else if (r == 0xAA) logl("[mouse] self-test OK");
        else if (r == 0x00) logl("[mouse] device ID 0");
    }
    /* enable streaming mode */
    logl("[mouse] enable streaming");
    mouse_cmd(0xF4);
    for (int i = 0; i < 16; i++) {
        if (ps2_wait_read() != 0) break;
        inb(0x60);
    }
    logl("[mouse] init done");
}

#define BG_TOP    0xFF2D2D30u
#define BG_BOTTOM 0xFF1A1A1Eu
#define BG_MID    0xFF3C3C40u
#define CARD_BG   0xFF353539u
#define INPUT_BG  0xFF2A2A2Eu
#define TEXT_FG   0xFFE0E0E0u
#define WELCOME_FG 0xFFE8F2FCu

static u64 fb_a, fb_w, fb_h, fb_p;

static u32 blend(u32 c1, u32 c2, u32 a) {
    u32 na=256-a;
    u32 r=((c1&0xFF)*na+(c2&0xFF)*a)>>8;
    u32 g=(((c1>>8)&0xFF)*na+((c2>>8)&0xFF)*a)>>8;
    u32 b=(((c1>>16)&0xFF)*na+((c2>>16)&0xFF)*a)>>8;
    return 0xFF000000|(b<<16)|(g<<8)|r;
}

/* ASCII font — Consolas 18px grayscale, pre-rendered from C:\Windows\Fonts\consola.ttf */
#include "ascii_bitmaps.c"
/* ASCII text — Consolas 18px grayscale, alpha-blend rendered (skips transparent pixels) */
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

/* width of an ASCII string in pixels (used to center English subtitles) */
static i64 fb_text_width(const char *s) {
    i64 n = 0;
    while (s[n]) n++;
    return n * ASCII_STEP;
}

/* Alpha-blended ASCII text — same as fb_text but multiplied by a global alpha,
 * used to fade English subtitles in/out alongside the Chinese bitmaps. */
static void fb_text_alpha(u32 *fb, const char *s, i64 x, i64 y, u32 fg, u32 bg, u32 global_alpha) {
    for (i64 i=0; s[i]; i++) {
        u32 ch = (u32)(u8)s[i];
        if (ch < ' ' || ch > '~') ch = ' ';
        u32 idx = ch - ' ';
        const u8 *g = g_ascii[idx];
        for (i64 r=0; r<ASCII_H; r++) {
            i64 yy = y + r;
            if (yy < 0 || (u64)yy >= fb_h) continue;
            u32 *line = (u32 *)((u8 *)fb + (u64)yy * fb_p);
            for (i64 c=0; c<ASCII_W; c++) {
                u32 a = g[r * ASCII_W + c];
                if (a == 0) continue;
                u32 eff_a = ((u32)a * global_alpha) >> 8;
                if (eff_a == 0) continue;
                i64 xx = x + i*ASCII_STEP + c;
                if (xx < 0 || (u64)xx >= fb_w) continue;
                line[(u64)xx] = (eff_a == 255) ? fg : blend(bg, fg, eff_a);
            }
        }
    }
}

/* draw pre-rendered grayscale bitmap – writes only, no framebuffer read */
static void fb_bitmap_alpha(u32 *fb, const u8 *data, i64 w, i64 h, i64 x, i64 y, u32 fg, u32 bg, u32 global_alpha) {
    for (i64 r=0; r<h; r++) {
        i64 yy = y + r;
        if (yy < 0 || (u64)yy >= fb_h) continue;
        u32 *line = (u32 *)((u8 *)fb + (u64)yy * fb_p);
        for (i64 c=0; c<w; c++) {
            i64 xx = x + c;
            if (xx < 0 || (u64)xx >= fb_w) continue;
            u8 val = data[r*(u64)w + (u64)c];
            if (val == 0) continue;  /* skip transparent – let background show through */
            u32 a = ((u32)val * global_alpha) >> 8;
            line[(u64)xx] = blend(bg, fg, a);
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

static void delay_frame(void) {
    delay_ms_fi(11);  /* 实机: 基于 TSC 的 11ms 帧间隔 (原33ms/3，加快打字机效果) */
}

#include "text_bitmaps.c"

static void fade_ring(u32 *fb, i64 cx, i64 cy, u32 bg) {
    /* Erase the spinner sprite area with gradient background in one frame. */
    (void)bg;
    i64 r = 72;
    fill_gradient_rect(fb, cx-r, cy-r, r*2+1, r*2+1);
}

static void draw_card(u32 *fb, i64 x, i64 y, i64 w, i64 h, u32 card, u32 bg_color) {
    (void)bg_color;
    fill_rounded_rect(fb, x, y, w, h, 4, card);
}

/* Draw gradient background + card in a single pass — per-row blend eliminates flicker */
static void draw_card_fade(u32 *fb, i64 x, i64 y, i64 w, i64 h, i64 radius, u32 alpha) {
    for (i64 r=0; r<h; r++) {
        i64 yy = y + r;
        if (yy < 0 || (u64)yy >= fb_h) continue;
        u32 bg_color = bg_at_y(yy);
        u32 card_color = blend(bg_color, CARD_BG, alpha);
        u32 *line = (u32 *)((u8 *)fb + (u64)yy * fb_p);
        for (i64 c=0; c<w; c++) {
            i64 xx = x + c;
            if (xx < 0 || (u64)xx >= fb_w) continue;
            line[(u64)xx] = rounded_rect_contains(xx, yy, x, y, w, h, radius) ? card_color : bg_color;
        }
    }
}

static void draw_rounded_input(u32 *fb, i64 x, i64 y, i64 w, i64 h, const char *value, int mask, u32 fg, u32 card_bg, int active) {
    u32 fill = active ? 0xFF45454Au : INPUT_BG;
    fill_rounded_rect(fb, x, y, w, h, 2, fill);
    stroke_rounded_rect(fb, x, y, w, h, 2, 1, active ? 0xFF6C6C72 : 0xFF404044);
    char out[64]; int i=0;
    while(value[i] && i<60) { out[i] = mask ? '*' : value[i]; i++; }
    out[i]=0;
    if (out[0]) fb_text(fb, out, x+14, y+(h-ASCII_H)/2, fg, fill);
    if (active) {
        i64 cx2 = x + 14 + i * ASCII_STEP;
        fill_rect(fb, cx2, y+10, 2, h-20, fg);
    }
}

static char scan_to_ascii(u8 sc, int shift) {
    /* PS/2 scan code set 1 (controller translation enabled) */
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

/* ---- cursor save / restore (avoids full card redraw on mouse move) ---- */
#define CUR_W 24
#define CUR_H 24
static u32 cursor_bg[CUR_W * CUR_H];
static int cursor_bg_valid = 0;
static i64 cursor_cur_x = -100, cursor_cur_y = -100;

static const char *cursor_shape[CUR_H] = {
    "X                       ",
    "XX                      ",
    "XOX                     ",
    "XOOX                    ",
    "XOOOX                   ",
    "XOOOOX                  ",
    "XOOOOOX                 ",
    "XOOOOOOX                ",
    "XOOOOOOOX               ",
    "XOOOOOOOOX              ",
    "XOOOOOOOOOX             ",
    "XOOOOOOOOOOX            ",
    "XOOOOOOOXXXXX           ",
    "XOOOXOOX                ",
    "XOOXXOOX                ",
    "XOXX XOOX               ",
    "XXX  XOOX               ",
    "     XOOX               ",
    "      XOOX              ",
    "      XOOX              ",
    "       XX               ",
    "                        ",
    "                        ",
    "                        "
};

static void cursor_erase(u32 *fb) {
    if (!cursor_bg_valid) return;
    for (i64 y = 0; y < CUR_H; y++) {
        i64 py = cursor_cur_y + y;
        if (py < 0 || (u64)py >= fb_h) continue;
        u32 *line = (u32 *)((u8 *)fb + (u64)py * fb_p);
        for (i64 x = 0; x < CUR_W; x++) {
            i64 px = cursor_cur_x + x;
            if (px < 0 || (u64)px >= fb_w) continue;
            line[(u64)px] = cursor_bg[y * CUR_W + x];
        }
    }
    cursor_bg_valid = 0;
}

static void cursor_draw(u32 *fb, i64 mx, i64 my) {
    if (mx < 0) mx = 0;
    if (my < 0) my = 0;
    if ((u64)(mx + CUR_W) >= fb_w) mx = (i64)fb_w - CUR_W - 1;
    if ((u64)(my + CUR_H) >= fb_h) my = (i64)fb_h - CUR_H - 1;
    if (mx < 0) mx = 0;
    if (my < 0) my = 0;
    /* save background under cursor */
    for (i64 y = 0; y < CUR_H; y++) {
        i64 py = my + y;
        for (i64 x = 0; x < CUR_W; x++) {
            i64 px = mx + x;
            if (py < 0 || (u64)py >= fb_h || px < 0 || (u64)px >= fb_w) {
                cursor_bg[y * CUR_W + x] = 0;
            } else {
                u32 *line = (u32 *)((u8 *)fb + (u64)py * fb_p);
                cursor_bg[y * CUR_W + x] = line[(u64)px];
            }
        }
    }
    cursor_bg_valid = 1;
    cursor_cur_x = mx;
    cursor_cur_y = my;
    /* draw cursor shape */
    u32 white = 0xFFFFFFFF, dark = 0xFF304760;
    for (i64 y = 0; y < CUR_H; y++) {
        i64 py = my + y;
        if (py < 0 || (u64)py >= fb_h) continue;
        u32 *line = (u32 *)((u8 *)fb + (u64)py * fb_p);
        for (i64 x = 0; x < CUR_W; x++) {
            char p = cursor_shape[y][x];
            if (p == ' ') continue;
            i64 px = mx + x;
            if (px < 0 || (u64)px >= fb_w) continue;
            line[(u64)px] = (p == 'O') ? white : dark;
        }
    }
}

static void redraw_setup_card(u32 *fb, i64 card_x, i64 card_y, char *pc, char *user, char *pass, int active, u32 bg, u32 fg, i64 mx, i64 my) {
    u32 card = CARD_BG;
    u32 sub_fg = 0xFF9098A0;  /* dimmer English subtitle color on card */
    cursor_erase(fb);
    fill_gradient_rect(fb, card_x - 24, card_y - 24, 948, 648);
    draw_card(fb, card_x, card_y, 900, 600, card, bg);
    fb_bitmap_alpha(fb, g_txt_title, g_txt_title_w, g_txt_title_h, card_x+40, card_y+40, fg, card, 255);
    fb_text(fb, "Account Setup", card_x+40, card_y+40+g_txt_title_h+2, sub_fg, card);
    fb_bitmap_alpha(fb, g_txt_computer, g_txt_computer_w, g_txt_computer_h, card_x+40, card_y+130, fg, card, 255);
    fb_text(fb, "Computer Name", card_x+40, card_y+130+g_txt_computer_h+2, sub_fg, card);
    draw_rounded_input(fb, card_x+40, card_y+180, 820, 44, pc, 0, fg, card, active==0);
    fb_bitmap_alpha(fb, g_txt_username, g_txt_username_w, g_txt_username_h, card_x+40, card_y+280, fg, card, 255);
    fb_text(fb, "Username", card_x+40, card_y+280+g_txt_username_h+2, sub_fg, card);
    draw_rounded_input(fb, card_x+40, card_y+330, 820, 44, user, 0, fg, card, active==1);
    fb_bitmap_alpha(fb, g_txt_password, g_txt_password_w, g_txt_password_h, card_x+40, card_y+430, fg, card, 255);
    fb_text(fb, "Password", card_x+40, card_y+430+g_txt_password_h+2, sub_fg, card);
    draw_rounded_input(fb, card_x+40, card_y+480, 820, 44, pass, 1, fg, card, active==2);
    fb_bitmap_alpha(fb, g_txt_hint, g_txt_hint_w, g_txt_hint_h, card_x+40, card_y+555, 0xFF7F97AC, card, 255);
    fb_text(fb, "Press Enter to confirm each field", card_x+40, card_y+555+g_txt_hint_h+2, 0xFF6F7F8C, card);
    cursor_draw(fb, mx, my);
}

/* 仅重绘当前活动输入框，避免每次按键重绘整个卡片导致闪烁 */
static void redraw_input_only(u32 *fb, i64 card_x, i64 card_y, int field, char *pc, char *user, char *pass, i64 mx, i64 my, u32 fg) {
    u32 card = CARD_BG;
    char *buf = field==0 ? pc : (field==1 ? user : pass);
    int mask = (field == 2) ? 1 : 0;
    i64 fy = card_y + 180 + (i64)field * 150;
    cursor_erase(fb);
    fill_rect(fb, card_x + 40, fy, 820, 44, card);
    draw_rounded_input(fb, card_x+40, fy, 820, 44, buf, mask, fg, card, 1);
    cursor_draw(fb, mx, my);
}

/* Returns: 0-2 = switch to that field, 3 = all done. */
static int read_field(u32 *fb, i64 card_x, i64 card_y, int field, char *pc, char *user, char *pass, i64 *mx, i64 *my, u32 bg, u32 fg) {
    char *buf = field==0 ? pc : (field==1 ? user : pass);
    int max = 31;
    int len = 0;
    while (buf[len]) len++;
    int shift = 0;
    int release = 0;
    int mcnt = 0;
    u8 mpkt[3];
    u8 prev_btns = 0;
    int mouse_log_cnt = 0;  /* limit mouse log to first 20 packets */
    redraw_setup_card(fb, card_x, card_y, pc, user, pass, field, bg, fg, *mx, *my);
    for (;;) {
        u8 st = inb(0x64);
        if (!(st & 1)) { __asm__("pause"); continue; }
        u8 data = inb(0x60);
        /* [debug-point b6-io] */
        dbgb("[RF]", st, data);
        /* [/debug-point] */
        if (st & 0x20) {
            /* mouse data: assemble 3-byte packet, sync on bit 3 */
            if (mcnt == 0 && !(data & 0x08)) continue;
            mpkt[mcnt++] = data;
            if (mcnt < 3) continue;
            mcnt = 0;
            i64 dx = (i64)(i8)mpkt[1];
            i64 dy = (i64)(i8)mpkt[2];
            u8 btns = mpkt[0] & 0x07;
            if (mouse_log_cnt < 20 && (dx != 0 || dy != 0 || btns)) {
                logl("[mouse] pkt");
                mouse_log_cnt++;
            }
            if (dx != 0 || dy != 0) {
                cursor_erase(fb);
                *mx += dx;
                *my -= dy;  /* invert Y: mouse up = screen up */
                if (*mx < 0) *mx = 0;
                if (*my < 0) *my = 0;
                if ((u64)*mx >= fb_w) *mx = (i64)fb_w - 1;
                if ((u64)*my >= fb_h) *my = (i64)fb_h - 1;
                cursor_draw(fb, *mx, *my);
            }
            /* left click (rising edge) — switch to clicked input field */
            if ((btns & 1) && !(prev_btns & 1)) {
                for (int f = 0; f < 3; f++) {
                    i64 fy = card_y + 180 + (i64)f * 150;
                    if (*my >= fy && *my < fy + 44 && *mx >= card_x + 40 && *mx < card_x + 860) {
                        buf[len] = 0;
                        if (f != field) return f;
                        break;
                    }
                }
            }
            prev_btns = btns;
            continue;
        }
        /* keyboard data: PS/2 scan code set 1 (translation enabled) */
        u8 sc = data;
        if (sc == 0xF0) { release = 1; continue; }  /* tolerate untranslated Set2 release */
        if (release) { release = 0; continue; }
        if (sc == 0x2A || sc == 0x36) { shift = 1; continue; }
        if (sc == 0xAA || sc == 0xB6) { shift = 0; continue; }
        if (sc & 0x80) continue;
        /* BUG-010 防御: scan_to_ascii 内部有 sc>=58 返回 0，
         * 但防止未来变更导致越界访问 */
        if (sc >= 58) continue;
        char c = scan_to_ascii(sc, shift);
        if (!c) continue;
        if (c == '\n') {
            buf[len] = 0;
            return field < 2 ? field + 1 : 3;
        }
        if (c == 8) {
            if (len > 0) buf[--len] = 0;
        } else if (len < max-1 && c >= 32 && c <= 126) {
            buf[len++] = c;
            buf[len] = 0;
        }
        redraw_input_only(fb, card_x, card_y, field, pc, user, pass, *mx, *my, fg);
    }
}

typedef struct {
    int language;
    int region;
    int timezone;
    int keyboard;
    int theme;
} setup_prefs;

/* 语言选项: 索引 0-11 */
static const char *lang_names[] = {
    "Simplified Chinese", "English (US)", "English (UK)", "Japanese",
    "Korean", "French", "German", "Spanish",
    "Portuguese", "Russian", "Arabic", "Hindi"
};
#define LANG_COUNT 12

/* 键盘布局选项: 索引 0-8 */
static const char *kb_names[] = {
    "US-QWERTY", "CN-QWERTY", "UK-QWERTY", "JP-JIS",
    "KR-104", "FR-AZERTY", "DE-QWERTZ", "ES-QWERTY",
    "RU-JCUKEN"
};
#define KB_COUNT 9

/* 区域选项 */
static const char *region_names[] = {
    "China Mainland", "Global", "United States", "United Kingdom",
    "Japan", "Korea", "France", "Germany",
    "Spain", "Russia"
};
#define REGION_COUNT 10

/* 时区选项 */
static const char *tz_names[] = {
    "Asia/Shanghai", "UTC", "America/New_York", "America/Los_Angeles",
    "Europe/London", "Asia/Tokyo", "Asia/Seoul", "Europe/Paris",
    "Europe/Berlin", "Europe/Moscow"
};
#define TZ_COUNT 10

/* 主题选项 */
static const char *theme_names[] = { "Light", "Dark", "Auto" };
#define THEME_COUNT 3

static int pref_option_count(int row) {
    if (row == 0) return LANG_COUNT;
    if (row == 1) return REGION_COUNT;
    if (row == 2) return TZ_COUNT;
    if (row == 3) return KB_COUNT;
    if (row == 4) return THEME_COUNT;
    return 2;
}

static const char *pref_option_name(int row, int opt) {
    if (row == 0) return lang_names[opt];
    if (row == 1) return region_names[opt];
    if (row == 2) return tz_names[opt];
    if (row == 3) return kb_names[opt];
    if (row == 4) return theme_names[opt];
    return "";
}

static const char *pref_value(int row, int val) {
    return pref_option_name(row, val);
}

static int pref_get(const setup_prefs *p, int row) {
    if (row == 0) return p->language;
    if (row == 1) return p->region;
    if (row == 2) return p->timezone;
    if (row == 3) return p->keyboard;
    if (row == 4) return p->theme;
    return 0;
}

static void pref_set(setup_prefs *p, int row, int val) {
    if (row == 0) p->language = val;
    else if (row == 1) p->region = val;
    else if (row == 2) p->timezone = val;
    else if (row == 3) p->keyboard = val;
    else if (row == 4) p->theme = val;
}

static void pref_toggle(setup_prefs *p, int row) {
    int count = pref_option_count(row);
    int cur = pref_get(p, row);
    pref_set(p, row, (cur + 1) % count);
}

static void draw_option_row(u32 *fb, i64 x, i64 y, i64 w, i64 h,
                            const u8 *label, i64 lw, i64 lh,
                            const char *value, int active, u32 fg, u32 card,
                            const char *en) {
    u32 fill = active ? 0xFF45454Au : INPUT_BG;
    fb_bitmap_alpha(fb, label, lw, lh, x, y + 4, fg, card, 255);
    if (en) fb_text(fb, en, x, y + 4 + lh + 1, 0xFF9098A0, card);
    fill_rounded_rect(fb, x + 220, y, w - 220, h, 2, fill);
    stroke_rounded_rect(fb, x + 220, y, w - 220, h, 2, 1, active ? 0xFF6C6C72 : 0xFF404044);
    fb_text(fb, value, x + 238, y + (h - ASCII_H) / 2, fg, fill);
    /* dropdown arrow */
    {
        i64 ax = x + w - 22;
        i64 ay = y + (h - 6) / 2;
        fill_rect(fb, ax,     ay,     11, 1, fg);
        fill_rect(fb, ax + 1, ay + 1,  9, 1, fg);
        fill_rect(fb, ax + 2, ay + 2,  7, 1, fg);
        fill_rect(fb, ax + 3, ay + 3,  5, 1, fg);
        fill_rect(fb, ax + 4, ay + 4,  3, 1, fg);
        fill_rect(fb, ax + 5, ay + 5,  1, 1, fg);
    }
}

static void redraw_prefs_card(u32 *fb, i64 card_x, i64 card_y, setup_prefs *prefs, int active, i64 mx, i64 my, u32 bg, u32 fg) {
    u32 card = CARD_BG;
    cursor_erase(fb);
    fill_gradient_rect(fb, card_x - 24, card_y - 24, 948, 648);
    draw_card(fb, card_x, card_y, 900, 600, card, bg);
    fb_bitmap_alpha(fb, g_txt_prefs_title, g_txt_prefs_title_w, g_txt_prefs_title_h, card_x+40, card_y+40, fg, card, 255);
    fb_text(fb, "Preferences", card_x+40, card_y+40+g_txt_prefs_title_h+2, 0xFF9098A0, card);
    draw_option_row(fb, card_x+40, card_y+120, 820, 42, g_txt_prefs_language, g_txt_prefs_language_w, g_txt_prefs_language_h, pref_value(0, pref_get(prefs, 0)), active==0, fg, card, "Language");
    draw_option_row(fb, card_x+40, card_y+200, 820, 42, g_txt_prefs_region, g_txt_prefs_region_w, g_txt_prefs_region_h, pref_value(1, pref_get(prefs, 1)), active==1, fg, card, "Region");
    draw_option_row(fb, card_x+40, card_y+280, 820, 42, g_txt_prefs_timezone, g_txt_prefs_timezone_w, g_txt_prefs_timezone_h, pref_value(2, pref_get(prefs, 2)), active==2, fg, card, "Timezone");
    draw_option_row(fb, card_x+40, card_y+360, 820, 42, g_txt_prefs_keyboard, g_txt_prefs_keyboard_w, g_txt_prefs_keyboard_h, pref_value(3, pref_get(prefs, 3)), active==3, fg, card, "Keyboard");
    draw_option_row(fb, card_x+40, card_y+440, 820, 42, g_txt_prefs_theme, g_txt_prefs_theme_w, g_txt_prefs_theme_h, pref_value(4, pref_get(prefs, 4)), active==4, fg, card, "Theme");
    fb_bitmap_alpha(fb, g_txt_prefs_hint, g_txt_prefs_hint_w, g_txt_prefs_hint_h, card_x+40, card_y+555, 0xFF7F97AC, card, 255);
    fb_text(fb, "Tab/arrows to navigate, Enter to confirm", card_x+40, card_y+555+g_txt_prefs_hint_h+2, 0xFF6F7F8C, card);
    cursor_draw(fb, mx, my);
}

static void redraw_prefs_row(u32 *fb, i64 card_x, i64 card_y, setup_prefs *prefs, int row, int active, i64 mx, i64 my, u32 fg) {
    u32 card = CARD_BG;
    i64 ry = card_y + 120 + (i64)row * 80;
    cursor_erase(fb);
    fill_rounded_rect(fb, card_x + 36, ry - 8, 832, 58, 2, card);
    if (row == 0) draw_option_row(fb, card_x+40, ry, 820, 42, g_txt_prefs_language, g_txt_prefs_language_w, g_txt_prefs_language_h, pref_value(0, pref_get(prefs, 0)), active==0, fg, card, "Language");
    else if (row == 1) draw_option_row(fb, card_x+40, ry, 820, 42, g_txt_prefs_region, g_txt_prefs_region_w, g_txt_prefs_region_h, pref_value(1, pref_get(prefs, 1)), active==1, fg, card, "Region");
    else if (row == 2) draw_option_row(fb, card_x+40, ry, 820, 42, g_txt_prefs_timezone, g_txt_prefs_timezone_w, g_txt_prefs_timezone_h, pref_value(2, pref_get(prefs, 2)), active==2, fg, card, "Timezone");
    else if (row == 3) draw_option_row(fb, card_x+40, ry, 820, 42, g_txt_prefs_keyboard, g_txt_prefs_keyboard_w, g_txt_prefs_keyboard_h, pref_value(3, pref_get(prefs, 3)), active==3, fg, card, "Keyboard");
    else if (row == 4) draw_option_row(fb, card_x+40, ry, 820, 42, g_txt_prefs_theme, g_txt_prefs_theme_w, g_txt_prefs_theme_h, pref_value(4, pref_get(prefs, 4)), active==4, fg, card, "Theme");
    cursor_draw(fb, mx, my);
}

/* 绘制下拉菜单：最多显示 6 项，超出时显示滚动指示 */
static void draw_dropdown_menu(u32 *fb, i64 card_x, i64 card_y, int row, const setup_prefs *prefs, u32 fg) {
    i64 dd_x = card_x + 260;
    i64 dd_y = card_y + 120 + (i64)row * 80 + 42;
    i64 dd_w = 600;
    int current = pref_get(prefs, row);
    int opt_count = pref_option_count(row);
    int visible = opt_count > 6 ? 6 : opt_count;
    i64 opt_h = 32;
    i64 total_h = visible * opt_h + (opt_count > 6 ? 20 : 0);
    /* 深色背景 */
    fill_rect(fb, dd_x, dd_y, dd_w, total_h, 0xFF2A2A2E);
    /* 选项 */
    for (int opt = 0; opt < visible; opt++) {
        u32 bg_opt = (opt == current) ? 0xFF45454A : 0xFF2A2A2E;
        fill_rect(fb, dd_x, dd_y + opt * opt_h, dd_w, opt_h, bg_opt);
        fb_text(fb, pref_option_name(row, opt), dd_x + 18, dd_y + opt * opt_h + (opt_h - ASCII_H) / 2, fg, bg_opt);
    }
    /* 超过 6 项时显示滚动指示 */
    if (opt_count > 6) {
        fill_rect(fb, dd_x, dd_y + visible * opt_h, dd_w, 20, 0xFF252528);
        fb_text(fb, "...more", dd_x + 18, dd_y + visible * opt_h + 1, 0xFF808088, 0xFF252528);
    }
    /* 边框 */
    fill_rect(fb, dd_x, dd_y, dd_w, 1, 0xFF505055);
    fill_rect(fb, dd_x, dd_y + total_h - 1, dd_w, 1, 0xFF505055);
    fill_rect(fb, dd_x, dd_y, 1, total_h, 0xFF505055);
    fill_rect(fb, dd_x + dd_w - 1, dd_y, 1, total_h, 0xFF505055);
}

static void read_prefs_page(u32 *fb, i64 card_x, i64 card_y, setup_prefs *prefs, i64 *mx, i64 *my, u32 bg, u32 fg) {
    int active = 0;
    int mcnt = 0;
    u8 mpkt[3];
    u8 prev_btns = 0;
    int e0 = 0;
    int dropdown_open = -1;
    redraw_prefs_card(fb, card_x, card_y, prefs, active, *mx, *my, bg, fg);
    for (;;) {
        u8 st = inb(0x64);
        if (!(st & 1)) { __asm__("pause"); continue; }
        u8 data = inb(0x60);
        /* [debug-point b6-io] */
        dbgb("[PR]", st, data);
        /* [/debug-point] */
        if (st & 0x20) {
            if (mcnt == 0 && !(data & 0x08)) continue;
            mpkt[mcnt++] = data;
            if (mcnt < 3) continue;
            mcnt = 0;
            i64 dx = (i64)(i8)mpkt[1];
            i64 dy = (i64)(i8)mpkt[2];
            u8 btns = mpkt[0] & 0x07;
            if (dx || dy) {
                cursor_erase(fb);
                *mx += dx; *my -= dy;
                if (*mx < 0) *mx = 0;
                if (*my < 0) *my = 0;
                if ((u64)*mx >= fb_w) *mx = (i64)fb_w - 1;
                if ((u64)*my >= fb_h) *my = (i64)fb_h - 1;
                cursor_draw(fb, *mx, *my);
            }
            if ((btns & 1) && !(prev_btns & 1)) {
                if (dropdown_open >= 0) {
                    /* 检查点击是否在下拉选项上 */
                    i64 dd_x = card_x + 260;
                    i64 dd_y = card_y + 120 + (i64)dropdown_open * 80 + 42;
                    i64 dd_w = 600;
                    int opt_count = pref_option_count(dropdown_open);
                    int visible = opt_count > 6 ? 6 : opt_count;
                    i64 opt_h = 32;
                    int clicked_opt = -1;
                    for (int opt = 0; opt < visible; opt++) {
                        i64 opt_y = dd_y + opt * opt_h;
                        if (*mx >= dd_x && *mx < dd_x + dd_w && *my >= opt_y && *my < opt_y + opt_h) {
                            clicked_opt = opt;
                            break;
                        }
                    }
                    if (clicked_opt >= 0) {
                        pref_set(prefs, dropdown_open, clicked_opt);
                    }
                    dropdown_open = -1;
                    redraw_prefs_card(fb, card_x, card_y, prefs, active, *mx, *my, bg, fg);
                } else {
                    /* No dropdown open - check for value area clicks */
                    for (int r = 0; r < 5; r++) {
                        i64 ry = card_y + 120 + (i64)r * 80;
                        if (*mx >= card_x + 260 && *mx < card_x + 860 && *my >= ry && *my < ry + 42) {
                            active = r;
                            dropdown_open = r;
                            redraw_prefs_card(fb, card_x, card_y, prefs, active, *mx, *my, bg, fg);
                            cursor_erase(fb);
                            draw_dropdown_menu(fb, card_x, card_y, r, prefs, fg);
                            cursor_draw(fb, *mx, *my);
                            break;
                        }
                    }
                }
            }
            prev_btns = btns;
            continue;
        }
        u8 sc = data;
        if (sc == 0xE0) { e0 = 1; continue; }
        if (sc & 0x80) { e0 = 0; continue; }
        /* Close dropdown on any real key press */
        if (dropdown_open >= 0) {
            dropdown_open = -1;
            redraw_prefs_card(fb, card_x, card_y, prefs, active, *mx, *my, bg, fg);
            e0 = 0;
            continue;
        }
        if (e0 && sc == 0x48) {
            if (active > 0) { int old = active; active--; redraw_prefs_row(fb, card_x, card_y, prefs, old, active, *mx, *my, fg); redraw_prefs_row(fb, card_x, card_y, prefs, active, active, *mx, *my, fg); }
            e0 = 0; continue;
        }
        if (e0 && sc == 0x50) {
            if (active < 4) { int old = active; active++; redraw_prefs_row(fb, card_x, card_y, prefs, old, active, *mx, *my, fg); redraw_prefs_row(fb, card_x, card_y, prefs, active, active, *mx, *my, fg); }
            e0 = 0; continue;
        }
        e0 = 0;
        if (sc == 0x39 || sc == 0x4D || sc == 0x4B) {
            pref_toggle(prefs, active);
            redraw_prefs_row(fb, card_x, card_y, prefs, active, active, *mx, *my, fg);
            continue;
        }
        if (sc == 0x1C) {
            if (active < 4) {
                int old = active;
                active++;
                redraw_prefs_row(fb, card_x, card_y, prefs, old, active, *mx, *my, fg);
                redraw_prefs_row(fb, card_x, card_y, prefs, active, active, *mx, *my, fg);
            } else {
                cursor_bg_valid = 0;
                cursor_cur_x = -100;
                cursor_cur_y = -100;
                return;
            }
        }
    }
}

typedef struct {
    int selected;
    int connected;
    char ssid[32];
    char password[64];
} setup_net;

/* UI 显示行数（固定 4 行布局）；真实扫描结果数由 g_wifi_scan_count 决定。 */
#define WIFI_SCAN_COUNT 4
#define WIFI_MAX_RESULTS 8

/* 真实 WiFi 扫描结果（来自 ath9k 驱动通过 kernel_api.net.scan_* 查询）。 */
static char g_wifi_ssids[WIFI_MAX_RESULTS][34];
static char g_wifi_meta[WIFI_MAX_RESULTS][20];
static int  g_wifi_scan_count;
static int  g_wifi_scan_tried;  /* 是否已尝试查询扫描结果 */
static int  g_no_wireless_device;  /* 检测到无无线设备时跳过 WiFi 页面 */

/* dkm_net_api 函数指针类型（通过偏移访问 kernel_api.net）。 */
typedef u32  (*net_device_count_fn)(void);
typedef int  (*net_device_info_fn)(u32 index, void *out);
typedef int  (*net_scan_start_fn)(u32 index);
typedef int  (*net_scan_count_fn)(u32 index);
typedef int  (*net_scan_result_fn)(u32 index, u32 n, void *out);
typedef int  (*net_is_wireless_fn)(u32 index);

/* dkm_net_scan_result 布局（须与 UTSM/include/utsm/net.h 一致）。 */
typedef struct {
    char ssid[33];
    u8 bssid[6];
    u8 channel;
    i8 rssi;
    u8 security;
} net_scan_result;

#define DKM_NET_SEC_OPEN 0
#define DKM_NET_SEC_WEP  1
#define DKM_NET_SEC_WPA  2
#define DKM_NET_SEC_WPA2 3
#define DKM_NET_SEC_WPA3 4

/* 把加密类型 + RSSI 转为 "WPA2 strong" 这种简短描述。 */
static void wifi_meta_str(char *out, u32 cap, u8 security, i8 rssi) {
    if (!cap) return;
    const char *sec;
    switch (security) {
        case DKM_NET_SEC_WPA2: sec = "WPA2"; break;
        case DKM_NET_SEC_WPA:  sec = "WPA "; break;
        case DKM_NET_SEC_WPA3: sec = "WPA3"; break;
        case DKM_NET_SEC_WEP:  sec = "WEP "; break;
        default:               sec = "Open"; break;
    }
    /* RSSI 分级：>=-50 strong, -51..-65 good, -66..-75 weak, else very weak */
    const char *q;
    i8 r = rssi;
    if (r >= -50) q = "strong";
    else if (r >= -65) q = "good";
    else if (r >= -75) q = "weak";
    else q = "very weak";
    /* 手动拼接 "SEC  quality" */
    u32 i = 0;
    while (sec[i] && i + 1 < cap) { out[i] = sec[i]; i++; }
    while (i < 5 && i + 1 < cap) { out[i++] = ' '; }
    u32 j = 0;
    while (q[j] && i + 1 < cap) { out[i++] = q[j++]; }
    out[i] = 0;
}

/* 通过 kernel_api 查询无线设备并读取扫描结果。
 * kernel_api 偏移布局：
 *   +0x48 net_api 指针
 * net_api 偏移布局（追加 scan 接口后）：
 *   +0  register_device, +8  device_count, +16 device_info,
 *   +24 tx, +32 rx_poll, +40 scan_start, +48 scan_count,
 *   +56 scan_result, +64 is_wireless
 */
static void wifi_query_scan(u64 kernel_api) {
    g_wifi_scan_tried = 1;
    g_wifi_scan_count = 0;
    if (!kernel_api) { logl("[FirstInit] no kernel_api, wifi scan skipped"); return; }
    u64 net_api = *(u64 *)(kernel_api + 0x48);
    if (!net_api) { logl("[FirstInit] no net_api, wifi scan skipped"); return; }
    net_device_count_fn dcount = (net_device_count_fn)*(u64 *)(net_api + 8);
    net_scan_start_fn   sstart = (net_scan_start_fn)*(u64 *)(net_api + 40);
    net_scan_count_fn   scount = (net_scan_count_fn)*(u64 *)(net_api + 48);
    net_scan_result_fn  sresult = (net_scan_result_fn)*(u64 *)(net_api + 56);
    net_is_wireless_fn  iswl = (net_is_wireless_fn)*(u64 *)(net_api + 64);
    if (!dcount) { logl("[FirstInit] net device_count fn is null"); return; }
    if (!sstart) { logl("[FirstInit] net scan_start fn is null"); return; }
    if (!scount) { logl("[FirstInit] net scan_count fn is null"); return; }
    if (!sresult) { logl("[FirstInit] net scan_result fn is null"); return; }
    if (!iswl) { logl("[FirstInit] net is_wireless fn is null"); return; }
    u32 ndev = dcount();
    /* 串口日志：设备数 */
    {
        char b[24]; int p=0;
        const char *s="[FirstInit] net devices=";
        while(s[p]) b[p]=s[p], p++;
        b[p++]='0'+(ndev/10); b[p++]='0'+(ndev%10); b[p]=0;
        logl(b);
    }
    /* 找第一个无线设备 - 通过 device_info 检查 flags 而非调用 is_wireless */
    int wifi_idx = -1;
    for (u32 i = 0; i < ndev; i++) {
        /* 使用 device_info 获取 flags，避免直接调用 is_wireless 导致崩溃 */
        u64 devinfo_fn = *(u64 *)(net_api + 16);
        if (devinfo_fn) {
            /* dkm_net_device_info 布局: name(8) + mac(6) + pad(2) + flags(4) = 20 字节 */
            u8 info[24];
            for (int k = 0; k < 24; k++) info[k] = 0;
            int rc = ((int (*)(u32, void *))devinfo_fn)(i, info);
            if (rc == 0) {
                u32 flags = *(u32 *)(info + 16);
                if (flags & 8) { wifi_idx = (int)i; break; }  /* DKM_NET_F_WIRELESS = (1<<3) */
            }
        }
    }
    if (wifi_idx < 0) {
        logl("[FirstInit] no wireless device found");
        g_no_wireless_device = 1;
        return;
    }
    /* ath9k 驱动在 driver_init 时已同步扫描，但保险起见若 count=0 再触发一次 */
    int count = scount((u32)wifi_idx);
    if (count <= 0 && sstart) {
        logl("[FirstInit] triggering wifi scan...");
        int rc = sstart((u32)wifi_idx);
        {
            char b[32]; int p=0;
            const char *s="[FirstInit] scan_start rc=";
            while(s[p]) b[p]=s[p], p++;
            if (rc < 0) { b[p++]='-'; b[p++]='0'+(-rc); } else { b[p++]='0'+rc; }
            b[p]=0; logl(b);
        }
        count = scount((u32)wifi_idx);
    }
    if (count <= 0) {
        logl("[FirstInit] wifi scan: no results");
        return;
    }
    if (count > WIFI_MAX_RESULTS) count = WIFI_MAX_RESULTS;
    /* 读取每个扫描结果 */
    for (int i = 0; i < count; i++) {
        net_scan_result r;
        for (u32 k = 0; k < sizeof(r); k++) ((u8 *)&r)[k] = 0;
        int rc = sresult((u32)wifi_idx, (u32)i, &r);
        if (rc != 0) continue;
        /* 拷贝 SSID */
        u32 j = 0;
        while (r.ssid[j] && j + 1 < sizeof(g_wifi_ssids[i])) {
            g_wifi_ssids[i][j] = r.ssid[j]; j++;
        }
        g_wifi_ssids[i][j] = 0;
        /* 生成 meta 字符串 */
        wifi_meta_str(g_wifi_meta[i], sizeof(g_wifi_meta[i]), r.security, r.rssi);
        g_wifi_scan_count++;
    }
    {
        char b[32]; int p=0;
        const char *s="[FirstInit] wifi scan results=";
        while(s[p]) b[p]=s[p], p++;
        b[p++]='0'+(g_wifi_scan_count/10); b[p++]='0'+(g_wifi_scan_count%10); b[p]=0;
        logl(b);
    }
}

static void net_copy(char *dst, u32 cap, const char *src) {
    if (!cap) return;
    u32 i = 0;
    while (src[i] && i + 1 < cap) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static void net_select(setup_net *net, int idx) {
    if (idx < 0) idx = 0;
    if (idx >= WIFI_SCAN_COUNT) idx = WIFI_SCAN_COUNT - 1;
    if (net->selected != idx) {
        net->password[0] = 0;
        net->connected = 0;
    }
    net->selected = idx;
    if (idx < g_wifi_scan_count) {
        net_copy(net->ssid, sizeof(net->ssid), g_wifi_ssids[idx]);
    } else {
        net->ssid[0] = 0;
    }
}

static void draw_wifi_row(u32 *fb, i64 card_x, i64 card_y, setup_net *net, int row, int active, u32 fg) {
    u32 card = CARD_BG;
    i64 x = card_x + 40;
    i64 y = card_y + 132 + (i64)row * 56;
    u32 fill = active == row ? 0xFF45454A : (net->selected == row ? 0xFF3A3A3E : INPUT_BG);
    fill_rounded_rect(fb, x, y, 390, 46, 3, fill);
    stroke_rounded_rect(fb, x, y, 390, 46, 3, 1, active == row ? 0xFF6C6C72 : 0xFF404044);
    fb_text(fb, net->selected == row ? ">" : " ", x + 12, y + 14, fg, fill);
    if (row < g_wifi_scan_count) {
        fb_text(fb, g_wifi_ssids[row], x + 42, y + 6, fg, fill);
        fb_text(fb, g_wifi_meta[row], x + 42, y + 26, 0xFF808088, fill);
    } else {
        const char *placeholder = (g_wifi_scan_tried && g_wifi_scan_count == 0)
            ? "No networks found" : "(empty)";
        fb_text(fb, placeholder, x + 42, y + 14, 0xFF606068, fill);
    }
}

/* 网络页面活跃元素索引 */
#define NET_ACT_WIFI_BASE 0
#define NET_ACT_SSID      4
#define NET_ACT_PASS      5
#define NET_ACT_SAVE      6
#define NET_ACT_CONNECT   7
#define NET_ACT_COUNT     8

static void redraw_network_card(u32 *fb, i64 card_x, i64 card_y, setup_net *net, int active, i64 mx, i64 my, u32 bg, u32 fg) {
    u32 card = CARD_BG;
    cursor_erase(fb);
    fill_gradient_rect(fb, card_x - 24, card_y - 24, 948, 648);
    draw_card(fb, card_x, card_y, 900, 600, card, bg);
    fb_bitmap_alpha(fb, g_txt_network_title, g_txt_network_title_w, g_txt_network_title_h, card_x+40, card_y+40, fg, card, 255);
    fb_text(fb, "Network", card_x+40, card_y+40+g_txt_network_title_h+2, 0xFF9098A0, card);
    fb_text(fb, "Choose or enter network", card_x + 40, card_y + 92, 0xFF808088, card);

    /* 左侧: WiFi 列表 */
    for (int r=0; r<WIFI_SCAN_COUNT; r++) draw_wifi_row(fb, card_x, card_y, net, r, active, fg);

    /* 右侧: 手动输入区 */
    fb_text(fb, "Network name (SSID)", card_x + 470, card_y + 120, 0xFF808088, card);
    draw_rounded_input(fb, card_x + 470, card_y + 148, 390, 44, net->ssid, 0, fg, card, active == NET_ACT_SSID);

    fb_text(fb, "Password", card_x + 470, card_y + 210, 0xFF808088, card);
    draw_rounded_input(fb, card_x + 470, card_y + 238, 390, 44, net->password, 1, fg, card, active == NET_ACT_PASS);

    /* 保存按钮 */
    {
        u32 btn_bg = active == NET_ACT_SAVE ? 0xFF45454A : 0xFF3A3A3E;
        fill_rounded_rect(fb, card_x + 470, card_y + 310, 180, 44, 3, btn_bg);
        stroke_rounded_rect(fb, card_x + 470, card_y + 310, 180, 44, 3, 1, active == NET_ACT_SAVE ? 0xFF6C6C72 : 0xFF404044);
        fb_text(fb, "Save", card_x + 530, card_y + 322, fg, btn_bg);
    }
    /* 连接按钮 */
    {
        u32 btn_bg = active == NET_ACT_CONNECT ? 0xFF4A6A8A : 0xFF3A5070;
        fill_rounded_rect(fb, card_x + 680, card_y + 310, 180, 44, 3, btn_bg);
        stroke_rounded_rect(fb, card_x + 680, card_y + 310, 180, 44, 3, 1, active == NET_ACT_CONNECT ? 0xFF6A8AAA : 0xFF404060);
        fb_text(fb, "Connect", card_x + 725, card_y + 322, 0xFFE0E0E0, btn_bg);
    }

    /* 状态提示 */
    if (net->connected) {
        fb_text(fb, "Network profile saved", card_x + 470, card_y + 380, 0xFF5FAF6F, card);
    } else if (net->ssid[0]) {
        fb_text(fb, "Press Save or Connect", card_x + 470, card_y + 380, 0xFF808088, card);
    } else {
        fb_text(fb, "Select network or type SSID", card_x + 470, card_y + 380, 0xFF606068, card);
    }
    fb_text(fb, "Save=save config only", card_x + 470, card_y + 510, 0xFF606068, card);
    fb_text(fb, "Connect=save and connect", card_x + 470, card_y + 535, 0xFF606068, card);

    cursor_draw(fb, mx, my);
}

static void read_network_page(u32 *fb, i64 card_x, i64 card_y, setup_net *net, i64 *mx, i64 *my, u32 bg, u32 fg) {
    int active = 0, mcnt = 0, e0 = 0, shift = 0;
    u8 mpkt[3]; u8 prev_btns = 0;
    int plen = 0;
    int slen = 0;
    while (net->password[plen]) plen++;
    while (net->ssid[slen]) slen++;
    net_select(net, net->selected);
    redraw_network_card(fb, card_x, card_y, net, active, *mx, *my, bg, fg);
    for (;;) {
        u8 st = inb(0x64);
        if (!(st & 1)) { __asm__("pause"); continue; }
        u8 data = inb(0x60);
        /* [debug-point b6-io] */
        dbgb("[NP]", st, data);
        /* [/debug-point] */
        if (st & 0x20) {
            if (mcnt == 0 && !(data & 0x08)) continue;
            mpkt[mcnt++] = data;
            if (mcnt < 3) continue;
            mcnt = 0;
            i64 dx = (i64)(i8)mpkt[1], dy = (i64)(i8)mpkt[2];
            u8 btns = mpkt[0] & 0x07;
            if (dx || dy) {
                cursor_erase(fb);
                *mx += dx; *my -= dy;
                if (*mx < 0) *mx = 0; if (*my < 0) *my = 0;
                if ((u64)*mx >= fb_w) *mx = (i64)fb_w - 1;
                if ((u64)*my >= fb_h) *my = (i64)fb_h - 1;
                cursor_draw(fb, *mx, *my);
            }
            if ((btns & 1) && !(prev_btns & 1)) {
                /* WiFi 列表点击 */
                for (int r=0; r<WIFI_SCAN_COUNT; r++) {
                    i64 ry = card_y + 132 + (i64)r * 56;
                    if (*mx >= card_x + 40 && *mx < card_x + 430 && *my >= ry && *my < ry + 46) {
                        active = r;
                        net_select(net, r);
                        slen = 0;
                        while (net->ssid[slen]) slen++;
                        plen = 0;
                        redraw_network_card(fb, card_x, card_y, net, active, *mx, *my, bg, fg);
                    }
                }
                /* SSID 输入框 */
                if (*mx >= card_x + 470 && *mx < card_x + 860 && *my >= card_y + 148 && *my < card_y + 192) {
                    active = NET_ACT_SSID;
                    redraw_network_card(fb, card_x, card_y, net, active, *mx, *my, bg, fg);
                }
                /* 密码输入框 */
                if (*mx >= card_x + 470 && *mx < card_x + 860 && *my >= card_y + 238 && *my < card_y + 282) {
                    active = NET_ACT_PASS;
                    redraw_network_card(fb, card_x, card_y, net, active, *mx, *my, bg, fg);
                }
                /* 保存按钮 */
                if (*mx >= card_x + 470 && *mx < card_x + 650 && *my >= card_y + 310 && *my < card_y + 354) {
                    net->connected = 1;
                    logl("[FirstInit] network profile saved");
                    redraw_network_card(fb, card_x, card_y, net, NET_ACT_SAVE, *mx, *my, bg, fg);
                    for (int i=0;i<30;i++) delay_frame();
                    return;
                }
                /* 连接按钮 */
                if (*mx >= card_x + 680 && *mx < card_x + 860 && *my >= card_y + 310 && *my < card_y + 354) {
                    net->connected = 1;
                    logl("[FirstInit] network connect requested");
                    redraw_network_card(fb, card_x, card_y, net, NET_ACT_CONNECT, *mx, *my, bg, fg);
                    for (int i=0;i<30;i++) delay_frame();
                    return;
                }
            }
            prev_btns = btns;
            continue;
        }
        u8 sc = data;
        if (sc == 0xE0) { e0 = 1; continue; }
        if (sc == 0x2A || sc == 0x36) { shift = 1; continue; }
        if (sc == 0xAA || sc == 0xB6) { shift = 0; continue; }
        if (sc & 0x80) { e0 = 0; continue; }
        if (e0 && sc == 0x48) {
            if (active > 0) active--;
            redraw_network_card(fb, card_x, card_y, net, active, *mx, *my, bg, fg);
            e0=0; continue;
        }
        if (e0 && sc == 0x50) {
            if (active < NET_ACT_COUNT - 1) active++;
            redraw_network_card(fb, card_x, card_y, net, active, *mx, *my, bg, fg);
            e0=0; continue;
        }
        e0 = 0;
        char c = scan_to_ascii(sc, shift);
        if (!c) continue;
        if (c == '\n') {
            if (active < WIFI_SCAN_COUNT) {
                net_select(net, active);
                slen = 0;
                while (net->ssid[slen]) slen++;
                active = NET_ACT_PASS;
            } else if (active == NET_ACT_SSID) {
                active = NET_ACT_PASS;
            } else if (active == NET_ACT_PASS) {
                active = NET_ACT_CONNECT;
            } else if (active == NET_ACT_SAVE) {
                net->connected = 1;
                logl("[FirstInit] network profile saved");
                redraw_network_card(fb, card_x, card_y, net, active, *mx, *my, bg, fg);
                for (int i=0;i<30;i++) delay_frame();
                return;
            } else if (active == NET_ACT_CONNECT) {
                net->connected = 1;
                logl("[FirstInit] network connect requested");
                redraw_network_card(fb, card_x, card_y, net, active, *mx, *my, bg, fg);
                for (int i=0;i<30;i++) delay_frame();
                return;
            }
            redraw_network_card(fb, card_x, card_y, net, active, *mx, *my, bg, fg);
            continue;
        }
        if (c == '\t') {
            active = (active + 1) % NET_ACT_COUNT;
            redraw_network_card(fb, card_x, card_y, net, active, *mx, *my, bg, fg);
            continue;
        }
        /* SSID 输入 */
        if (active == NET_ACT_SSID) {
            if (c == 8) {
                if (slen > 0) net->ssid[--slen] = 0;
            } else if (slen < 31 && c >= 32 && c <= 126) {
                net->ssid[slen++] = c;
                net->ssid[slen] = 0;
            }
            net->connected = 0;
            redraw_network_card(fb, card_x, card_y, net, active, *mx, *my, bg, fg);
        }
        /* 密码输入 */
        if (active == NET_ACT_PASS) {
            if (c == 8) {
                if (plen > 0) net->password[--plen] = 0;
            } else if (plen < 63 && c >= 32 && c <= 126) {
                net->password[plen++] = c;
                net->password[plen] = 0;
            }
            net->connected = 0;
            redraw_network_card(fb, card_x, card_y, net, active, *mx, *my, bg, fg);
        }
    }
}

/* compact SHA-256 */
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

static u8 g_conf_data[1024];
static u32 g_conf_size;

static void build_user_conf(const char *pc, const char *user, const char *pass, const setup_prefs *prefs, const setup_net *net) {
    u8 hash[32]; sha256(pass, hash);
    char *conf = (char *)g_conf_data; int p=0;
    const char *pre="DESHAB_USERCONF_V1\ncomputer=";
    for(int i=0;pre[i];i++) conf[p++]=pre[i];
    for(int i=0;pc[i]&&p<740;i++) conf[p++]=pc[i];
    const char *mid="\nusername="; for(int i=0;mid[i];i++) conf[p++]=mid[i];
    for(int i=0;user[i]&&p<740;i++) conf[p++]=user[i];
    const char *hs="\npasswordSha256="; for(int i=0;hs[i];i++) conf[p++]=hs[i];
    static const char hx[]="0123456789abcdef";
    for(int i=0;i<32;i++){conf[p++]=hx[hash[i]>>4];conf[p++]=hx[hash[i]&15];}
    const char *pref="\nlocale="; for(int i=0;pref[i];i++) conf[p++]=pref[i];
    const char *v0 = prefs->language ? "en-US" : "zh-CN"; for(int i=0;v0[i];i++) conf[p++]=v0[i];
    const char *r0="\nregion="; for(int i=0;r0[i];i++) conf[p++]=r0[i];
    const char *v1 = prefs->region ? "GLOBAL" : "CN"; for(int i=0;v1[i];i++) conf[p++]=v1[i];
    const char *t0="\ntimezone="; for(int i=0;t0[i];i++) conf[p++]=t0[i];
    const char *v2 = prefs->timezone ? "UTC" : "Asia/Shanghai"; for(int i=0;v2[i];i++) conf[p++]=v2[i];
    const char *k0="\nkeyboard="; for(int i=0;k0[i];i++) conf[p++]=k0[i];
    const char *v3 = prefs->keyboard ? "cn-qwerty" : "us-qwerty"; for(int i=0;v3[i];i++) conf[p++]=v3[i];
    const char *th0="\ntheme="; for(int i=0;th0[i];i++) conf[p++]=th0[i];
    const char *v4 = prefs->theme ? "dark" : "light"; for(int i=0;v4[i];i++) conf[p++]=v4[i];
    const char *nm="\nnetwork.mode="; for(int i=0;nm[i];i++) conf[p++]=nm[i];
    const char *nv0 = net->connected ? "wifi" : "disabled"; for(int i=0;nv0[i];i++) conf[p++]=nv0[i];
    const char *ns="\nnetwork.ssid="; for(int i=0;ns[i];i++) conf[p++]=ns[i];
    for(int i=0;net->ssid[i]&&p<740;i++) conf[p++]=net->ssid[i];
    const char *np="\nnetwork.password="; for(int i=0;np[i];i++) conf[p++]=np[i];
    for(int i=0;net->password[i]&&p<740;i++) conf[p++]=net->password[i];
    const char *ni="\nnetwork.ip=dhcp"; for(int i=0;ni[i];i++) conf[p++]=ni[i];
    const char *dns="\nnetwork.dns=auto"; for(int i=0;dns[i];i++) conf[p++]=dns[i];
    conf[p++]='\n';
    /* XOR encrypt buffer with hash-derived stream; DSK writes to FAT32 after return. */
    for(int i=0;i<p;i++) conf[i]^=hash[i&31];
    g_conf_size = (u32)p;
    logl("[FirstInit] user.conf built and encrypted");
}

__attribute__((visibility("default")))
void dsk_entry(const dsk_boot_context *ctx) {
    __asm__ volatile("cli");  /* prevent IRQ1 (ps2kbd) from racing with our polling */
    logl("[FirstInit] boot");
    logl("[FirstInit] calibrating TSC (实机 CPU 频率计算)");
    tsc_calibrate_fi();

    if (!ctx || ctx->magic != 0x44534B31424F4F54ULL) {
        logl("[FirstInit] bad context");
        for(;;) __asm__("hlt");
    }

    fb_a = ctx->framebuffer_address;
    fb_w = ctx->framebuffer_width;
    fb_h = ctx->framebuffer_height;
    fb_p = ctx->framebuffer_pitch;

    logl("[FirstInit] framebuffer info:");
    swrite("[FirstInit]   fb_addr="); { char b[19]; u64 v=fb_a; b[0]='0';b[1]='x';
      for(int j=15;j>=0;j--) b[2+15-j]="0123456789abcdef"[(v>>(j*4))&0xf]; b[18]=0; swrite(b); swrite("\n"); }
    swrite("[FirstInit]   fb_w="); { char b[19]; u64 v=fb_w; b[0]='0';b[1]='x';
      for(int j=15;j>=0;j--) b[2+15-j]="0123456789abcdef"[(v>>(j*4))&0xf]; b[18]=0; swrite(b); swrite("\n"); }
    swrite("[FirstInit]   fb_h="); { char b[19]; u64 v=fb_h; b[0]='0';b[1]='x';
      for(int j=15;j>=0;j--) b[2+15-j]="0123456789abcdef"[(v>>(j*4))&0xf]; b[18]=0; swrite(b); swrite("\n"); }

    u32 bg = BG_MID, fg = TEXT_FG;
    i64 cx=(i64)fb_w/2, cy=(i64)fb_h/2;
    u32 *fb = (u32 *)(u64)fb_a;
    fill_gradient_rect(fb, 0, 0, (i64)fb_w, (i64)fb_h);

    /* fade out loading ring */
    logl("[FirstInit] fading out ring");
    fade_ring(fb, cx, cy, bg);
    logl("[FirstInit] ring faded");

    /* center text: Chinese bitmaps + English subtitle below */
    i64 tw = g_txt_welcome_w, tx = cx - tw/2, ty = cy - g_txt_welcome_h/2;
    /* English subtitle sits below the Chinese bitmap, centered independently */
    const char *welcome_en = "Welcome to Deshab";
    i64 en_w = fb_text_width(welcome_en);
    i64 en_x = cx - en_w/2, en_y = ty + g_txt_welcome_h + 6;
    /* subtitle color: dimmer than WELCOME_FG so it reads as secondary */
    u32 sub_fg = 0xFFA8B8C8;

    logl("[FirstInit] fading in welcome");
    for (u32 a=0; a<=255; a+=17) {
        fill_gradient_rect(fb, tx-4, ty-4, tw+8, g_txt_welcome_h + 6 + ASCII_H + 4);
        fb_bitmap_alpha(fb, g_txt_welcome, tw, g_txt_welcome_h, tx, ty, WELCOME_FG, bg_at_y(ty + g_txt_welcome_h / 2), a);
        fb_text_alpha(fb, welcome_en, en_x, en_y, sub_fg, bg_at_y(en_y + ASCII_H/2), a);
        delay_frame();
    }
    logl("[FirstInit] welcome drawn");

    for (int i=0; i<90; i++) delay_frame();

    logl("[FirstInit] cross-fading to setup message");
    i64 tw2 = g_txt_setup_w, tx2 = cx - tw2/2;
    const char *setup_en = "Let's set up your system";
    i64 en2_w = fb_text_width(setup_en);
    i64 en2_x = cx - en2_w/2;
    i64 area_x = (tx2 < tx ? tx2 : tx);
    if (en_x < area_x) area_x = en_x;
    if (en2_x < area_x) area_x = en2_x;
    i64 area_right = tx + tw; if (tx2 + tw2 > area_right) area_right = tx2 + tw2;
    if (en_x + en_w > area_right) area_right = en_x + en_w;
    if (en2_x + en2_w > area_right) area_right = en2_x + en2_w;
    i64 area_w = area_right - area_x;
    i64 area_h = (g_txt_setup_h > g_txt_welcome_h ? g_txt_setup_h : g_txt_welcome_h) + 6 + ASCII_H + 8;

    for (u32 step=0; step<=15; step++) {
        u32 a2 = step * 17;
        u32 a1 = 255 - a2;
        fill_gradient_rect(fb, area_x-4, ty-4, area_w+8, area_h);
        fb_bitmap_alpha(fb, g_txt_welcome, tw, g_txt_welcome_h, tx, ty, WELCOME_FG, bg_at_y(ty + g_txt_welcome_h / 2), a1);
        fb_text_alpha(fb, welcome_en, en_x, en_y, sub_fg, bg_at_y(en_y + ASCII_H/2), a1);
        fb_bitmap_alpha(fb, g_txt_setup, tw2, g_txt_setup_h, tx2, ty, WELCOME_FG, bg_at_y(ty + g_txt_setup_h / 2), a2);
        fb_text_alpha(fb, setup_en, en2_x, en_y, sub_fg, bg_at_y(en_y + ASCII_H/2), a2);
        delay_frame();
    }
    logl("[FirstInit] setup message drawn");

    for (int i=0; i<35; i++) delay_frame();

    logl("[FirstInit] drawing account setup card");
    char pc[32]; char user[32]; char pass[32];
    pc[0]=0; user[0]=0; pass[0]=0;
    i64 card_x = cx - 450;
    i64 card_y = cy - 300;
    /* fade in card + title — per-row gradient blend eliminates flicker */
    logl("[FI] fadein start");
    /* Phase 1: fade in card background only (no title) — avoids title/card color mismatch flicker */
    for (u32 a=0; a<=255; a+=17) {
        draw_card_fade(fb, card_x, card_y, 900, 600, 4, a);
        delay_frame();
    }
    /* Phase 2: draw title once on fully-rendered card */
    {
        u32 title_bg = blend(bg_at_y(card_y + 40 + g_txt_title_h/2), CARD_BG, 255);
        fb_bitmap_alpha(fb, g_txt_title, g_txt_title_w, g_txt_title_h, card_x+40, card_y+40, fg, title_bg, 255);
        fb_text(fb, "Account Setup", card_x+40, card_y+40+g_txt_title_h+2, 0xFF9098A0, title_bg);
    }
    logl("[FI] fadein done");
    logl("[FirstInit] initializing PS/2 mouse");
    mouse_init();
    i64 mx = cx, my = cy;
    {
        int field = 0;
        for (;;) {
            while (field < 3) {
                field = read_field(fb, card_x, card_y, field, pc, user, pass, &mx, &my, bg, fg);
            }
            /* [debug-point b6-io] */
            logl("[RF] fields done, content:");
            logl(pc); logl(user); logl(pass);
            /* [/debug-point] */
            /* Validate: all fields must be non-empty */
            if (pc[0] != 0 && user[0] != 0 && pass[0] != 0) break;
            /* Show warning message on card (overwrite hint area) */
            cursor_erase(fb);
            {
                u32 card = CARD_BG;
                fill_rect(fb, card_x + 40, card_y + 550, 820, 45, card);
                fb_text(fb, "Please fill in all fields", card_x + 40, card_y + 555, 0xFFFF6B6B, card);
                fb_text(fb, "Press Enter to continue", card_x + 40, card_y + 575, 0xFFC0C0C0, card);
            }
            cursor_draw(fb, mx, my);  /* 重绘鼠标光标，避免校验失败时光标消失 */
            logl("[FI] validation failed: empty field(s)");
            /* 等待用户确认:Enter/任意键回到 field 0，或点击输入框切换字段。
             * 避免死等 Enter 导致用户点击/输入无响应。 */
            int next_field = 0;
            {
                int release = 0;
                int mcnt = 0;
                u8 mpkt[3];
                u8 prev_btns = 0;
                int done = 0;
                while (!done) {
                    u8 st = inb(0x64);
                    if (!(st & 1)) { __asm__("pause"); continue; }
                    u8 data = inb(0x60);
                    /* [debug-point b6-io] */
                    dbgb("[VW]", st, data);
                    /* [/debug-point] */
                    if (st & 0x20) {
                        /* 鼠标数据:组装 3 字节包，同步位 bit 3 */
                        if (mcnt == 0 && !(data & 0x08)) continue;
                        mpkt[mcnt++] = data;
                        if (mcnt < 3) continue;
                        mcnt = 0;
                        i64 dx = (i64)(i8)mpkt[1];
                        i64 dy = (i64)(i8)mpkt[2];
                        u8 btns = mpkt[0] & 0x07;
                        if (dx != 0 || dy != 0) {
                            cursor_erase(fb);
                            mx += dx;
                            my -= dy;  /* invert Y: 鼠标上移 = 屏幕上移 */
                            if (mx < 0) mx = 0;
                            if (my < 0) my = 0;
                            if ((u64)mx >= fb_w) mx = (i64)fb_w - 1;
                            if ((u64)my >= fb_h) my = (i64)fb_h - 1;
                            cursor_draw(fb, mx, my);
                        }
                        /* 左键点击输入框:切换到该字段重新输入 */
                        if ((btns & 1) && !(prev_btns & 1)) {
                            for (int f = 0; f < 3; f++) {
                                i64 fy = card_y + 180 + (i64)f * 150;
                                if (my >= fy && my < fy + 44 && mx >= card_x + 40 && mx < card_x + 860) {
                                    next_field = f;
                                    done = 1;
                                    break;
                                }
                            }
                        }
                        prev_btns = btns;
                        continue;
                    }
                    u8 sc = data;
                    if (sc == 0xF0) { release = 1; continue; }
                    if (release) { release = 0; continue; }
                    if (sc & 0x80) continue;  /* Set 1 release code */
                    /* Enter 或任意按键:退出等待，回到 field 0 重新输入 */
                    next_field = 0;
                    done = 1;
                }
            }
            /* Reset all fields and restart input from next_field.
             * read_field() will call redraw_setup_card() which fully
             * redraws the card (title, labels, inputs, hint) and cursor. */
            pc[0] = 0; user[0] = 0; pass[0] = 0;
            field = next_field;
        }
    }
    logl("[FI] all fields done");
    /* 重置光标状态，避免页面切换时崩溃 */
    cursor_bg_valid = 0;
    cursor_cur_x = -100;
    cursor_cur_y = -100;
    setup_prefs prefs;
    prefs.language = 0;  /* zh-CN */
    prefs.region = 0;    /* CN */
    prefs.timezone = 0;  /* Asia/Shanghai */
    prefs.keyboard = 0;  /* US-QWERTY */
    prefs.theme = 0;     /* Light */
    read_prefs_page(fb, card_x, card_y, &prefs, &mx, &my, bg, fg);
    /* 重置光标状态，避免页面切换时崩溃 */
    cursor_bg_valid = 0;
    cursor_cur_x = -100;
    cursor_cur_y = -100;
    /* 查询真实 WiFi 扫描结果（来自 ath9k 驱动通过 kernel_api.net.scan_*） */
    wifi_query_scan(ctx->dkm_kernel_api);
    setup_net net;
    net.selected = 0;
    net.connected = 0;
    net.ssid[0] = 0;
    net.password[0] = 0;
    /* 始终显示网络设置页面，即使无无线设备也展示页面让用户确认 */
    cursor_bg_valid = 0;
    cursor_cur_x = -100;
    cursor_cur_y = -100;
    logl("[FirstInit] entering network setup page");
    read_network_page(fb, card_x, card_y, &net, &mx, &my, bg, fg);
    logl("[FirstInit] network setup page returned");
    build_user_conf(pc, user, pass, &prefs, &net);

    /* Pass config buffer to DSK via boot context reserved fields */
    {
        dsk_boot_context *ctx_mut = (dsk_boot_context *)ctx;
        ctx_mut->reserved[0] = (u64)(u64*)g_conf_data;
        ctx_mut->reserved[1] = (u64)g_conf_size;
    }
    logl("[FirstInit] config ready for DSK to write");

    cursor_erase(fb);
    fill_gradient_rect(fb, card_x - 24, card_y - 24, 948, 648);
    fb_bitmap_alpha(fb, g_txt_done, g_txt_done_w, g_txt_done_h, cx - g_txt_done_w/2, cy - 30, fg, bg_at_y(cy), 255);
    /* English subtitle below the Chinese done bitmap */
    {   const char *done_en = "Setup Complete";
        i64 dw = fb_text_width(done_en);
        fb_text(fb, done_en, cx - dw/2, cy + 2, 0xFFA8B8C8, bg_at_y(cy + 10));
    }
    /* status line below the subtitle */
    {   const char *status = "System starting...";
        i64 sw = fb_text_width(status);
        fb_text(fb, status, cx - sw/2, cy + 28, 0xFF808088, bg_at_y(cy + 36));
    }
    logl("[FirstInit] user setup finished");

    /* 短暂停留后返回，由 DSK 接管加载 shell */
    for (int i = 0; i < 60; i++) delay_frame();
}
