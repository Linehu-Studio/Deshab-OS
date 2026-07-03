/* Deshab Shell — 命令行交互界面
 * 接收 PS/2 键盘输入，执行内置命令。
 * 使用 Deshab "Sealed Arc" 视觉风格系统。
 */

#include "../UTSM/include/utsm/dsk.h"

typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;
typedef long long          i64;

#define COM1 0x3F8

static __inline__ void outb(u16 port, u8 value) { __asm__ volatile("outb %0,%1"::"a"(value),"Nd"(port)); }
static __inline__ u8 inb(u16 port) { u8 v; __asm__ volatile("inb %1,%0":"=a"(v):"Nd"(port)); return v; }

static void sputc(char c) {
    for (unsigned int i=0; i<100000; i++) { if (inb(COM1+5)&0x20) break; }
    outb(COM1, (unsigned char)c);
}
static void swrite(const char *s) { while(*s) { if(*s=='\n')sputc('\r'); sputc(*s++); } }
static void logl(const char *s) { swrite(s); swrite("\n"); }

/* ASCII 字体 — 必须在 deshab_ui.h 之前包含，因为 du_draw_char 引用 g_ascii */
#include "../firstInit/ascii_bitmaps.c"
#include "../UTSM/include/utsm/deshab_ui.h"

/* ---- 风格令牌（引用 deshab_ui.h 语义色） ---- */
#define BG_COLOR      DS_DARK_BG_PRIMARY
#define TEXT_FG       DS_DARK_TEXT_PRIMARY
#define PROMPT_FG     DS_DARK_PROMPT
#define ERROR_FG      DP_ERROR
#define OK_FG         DP_SUCCESS
#define DIM_FG        DS_DARK_TEXT_DIM
#define ACCENT_FG     DS_DARK_ACCENT
#define DECORATIVE_FG DS_DARK_DECORATIVE

/* 标题栏参数 */
#define TITLEBAR_H    28
#define TITLEBAR_BG   DP_ABYSS_800
#define TITLEBAR_FG   DS_DARK_TEXT_SECONDARY
#define TITLEBAR_ACCENT DS_DARK_ACCENT

/* 状态栏参数 */
#define STATUSBAR_H   22
#define STATUSBAR_BG  DP_ABYSS_800

/* 终端尺寸 */
#define TERM_COLS  100
#define TERM_ROWS  35
#define TERM_MAX_CHARS (TERM_COLS * TERM_ROWS)

static du_context g_ctx;
static u64 fb_a, fb_w, fb_h, fb_p;

/* 终端缓冲区 — 字符 + 颜色 */
static u8  term_ch[TERM_MAX_CHARS];
static u32 term_fg[TERM_MAX_CHARS];
static int term_w = TERM_COLS;
static int term_h = TERM_ROWS;
static int cur_col = 0;
static int cur_row = 0;
static int cur_visible = 1;

/* ---- 终端像素坐标 ---- */
static int term_start_x, term_start_y;

static void term_init_pos(void) {
    int margin_x = (int)DU_SPACE_MD;
    term_start_x = margin_x;
    /* 标题栏下方开始 */
    term_start_y = TITLEBAR_H + (int)DU_SPACE_SM;
    int avail_w = (int)fb_w - margin_x * 2;
    int avail_h = (int)fb_h - term_start_y - STATUSBAR_H - (int)DU_SPACE_SM;
    term_w = avail_w / (int)DU_ASCII_STEP;
    term_h = avail_h / (int)DU_ASCII_LINE_H;
    if (term_w < 10) term_w = 10;
    if (term_h < 5) term_h = 5;
    if (term_w > TERM_COLS) term_w = TERM_COLS;
    if (term_h > TERM_ROWS) term_h = TERM_ROWS;
}

static void term_clear(void) {
    for (int i = 0; i < TERM_MAX_CHARS; i++) {
        term_ch[i] = ' ';
        term_fg[i] = TEXT_FG;
    }
    cur_col = 0;
    cur_row = 0;
}

static void term_scroll(void) {
    for (int r = 1; r < term_h; r++) {
        for (int c = 0; c < term_w; c++) {
            int src = r * TERM_COLS + c;
            int dst = (r-1) * TERM_COLS + c;
            term_ch[dst] = term_ch[src];
            term_fg[dst] = term_fg[src];
        }
    }
    for (int c = 0; c < term_w; c++) {
        term_ch[(term_h-1) * TERM_COLS + c] = ' ';
        term_fg[(term_h-1) * TERM_COLS + c] = TEXT_FG;
    }
    cur_row = term_h - 1;
    cur_col = 0;
}

static void term_newline(void) {
    cur_col = 0;
    cur_row++;
    if (cur_row >= term_h) term_scroll();
}

static void term_putc_color(char c, u32 color) {
    if (c == '\n') { term_newline(); return; }
    if (c == '\r') { cur_col = 0; return; }
    if (c == '\t') {
        int spaces = 4 - (cur_col % 4);
        for (int i = 0; i < spaces; i++) term_putc_color(' ', color);
        return;
    }
    if (c < ' ' || c > '~') c = '?';
    if (cur_col >= term_w) term_newline();
    int idx = cur_row * TERM_COLS + cur_col;
    term_ch[idx] = (u8)c;
    term_fg[idx] = color;
    cur_col++;
}

static void term_putc(char c) { term_putc_color(c, TEXT_FG); }
static void term_puts_color(const char *s, u32 color) { while (*s) term_putc_color(*s++, color); }
static void term_puts(const char *s) { term_puts_color(s, TEXT_FG); }

/* ---- 标题栏渲染 ---- */
static void draw_titlebar(void) {
    du_fill_rect(&g_ctx, 0, 0, (i64)fb_w, TITLEBAR_H, TITLEBAR_BG);
    /* 底部强调线 */
    du_fill_rect(&g_ctx, 0, TITLEBAR_H - 2, (i64)fb_w, 2, TITLEBAR_ACCENT);
    /* 左侧标题文字 */
    du_draw_string(&g_ctx, "Deshab Shell",
                   (i64)DU_SPACE_MD, (TITLEBAR_H - (i64)DU_ASCII_CELL_H) / 2,
                   DS_DARK_TEXT_PRIMARY, TITLEBAR_BG, DU_ASCII_STEP);
    /* 右侧版本号 */
    du_draw_string(&g_ctx, "v0.1.0",
                   (i64)fb_w - 6 * (i64)DU_ASCII_STEP - (i64)DU_SPACE_MD,
                   (TITLEBAR_H - (i64)DU_ASCII_CELL_H) / 2,
                   DS_DARK_TEXT_DIM, TITLEBAR_BG, DU_ASCII_STEP);
}

/* ---- 状态栏渲染 ---- */
static void draw_statusbar(void) {
    i64 sy = (i64)fb_h - STATUSBAR_H;
    du_fill_rect(&g_ctx, 0, sy, (i64)fb_w, STATUSBAR_H, STATUSBAR_BG);
    /* 顶部强调线 */
    du_fill_rect(&g_ctx, 0, sy, (i64)fb_w, 1, DS_DARK_DIVIDER);
    /* 左侧：当前路径 */
    du_draw_string(&g_ctx, "/",
                   (i64)DU_SPACE_MD, sy + (STATUSBAR_H - (i64)DU_ASCII_CELL_H) / 2,
                   DS_DARK_TEXT_DIM, STATUSBAR_BG, DU_ASCII_STEP);
    /* 右侧：root@deshab */
    const char *user = "root@deshab";
    int ulen = 0;
    while (user[ulen]) ulen++;
    du_draw_string(&g_ctx, user,
                   (i64)fb_w - ulen * (i64)DU_ASCII_STEP - (i64)DU_SPACE_MD,
                   sy + (STATUSBAR_H - (i64)DU_ASCII_CELL_H) / 2,
                   DS_DARK_ACCENT, STATUSBAR_BG, DU_ASCII_STEP);
}

static void term_redraw_all(void) {
    /* 清屏背景 */
    du_fill_bg_solid(&g_ctx, BG_COLOR);
    /* 标题栏 */
    draw_titlebar();
    /* 渲染所有字符 */
    for (int r = 0; r < term_h; r++) {
        for (int c = 0; c < term_w; c++) {
            int idx = r * TERM_COLS + c;
            u8 ch = term_ch[idx];
            if (ch == ' ') continue;
            i64 x = term_start_x + (i64)c * (i64)DU_ASCII_STEP;
            i64 y = term_start_y + (i64)r * (i64)DU_ASCII_LINE_H;
            du_draw_char(&g_ctx, ch, x, y, term_fg[idx], BG_COLOR);
        }
    }
    /* 光标 */
    if (cur_visible && cur_col < term_w && cur_row < term_h) {
        i64 x = term_start_x + (i64)cur_col * (i64)DU_ASCII_STEP;
        i64 y = term_start_y + (i64)cur_row * (i64)DU_ASCII_LINE_H;
        du_fill_rect(&g_ctx, x, y + (i64)DU_ASCII_CELL_H - 3,
                     (i64)DU_ASCII_CELL_W, 2, DS_DARK_CURSOR);
    }
    /* 状态栏 */
    draw_statusbar();
}

static void term_redraw_cursor(void) {
    if (cur_visible && cur_col < term_w && cur_row < term_h) {
        i64 x = term_start_x + (i64)cur_col * (i64)DU_ASCII_STEP;
        i64 y = term_start_y + (i64)cur_row * (i64)DU_ASCII_LINE_H;
        du_fill_rect(&g_ctx, x, y + (i64)DU_ASCII_CELL_H - 3,
                     (i64)DU_ASCII_CELL_W, 2, DS_DARK_CURSOR);
    }
}

static void term_clear_cursor(void) {
    if (cur_col < term_w && cur_row < term_h) {
        i64 x = term_start_x + (i64)cur_col * (i64)DU_ASCII_STEP;
        i64 y = term_start_y + (i64)cur_row * (i64)DU_ASCII_LINE_H;
        du_fill_rect(&g_ctx, x, y + (i64)DU_ASCII_CELL_H - 3,
                     (i64)DU_ASCII_CELL_W, 2, BG_COLOR);
    }
}

/* ---- 输入缓冲 ---- */
static char input_buf[256];
static int input_len = 0;
static int input_cursor = 0;

/* ---- 键盘扫描码 → ASCII（Set 1） ---- */
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

/* ---- 内置命令 ---- */
static int str_eq(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

static void cmd_help(void) {
    term_puts_color("Deshab Shell — 内置命令列表:\n", PROMPT_FG);
    term_puts("  help          显示此帮助\n");
    term_puts("  clear         清屏\n");
    term_puts("  echo <text>   显示文本\n");
    term_puts("  version       显示系统版本\n");
    term_puts("  uname         显示系统名称\n");
    term_puts("  date          显示当前日期\n");
    term_puts("  ver           显示内核版本（同 version）\n");
    term_puts("  about         关于 Deshab\n");
    term_puts("  reboot        重启系统\n");
    term_puts("  halt          关机（停止 CPU）\n");
    term_puts("  ls            列出根目录文件（暂未实现，需要 VFS）\n");
    term_puts("  pci           列出 PCI 设备（暂未实现）\n");
}

static void cmd_version(void) {
    term_puts_color("Deshab OS v0.1.0\n", OK_FG);
    term_puts("  Kernel: DSK (Deshab System Kernel)\n");
    term_puts("  Architecture: x86_64\n");
    term_puts("  ABI: DKM (Deshab Kernel Module) v1\n");
    term_puts("  Bootloader: Limine\n");
    term_puts_color("  Style: Sealed Arc (Deshab UI)\n", DIM_FG);
}

static void cmd_uname(void) {
    term_puts("Deshab\n");
}

static void cmd_about(void) {
    term_puts_color("=== Deshab OS ===\n", PROMPT_FG);
    term_puts("单地址空间 Ring0 内核实验系统\n");
    term_puts("架构: UTSM + DSK + DKM 驱动模型\n");
    term_puts_color("视觉: Sealed Arc 设计语言\n", DECORATIVE_FG);
    term_puts_color("Built with Clang + lld\n", DIM_FG);
    term_puts_color("(c) 2026 Deshab Project\n", DIM_FG);
}

static void cmd_clear(void) {
    term_clear();
    term_redraw_all();
}

static void cmd_reboot(void) {
    term_puts_color("Rebooting...\n", PROMPT_FG);
    term_redraw_all();
    outb(0x64, 0xFE);
    __asm__ volatile("int $0x03");
    for(;;) __asm__("hlt");
}

static void cmd_halt(void) {
    term_puts_color("System halted.\n", PROMPT_FG);
    term_redraw_all();
    for(;;) __asm__("hlt");
}

static void cmd_echo(const char *args) {
    if (args && *args) term_puts_color(args, TEXT_FG);
    term_putc('\n');
}

static u8 read_rtc_reg(u8 reg);
static u8 bcd_to_bin_impl(u8 bcd);

static void cmd_date(void) {
    u8 year = bcd_to_bin_impl(read_rtc_reg(9));
    u8 month = bcd_to_bin_impl(read_rtc_reg(8));
    u8 day = bcd_to_bin_impl(read_rtc_reg(7));
    u8 hour = bcd_to_bin_impl(read_rtc_reg(4));
    u8 minute = bcd_to_bin_impl(read_rtc_reg(2));
    u8 second = bcd_to_bin_impl(read_rtc_reg(0));
    char buf[32];
    int p = 0;
    buf[p++] = '2'; buf[p++] = '0';
    buf[p++] = '0' + year / 10; buf[p++] = '0' + year % 10;
    buf[p++] = '-';
    buf[p++] = '0' + month / 10; buf[p++] = '0' + month % 10;
    buf[p++] = '-';
    buf[p++] = '0' + day / 10; buf[p++] = '0' + day % 10;
    buf[p++] = ' ';
    buf[p++] = '0' + hour / 10; buf[p++] = '0' + hour % 10;
    buf[p++] = ':';
    buf[p++] = '0' + minute / 10; buf[p++] = '0' + minute % 10;
    buf[p++] = ':';
    buf[p++] = '0' + second / 10; buf[p++] = '0' + second % 10;
    buf[p] = 0;
    term_puts(buf);
    term_putc('\n');
}

static void cmd_unknown(const char *cmd) {
    term_puts_color("未知命令: ", ERROR_FG);
    term_puts_color(cmd, ERROR_FG);
    term_putc('\n');
    term_puts_color("输入 'help' 查看可用命令\n", DIM_FG);
}

static void cmd_not_impl(const char *cmd) {
    term_puts_color(cmd, ERROR_FG);
    term_puts_color(": 此命令尚未实现\n", ERROR_FG);
}

static void execute_command(const char *cmd) {
    while (*cmd == ' ') cmd++;
    if (*cmd == 0) return;
    const char *args = cmd;
    while (*args && *args != ' ') args++;
    int cmd_name_len = (int)(args - cmd);
    char name[32];
    if (cmd_name_len >= 32) cmd_name_len = 31;
    for (int i = 0; i < cmd_name_len; i++) name[i] = cmd[i];
    name[cmd_name_len] = 0;
    while (*args == ' ') args++;
    if (str_eq(name, "help") || str_eq(name, "?")) cmd_help();
    else if (str_eq(name, "version") || str_eq(name, "ver")) cmd_version();
    else if (str_eq(name, "uname")) cmd_uname();
    else if (str_eq(name, "about")) cmd_about();
    else if (str_eq(name, "clear") || str_eq(name, "cls")) cmd_clear();
    else if (str_eq(name, "reboot")) cmd_reboot();
    else if (str_eq(name, "halt") || str_eq(name, "shutdown")) cmd_halt();
    else if (str_eq(name, "echo")) cmd_echo(args);
    else if (str_eq(name, "date") || str_eq(name, "time")) cmd_date();
    else if (str_eq(name, "ls") || str_eq(name, "dir")) cmd_not_impl("ls");
    else if (str_eq(name, "pci")) cmd_not_impl("pci");
    else if (str_eq(name, "cat")) cmd_not_impl("cat");
    else if (str_eq(name, "cd")) cmd_not_impl("cd");
    else if (str_eq(name, "pwd")) { term_puts("/\n"); }
    else if (str_eq(name, "whoami")) { term_puts("root\n"); }
    else if (str_eq(name, "id")) { term_puts("uid=0(root) gid=0(root)\n"); }
    else cmd_unknown(name);
}

/* ---- CMOS RTC 读取 ---- */
static u8 read_rtc_reg(u8 reg) {
    outb(0x70, reg);
    return inb(0x71);
}

static u8 bcd_to_bin_impl(u8 bcd) {
    return (u8)((bcd >> 4) * 10 + (bcd & 0x0F));
}

/* ---- 提示符 ---- */
static const char *PROMPT = "deshab# ";

static void draw_prompt(void) {
    term_puts_color(PROMPT, PROMPT_FG);
}

/* ---- 输入行编辑 ---- */
static void redraw_input_line(void) {
    int prompt_len = 0;
    while (PROMPT[prompt_len]) prompt_len++;
    i64 y = term_start_y + (i64)cur_row * (i64)DU_ASCII_LINE_H;
    du_fill_rect(&g_ctx, term_start_x, y,
                 (i64)term_w * (i64)DU_ASCII_STEP,
                 (i64)DU_ASCII_CELL_H, BG_COLOR);
    for (int i = 0; i < prompt_len; i++) {
        i64 x = term_start_x + (i64)i * (i64)DU_ASCII_STEP;
        du_draw_char(&g_ctx, PROMPT[i], x, y, PROMPT_FG, BG_COLOR);
    }
    for (int i = 0; i < input_len; i++) {
        i64 x = term_start_x + (i64)(prompt_len + i) * (i64)DU_ASCII_STEP;
        du_draw_char(&g_ctx, input_buf[i], x, y, TEXT_FG, BG_COLOR);
    }
    cur_col = prompt_len + input_cursor;
    term_redraw_cursor();
}

static void insert_char(char c) {
    if (input_len >= 255) return;
    for (int i = input_len; i > input_cursor; i--) {
        input_buf[i] = input_buf[i-1];
    }
    input_buf[input_cursor] = c;
    input_len++;
    input_cursor++;
    redraw_input_line();
}

static void delete_char_back(void) {
    if (input_cursor == 0) return;
    for (int i = input_cursor - 1; i < input_len - 1; i++) {
        input_buf[i] = input_buf[i+1];
    }
    input_len--;
    input_cursor--;
    input_buf[input_len] = 0;
    redraw_input_line();
}

static void delete_char_fwd(void) {
    if (input_cursor >= input_len) return;
    for (int i = input_cursor; i < input_len - 1; i++) {
        input_buf[i] = input_buf[i+1];
    }
    input_len--;
    input_buf[input_len] = 0;
    redraw_input_line();
}

static void cursor_left(void) {
    if (input_cursor > 0) {
        input_cursor--;
        term_clear_cursor();
        cur_col--;
        if (cur_col < 0) { cur_col = term_w - 1; cur_row--; }
        term_redraw_cursor();
    }
}

static void cursor_right(void) {
    if (input_cursor < input_len) {
        input_cursor++;
        term_clear_cursor();
        cur_col++;
        if (cur_col >= term_w) { cur_col = 0; cur_row++; }
        term_redraw_cursor();
    }
}

static void cursor_home(void) {
    term_clear_cursor();
    int prompt_len = 0;
    while (PROMPT[prompt_len]) prompt_len++;
    cur_col = prompt_len;
    input_cursor = 0;
    term_redraw_cursor();
}

static void cursor_end(void) {
    term_clear_cursor();
    int prompt_len = 0;
    while (PROMPT[prompt_len]) prompt_len++;
    cur_col = prompt_len + input_len;
    input_cursor = input_len;
    term_redraw_cursor();
}

static void kill_to_end(void) {
    input_len = input_cursor;
    input_buf[input_len] = 0;
    redraw_input_line();
}

static void kill_line(void) {
    input_len = 0;
    input_cursor = 0;
    input_buf[0] = 0;
    redraw_input_line();
}

/* ---- 主入口 ---- */
__attribute__((visibility("default")))
void dsk_entry(const dsk_boot_context *ctx) {
    __asm__ volatile("cli");
    logl("[shell] boot");

    if (!ctx || ctx->magic != 0x44534B31424F4F54ULL) {
        logl("[shell] bad context");
        for(;;) __asm__("hlt");
    }

    fb_a = ctx->framebuffer_address;
    fb_w = ctx->framebuffer_width;
    fb_h = ctx->framebuffer_height;
    fb_p = ctx->framebuffer_pitch;

    /* 初始化风格系统渲染上下文 */
    du_context_init(&g_ctx, fb_a, fb_w, fb_h, fb_p);

    term_init_pos();
    term_clear();
    term_redraw_all();

    /* 启动 banner — 使用风格令牌 */
    term_puts_color("=== Deshab OS v0.1.0 ===\n", ACCENT_FG);
    term_puts_color("欢迎使用 Deshab Shell\n", OK_FG);
    term_puts_color("输入 'help' 查看可用命令\n", DIM_FG);
    term_putc('\n');

    /* 主循环 */
    int shift = 0;
    int e0 = 0;
    for (;;) {
        draw_prompt();
        input_len = 0;
        input_cursor = 0;
        input_buf[0] = 0;
        int prompt_len = 0;
        while (PROMPT[prompt_len]) prompt_len++;
        cur_col = prompt_len;
        term_redraw_cursor();

        int cmd_done = 0;
        while (!cmd_done) {
            u8 st = inb(0x64);
            if (!(st & 1)) { __asm__("pause"); continue; }
            u8 data = inb(0x60);
            if (st & 0x20) continue;
            u8 sc = data;
            if (sc == 0xE0) { e0 = 1; continue; }
            if (sc == 0x2A || sc == 0x36) { shift = 1; continue; }
            if (sc == 0xAA || sc == 0xB6) { shift = 0; continue; }
            if (sc & 0x80) {
                if (e0 && sc == 0x9C) { e0 = 0; }
                else if (e0 && sc == 0xCB) { e0 = 0; }
                else if (e0 && sc == 0xCD) { e0 = 0; }
                else if (e0 && sc == 0xC8) { e0 = 0; }
                else if (e0 && sc == 0xD0) { e0 = 0; }
                else if (e0 && sc == 0xD3) { e0 = 0; }
                else if (e0 && sc == 0xC7) { e0 = 0; }
                else if (e0 && sc == 0xCF) { e0 = 0; }
                e0 = 0;
                continue;
            }
            if (e0) {
                if (sc == 0x4B) cursor_left();
                else if (sc == 0x4D) cursor_right();
                else if (sc == 0x47) cursor_home();
                else if (sc == 0x4F) cursor_end();
                else if (sc == 0x53) delete_char_fwd();
                else if (sc == 0x48) { }
                else if (sc == 0x50) { }
                e0 = 0;
                continue;
            }
            if (sc == 0x1C) {
                term_clear_cursor();
                input_buf[input_len] = 0;
                term_putc('\n');
                execute_command(input_buf);
                cmd_done = 1;
                continue;
            }
            if (sc == 0x0E) { delete_char_back(); continue; }
            if (sc == 0x01) { kill_line(); continue; }
            if (sc == 0x15 && !shift) { kill_line(); continue; }
            if (sc == 0x17 && !shift) { while (input_cursor > 0) delete_char_back(); continue; }
            if (sc == 0x0B && !shift) { kill_to_end(); continue; }
            char c = scan_to_ascii(sc, shift);
            if (c && c >= 32 && c <= 126) {
                insert_char(c);
            }
        }
    }
}
