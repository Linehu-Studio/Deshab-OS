/* desktop_app_linux.h — Deshab 应用 Linux 预览后端 (SDL2)
 *
 * 允许在 Linux 开发机上预览 Deshab-OS 应用的 UI 效果。
 * 使用方法：
 *   1. #
define DA_LINUX_PREVIEW 1 *   2. #include "desktop_app_linux.h"
 *   3. 实现 dsk_entry() 函数（与 Deshab-OS 版本相同）
 *   4. 编译：make -f Makefile.linux
 *
 * 编译后会在 X11/Wayland 窗口中显示应用 UI，可用于调试布局和渲染。
 */

#ifndef DESHAB_DESKTOP_APP_LINUX_H
#define DESHAB_DESKTOP_APP_LINUX_H

#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ===================================================================
 *  基础类型（与 desktop_app.h 一致）
 * =================================================================== */

typedef uint8_t      u8;
typedef uint16_t     u16;
typedef uint32_t     u32;
typedef uint64_t     u64;
typedef int64_t      i64;

/* ===================================================================
 *  颜色常量（与 desktop_app.h 一致）
 * =================================================================== */

#define DA_BG_PRIMARY    0xFF0A1428u
#define DA_BG_SECONDARY  0xFF0F1E38u
#define DA_BG_TERTIARY   0xFF162848u
#define DA_TEXT_PRIMARY   0xFFE8E8F0u
#define DA_TEXT_DIM       0xFF6A6A88u
#define DA_ACCENT         0xFF00A8CCu
#define DA_ACCENT_LIGHT   0xFF44CCF0u
#define DA_BORDER         0xFF284888u
#define DA_BORDER_FOCUS   0xFF44CCF0u
#define DA_CURSOR_COLOR   0xFF00A8CCu
#define DA_SUCCESS        0xFF40C880u
#define DA_ERROR          0xFFFF4466u
#define DA_WARNING        0xFFF0A030u

#define DA_TITLEBAR_H 28
#define DA_BORDER_W   2
#define DA_STATUSBAR_H 20

/* ===================================================================
 *  渲染上下文（SDL2 版本）
 * =================================================================== */

typedef struct {
         Uint32 *pixels;     /* SDL2 像素缓冲 */
    int width;
    int     height;
    int     pitch;       /* 行字节宽 */
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *texture;
} da_app_context;

/* ===================================================================
 *  ASCII 字体声明（需要从 ascii_bitmaps.c 包含）
 * =================================================================== */

/* 如果外部未定义字体，则声明 extern */
/* 在 notes_preview.c 中 include ascii_bitmaps.c 后会定义实际变量 */
#ifndef DA_FONT_DEFINED
extern const unsigned char *g_ascii[];
extern int g_ascii_w;
extern int g_ascii_h;
#else
/* 使用 extern 引用 ascii_bitmaps.c 中定义的 static 变量 */
/* 注意：ascii_bitmaps.c 需要改为非 static */
extern const unsigned char g_ascii[][11*18];
extern int g_ascii_w;
extern int g_ascii_h;
#endif

/* ===================================================================
 *  绘制原语（SDL2 实现）
 * =================================================================== */

/* 颜色混合 */
static inline Uint32 da_blend(Uint32 bg, Uint32 fg, Uint32 alpha) {
    Uint32 na = 256 - alpha;
    Uint32 r = ((bg & 0xFF) * na + (fg & 0xFF) * alpha) >> 8;
    Uint32 g = (((bg >> 8) & 0xFF) * na + ((fg >> 8) & 0xFF) * alpha) >> 8;
    Uint32 b = (((bg >> 16) & 0xFF) * na + ((fg >> 16) & 0xFF) * alpha) >> 8;
    return 0xFF000000u | (b << 16) | (g << 8) | r;
}

/* 像素写入（SDL2 锁定时） */
static inline void da_pixel(da_app_context *ac, int x, int y, Uint32 color) {
    if (x < 0 || x >= ac->width || y < 0 || y >= ac->height) return;
    ac->pixels[y * ac->width + x] = color;
}

/* 矩形填充 */
static inline void da_fill_rect(da_app_context *ac, int x, int y, int w, int h, Uint32 color) {
    for (int r = 0; r < h; r++) {
        for (int c = 0; c < w; c++) {
            int xx = x + c, yy = y + r;
            if (xx >= 0 && xx < ac->width && yy >= 0 && yy < ac->height) {
                ac->pixels[yy * ac->width + xx] = color;
            }
        }
    }
}

/* 全屏背景 */
static inline void da_fill_bg(da_app_context *ac, Uint32 color)   {
    memset(ac->pixels,0, (size_t)ac->width * ac->height *4);
    da_fill_rect(ac, 0, 0, ac->width, ac->height, color);
}

/* 圆角矩形（简化版） */
static inline void da_fill_rounded_rect(da_app_context *ac, int x, int y                                        , int w, int h,
 Uint32 color, int radius) {
    /* 中间区域 */
    da_fill_rect(ac, x + radius, y, w - 2 * radius, h, color);
    da_fill_rect(ac, x, y + radius, radius, h - 2 * radius, color);
    da_fill_rect(ac, x + w - radius, y + radius, radius, h - 2 * radius, color);
    /* 四个角（简化为填充） */
    for (int dy = 0; dy < radius; dy++) {
        for (int dx = 0; dx < radius; dx++) {
            if (dx * dx + dy * dy <= radius * radius) {
                da_pixel(ac, x + radius - 1 - dx, y + radius - 1 - dy, color);
                da_pixel(ac, x + w - radius + dx, y + radius - 1 - dy, color);
                da_pixel(ac, x + radius - 1 - dx, y + h - radius + dy, color);
                da_pixel(ac, x + w - radius + dx, y + h - radius + dy, color);
            }
        }
    }
}

/* 字符渲染 */
static inline void da_draw_char(da_app_context *ac, char ch, int x, int y, Uint32 fg, Uint32 bg) {
    Uint32 idx = (Uint32)(ch - ' ');
    if (idx > 94) idx = 0;
    if (!g_ascii[idx]) return;
    
    const unsigned char *glyph = g_ascii[idx];
    int gw = g_ascii_w, gh = g_ascii_h;
    
    for (int r = 0; r < gh; r++) {
        for (int c = 0; c < gw; c++) {
            unsigned char a = glyph[r * gw + c];
            if (a == 0) continue;
            int xx = x + c, yy = y + r;
            if (xx < 0 || xx >= ac->width || yy < 0 || yy >= ac->height) continue;
            
            Uint32 pixel = (a == 255) ? fg : da_blend(bg, fg, a);
            ac->pixels[yy * ac->width + xx] = pixel;
        }
    }
}

/* 字符串渲染 */
static inline void da_draw_string(da_app_context *ac, const char *s, int x, int y,
                                   Uint32 fg, Uint32 bg, int step) {
    int cx = x;
    while (*s) {
        da_draw_char(ac, *s, cx, y, fg, bg);
        cx += step;
        s ++;
    }
}

/*标题栏 */
static inline void da_draw_titlebar(da_app_context *ac, const char *title, int w) {
    da_fill_rect(ac, 0, 0, w, DA_TITLEBAR_H, DA_BG_SECONDARY);
    da_draw_string(ac, title, 8, (DA_TITLEBAR_H - 18) / 2, DA_TEXT_PRIMARY, DA_BG_SECONDARY, 12);
    /* 关闭按钮 */
    da_fill_rounded_rect(ac, w - 28, 4, 24, 20, DA_ERROR, 4);
    da_draw_string(ac, "X", w - 20, 5, 0xFF0A1428u, DA_ERROR, 12);
    /* 底部分隔线 */
    da_fill_rect(ac, 0, DA_TITLEBAR_H, w, 1, DA_ACCENT);
}

/* 状态栏 */
static inline void da_draw_statusbar(da_app_context *ac, const char *text, int w, int h) {
    int y = h - DA_STATUSBAR_H;
    da_fill_rect(ac, 0, y, w, DA_STATUSBAR_H, DA_BG_SECONDARY);
    da_draw_string(ac, text, 4, y + 1, DA_TEXT_DIM, DA_BG_SECONDARY, 12);
    da_fill_rect(ac, 0, y, w, 1, DA_BORDER);
}

/* ===================================================================
 *  上下文初始化/刷新
 * =================================================================== */

static inline int da_init_linux(da_app_context *ac, const char *title, int width, int height) {
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "SDL2 init failed: %s\n", SDL_GetError());
        return -1;
    }
    
                                      ac->window = SDL_CreateWindow(title,
 SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                   width, height,
                                   SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!ac->window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return -1;
    }
    
    ac->renderer = SDL_CreateRenderer(ac->window, -1, SDL_RENDERER_ACCELERATED);
    if (!ac->renderer) {
        fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        return -1;
    }
    

                                        ac->texture = SDL_CreateTexture(ac->renderer, SDL_PIXELFORMAT_ARGB8888,
                                     SDL_TEXTUREACCESS_STREAMING,
                                     width, height);
    if (!ac->texture) {
        fprintf(stderr, "SDL_CreateTexture failed: %s\n", SDL_GetError());
        return -1;
    }
    
    ac->width = width;
    ac->height = height;
    ac->pitch = width * 4;
    ac->pixels = (Uint32 *)malloc((size_t)width * height * 4);
    if (!ac->pixels) {
        fprintf(stderr, "malloc failed\n");

           return -1;
    } memset(ac->pixels, 0, (size_t)width * height * 4);
    
    return 0;
}

/* 刷新帧缓冲到屏幕 */
static inline void da_flush(da_app_context *ac) {
    SDL_UpdateTexture(ac->texture, NULL, ac->pixels, ac->pitch);
    SDL_RenderClear(ac->renderer);
    SDL_RenderCopy(ac->renderer, ac->texture, NULL, NULL);
    SDL_RenderPresent(ac->renderer);
}

/* 清理资源 */
static inline void da_cleanup(da_app_context *ac) {
    free(ac->pixels);
    if (ac->texture) SDL_DestroyTexture(ac->texture);
    if (ac->renderer) SDL_DestroyRenderer(ac->renderer);
    if (ac->window) SDL_DestroyWindow(ac->window);
    SDL_Quit();
}

/* 事件循环（等待关闭窗口） */
static inline void da_event_loop(void) {
    SDL_Event event;
    int running = 1;
    while (running) {
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) running = 0;
            if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE) running = 0;
        }
        SDL_Delay(16); /* ~60 FPS */
    }
}

/* ===================================================================
 *  鼠标光标（简化版）
 * =================================================================== */

#define DA_CURSOR_SIZE 24

static const unsigned char da_cursor_shape[24][24] = {
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
    int mx, my;
    int btn;
} da_cursor;

static inline void da_cursor_init(da_cursor *c, int fb_w, int fb_h) {
    c->mx = fb_w / 2;
    c->my = fb_h / 2;
    c->btn = 0;
}

static inline void da_cursor_draw(da_app_context *ac, da_cursor *c, Uint32 color) {
    for (int r = 0; r < DA_CURSOR_SIZE; r++) {
        for (int col = 0; col < DA_CURSOR_SIZE; col++) {
            if (da_cursor_shape[r][col]) {
                int x = c->mx + col, y = c->my + r;
                if (x >= 0 && x < ac->width && y >= 0 && y < ac->height) {
                    ac->pixels[y * ac->width + x] = color;
                }
            }
        }
    }
}

/* ===================================================================
 *  PS/2 鼠标模拟（SDL2 事件转换）
 * =================================================================== */

typedef struct {
    int has_packet;
    int dx, dy;
    int buttons;
} da_mouse;

static inline void da_mouse_init(da_mouse *m) {
    memset(m, 0, sizeof(*m));
}

/* 将 SDL 鼠标事件转换为内部状态 */
static inline void da_mouse_update(da_mouse *m, SDL_Event *event) {
    if (event->type == SDL_MOUSEMOTION) {
        m->dx += event->motion.xrel;
        m->dy += event->motion.yrel;
        m->has_packet = 1;
    }
    if (event->type == SDL_MOUSEBUTTONDOWN) { 
        m->buttons |= (1 << (event->button.button -1));
        m->has_packet = 1;
    }
    if (event->type == SDL_MOUSEBUTTONUP) {
        m->buttons &= ~(1 << (event->button.button - 1));
        m->has_packet = 1;
    }
}

/* ===================================================================
 *  PS/2 键盘模拟（SDL2 事件转换）
 * =================================================================== */

/* SDL scancode 到 Deshab PS/2 scan code 映射 */
static inline u8 da_sdl_to_ps2_scan(Uint32 sdl_scancode) {
    /* 简化映射：只支持基本字母数字键 */
    switch (sdl_scancode) {
        case SDLK_ESCAPE: return 0x01;
        case SDLK_1: return 0x02;
        case SDLK_2: return 0x03;
        case SDLK_3: return 0x04;
        case SDLK_4: return 0x05;
        case SDLK_5: return 0x06;
        case SDLK_6: return 0x07;
        case SDLK_7: return 0x08;
        case SDLK_8: return 0x09;
        case SDLK_9: return 0x0A;
        case SDLK_0: return 0x0B;
        case SDLK_MINUS: return 0x0C;
        case SDLK_EQUALS: return 0x0D;
        case SDLK_BACKSPACE: return 0x0E;
        case SDLK_TAB: return 0x0F;
        case SDLK_q: return 0x10;
        case SDLK_w: return 0x11;
        case SDLK_e: return 0x12;
        case SDLK_r: return 0x13;
        case SDLK_t: return 0x14;
        case SDLK_y: return 0x15;
        case SDLK_u: return 0x16;
        case SDLK_i: return 0x17;
        case SDLK_o: return 0x18;
        case SDLK_p: return 0x19;
        case SDLK_LEFTBRACKET: return 0x1A;
        case SDLK_RIGHTBRACKET: return 0x1B;
        case SDLK_RETURN: return 0x1C;
        case SDLK_a: return 0x1E;
        case SDLK_s: return 0x1F;
        case SDLK_d: return 0x20;
        case SDLK_f: return 0x21;
        case SDLK_g: return 0x22;
        case SDLK_h: return 0x23;
        case SDLK_j: return 0x24;
        case SDLK_k: return 0x25;
        case SDLK_l: return 0x26;
        case SDLK_SEMICOLON: return 0x27;
        case SDLK_QUOTE: return 0x28;
        case SDLK_BACKQUOTE: return 0x29;
        case SDLK_BACKSLASH: return 0x2B;
        case SDLK_z: return 0x2C;
        case SDLK_x: return 0x2D;
        case SDLK_c: return 0x2E;
        case SDLK_v: return 0x2F;
        case SDLK_b: return 0x30;
        case SDLK_n: return 0x31;
        case SDLK_m: return 0x32;
        case SDLK_COMMA: return 0x33;
        case SDLK_PERIOD: return 0x34;
        case SDLK_SLASH: return 0x35;
        case SDLK_RSHIFT:
        case SDLK_LSHIFT: return 0;  /* Shift 处理 */
        case SDLK_SPACE: return 0x39;
        case SDLK_LEFT: return 0x4B;
        case SDLK_RIGHT: return 0x4D;
        case SDLK_UP: return 0x48;
        case SDLK_DOWN: return 0x50;
        default: return 0;
    }
}

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

#endif /* DESHAB_DESKTOP_APP_LINUX_H */