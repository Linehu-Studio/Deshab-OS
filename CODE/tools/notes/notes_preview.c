/* notes_preview.c — Notes 应用的 Linux 预览版本
 *
 * 这个文件可以在 Linux 上编译预览 UI。
 * 使用自定义位图字体渲染字符。
 * 
 * 编译方式：
 *   gcc -o notes_preview notes_preview.c -I.. -lSDL2
 */

#define DA_LINUX_PREVIEW
#include "../desktop_app_linux.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

/* 全局状态 */
static da_app_context g_ac;
static int g_running = 1;

/* 文本内容 */
static char g_lines[512][256];
static int g_line_count = 0;
static int g_current_line = 0;
static int g_current_col = 0;
static int g_scroll_top = 0;
static int g_visible_rows;

/*  5x7 位图字体 (ASCII 32-126, 共95 个字符) */
static const unsigned char font_5x7[95][5] = {
    /* 0x20 space */
    {0x00, 0x00, 0x00, 0x00, 0x00},
    /* 0x21 ! */
    {0x00, 0x00, 0x30, 0x30, 0x00},
    /* 0x22 " */
    {0x30, 0x30, 0x00, 0x00, 0x00},
    /* 0x23 # */
    {0x18, 0x7E, 0x18, 0x7E, 0x18},
    /* 0x24 $ */
    {0x0C, 0x36, 0x6C, 0x36, 0x0C},
    /* 0x25 % */
    {0x60, 0x9C, 0x18, 0x36, 0x60},
    /* 0x26 & */
    {0x30, 0x7E, 0x1C, 0x36, 0x70},
    /* 0x27 ' */
    {0x30, 0x30, 0x00, 0x00, 0x00},
    /* 0x28 ( */
    {0x0C, 0x18, 0x30, 0x18, 0x0C},
    /* 0x29 ) */
    {0x30, 0x18, 0x0C, 0x18, 0x30},
    /* 0x2A * */
    {0x00, 0x18, 0x7E, 0x18, 0x00},
    /* 0x2B + */
    {0x00, 0x18, 0x7E, 0x18, 0x00},
    /* 0x2C , */
    {0x00, 0x00, 0x00, 0x30, 0x30},
    /* 0x2D - */
    {0x00, 0x00, 0x7E, 0x00, 0x00},
    /* 0x2E . */
    {0x00, 0x00, 0x00, 0x30, 0x30},
    /* 0x2F / */
    {0x06, 0x0C, 0x18, 0x30, 0x60},
    /* 0x30 0 */
    {0x38, 0x6C, 0x36, 0x6C, 0x38},
    /* 0x31 1 */
    {0x18, 0x38, 0x18, 0x18, 0x3C},
    /* 0x32 2 */
    {0x78, 0x60, 0x30, 0x18, 0x7C},
    /* 0x33 3 */
    {0x78, 0x60, 0x38, 0x00, 0x78},
    /* 0x34 4 */
    {0x0C, 0x1C, 0x3C, 0x7E, 0x0C},
    /* 0x35 5 */
    {0x7C, 0x60, 0x78, 0x06, 0x7C},
    /* 0x36 6 */
    {0x38, 0x60, 0x7C, 0x66, 0x3C},
    /* 0x37 7 */
    {0x7C, 0x60, 0x30, 0x30, 0x30},
    /* 0x38 8 */
    {0x3C, 0x66, 0x3C, 0x66, 0x3C},
    /* 0x39 9 */
    {0x3C, 0x66, 0x3E, 0x60, 0x3C},
    /* 0x3A : */
    {0x00, 0x30, 0x00, 0x30, 0x00},
    /* 0x3B ; */
    {0x00, 0x30, 0x00, 0x30, 0x30},
    /* 0x3C < */
    {0x0C, 0x18, 0x30, 0x18, 0x0C},
    /* 0x3D = */
    {0x00, 0x7E, 0x00, 0x7E, 0x00},
    /* 0x3E > */
    {0x30, 0x18, 0x0C, 0x18, 0x30},
    /* 0x3F ? */
    {0x78, 0x60, 0x30, 0x00, 0x30},
    /* 0x40 @ */
    {0x3C, 0x66, 0x7E, 0x06, 0x3C},
    /* 0x41 A */
    {0x18, 0x3C, 0x7E, 0x66, 0x66},
    /* 0x42 B */
    {0x7C, 0x66, 0x7C, 0x66, 0x7C},
    /* 0x43 C */
    {0x3C, 0x66, 0x60, 0x60, 0x3C},
    /* 0x44 D */
    {0x78, 0x6C, 0x66, 0x66, 0x78},
    /* 0x45 E */
    {0x7E, 0x60, 0x7C, 0x60, 0x7E},
    /* 0x46 F */
    {0x7E, 0x60, 0x7C, 0x60, 0x60},
    /* 0x47 G */
    {0x3C, 0x66, 0x6C, 0x66, 0x3C},
    /* 0x48 H */
    {0x66, 0x66, 0x7E, 0x66, 0x66},
    /* 0x49 I */
    {0x3C, 0x18, 0x18, 0x18, 0x3C},
    /* 0x4A J */
    {0x7C, 0x06, 0x06, 0x06, 0x3C},
    /* 0x4B K */
    {0x66, 0x6C, 0x78, 0x6C, 0x66},
    /* 0x4C L */
    {0x60, 0x60, 0x60, 0x60, 0x7E},
    /* 0x4D M */
    {0x63, 0x77, 0x7F, 0x6B, 0x63},
    /* 0x4E N */
    {0x66, 0x76, 0x7E, 0x6E, 0x66},
    /* 0x4F O */
    {0x3C, 0x66, 0x66, 0x66, 0x3C},
    /* 0x50 P */
    {0x7C, 0x66, 0x7C, 0x60, 0x60},
    /* 0x51 Q */
    {0x3C, 0x66, 0x76, 0x6C, 0x38},
    /* 0x52 R */
    {0x7C, 0x66, 0x7C, 0x6C, 0x66},
    /* 0x53 S */
    {0x3C, 0x66, 0x3C, 0x06, 0x7C},
    /* 0x54 T */
    {0x7E, 0x7E, 0x18, 0x18, 0x18},
    /* 0x55 U */
    {0x66, 0x66, 0x66, 0x66, 0x3C},
    /* 0x56 V */
    {0x66, 0x66, 0x66, 0x3C, 0x18},
    /* 0x57 W */
    {0x63, 0x63, 0x77, 0x7F, 0x36},
    /* 0x58 X */
    {0x66, 0x3C, 0x18, 0x3C, 0x66},
    /* 0x59 Y */
    {0x66, 0x66, 0x3C, 0x18, 0x18},
    /* 0x5A Z */
    {0x7C, 0x66, 0x3C, 0x18, 0x7C},
    /* 0x5B [ */
    {0x3C, 0x30, 0x30, 0x30, 0x3C},
    /* 0x5C backslash */
    {0x60, 0x30, 0x18, 0x0C, 0x06},
    /* 0x5D ] */
    {0x3C, 0x0C, 0x0C, 0x0C, 0x3C},
    /* 0x5E ^ */
    {0x18, 0x3C, 0x66, 0x00, 0x00},
    /* 0x5F _ */
    {0x00, 0x00, 0x00, 0x00, 0x7E},
    /* 0x60 ` */
    {0x30, 0x18, 0x00, 0x00, 0x00},
    /* 0x61 a */
    {0x00, 0x00, 0x3C, 0x66, 0x3C},
    /* 0x62 b */
    {0x60, 0x60, 0x7C, 0x66, 0x7C},
    /* 0x63 c */
    {0x00, 0x00, 0x3C, 0x66, 0x66},
    /* 0x64 d */
    {0x06, 0x06, 0x7E, 0x66, 0x7E},
    /* 0x65 e */
    {0x00, 0x00, 0x3C, 0x6E, 0x3C},
    /* 0x66 f */
    {0x30, 0x7C, 0x06, 0x06, 0x3C},
    /* 0x67 g */
    {0x00, 0x3E, 0x66, 0x66, 0x3C},
    /* 0x68 h */
    {0x60, 0x60, 0x7C, 0x66, 0x66},
    /* 0x69 i */
    {0x00, 0x18, 0x18, 0x00, 0x3C},
    /* 0x6A j */
    {0x06, 0x06, 0x06, 0x06, 0x3C},
    /* 0x6B k */
    {0x60, 0x6C, 0x78, 0x6C, 0x66},
    /* 0x6C l */
    {0x3C, 0x18, 0x18, 0x18, 0x3C},
    /* 0x6D m */
    {0x00, 0x00, 0x7E, 0x6E, 0x76},
    /* 0x6E n */
    {0x00, 0x00, 0x7C, 0x66, 0x66},
    /* 0x6F o */
    {0x00, 0x00, 0x3C, 0x66, 0x3C},
    /* 0x70 p */
    {0x00, 0x00, 0x7C, 0x66, 0x7C},
    /* 0x71 q */
    {0x00, 0x00, 0x7E, 0x66, 0x3E},
    /* 0x72 r */
    {0x00, 0x00, 0x7C, 0x60, 0x60},
    /* 0x73 s */
    {0x00, 0x00, 0x3C, 0x60, 0x3C},
    /* 0x74 t */
    {0x0C, 0x7E, 0x0C, 0x0C, 0x38},
    /* 0x75 u */
    {0x00, 0x00, 0x66, 0x66, 0x3C},
    /* 0x76 v */
    {0x00, 0x00, 0x66, 0x3C, 0x18},
    /* 0x77 w */
    {0x00, 0x00, 0x67, 0x67, 0x3E},
    /* 0x78 x */
    {0x00, 0x00, 0x66, 0x3C, 0x66},
    /* 0x79 y */
    {0x00, 0x00, 0x66, 0x66, 0x3C},
    /* 0x7A z */
    {0x00, 0x00, 0x7C, 0x7C, 0x78},
    /* 0x7B { */
    {0x0C, 0x18, 0x30, 0x18, 0x0C},
    /* 0x7C | */
    {0x30, 0x30, 0x30, 0x30, 0x30},
    /* 0x7D } */
    {0x30, 0x18, 0x0C, 0x18, 0x30},
    /* 0x7E ~ */
    {0x00, 0x00, 0x00, 0x00, 0x00},
};

#define FONT_W 5
#define FONT_H 7
#define FONT_STEP 9

/* 颜色常量 */
#define CURSOR_COLOR 0xFF0055FFu   /* 蓝色光标 */
#define BG_COLOR 0xFF1E1E1Eu       /* 深色背景 */
#define TEXT_COLOR 0xFFD4D4D4u     /* 浅色文字 */
#define DIM_COLOR 0xFF808080u      /* 灰色行号 */
#define STATUS_BG 0xFF007ACCu      /* 蓝色状态栏 */

/* 绘制单个字符（使用位图字体） */
static void draw_char_bitmap(char ch, int x, int y, u32 color) {
    if (ch < ' ' || ch > '~') return;
    int idx = ch - ' ';
    if (idx < 0 || idx >= 95) return;
    
    for (int row = 0; row < FONT_H; row++) {
        unsigned char bits = font_5x7[idx][row];
        for (int col = 0; col < FONT_W; col++) {
            if (bits & (1 << (FONT_W - 1 - col))) {
                da_fill_rect(&g_ac, x + col, y + row, 1, 1, color);
            }
        }
    }
}

/* 绘制文本行 */
static void draw_text_line(const char *text, int x, int y, u32 color) {
    int cx = x;
    while (*text) {
        draw_char_bitmap(*text, cx, y, color);
        cx += FONT_STEP;
        text++;
    }
}

/* 重绘所有 */
static void redraw_all(void) {
    da_fill_bg(&g_ac, BG_COLOR);
    
    int margin_x = 10;
    int top_y = 44;
    
    /* 计算可见列数 */
    int visible_cols = (g_ac.width - 20) / FONT_STEP;
    if (visible_cols < 10) visible_cols = 10;
    
    /* 绘制可见行 */
    for (int line = g_scroll_top; line < g_line_count && line < g_scroll_top + g_visible_rows; line++) {
        int screen_y = top_y + (line - g_scroll_top) * FONT_STEP;
        
        /* 行号 */
        char ln[8];
        int p = snprintf(ln, sizeof(ln), "%d ", line + 1);
        
        /* 行号区域背景 */
        int line_num_width = p * FONT_STEP;
        da_fill_rect(&g_ac, 0, screen_y - 2, line_num_width + 10, FONT_STEP, 0xFF252526u);
        
        /* 绘制行号 */
        draw_text_line(ln, 5, screen_y, DIM_COLOR);
        
        /* 文本内容 */
        const char *line_text = g_lines[line];
        int line_len = (int)strlen(line_text);
        int draw_len = line_len;
        if (draw_len > visible_cols) draw_len = visible_cols;
        
        int text_start_x = margin_x + line_num_width;
        for (int col = 0; col < draw_len; col++) {
            draw_char_bitmap(line_text[col], text_start_x + col * FONT_STEP, screen_y, TEXT_COLOR);
        }
    }
    
    /* 光标 */
    if (g_current_line >= g_scroll_top && g_current_line < g_scroll_top + g_visible_rows) {
        int screen_y = top_y + (g_current_line - g_scroll_top) * FONT_STEP;
        
        /* 计算行号宽度 */
        char tmp_ln[8];
        snprintf(tmp_ln, sizeof(tmp_ln), "%d ", g_current_line + 1);
        int line_num_width = (int)strlen(tmp_ln) * FONT_STEP;
        int text_start_x = margin_x + line_num_width;
        
        /* 高亮当前光标位置 */
        int cursor_x = text_start_x + g_current_col * FONT_STEP;
        da_fill_rect(&g_ac, cursor_x, screen_y, FONT_W, FONT_H, CURSOR_COLOR);
    }
    
    /* 状态栏 */
    char status[128];
    snprintf(status, sizeof(status), "Line: %d  Col: %d  Lines: %d  ESC=Exit",
             g_current_line + 1, g_current_col + 1, g_line_count);
    
    da_fill_rect(&g_ac, 0, g_ac.height - 24, g_ac.width, 24, STATUS_BG);
    draw_text_line(status, 10, g_ac.height - 22, 0xFFFFFFFFu);
    
    /* 标题栏 */
    da_fill_rect(&g_ac, 0, 0, g_ac.width, 36, 0xFF2D2D2Du);
    draw_text_line("Notes Preview (Deshab-OS)", 10, 6, 0xFFFFFFFFu);
    da_fill_rect(&g_ac, 0, 36, g_ac.width, 2, 0xFF007ACCu);
}

static void scroll_cursor_into_view(void) {
    if (g_current_line < g_scroll_top) {
        g_scroll_top = g_current_line;
    }
    if (g_current_line >= g_scroll_top + g_visible_rows) {
        g_scroll_top = g_current_line - g_visible_rows + 1;
    }
}

/* 换行 */
static void handle_enter(void) {
    if (g_current_line >= 511) return;
    
    int cur_len = (int)strlen(g_lines[g_current_line]);
    if (g_current_col < cur_len) {
        int remaining = cur_len - g_current_col;
        memmove(g_lines[g_current_line] + g_current_col + remaining, 
                g_lines[g_current_line] + g_current_col, remaining + 1);
        g_lines[g_current_line][g_current_col + remaining] = '\0';
    } else {
        g_lines[g_current_line][g_current_col] = '\0';
    }
    
    /* 移动下一行 */
    memmove(g_lines[g_current_line + 1], g_lines[g_current_line + 1], 256);
    g_line_count++;
    g_current_line++;
    g_current_col = 0;
    
    scroll_cursor_into_view();
}

/* Backspace删除 */
static void handle_backspace(void) {
    if (g_current_col > 0) {
        int len = (int)strlen(g_lines[g_current_line]);
        memmove(g_lines[g_current_line] + g_current_col - 1, 
                g_lines[g_current_line] + g_current_col, len - g_current_col + 1);
        g_current_col--;
    } else if (g_current_line > 0) {
        int prev_len = (int)strlen(g_lines[g_current_line - 1]);
        int cur_len = (int)strlen(g_lines[g_current_line]);
        if (prev_len + cur_len < 255) {
            memcpy(g_lines[g_current_line - 1] + prev_len, g_lines[g_current_line], cur_len + 1);
            for (int i = g_current_line; i < g_line_count - 1; i++) {
                memcpy(g_lines[i], g_lines[i+1], 256);
            }
            g_line_count--;
            g_current_line--;
            g_current_col = prev_len;
        }
    }
}

/* 插入字符 */
static void insert_char(char c) {
    if (c < 32 || c > 126) return;
    
    int len = (int)strlen(g_lines[g_current_line]);
    if (len >= 255) return;
    
    memmove(g_lines[g_current_line] + g_current_col + 1, 
            g_lines[g_current_line] + g_current_col, len - g_current_col + 1);
    g_lines[g_current_line][g_current_col] = c;
    g_current_col++;
}

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;
    
    const char *title = "Notes Preview (Deshab-OS) - ESC to exit";
    int width = 1024;
    int height = 768;
    
    if (da_init_linux(&g_ac, title, width, height) != 0) {
        return 1;
    }
    
    g_visible_rows = (height - 60 - 24) / FONT_STEP;
    if (g_visible_rows < 5) g_visible_rows = 5;
    
    /* 初始化空文档 */
    g_line_count = 1;
    g_lines[0][0] = '\0';
    
    redraw_all();
    da_flush(&g_ac);
    
    /* 主事件循环 */
    SDL_Event event;
    while (g_running) {
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                g_running = 0;
                break;
            }
            
            /* 处理文本输入事件（支持输入法） */
            if (event.type == SDL_TEXTINPUT) {
                for (int i = 0; event.text.text[i]; i++) {
                    unsigned char c = (unsigned char)event.text.text[i];
                    if (c >= 32 && c <= 126) {
                        insert_char((char)c);
                    }
                }
                scroll_cursor_into_view();
                redraw_all();
                da_flush(&g_ac);
            }
            
            if (event.type == SDL_KEYDOWN) {
                u8 sc = da_sdl_to_ps2_scan(event.key.keysym.sym);
                
                if (sc == 0x01) { /* Esc */
                    g_running = 0;
                } else if (sc == 0x1C) { /* Enter */
                    handle_enter();
                    redraw_all();
                    da_flush(&g_ac);
                } else if (sc == 0x0E) { /* Backspace */
                    handle_backspace();
                    redraw_all();
                    da_flush(&g_ac);
                } else if (sc == 0x4B) { /* Left */
                    if (g_current_col > 0) g_current_col--;
                    else if (g_current_line > 0) {
                        g_current_line--;
                        g_current_col = (int)strlen(g_lines[g_current_line]);
                    }
                    redraw_all();
                    da_flush(&g_ac);
                } else if (sc == 0x4D) { /* Right */
                    if (g_current_col < (int)strlen(g_lines[g_current_line])) g_current_col++;
                    else if (g_current_line < g_line_count - 1) {
                        g_current_line++;
                        g_current_col = 0;
                    }
                    redraw_all();
                    da_flush(&g_ac);
                } else if (sc == 0x48) { /* Up */
                    if (g_current_line > 0) g_current_line--;
                    if (g_current_col > (int)strlen(g_lines[g_current_line])) {
                        g_current_col = (int)strlen(g_lines[g_current_line]);
                    }
                    redraw_all();
                    da_flush(&g_ac);
                } else if (sc == 0x50) { /* Down */
                    if (g_current_line < g_line_count - 1) g_current_line++;
                    if (g_current_col > (int)strlen(g_lines[g_current_line])) {
                        g_current_col = (int)strlen(g_lines[g_current_line]);
                    }
                    redraw_all();
                    da_flush(&g_ac);
                }
            }
        }
        SDL_Delay(16);
    }
    
    da_cleanup(&g_ac);
    return 0;
}