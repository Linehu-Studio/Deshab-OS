/* apps_builtin.c — 内置应用 bash/editor/calc + FILES 面板（M0 自 main.c 平移）
 */
#include "desktop.h"

/* ---- FILES 面板全局状态 ---- */
fentry g_files[MAX_FILES];
int g_file_count = 0;
int g_file_sel = -1;
int g_files_loaded = 0;

/* ============================================================
 *  终端 (Bash) — 面板/窗口共用核心
 * ============================================================ */

const char *BASH_PROMPT = "deshab# ";

void bash_reset(bash_state *s, int w, int h) {
    for (int i = 0; i < TERM_MAX_CHARS; i++) {
        s->ch[i] = ' '; s->fg[i] = KS_TEXT_PRIMARY;
    }
    s->cols = (w - (int)DU_SPACE_SM * 2) / (int)DU_ASCII_STEP;
    s->rows = (h - (int)DU_SPACE_SM * 2) / (int)DU_ASCII_LINE_H;
    if (s->cols > TERM_MAX_COLS) s->cols = TERM_MAX_COLS;
    if (s->rows > TERM_MAX_ROWS) s->rows = TERM_MAX_ROWS;
    if (s->cols < 10) s->cols = 10;
    if (s->rows < 3) s->rows = 3;
    s->cur_col = 0; s->cur_row = 0;
    s->input_len = 0; s->input_cursor = 0;
    s->prompt_len = kstrlen(BASH_PROMPT);
    /* 初始化脏行标记：所有行标记为脏，确保首次渲染 */
    for (int i = 0; i < TERM_MAX_ROWS; i++) s->dirty_rows[i] = 1;
    s->any_dirty = 1;
}

static void bash_putc(bash_state *s, char c, u32 color) {
    if (c == '\n') {
        s->dirty_rows[s->cur_row] = 1;  /* 标记当前行为脏 */
        s->any_dirty = 1;
        s->cur_col = 0; s->cur_row++;
        return;
    }
    if (s->cur_row >= s->rows) {
        /* 滚动：所有行都需要重绘 */
        for (int r = 1; r < s->rows; r++) {
            for (int c2 = 0; c2 < s->cols; c2++) {
                s->ch[(r-1)*TERM_MAX_COLS+c2] = s->ch[r*TERM_MAX_COLS+c2];
                s->fg[(r-1)*TERM_MAX_COLS+c2] = s->fg[r*TERM_MAX_COLS+c2];
            }
        }
        for (int c2 = 0; c2 < s->cols; c2++) {
            s->ch[(s->rows-1)*TERM_MAX_COLS+c2] = ' ';
        }
        s->cur_row = s->rows - 1;
        /* 标记所有行为脏 */
        for (int r = 0; r < s->rows; r++) s->dirty_rows[r] = 1;
        s->any_dirty = 1;
    }
    if (s->cur_col >= s->cols) { s->cur_col = 0; s->cur_row++; }
    int idx = s->cur_row * TERM_MAX_COLS + s->cur_col;
    s->ch[idx] = (u8)c; s->fg[idx] = color;
    s->cur_col++;
    /* 标记当前行为脏 */
    s->dirty_rows[s->cur_row] = 1;
    s->any_dirty = 1;
}

void bash_puts(bash_state *s, const char *str, u32 color) {
    while (*str) bash_putc(s, *str++, color);
}

static void bash_execute(bash_state *s, const char *cmd) {
    while (*cmd == ' ') cmd++;
    bash_puts(s, BASH_PROMPT, KS_ACCENT2);
    bash_puts(s, cmd, KS_TEXT_PRIMARY);
    bash_putc(s, '\n', 0);

    if (*cmd == 0) return;
    if (cmd[0]=='h'&&cmd[1]=='e'&&cmd[2]=='l'&&cmd[3]=='p') {
        bash_puts(s, "  help    clear    echo    version\n", KS_ACCENT2);
        bash_puts(s, "  uname   about    reboot   halt\n", KS_ACCENT2);
        bash_puts(s, "  pwd     whoami   id\n", KS_ACCENT2);
    } else if (cmd[0]=='c'&&cmd[1]=='l'&&cmd[2]=='e'&&cmd[3]=='a'&&cmd[4]=='r') {
        for (int i = 0; i < TERM_MAX_CHARS; i++) { s->ch[i] = ' '; }
        s->cur_col = 0; s->cur_row = 0;
    } else if (cmd[0]=='v'&&cmd[1]=='e'&&cmd[2]=='r') {
        bash_puts(s, "Deshab OS v0.1.0 (Winux-Kate shell)\n", KS_ACCENT2);
    } else if (cmd[0]=='u'&&cmd[1]=='n'&&cmd[2]=='a') {
        bash_puts(s, "Deshab\n", KS_TEXT_PRIMARY);
    } else if (cmd[0]=='a'&&cmd[1]=='b'&&cmd[2]=='o'&&cmd[3]=='u'&&cmd[4]=='t') {
        bash_puts(s, "Deshab OS - SAS-R0 Kernel\n", KS_ACCENT);
        bash_puts(s, "Winux-Kate UI Structure\n", KS_ACCENT2);
    } else if (cmd[0]=='e'&&cmd[1]=='c'&&cmd[2]=='h'&&cmd[3]=='o') {
        const char *arg = cmd + 4;
        while (*arg == ' ') arg++;
        bash_puts(s, arg, KS_TEXT_PRIMARY);
        bash_putc(s, '\n', 0);
    } else if (cmd[0]=='p'&&cmd[1]=='w'&&cmd[2]=='d') {
        bash_puts(s, "/\n", KS_TEXT_PRIMARY);
    } else if (cmd[0]=='w'&&cmd[1]=='h'&&cmd[2]=='o') {
        bash_puts(s, "root\n", KS_TEXT_PRIMARY);
    } else if (cmd[0]=='i'&&cmd[1]=='d') {
        bash_puts(s, "uid=0(root) gid=0(root)\n", KS_TEXT_PRIMARY);
    } else if (cmd[0]=='r'&&cmd[1]=='e'&&cmd[2]=='b') {
        bash_puts(s, "Rebooting...\n", KS_ACCENT2);
        outb(0x64, 0xFE);
        for(;;) __asm__("hlt");
    } else if (cmd[0]=='h'&&cmd[1]=='a'&&cmd[2]=='l'&&cmd[3]=='t') {
        bash_puts(s, "Halted.\n", KS_ACCENT2);
        for(;;) __asm__("hlt");
    } else {
        bash_puts(s, "unknown: ", KS_DANGER);
        bash_puts(s, cmd, KS_DANGER);
        bash_putc(s, '\n', 0);
    }
}

void bash_key(bash_state *s, u8 sc, int shift) {
    if (sc == 0x1C) {
        s->input_buf[s->input_len] = 0;
        bash_execute(s, s->input_buf);
        s->input_len = 0; s->input_cursor = 0;
    } else if (sc == 0x0E) {
        if (s->input_cursor > 0) {
            for (int i = s->input_cursor-1; i < s->input_len-1; i++)
                s->input_buf[i] = s->input_buf[i+1];
            s->input_len--; s->input_cursor--;
        }
    } else {
        char c = scan_to_ascii(sc, shift);
        if (c && c >= 32 && c <= 126 && s->input_len < 255) {
            for (int i = s->input_len; i > s->input_cursor; i--)
                s->input_buf[i] = s->input_buf[i-1];
            s->input_buf[s->input_cursor++] = c;
            s->input_len++;
        }
    }
}

/* 绘制终端到指定矩形区域（面板或窗口客户区）
 * 性能优化：增量渲染，只绘制脏行 */
void bash_draw_to(bash_state *s, int cx, int cy, int cw, int ch) {
    /* 快速退出：终端内容未修改，跳过绘制 */
    if (!s->any_dirty) return;

    du_fill_rect(&g_fb, cx, cy, cw, ch, KS_BG_PRIMARY);

    for (int r = 0; r < s->rows; r++) {
        /* 增量渲染：只绘制脏行 */
        if (!s->dirty_rows[r]) continue;

        for (int c = 0; c < s->cols; c++) {
            int idx = r * TERM_MAX_COLS + c;
            if (s->ch[idx] == ' ') continue;
            int px = cx + (int)DU_SPACE_SM + c * (int)DU_ASCII_STEP;
            int py = cy + (int)DU_SPACE_SM + r * (int)DU_ASCII_LINE_H;
            if (px + (int)DU_ASCII_CELL_W > cx + cw) continue;
            if (py + (int)DU_ASCII_CELL_H > cy + ch) continue;
            du_draw_char(&g_fb, s->ch[idx], px, py, s->fg[idx], KS_BG_PRIMARY);
        }

        /* 清除脏行标记 */
        s->dirty_rows[r] = 0;
    }

    int input_y = cy + (int)DU_SPACE_SM + s->cur_row * (int)DU_ASCII_LINE_H;
    if (input_y + (int)DU_ASCII_CELL_H <= cy + ch) {
        du_draw_string(&g_fb, BASH_PROMPT,
                       cx + (int)DU_SPACE_SM, input_y,
                       KS_ACCENT2, KS_BG_PRIMARY, DU_ASCII_STEP);
        du_draw_string(&g_fb, s->input_buf,
                       cx + (int)DU_SPACE_SM + s->prompt_len * (int)DU_ASCII_STEP,
                       input_y,
                       KS_TEXT_PRIMARY, KS_BG_PRIMARY, DU_ASCII_STEP);
        int cursor_x = cx + (int)DU_SPACE_SM + (s->prompt_len + s->input_cursor) * (int)DU_ASCII_STEP;
        du_fill_rect(&g_fb, cursor_x, input_y + (int)DU_ASCII_CELL_H - 3,
                     (int)DU_ASCII_CELL_W, 2, KS_ACCENT);
    }

    /* 清除全局脏标记 */
    s->any_dirty = 0;
}

/* ---- 窗口适配 ---- */
__attribute__((unused))
static void *bash_on_create(app_ctx *ctx) {
    bash_state *s = (bash_state *)ADDR_TERMWIN;
    bash_reset(s, ctx->client_w, ctx->client_h);
    bash_puts(s, "Deshab Bash v0.1\nType help for commands\n\n", KS_TEXT_PRIMARY);
    return s;
}
__attribute__((unused))
static void bash_on_event(void *state, app_ctx *ctx, desktop_event *ev) {
    if (ev->type != EV_KEY_DOWN) return;
    bash_key((bash_state *)state, ev->scancode, ev->shift);
    ctx->win->dirty = 1;
}
__attribute__((unused))
static void bash_on_draw(void *state, app_ctx *ctx) {
    bash_draw_to((bash_state *)state, ctx->client_x, ctx->client_y,
                 ctx->client_w, ctx->client_h);
}
__attribute__((unused))
static void bash_on_destroy(void *state) { (void)state; }

/* ============================================================
 *  文本编辑器 (Editor) — 面板/IDE/窗口共用核心
 * ============================================================ */

static void editor_recalc_cursor(editor_state *s) {
    s->cursor_line = 0; s->cursor_col = 0;
    for (int i = 0; i < s->cursor_pos && i < s->text_len; i++) {
        if (s->text[i] == '\n') { s->cursor_line++; s->cursor_col = 0; }
        else s->cursor_col++;
    }
}

static int editor_line_start(editor_state *s, int line) {
    int l = 0;
    for (int i = 0; i < s->text_len; i++) {
        if (l == line) return i;
        if (s->text[i] == '\n') l++;
    }
    return s->text_len;
}

void editor_reset(editor_state *s, const char *sample) {
    s->text_len = 0; s->cursor_pos = 0;
    s->scroll_y = 0; s->modified = 0;
    s->has_file = 0;
    s->disp[0] = 0;
    while (*sample && s->text_len < EDITOR_BUF_SIZE - 1) {
        s->text[s->text_len++] = *sample++;
    }
    s->cursor_pos = s->text_len;
    editor_recalc_cursor(s);
}

void editor_key(editor_state *s, u8 sc, int shift) {
    if (sc == 0x1C) {
        if (s->text_len < EDITOR_BUF_SIZE - 1) {
            for (int i = s->text_len; i > s->cursor_pos; i--)
                s->text[i] = s->text[i-1];
            s->text[s->cursor_pos] = '\n';
            s->text_len++; s->cursor_pos++;
            s->modified = 1;
        }
    } else if (sc == 0x0E) {
        if (s->cursor_pos > 0) {
            for (int i = s->cursor_pos - 1; i < s->text_len - 1; i++)
                s->text[i] = s->text[i+1];
            s->text_len--; s->cursor_pos--;
            s->modified = 1;
        }
    } else if (sc == 0x48) {
        if (s->cursor_line > 0) {
            int prev_start = editor_line_start(s, s->cursor_line - 1);
            int col = s->cursor_col;
            s->cursor_pos = prev_start;
            for (int i = 0; i < col && s->cursor_pos < s->text_len && s->text[s->cursor_pos] != '\n'; i++)
                s->cursor_pos++;
        }
    } else if (sc == 0x50) {
        int next_start = editor_line_start(s, s->cursor_line + 1);
        if (next_start < s->text_len || s->cursor_line < 999) {
            int col = s->cursor_col;
            s->cursor_pos = next_start;
            for (int i = 0; i < col && s->cursor_pos < s->text_len && s->text[s->cursor_pos] != '\n'; i++)
                s->cursor_pos++;
        }
    } else if (sc == 0x4B) {
        if (s->cursor_pos > 0) s->cursor_pos--;
    } else if (sc == 0x4D) {
        if (s->cursor_pos < s->text_len) s->cursor_pos++;
    } else if (sc == 0x47) {
        s->cursor_pos = editor_line_start(s, s->cursor_line);
    } else if (sc == 0x4F) {
        s->cursor_pos = editor_line_start(s, s->cursor_line);
        while (s->cursor_pos < s->text_len && s->text[s->cursor_pos] != '\n')
            s->cursor_pos++;
    } else {
        char c = scan_to_ascii(sc, shift);
        if (c && c >= 32 && c <= 126 && s->text_len < EDITOR_BUF_SIZE - 1) {
            for (int i = s->text_len; i > s->cursor_pos; i--)
                s->text[i] = s->text[i-1];
            s->text[s->cursor_pos] = c;
            s->text_len++; s->cursor_pos++;
            s->modified = 1;
        }
    }
    editor_recalc_cursor(s);
}

/* 绘制编辑器到指定矩形区域 */
void editor_draw_to(editor_state *s, int cx, int cy, int cw, int ch) {
    int line_num_w = 5 * (int)DU_ASCII_STEP + (int)DU_SPACE_SM;
    int text_area_x = cx + line_num_w;
    int text_area_w = cw - line_num_w;
    int visible_rows = (ch - 20) / (int)DU_ASCII_LINE_H;
    int visible_cols = text_area_w / (int)DU_ASCII_STEP;
    if (visible_rows < 1) visible_rows = 1;
    if (visible_cols < 4) visible_cols = 4;

    du_fill_rect(&g_fb, cx, cy, cw, ch, KS_BG_SECONDARY);

    if (s->cursor_line < s->scroll_y) s->scroll_y = s->cursor_line;
    if (s->cursor_line >= s->scroll_y + visible_rows) s->scroll_y = s->cursor_line - visible_rows + 1;

    int line = 0;
    int pos = 0;
    for (int i = 0; i < s->text_len || line <= s->cursor_line; ) {
        if (line >= s->scroll_y + visible_rows) break;

        if (line >= s->scroll_y) {
            int screen_y = cy + (line - s->scroll_y) * (int)DU_ASCII_LINE_H;

            char ln[8];
            ln[0] = '0' + ((line+1)/100)%10;
            ln[1] = '0' + ((line+1)/10)%10;
            ln[2] = '0' + (line+1)%10;
            ln[3] = 0;
            du_draw_string(&g_fb, ln, cx + 2, screen_y,
                           KS_TEXT_DIM, KS_BG_SECONDARY, DU_ASCII_STEP);

            du_fill_rect(&g_fb, cx + line_num_w - 2, screen_y, 1,
                         (int)DU_ASCII_CELL_H, KS_BORDER_DIM);

            int col = 0;
            while (pos < s->text_len && s->text[pos] != '\n' && col < visible_cols) {
                du_draw_char(&g_fb, s->text[pos],
                             text_area_x + col * (int)DU_ASCII_STEP,
                             screen_y,
                             KS_TEXT_PRIMARY, KS_BG_SECONDARY);
                col++; pos++;
            }
            if (pos < s->text_len && s->text[pos] == '\n') pos++;
        } else {
            while (pos < s->text_len && s->text[pos] != '\n') pos++;
            if (pos < s->text_len) pos++;
        }
        line++;
        if (pos >= s->text_len && line > s->cursor_line) break;
    }

    /* 光标（青色竖条） */
    if (s->cursor_line >= s->scroll_y && s->cursor_line < s->scroll_y + visible_rows) {
        int cursor_y = cy + (s->cursor_line - s->scroll_y) * (int)DU_ASCII_LINE_H;
        int cursor_x = text_area_x + s->cursor_col * (int)DU_ASCII_STEP;
        du_fill_rect(&g_fb, cursor_x, cursor_y, 2, (int)DU_ASCII_CELL_H, KS_ACCENT);
    }

    /* 底部迷你状态栏 */
    int status_y = cy + ch - 20;
    du_fill_rect(&g_fb, cx, status_y, cw, 20, KS_BG_TERTIARY);
    du_divider_h(&g_fb, cx, status_y, cw, KS_BORDER);
    char status[64];
    int sp = 0;
    const char *p1 = "L:"; while(*p1) status[sp++] = *p1++;
    status[sp++] = '0' + (s->cursor_line+1)/10%10;
    status[sp++] = '0' + (s->cursor_line+1)%10;
    const char *p2 = " C:"; while(*p2) status[sp++] = *p2++;
    status[sp++] = '0' + (s->cursor_col+1)/10%10;
    status[sp++] = '0' + (s->cursor_col+1)%10;
    if (s->modified) { const char *m = " *"; while(*m) status[sp++] = *m++; }
    if (s->has_file) {
        const char *m = "  "; while(*m) status[sp++] = *m++;
        for (int i = 0; s->disp[i] && sp < 60; i++) status[sp++] = s->disp[i];
    }
    status[sp] = 0;
    du_draw_string(&g_fb, status, cx + 4, status_y + 1,
                   KS_TEXT_DIM, KS_BG_TERTIARY, DU_ASCII_STEP);
}

/* ---- 窗口适配 ---- */
void *editor_on_create(app_ctx *ctx) {
    editor_state *s = (editor_state *)ADDR_EDWIN;
    editor_reset(s, "# Welcome to Deshab Editor\n#\n\nStart typing here...\n");
    for (int i = 0; i < 47; i++) ctx->win->title[i] = 0;
    const char *t = "Editor - untitled";
    for (int i = 0; t[i] && i < 47; i++) ctx->win->title[i] = t[i];
    return s;
}
void editor_on_event(void *state, app_ctx *ctx, desktop_event *ev) {
    if (ev->type != EV_KEY_DOWN) return;
    editor_key((editor_state *)state, ev->scancode, ev->shift);
    ctx->win->dirty = 1;
}
void editor_on_draw(void *state, app_ctx *ctx) {
    editor_draw_to((editor_state *)state, ctx->client_x, ctx->client_y,
                   ctx->client_w, ctx->client_h);
}
void editor_on_destroy(void *state) { (void)state; }

/* ============================================================
 *  计算器 (Calculator)
 * ============================================================ */

static void calc_update_display(calc_state *s) {
    s->display_len = 0;
    i64 val = s->current;
    if (val < 0) { s->display[s->display_len++] = '-'; val = -val; }
    if (val == 0) { s->display[s->display_len++] = '0'; }
    else {
        char tmp[20]; int tl = 0;
        while (val > 0) { tmp[tl++] = '0' + (int)(val % 10); val /= 10; }
        for (int i = tl-1; i >= 0; i--) s->display[s->display_len++] = tmp[i];
    }
    s->display[s->display_len] = 0;
}

void *calc_on_create(app_ctx *ctx) {
    calc_state *s = (calc_state *)ADDR_CALC;
    s->accumulator = 0; s->current = 0;
    s->op = 0; s->new_number = 1;
    calc_update_display(s);
    const char *t = "Calculator";
    for (int i = 0; t[i] && i < 47; i++) ctx->win->title[i] = t[i];
    return s;
}

void calc_on_event(void *state, app_ctx *ctx, desktop_event *ev) {
    calc_state *s = (calc_state *)state;
    if (ev->type != EV_KEY_DOWN) return;
    char c = scan_to_ascii(ev->scancode, ev->shift);
    if (c >= '0' && c <= '9') {
        if (s->new_number) { s->current = 0; s->new_number = 0; }
        s->current = s->current * 10 + (c - '0');
    } else if (c == '+') { s->accumulator = s->current; s->op = 1; s->new_number = 1; }
    else if (c == '-') { s->accumulator = s->current; s->op = 2; s->new_number = 1; }
    else if (c == '*') { s->accumulator = s->current; s->op = 3; s->new_number = 1; }
    else if (c == '/') { s->accumulator = s->current; s->op = 4; s->new_number = 1; }
    else if (c == '=' || c == '\n') {
        switch (s->op) {
        case 1: s->current = s->accumulator + s->current; break;
        case 2: s->current = s->accumulator - s->current; break;
        case 3: s->current = s->accumulator * s->current; break;
        case 4: s->current = (s->current != 0) ? s->accumulator / s->current : 0; break;
        }
        s->op = 0; s->new_number = 1;
    } else if (c == 'c' || c == 'C') {
        s->current = 0; s->accumulator = 0; s->op = 0; s->new_number = 1;
    }
    calc_update_display(s);
    ctx->win->dirty = 1;
}

void calc_on_draw(void *state, app_ctx *ctx) {
    calc_state *s = (calc_state *)state;
    int cx = ctx->client_x;
    int cy = ctx->client_y;
    int cw = ctx->client_w;
    int ch = ctx->client_h;

    du_fill_rect(&g_fb, cx, cy, cw, ch, KS_BG_SECONDARY);

    int disp_h = 40;
    du_fill_rounded_rect(&g_fb, cx + 8, cy + 8, cw - 16, disp_h,
                          KS_BG_PRIMARY, DU_RADIUS_MD);
    du_rect_outline(&g_fb, cx + 8, cy + 8, cw - 16, disp_h,
                    KS_ACCENT, DU_RADIUS_MD);
    int text_x = cx + cw - 16 - s->display_len * (int)DU_ASCII_STEP - 8;
    du_draw_string(&g_fb, s->display, text_x, cy + 16,
                   KS_ACCENT, KS_BG_PRIMARY, DU_ASCII_STEP);

    const char *btn_labels[] = {
        "C", "+/-", "%", "/",
        "7", "8", "9", "*",
        "4", "5", "6", "-",
        "1", "2", "3", "+",
        "0", ".",  "=",  0
    };
    int btn_w = (cw - 16 - 12) / 4;
    int btn_h = 32;
    int btn_y = cy + 8 + disp_h + 8;
    for (int i = 0; i < 19; i++) {
        int row = i / 4, col = i % 4;
        int bx = cx + 8 + col * (btn_w + 3);
        int by = btn_y + row * (btn_h + 3);
        if (by + btn_h > cy + ch - 4) break;
        u32 bg = (i < 4) ? KS_ACCENT_DIM :
                 (i % 4 == 3) ? KS_ACCENT :
                 (i == 18) ? KS_ACCENT2 :
                 KS_BG_TERTIARY;
        u32 fg = ((i % 4 == 3) || (i == 18)) ? KS_TEXT_INVERT : KS_TEXT_PRIMARY;
        du_fill_rounded_rect(&g_fb, bx, by, btn_w, btn_h, bg, DU_RADIUS_SM);
        du_rect_outline(&g_fb, bx, by, btn_w, btn_h, KS_BORDER, DU_RADIUS_SM);
        du_draw_string(&g_fb, btn_labels[i],
                       bx + (btn_w - 1*(int)DU_ASCII_STEP)/2,
                       by + (btn_h - (int)DU_ASCII_CELL_H)/2,
                       fg, bg, DU_ASCII_STEP);
    }
}

void calc_on_destroy(void *state) { (void)state; }

/* ============================================================
 *  FILES 面板（FAT32 真实读写）
 * ============================================================ */

static int files_list_cb(const char *name, u32 size, u8 attr, void *ud) {
    (void)ud;
    if (attr & 0x10) return 0;                 /* 跳过子目录 */
    if (g_file_count >= MAX_FILES) return 1;   /* 停止 */

    /* 只显示 user 文件：跳过 .ELF 可执行文件和系统模块 */
    int nl = kstrlen(name);
    if (nl >= 4 && name[nl-4] == '.' && name[nl-3] == 'E' &&
        name[nl-2] == 'L' && name[nl-1] == 'F') return 0;
    /* FIRSTINI* (FirstInit.elf，Name83=FIRSTINIT → 显示名 FIRSTINI.T) */
    if (nl >= 8 && name[0] == 'F' && name[1] == 'I' && name[2] == 'R' &&
        name[3] == 'S' && name[4] == 'T' && name[5] == 'I' && name[6] == 'N' &&
        name[7] == 'I') return 0;

    fentry *f = &g_files[g_file_count];
    f->size = size;
    kstrcpy(f->disp, name, 13);
    /* disp (NAME.EXT) → name11 (NAME    EXT) */
    for (int i = 0; i < 11; i++) f->name11[i] = ' ';
    int i = 0, o = 0;
    while (name[i] && name[i] != '.' && o < 8) f->name11[o++] = name[i++];
    while (name[i] && name[i] != '.') i++;
    if (name[i] == '.') {
        i++;
        o = 8;
        while (name[i] && o < 11) f->name11[o++] = name[i++];
    }
    g_file_count++;
    return 0;
}

void files_refresh(void) {
    g_file_count = 0;
    g_file_sel = -1;
    if (!g_block_read) { g_files_loaded = 0; return; }
    if (f32_list_root_w(files_list_cb, 0) == 0) g_files_loaded = 1;
    else g_files_loaded = 0;
}

/* 双击打开文件 → 载入 DASHBOARD EDITOR */
void files_open(int idx) {
    if (idx < 0 || idx >= g_file_count) return;
    editor_state *ed = (editor_state *)ADDR_EDDASH;
    fentry *f = &g_files[idx];

    u8 *data = 0; u32 size = 0;
    if (f32_read_root_file_w(f->name11, &data, &size) != 0) {
        slog("file open failed");
        return;
    }
    ed->text_len = 0;
    u32 cap = EDITOR_BUF_SIZE - 1;
    if (size > cap) size = cap;
    for (u32 i = 0; i < size; i++) {
        char c = (char)data[i];
        if ((u8)c >= 32 || c == '\n' || c == '\t')
            ed->text[ed->text_len++] = c;
    }
    for (int i = 0; i < 11; i++) ed->name11[i] = f->name11[i];
    kstrcpy(ed->disp, f->disp, 13);
    ed->has_file = 1;
    ed->modified = 0;
    ed->scroll_y = 0;
    ed->cursor_pos = 0;
    editor_recalc_cursor(ed);
    g_dash_focus = 2;
    slog("file loaded into editor");
}

/* EDITOR SAVE 回写磁盘 */
void editor_save(void) {
    editor_state *ed = (editor_state *)ADDR_EDDASH;
    if (!ed->has_file) return;
    if (f32_write_root_file_w(ed->name11, (const u8 *)ed->text, (u32)ed->text_len) == 0) {
        ed->modified = 0;
        slog("file saved");
    } else {
        slog("file save failed");
    }
}
