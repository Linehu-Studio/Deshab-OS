/* Deshab Shell — 命令行交互界面
 * 接收 PS/2 键盘输入，执行内置命令。
 * 显示在 framebuffer 上，使用 Consolas 18px 等宽字体。
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

static void sputc(char c) {
    for (unsigned int i=0; i<100000; i++) { if (inb(COM1+5)&0x20) break; }
    outb(COM1, (unsigned char)c);
}
static void swrite(const char *s) { while(*s) { if(*s=='\n')sputc('\r'); sputc(*s++); } }
static void logl(const char *s) { swrite(s); swrite("\n"); }

/* ASCII 字体 — Consolas 18px 灰度位图（与 FirstInit 共享） */
#include "../firstInit/ascii_bitmaps.c"

#define ASCII_W  11
#define ASCII_H  18
#define ASCII_STEP 11

/* 终端尺寸 */
#define TERM_COLS  100
#define TERM_ROWS  35
#define TERM_MAX_CHARS (TERM_COLS * TERM_ROWS)

/* 颜色 — 深色主题，类似经典终端 */
#define BG_COLOR    0xFF0A1428u   /* 深蓝黑 */
#define TEXT_FG     0xFFE0E8F0u   /* 浅灰白 */
#define PROMPT_FG   0xFF66CCFFu   /* 青色提示符 */
#define ERROR_FG    0xFFFF6680u   /* 红色错误 */
#define OK_FG       0xFF66FF99u   /* 绿色成功 */
#define DIM_FG      0xFF7080A0u   /* 灰色注释 */

static u64 fb_a, fb_w, fb_h, fb_p;
static u32 *g_fb;

/* 终端缓冲区 — 字符 + 颜色 */
static u8  term_ch[TERM_MAX_CHARS];
static u32 term_fg[TERM_MAX_CHARS];
static int term_w = TERM_COLS;
static int term_h = TERM_ROWS;
static int cur_col = 0;
static int cur_row = 0;
static int cur_visible = 1;

/* 颜色混合 */
static u32 blend(u32 c1, u32 c2, u32 a) {
    u32 na=256-a;
    u32 r=((c1&0xFF)*na+(c2&0xFF)*a)>>8;
    u32 g=(((c1>>8)&0xFF)*na+((c2>>8)&0xFF)*a)>>8;
    u32 b=(((c1>>16)&0xFF)*na+((c2>>16)&0xFF)*a)>>8;
    return 0xFF000000|(b<<16)|(g<<8)|r;
}

/* ---- 字符渲染（与 FirstInit 类似，但用纯色背景） ---- */
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

/* ---- 终端逻辑 ---- */
static int term_start_x, term_start_y;  /* 像素坐标 */

static void term_init_pos(void) {
    /* 居中布局，每字符 11px 宽，行高 20px */
    int margin_x = 16;
    int margin_y = 16;
    term_start_x = margin_x;
    term_start_y = margin_y;
    /* 自适应行数/列数到 framebuffer */
    int avail_w = (int)fb_w - margin_x * 2;
    int avail_h = (int)fb_h - margin_y * 2;
    term_w = avail_w / ASCII_STEP;
    term_h = avail_h / (ASCII_H + 2);
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
    /* 向上滚动一行 */
    for (int r = 1; r < term_h; r++) {
        for (int c = 0; c < term_w; c++) {
            int src = r * TERM_COLS + c;
            int dst = (r-1) * TERM_COLS + c;
            term_ch[dst] = term_ch[src];
            term_fg[dst] = term_fg[src];
        }
    }
    /* 清空最后一行 */
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

static void term_puts_color(const char *s, u32 color) {
    while (*s) term_putc_color(*s++, color);
}

static void term_puts(const char *s) { term_puts_color(s, TEXT_FG); }

static void term_redraw_all(void) {
    /* 清屏背景 */
    fill_rect(g_fb, 0, 0, (i64)fb_w, (i64)fb_h, BG_COLOR);
    /* 渲染所有字符 */
    for (int r = 0; r < term_h; r++) {
        for (int c = 0; c < term_w; c++) {
            int idx = r * TERM_COLS + c;
            u8 ch = term_ch[idx];
            if (ch == ' ') continue;
            i64 x = term_start_x + (i64)c * ASCII_STEP;
            i64 y = term_start_y + (i64)r * (ASCII_H + 2);
            fb_char(g_fb, ch, x, y, term_fg[idx], BG_COLOR);
        }
    }
    /* 渲染光标 */
    if (cur_visible && cur_col < term_w && cur_row < term_h) {
        i64 x = term_start_x + (i64)cur_col * ASCII_STEP;
        i64 y = term_start_y + (i64)cur_row * (ASCII_H + 2);
        fill_rect(g_fb, x, y + ASCII_H - 3, ASCII_W, 2, TEXT_FG);
    }
}

static void term_redraw_cursor(void) {
    if (cur_visible && cur_col < term_w && cur_row < term_h) {
        i64 x = term_start_x + (i64)cur_col * ASCII_STEP;
        i64 y = term_start_y + (i64)cur_row * (ASCII_H + 2);
        fill_rect(g_fb, x, y + ASCII_H - 3, ASCII_W, 2, TEXT_FG);
    }
}

static void term_clear_cursor(void) {
    if (cur_col < term_w && cur_row < term_h) {
        i64 x = term_start_x + (i64)cur_col * ASCII_STEP;
        i64 y = term_start_y + (i64)cur_row * (ASCII_H + 2);
        fill_rect(g_fb, x, y + ASCII_H - 3, ASCII_W, 2, BG_COLOR);
    }
}

/* ---- 输入缓冲 ---- */
static char input_buf[256];
static int input_len = 0;
static int input_cursor = 0;  /* 在 input_buf 中的位置（支持左右移动） */

/* ---- 键盘扫描码 → ASCII（Set 1，与 FirstInit 共享逻辑） ---- */
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
}

static void cmd_uname(void) {
    term_puts("Deshab\n");
}

static void cmd_about(void) {
    term_puts_color("=== Deshab OS ===\n", PROMPT_FG);
    term_puts("单地址空间 Ring0 内核实验系统\n");
    term_puts("架构: UTSM + DSK + DKM 驱动模型\n");
    term_puts_color("Built with Clang + lld\n", DIM_FG);
    term_puts_color("© 2026 Deshab Project\n", DIM_FG);
}

static void cmd_clear(void) {
    term_clear();
}

static void cmd_reboot(void) {
    term_puts_color("Rebooting...\n", PROMPT_FG);
    term_redraw_all();
    /* ACPI 重启：写 0x64 = 0xFE 触发键盘控制器复位 */
    outb(0x64, 0xFE);
    /* 备用：三重故障 */
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

/* 前向声明 — CMOS RTC 读取 */
static u8 read_rtc_reg(u8 reg);
static u8 bcd_to_bin_impl(u8 bcd);

static void cmd_date(void) {
    /* 通过 CMOS RTC 读取日期 */
    u8 year = bcd_to_bin_impl(read_rtc_reg(9));
    u8 month = bcd_to_bin_impl(read_rtc_reg(8));
    u8 day = bcd_to_bin_impl(read_rtc_reg(7));
    u8 hour = bcd_to_bin_impl(read_rtc_reg(4));
    u8 minute = bcd_to_bin_impl(read_rtc_reg(2));
    u8 second = bcd_to_bin_impl(read_rtc_reg(0));
    /* 简单打印 */
    char buf[32];
    int p = 0;
    /* YYYY-MM-DD HH:MM:SS */
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
    /* 跳过前导空格 */
    while (*cmd == ' ') cmd++;
    if (*cmd == 0) return;
    /* 找命令名（第一个单词） */
    const char *args = cmd;
    while (*args && *args != ' ') args++;
    int cmd_name_len = (int)(args - cmd);
    /* 复制命令名到本地缓冲 */
    char name[32];
    if (cmd_name_len >= 32) cmd_name_len = 31;
    for (int i = 0; i < cmd_name_len; i++) name[i] = cmd[i];
    name[cmd_name_len] = 0;
    /* 跳过参数前的空格 */
    while (*args == ' ') args++;
    /* 分派 */
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
    /* 清除当前行从提示符开始的部分并重绘 */
    int prompt_len = 0;
    while (PROMPT[prompt_len]) prompt_len++;
    /* 清除整行 */
    i64 y = term_start_y + (i64)cur_row * (ASCII_H + 2);
    fill_rect(g_fb, term_start_x, y, (i64)term_w * ASCII_STEP, ASCII_H, BG_COLOR);
    /* 重绘提示符 */
    for (int i = 0; i < prompt_len; i++) {
        i64 x = term_start_x + (i64)i * ASCII_STEP;
        fb_char(g_fb, PROMPT[i], x, y, PROMPT_FG, BG_COLOR);
    }
    /* 重绘输入缓冲 */
    for (int i = 0; i < input_len; i++) {
        i64 x = term_start_x + (i64)(prompt_len + i) * ASCII_STEP;
        fb_char(g_fb, input_buf[i], x, y, TEXT_FG, BG_COLOR);
    }
    /* 同步 cur_col 到输入光标 */
    cur_col = prompt_len + input_cursor;
    term_redraw_cursor();
}

static void insert_char(char c) {
    if (input_len >= 255) return;
    /* 在 input_cursor 处插入 */
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
    /* 删除 input_cursor-1 处的字符 */
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
    g_fb = (u32 *)(u64)fb_a;

    term_init_pos();
    term_clear();
    term_redraw_all();

    /* 启动 banner */
    term_puts_color("=== Deshab OS v0.1.0 ===\n", PROMPT_FG);
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

        /* 等待命令输入 */
        int cmd_done = 0;
        while (!cmd_done) {
            u8 st = inb(0x64);
            if (!(st & 1)) { __asm__("pause"); continue; }
            u8 data = inb(0x60);
            /* 忽略鼠标数据（AUX 位） */
            if (st & 0x20) continue;
            u8 sc = data;
            if (sc == 0xE0) { e0 = 1; continue; }
            if (sc == 0x2A || sc == 0x36) { shift = 1; continue; }
            if (sc == 0xAA || sc == 0xB6) { shift = 0; continue; }
            if (sc & 0x80) {
                /* 松开事件 — 仅处理扩展键 */
                if (e0 && sc == 0x9C) { e0 = 0; }  /* Enter 释放 */
                else if (e0 && sc == 0xCB) { e0 = 0; }  /* Left 释放 */
                else if (e0 && sc == 0xCD) { e0 = 0; }  /* Right 释放 */
                else if (e0 && sc == 0xC8) { e0 = 0; }  /* Up 释放 */
                else if (e0 && sc == 0xD0) { e0 = 0; }  /* Down 释放 */
                else if (e0 && sc == 0xD3) { e0 = 0; }  /* Delete 释放 */
                else if (e0 && sc == 0xC7) { e0 = 0; }  /* Home 释放 */
                else if (e0 && sc == 0xCF) { e0 = 0; }  /* End 释放 */
                e0 = 0;
                continue;
            }
            /* 按下事件 */
            if (e0) {
                /* 扩展键 */
                if (sc == 0x4B) cursor_left();        /* Left */
                else if (sc == 0x4D) cursor_right();  /* Right */
                else if (sc == 0x47) cursor_home();   /* Home */
                else if (sc == 0x4F) cursor_end();    /* End */
                else if (sc == 0x53) delete_char_fwd(); /* Delete */
                else if (sc == 0x48) { /* Up — 命令历史暂未实现 */ }
                else if (sc == 0x50) { /* Down — 命令历史暂未实现 */ }
                e0 = 0;
                continue;
            }
            /* 控制键 */
            if (sc == 0x1C) {
                /* Enter — 执行命令 */
                term_clear_cursor();
                input_buf[input_len] = 0;
                term_putc('\n');
                execute_command(input_buf);
                cmd_done = 1;
                continue;
            }
            if (sc == 0x0E) {
                /* Backspace */
                delete_char_back();
                continue;
            }
            if (sc == 0x01) {
                /* Esc — 清行 */
                kill_line();
                continue;
            }
            /* Ctrl+U (kill line) */
            if (sc == 0x15 && !shift) {
                kill_line();
                continue;
            }
            if (sc == 0x17 && !shift) {
                /* Ctrl+W — 删除到行首 */
                while (input_cursor > 0) delete_char_back();
                continue;
            }
            if (sc == 0x0B && !shift) {
                /* Ctrl+K — 删除到行尾 */
                kill_to_end();
                continue;
            }
            /* 普通字符 */
            char c = scan_to_ascii(sc, shift);
            if (c && c >= 32 && c <= 126) {
                insert_char(c);
            }
        }
    }
}
