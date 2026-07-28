/* Deshab CMD — Windows 风格命令行（PE/EXE 兼容层前端）
 *
 * 黑底白字 cmd.exe 风格，PS/2 键盘输入。
 * 内置命令：dir/cd/type/copy/del/ren/mkdir/rmdir/echo/cls/ver/set/help/exit
 * PE 执行：输入 foo.exe 或 foo → 从 FAT32 根读取 → 调用 UTSM PE 服务执行
 *   - PE32+ (64位)：UTSM 原生 Ring0 执行
 *   - PE32  (32位)：UTSM 内嵌 x86-32 解释器执行
 * Esc 返回桌面。
 */

#include "../UTSM/include/utsm/dsk.h"
#include "../UTSM/include/utsm/pe.h"

typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;
typedef long long          i64;

#define COM1 0x3F8

static __inline__ void outb(u16 port, u8 value) { __asm__ volatile("outb %0,%1"::"a"(value),"Nd"(port)); }
static __inline__ u8 inb(u16 port) { u8 v; __asm__ volatile("inb %1,%0":"=a"(v):"Nd"(port)); return v; }

/* ---- TSC 计时 ---- */
static u64 g_tsc_per_ms = 0;

static __inline__ u64 rdtsc_cmd(void) {
    u32 lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((u64)hi << 32) | lo;
}

static void tsc_calibrate_cmd(void) {
    outb(0x43, 0x30);
    outb(0x40, 0x7c);
    outb(0x40, 0x2e);
    u64 tsc_start = rdtsc_cmd();
    u16 prev = 0; u64 loops = 0;
    for (;;) {
        outb(0x43, 0x00);
        u16 cur = (u16)inb(0x40) | ((u16)inb(0x40) << 8);
        if (cur > prev && loops > 10) break;
        prev = cur; loops++;
    }
    u64 tsc_end = rdtsc_cmd();
    g_tsc_per_ms = (tsc_end - tsc_start) / 10;
}

static void serial_wait_tx(void) {
    if (g_tsc_per_ms == 0) tsc_calibrate_cmd();
    if (g_tsc_per_ms == 0) {
        for (u32 i = 0; i < 100000; i++) { if (inb(COM1 + 5) & 0x20) break; }
        return;
    }
    u64 deadline = rdtsc_cmd() + g_tsc_per_ms / 10;
    while (rdtsc_cmd() < deadline) {
        if (inb(COM1 + 5) & 0x20) break;
        __asm__ volatile("pause");
    }
}

static void sputc(char c) { serial_wait_tx(); outb(COM1, (unsigned char)c); }
static void swrite(const char *s) { while (*s) { if (*s == '\n') sputc('\r'); sputc(*s++); } }
static void logl(const char *s) { swrite(s); swrite("\n"); }

/* ASCII 字体 — 必须在 deshab_ui.h 之前包含 */
#include "../firstInit/ascii_bitmaps.c"
#include "../UTSM/include/utsm/deshab_ui.h"
/* FAT32 共享读写库 */
#include "../tools/fat32_io.h"

/* ============================================================
 *  颜色定义 — Windows cmd.exe 风格（黑底浅灰字）
 * ============================================================ */

#define CMD_BG       0xFF000000u   /* 纯黑背景 */
#define CMD_TEXT     0xFFC0C0C0u   /* 浅灰文字（cmd.exe 默认） */
#define CMD_PROMPT   0xFFFFFFFFu   /* 白色提示符 */
#define CMD_ERROR    0xFFFF6060u   /* 浅红错误 */
#define CMD_OK       0xFF60FF60u   /* 浅绿成功 */
#define CMD_INFO     0xFF60A0FFu   /* 浅蓝信息 */
#define CMD_DIM      0xFF808080u   /* 灰色暗淡 */
#define CMD_WARN     0xFFFFFF60u   /* 黄色警告 */

/* ============================================================
 *  终端缓冲区与渲染
 * ============================================================ */

#define TERM_COLS  100
#define TERM_ROWS  40
#define TERM_MAX_CHARS (TERM_COLS * TERM_ROWS)

static du_context g_ctx;
static u64 fb_a, fb_w, fb_h, fb_p;

static u8  term_ch[TERM_MAX_CHARS];
static u32 term_fg[TERM_MAX_CHARS];
static int term_w = TERM_COLS;
static int term_h = TERM_ROWS;
static int cur_col = 0;
static int cur_row = 0;
static int cur_visible = 1;
static int term_start_x, term_start_y;

static const dsk_boot_context *g_boot_ctx = 0;
static u64 g_kernel_api = 0;
static const pe_service *g_pe_svc = 0;

/* dev_mode 串口镜像 */
static int g_serial_mirror = 0;

static void term_init_pos(void) {
    int margin = 4;
    term_start_x = margin;
    term_start_y = margin;
    int avail_w = (int)fb_w - margin * 2;
    int avail_h = (int)fb_h - margin * 2;
    term_w = avail_w / (int)DU_ASCII_STEP;
    term_h = avail_h / (int)DU_ASCII_LINE_H;
    if (term_w < 10) term_w = 10;
    if (term_h < 5) term_h = 5;
    if (term_w > TERM_COLS) term_w = TERM_COLS;
    if (term_h > TERM_ROWS) term_h = TERM_ROWS;
}

static void term_clear_buf(void) {
    for (int i = 0; i < TERM_MAX_CHARS; i++) {
        term_ch[i] = ' ';
        term_fg[i] = CMD_TEXT;
    }
    cur_col = 0;
    cur_row = 0;
}

static void term_scroll(void) {
    for (int r = 1; r < term_h; r++) {
        for (int c = 0; c < term_w; c++) {
            int src = r * TERM_COLS + c;
            int dst = (r - 1) * TERM_COLS + c;
            term_ch[dst] = term_ch[src];
            term_fg[dst] = term_fg[src];
        }
    }
    for (int c = 0; c < term_w; c++) {
        term_ch[(term_h - 1) * TERM_COLS + c] = ' ';
        term_fg[(term_h - 1) * TERM_COLS + c] = CMD_TEXT;
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
    if (g_serial_mirror) { if (c == '\n') sputc('\r'); sputc(c); }
    if (c == '\n') { term_newline(); return; }
    if (c == '\r') { cur_col = 0; return; }
    if (c == '\t') {
        int spaces = 8 - (cur_col % 8);
        for (int i = 0; i < spaces; i++) term_putc_color(' ', color);
        return;
    }
    if (c < ' ' || c > '~') c = '.';
    if (cur_col >= term_w) term_newline();
    int idx = cur_row * TERM_COLS + cur_col;
    term_ch[idx] = (u8)c;
    term_fg[idx] = color;
    cur_col++;
}

static void term_putc(char c) { term_putc_color(c, CMD_TEXT); }
static void term_puts_color(const char *s, u32 color) { while (*s) term_putc_color(*s++, color); }
static void term_puts(const char *s) { term_puts_color(s, CMD_TEXT); }

/* ---- 数字转字符串 ---- */
static void term_putu(u32 v) {
    char buf[12];
    int i = 11;
    buf[i--] = 0;
    if (v == 0) { buf[i--] = '0'; }
    while (v > 0 && i >= 0) { buf[i--] = (char)('0' + (v % 10)); v /= 10; }
    term_puts(&buf[i + 1]);
}

static void term_putx(u32 v) {
    static const char hex[] = "0123456789ABCDEF";
    term_putc('0'); term_putc('x');
    for (int i = 28; i >= 0; i -= 4) {
        u32 nib = (v >> i) & 0xF;
        if (nib != 0 || i <= 12) term_putc(hex[nib]);
    }
}

/* ---- 终端全屏渲染 ---- */
static void term_redraw_all(void) {
    du_fill_rect(&g_ctx, 0, 0, (i64)fb_w, (i64)fb_h, CMD_BG);
    for (int r = 0; r < term_h; r++) {
        i64 y = term_start_y + (i64)r * (i64)DU_ASCII_LINE_H;
        for (int c = 0; c < term_w; c++) {
            int idx = r * TERM_COLS + c;
            i64 x = term_start_x + (i64)c * (i64)DU_ASCII_STEP;
            du_draw_char(&g_ctx, (char)term_ch[idx], x, y, term_fg[idx], CMD_BG);
        }
    }
}

static void term_redraw_cursor(void) {
    if (cur_col >= term_w) return;
    i64 x = term_start_x + (i64)cur_col * (i64)DU_ASCII_STEP;
    i64 y = term_start_y + (i64)cur_row * (i64)DU_ASCII_LINE_H;
    du_fill_rect(&g_ctx, x, y, (i64)DU_ASCII_STEP - 1, (i64)DU_ASCII_CELL_H, CMD_PROMPT);
}

static void term_clear_cursor(void) {
    if (cur_col >= term_w) return;
    i64 x = term_start_x + (i64)cur_col * (i64)DU_ASCII_STEP;
    i64 y = term_start_y + (i64)cur_row * (i64)DU_ASCII_LINE_H;
    int idx = cur_row * TERM_COLS + cur_col;
    du_fill_rect(&g_ctx, x, y, (i64)DU_ASCII_STEP - 1, (i64)DU_ASCII_CELL_H, CMD_BG);
    du_draw_char(&g_ctx, (char)term_ch[idx], x, y, term_fg[idx], CMD_BG);
}

/* ============================================================
 *  键盘输入（PS/2 Scan Code Set 1）
 * ============================================================ */

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

/* ============================================================
 *  字符串工具
 * ============================================================ */

static int str_eq(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

static int str_len(const char *s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

static void str_copy(char *dst, const char *src, int max) {
    int i = 0;
    while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static int str_ieq(const char *a, const char *b) {
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'a' && ca <= 'z') ca -= 32;
        if (cb >= 'a' && cb <= 'z') cb -= 32;
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == *b;
}

/* ============================================================
 *  输入行编辑
 * ============================================================ */

static char input_buf[256];
static int  input_len = 0;
static int  input_cursor = 0;

static const char *PROMPT = "C:\\> ";

static void redraw_input_line(void) {
    int prompt_len = str_len(PROMPT);
    i64 y = term_start_y + (i64)cur_row * (i64)DU_ASCII_LINE_H;
    du_fill_rect(&g_ctx, term_start_x, y,
                 (i64)term_w * (i64)DU_ASCII_STEP,
                 (i64)DU_ASCII_CELL_H, CMD_BG);
    for (int i = 0; i < prompt_len; i++) {
        i64 x = term_start_x + (i64)i * (i64)DU_ASCII_STEP;
        du_draw_char(&g_ctx, PROMPT[i], x, y, CMD_PROMPT, CMD_BG);
    }
    for (int i = 0; i < input_len; i++) {
        i64 x = term_start_x + (i64)(prompt_len + i) * (i64)DU_ASCII_STEP;
        du_draw_char(&g_ctx, input_buf[i], x, y, CMD_TEXT, CMD_BG);
    }
    cur_col = prompt_len + input_cursor;
    term_redraw_cursor();
}

static void insert_char(char c) {
    if (input_len >= 255) return;
    for (int i = input_len; i > input_cursor; i--)
        input_buf[i] = input_buf[i - 1];
    input_buf[input_cursor] = c;
    input_len++;
    input_cursor++;
    redraw_input_line();
}

static void delete_char_back(void) {
    if (input_cursor == 0) return;
    for (int i = input_cursor - 1; i < input_len - 1; i++)
        input_buf[i] = input_buf[i + 1];
    input_len--;
    input_cursor--;
    input_buf[input_len] = 0;
    redraw_input_line();
}

static void cursor_left(void) {
    if (input_cursor > 0) { input_cursor--; redraw_input_line(); }
}

static void cursor_right(void) {
    if (input_cursor < input_len) { input_cursor++; redraw_input_line(); }
}

static void cursor_home(void) {
    input_cursor = 0;
    redraw_input_line();
}

static void cursor_end(void) {
    input_cursor = input_len;
    redraw_input_line();
}

static void kill_line(void) {
    input_len = 0;
    input_cursor = 0;
    input_buf[0] = 0;
    redraw_input_line();
}

/* ============================================================
 *  内置命令
 * ============================================================ */

static void cmd_help(void) {
    term_puts_color("Deshab CMD — 命令列表:\r\n", CMD_INFO);
    term_puts("  dir          列出文件\r\n");
    term_puts("  cd           显示当前目录\r\n");
    term_puts("  type <file>  显示文件内容\r\n");
    term_puts("  copy <a> <b> 复制文件\r\n");
    term_puts("  del <file>   删除文件\r\n");
    term_puts("  ren <a> <b>  重命名文件\r\n");
    term_puts("  echo <text>  显示文本 (echo text > file 写入)\r\n");
    term_puts("  cls          清屏\r\n");
    term_puts("  ver          显示版本\r\n");
    term_puts("  set          显示环境变量\r\n");
    term_puts("  help         显示此帮助\r\n");
    term_puts("  exit         返回桌面\r\n");
    term_puts_color("\nPE 程序执行:\r\n", CMD_WARN);
    term_puts("  <name>.exe   运行 PE32+/PE32 程序 (如 hello.exe)\r\n");
    term_puts("  <name>       自动查找 name.exe 并运行\r\n");
}

static void cmd_ver(void) {
    term_puts_color("Deshab CMD [Version 0.1.0]\r\n", CMD_OK);
    term_puts_color("(c) 2026 Deshab Project. PE/EXE 兼容层 by UTSM.\r\n", CMD_DIM);
}

static void cmd_cls(void) {
    term_clear_buf();
    term_redraw_all();
}

static void cmd_cd(void) {
    term_puts("C:\\\r\n");
}

static void cmd_set(void) {
    term_puts_color("环境变量:\r\n", CMD_INFO);
    term_puts("  PATH=C:\\;C:\\bin\r\n");
    term_puts("  PROMPT=$P$G\r\n");
    term_puts("  OS=Deshab\r\n");
    term_puts("  PE_COMPAT=UTSM_PE32+_PE32\r\n");
}

/* dir — 列出 FAT32 根目录文件 */
typedef struct {
    int count;
    u32 total_bytes;
} dir_ctx_static;

static int dir_list_cb(const char *name, u32 size, u8 attr, void *ud) {
    dir_ctx_static *st = (dir_ctx_static *)ud;
    if (attr & 0x10) {
        term_puts_color(name, CMD_INFO);
        term_puts_color("  <DIR>        ", CMD_DIM);
    } else {
        term_puts(name);
        term_puts("  ");
        /* 右对齐文件大小 */
        u32 sz = size;
        char numbuf[12];
        int ni = 11;
        numbuf[ni--] = 0;
        if (sz == 0) numbuf[ni--] = '0';
        while (sz > 0 && ni >= 0) { numbuf[ni--] = (char)('0' + sz % 10); sz /= 10; }
        while (ni >= 0) numbuf[ni--] = ' ';
        term_puts(&numbuf[0]);
        st->total_bytes += size;
    }
    term_puts("\r\n");
    st->count++;
    return 0;
}

static void cmd_dir(void) {
    if (!f32_blk_read) {
        term_puts_color("dir: 无块设备\r\n", CMD_ERROR);
        return;
    }
    term_puts_color(" C 盘的卷是 DESHAB\r\n", CMD_DIM);
    term_puts_color(" 卷序列号为 DE5A-B002\r\n", CMD_DIM);
    term_puts("\r\n C:\\ 的目录\r\n\r\n");

    dir_ctx_static st = {0, 0};
    f32_list_root(dir_list_cb, &st);

    term_puts("\r\n");
    term_putu((u32)st.count);
    term_puts(" 个文件  ");
    term_putu(st.total_bytes);
    term_puts(" 字节\r\n");
}

/* type <file> — 显示文件内容 */
static void cmd_type(const char *args) {
    if (!*args) { term_puts_color("type: 缺少文件名\r\n", CMD_ERROR); return; }
    if (!f32_blk_read) { term_puts_color("type: 无块设备\r\n", CMD_ERROR); return; }

    char name11[11];
    if (f32_name_to_83(args, name11) != 0) {
        term_puts_color("type: 无效文件名\r\n", CMD_ERROR);
        return;
    }
    u8 *data = 0; u32 size = 0;
    if (f32_read_root_file(name11, &data, &size) != 0) {
        term_puts_color("type: 文件不存在\r\n", CMD_ERROR);
        return;
    }
    /* 输出文件内容（按字符） */
    for (u32 i = 0; i < size; i++) {
        char c = (char)data[i];
        if (c == '\n') term_putc('\r');
        term_putc(c);
    }
    if (size > 0 && data[size - 1] != '\n') term_putc('\r');
    term_putc('\n');
}

/* echo <text> — 显示文本或重定向到文件 */
static void cmd_echo(const char *args) {
    /* 检查重定向: echo text > file */
    char text[256];
    str_copy(text, args, sizeof(text));

    /* 查找 > */
    int redir = -1;
    for (int i = 0; text[i]; i++) {
        if (text[i] == '>') { redir = i; break; }
    }

    if (redir >= 0) {
        /* 重定向到文件 */
        text[redir] = 0;
        /* 去尾部空格 */
        int tl = str_len(text);
        while (tl > 0 && text[tl - 1] == ' ') text[--tl] = 0;

        /* 获取文件名 */
        char *fn = text + redir + 1;
        while (*fn == ' ') fn++;
        if (!*fn) { term_puts_color("echo: 重定向缺少文件名\r\n", CMD_ERROR); return; }

        char name11[11];
        if (f32_name_to_83(fn, name11) != 0) {
            term_puts_color("echo: 无效文件名\r\n", CMD_ERROR);
            return;
        }
        if (!f32_blk_write) { term_puts_color("echo: 无块设备写能力\r\n", CMD_ERROR); return; }

        /* 构造写入内容 */
        char content[256];
        int ci = 0;
        for (int i = 0; i < tl && ci < 254; i++) content[ci++] = text[i];
        content[ci++] = '\r';
        content[ci++] = '\n';

        if (f32_write_root_file(name11, (const u8 *)content, (u32)ci) != 0) {
            term_puts_color("echo: 写入失败\r\n", CMD_ERROR);
            return;
        }
        term_puts_color("echo: 已写入 ", CMD_OK);
        term_puts(fn);
        term_puts("\r\n");
    } else {
        /* 直接显示 */
        if (*args) {
            term_puts(args);
        }
        term_puts("\r\n");
    }
}

/* copy <src> <dst> — 复制文件 */
static void cmd_copy(const char *args) {
    char src[64], dst[64];
    int si = 0, di = 0;
    const char *p = args;
    while (*p == ' ') p++;
    while (*p && *p != ' ' && si < 63) src[si++] = *p++;
    src[si] = 0;
    while (*p == ' ') p++;
    while (*p && di < 63) dst[di++] = *p++;
    dst[di] = 0;

    if (!src[0] || !dst[0]) {
        term_puts_color("copy: 用法 copy <源文件> <目标文件>\r\n", CMD_ERROR);
        return;
    }
    if (!f32_blk_read || !f32_blk_write) {
        term_puts_color("copy: 无块设备\r\n", CMD_ERROR);
        return;
    }

    char s83[11], d83[11];
    if (f32_name_to_83(src, s83) != 0 || f32_name_to_83(dst, d83) != 0) {
        term_puts_color("copy: 无效文件名\r\n", CMD_ERROR);
        return;
    }

    u8 *data = 0; u32 size = 0;
    if (f32_read_root_file(s83, &data, &size) != 0) {
        term_puts_color("copy: 源文件不存在\r\n", CMD_ERROR);
        return;
    }
    if (f32_write_root_file(d83, data, size) != 0) {
        term_puts_color("copy: 写入失败\r\n", CMD_ERROR);
        return;
    }
    term_puts_color("        1 个文件已复制。\r\n", CMD_OK);
}

/* del <file> — 删除文件 */
static void cmd_del(const char *args) {
    if (!*args) { term_puts_color("del: 缺少文件名\r\n", CMD_ERROR); return; }
    if (!f32_blk_write) { term_puts_color("del: 无块设备写能力\r\n", CMD_ERROR); return; }

    char name11[11];
    if (f32_name_to_83(args, name11) != 0) {
        term_puts_color("del: 无效文件名\r\n", CMD_ERROR);
        return;
    }
    if (f32_delete_root_file(name11) != 0) {
        term_puts_color("del: 文件不存在或删除失败\r\n", CMD_ERROR);
        return;
    }
    term_puts_color("del: 已删除\r\n", CMD_OK);
}

/* ren <old> <new> — 重命名文件 */
static void cmd_ren(const char *args) {
    char old[64], nw[64];
    int oi = 0, ni = 0;
    const char *p = args;
    while (*p == ' ') p++;
    while (*p && *p != ' ' && oi < 63) old[oi++] = *p++;
    old[oi] = 0;
    while (*p == ' ') p++;
    while (*p && ni < 63) nw[ni++] = *p++;
    nw[ni] = 0;

    if (!old[0] || !nw[0]) {
        term_puts_color("ren: 用法 ren <旧名> <新名>\r\n", CMD_ERROR);
        return;
    }
    if (!f32_blk_read || !f32_blk_write) {
        term_puts_color("ren: 无块设备\r\n", CMD_ERROR);
        return;
    }

    char o83[11], n83[11];
    if (f32_name_to_83(old, o83) != 0 || f32_name_to_83(nw, n83) != 0) {
        term_puts_color("ren: 无效文件名\r\n", CMD_ERROR);
        return;
    }

    u8 *data = 0; u32 size = 0;
    if (f32_read_root_file(o83, &data, &size) != 0) {
        term_puts_color("ren: 源文件不存在\r\n", CMD_ERROR);
        return;
    }
    if (f32_write_root_file(n83, data, size) != 0) {
        term_puts_color("ren: 写入新文件失败\r\n", CMD_ERROR);
        return;
    }
    if (f32_delete_root_file(o83) != 0) {
        term_puts_color("ren: 删除旧文件失败（警告）\r\n", CMD_WARN);
    }
    term_puts_color("ren: 已重命名\r\n", CMD_OK);
}

static void cmd_mkdir(void) { term_puts_color("mkdir: FAT32 根目录不支持子目录\r\n", CMD_WARN); }
static void cmd_rmdir(void) { term_puts_color("rmdir: FAT32 根目录不支持子目录\r\n", CMD_WARN); }

/* ============================================================
 *  PE/EXE 程序执行
 * ============================================================ */

/* 检查文件名是否以 .exe/.com/.bat 结尾（不区分大小写） */
static int has_exec_ext(const char *name) {
    int len = str_len(name);
    if (len < 4) return 0;
    const char *ext = name + len - 4;
    if (ext[0] != '.') return 0;
    if (str_ieq(ext, ".exe")) return 1;
    if (str_ieq(ext, ".com")) return 1;
    if (str_ieq(ext, ".bat")) return 2;
    return 0;
}

/* 执行 PE/EXE 程序。
 * prog_name = 程序名（可能带或不带 .exe 后缀）
 * full_cmdline = 完整命令行（程序名 + 参数，即 GetCommandLineA 返回值）
 */
static void exec_pe(const char *prog_name, const char *full_cmdline) {
    if (!g_pe_svc) {
        term_puts_color("PE 兼容层不可用（UTSM PE 服务未初始化）\r\n", CMD_ERROR);
        return;
    }
    if (!f32_blk_read) {
        term_puts_color("无块设备，无法读取 PE 文件\r\n", CMD_ERROR);
        return;
    }

    /* 构造 8.3 文件名 */
    char base[64];
    str_copy(base, prog_name, sizeof(base));

    /* 如果没有扩展名，自动补 .exe */
    int has_dot = 0;
    for (int i = 0; base[i]; i++) {
        if (base[i] == '.') { has_dot = 1; break; }
    }
    if (!has_dot) {
        int bl = str_len(base);
        if (bl > 59) bl = 59;
        base[bl] = '.'; base[bl + 1] = 'e'; base[bl + 2] = 'x';
        base[bl + 3] = 'e'; base[bl + 4] = 0;
    }

    char name11[11];
    if (f32_name_to_83(base, name11) != 0) {
        term_puts_color("无效文件名\r\n", CMD_ERROR);
        return;
    }

    /* 从 FAT32 根目录读取 */
    u8 *pe_data = 0; u32 pe_size = 0;
    if (f32_read_root_file(name11, &pe_data, &pe_size) != 0) {
        term_puts_color("'", CMD_ERROR);
        term_puts_color(prog_name, CMD_ERROR);
        term_puts_color("' 不是内部或外部命令，也不是可运行的程序。\r\n", CMD_ERROR);
        return;
    }

    /* 验证 PE 头（MZ 签名） */
    if (pe_size < 2 || pe_data[0] != 'M' || pe_data[1] != 'Z') {
        term_puts_color("不是有效的 PE/EXE 文件（缺少 MZ 签名）\r\n", CMD_ERROR);
        return;
    }

    term_puts_color("正在执行 ", CMD_INFO);
    term_puts_color(prog_name, CMD_WARN);
    term_puts_color(" ...\r\n", CMD_INFO);
    term_redraw_all();

    /* 调用 UTSM PE 服务执行 */
    logl("[cmd] PE run begin");
    swrite("[cmd] cmdline: "); swrite(full_cmdline); swrite("\n");

    u64 exit_code = 0;
    int rc = g_pe_svc->run(pe_data, (u64)pe_size, full_cmdline, &exit_code);

    if (rc != 0) {
        term_puts_color("PE 执行失败，错误码: ", CMD_ERROR);
        term_putx((u32)rc);
        term_puts("\r\n");
    } else {
        term_puts_color("程序退出，返回码: ", CMD_DIM);
        term_putu((u32)exit_code);
        term_puts("\r\n");
    }

    logl("[cmd] PE run end");
}

/* ============================================================
 *  命令解析与分发
 * ============================================================ */

static int g_should_exit = 0;

static void execute_command(const char *cmd) {
    while (*cmd == ' ') cmd++;
    if (*cmd == 0) return;

    /* 提取命令名 */
    const char *args = cmd;
    while (*args && *args != ' ') args++;
    int cmd_name_len = (int)(args - cmd);
    char name[64];
    if (cmd_name_len >= 64) cmd_name_len = 63;
    for (int i = 0; i < cmd_name_len; i++) name[i] = cmd[i];
    name[cmd_name_len] = 0;
    while (*args == ' ') args++;

    /* 内置命令（不区分大小写） */
    if (str_ieq(name, "help") || str_eq(name, "?")) { cmd_help(); return; }
    if (str_ieq(name, "ver")) { cmd_ver(); return; }
    if (str_ieq(name, "cls") || str_ieq(name, "clear")) { cmd_cls(); return; }
    if (str_ieq(name, "dir") || str_ieq(name, "ls")) { cmd_dir(); return; }
    if (str_ieq(name, "cd")) { cmd_cd(); return; }
    if (str_ieq(name, "type") || str_ieq(name, "cat")) { cmd_type(args); return; }
    if (str_ieq(name, "copy") || str_ieq(name, "cp")) { cmd_copy(args); return; }
    if (str_ieq(name, "del") || str_ieq(name, "rm")) { cmd_del(args); return; }
    if (str_ieq(name, "ren") || str_ieq(name, "mv")) { cmd_ren(args); return; }
    if (str_ieq(name, "mkdir") || str_ieq(name, "md")) { cmd_mkdir(); return; }
    if (str_ieq(name, "rmdir") || str_ieq(name, "rd")) { cmd_rmdir(); return; }
    if (str_ieq(name, "echo")) { cmd_echo(args); return; }
    if (str_ieq(name, "set")) { cmd_set(); return; }
    if (str_ieq(name, "exit") || str_ieq(name, "quit")) {
        term_puts_color("返回桌面...\r\n", CMD_DIM);
        g_should_exit = 1;
        return;
    }
    if (str_ieq(name, "reboot")) {
        term_puts_color("重启中...\r\n", CMD_WARN);
        term_redraw_all();
        for (u64 i = 0; i < 100000000ULL; i++) __asm__ volatile("pause");
        /* 触发三重故障重启 */
        __asm__ volatile("int $0x03");
        return;
    }

    /* PE/EXE 执行：检查是否为可执行文件 */
    int ext_type = has_exec_ext(name);
    if (ext_type == 1) {
        /* .exe/.com → PE 执行 */
        exec_pe(name, cmd);
        return;
    }
    if (ext_type == 2) {
        /* .bat → 批处理（暂不支持） */
        term_puts_color("批处理文件 (.bat) 暂不支持\r\n", CMD_WARN);
        return;
    }
    /* 无扩展名 → 尝试自动补 .exe 执行 */
    if (cmd_name_len > 0 && name[0] != '.') {
        exec_pe(name, cmd);
        return;
    }

    /* 未知命令 */
    term_puts_color("'", CMD_ERROR);
    term_puts_color(name, CMD_ERROR);
    term_puts_color("' 不是内部或外部命令，也不是可运行的程序。\r\n", CMD_ERROR);
}

/* ============================================================
 *  主入口
 * ============================================================ */

__attribute__((visibility("default")))
void dsk_entry(const dsk_boot_context *ctx) {
    __asm__ volatile("cli");
    logl("[cmd] boot");

    if (!ctx || ctx->magic != 0x44534B31424F4F54ULL) {
        logl("[cmd] bad context");
        for (;;) __asm__("hlt");
    }

    fb_a = ctx->framebuffer_address;
    fb_w = ctx->framebuffer_width;
    fb_h = ctx->framebuffer_height;
    fb_p = ctx->framebuffer_pitch;

    g_kernel_api = ctx->dkm_kernel_api;
    g_boot_ctx = ctx;

    /* 获取 block 设备 read/write */
    {
        u64 api = ctx->dkm_kernel_api;
        u64 blk = *(u64 *)(api + 0xA8);
        f32_block_read_fn  bread = blk ? (f32_block_read_fn)*(u64 *)(blk + 16) : 0;
        f32_block_write_fn bwrite = blk ? (f32_block_write_fn)*(u64 *)(blk + 24) : 0;
        f32_init(bread, bwrite);
    }
    if (f32_blk_read)  logl("[cmd] block_read ok");
    if (f32_blk_write) logl("[cmd] block_write ok");

    /* 获取 PE 服务（reserved[4]） */
    {
        u64 pe_addr = ctx->reserved[4];
        if (pe_addr) {
            const pe_service *pe = (const pe_service *)pe_addr;
            if (pe->magic == PE_SERVICE_MAGIC) {
                g_pe_svc = pe;
                logl("[cmd] PE service ok");
            } else {
                logl("[cmd] PE service magic mismatch");
            }
        } else {
            logl("[cmd] PE service not available");
        }
    }

    /* dev_mode 串口镜像 */
    if (ctx->reserved[3] == 1) {
        g_serial_mirror = 1;
    }

    /* 初始化渲染上下文 */
    du_context_init(&g_ctx, fb_a, fb_w, fb_h, fb_p);
    term_init_pos();
    term_clear_buf();
    term_redraw_all();

    /* 启动 banner */
    term_puts_color("Deshab CMD [Version 0.1.0]\r\n", CMD_OK);
    term_puts_color("(c) 2026 Deshab Project. PE/EXE 兼容层 by UTSM.\r\n", CMD_DIM);
    term_putc('\r');
    term_putc('\n');
    if (g_pe_svc) {
        term_puts_color("PE 兼容层已就绪 (PE32+ 原生 + PE32 解释器)\r\n", CMD_OK);
    } else {
        term_puts_color("警告: PE 兼容层不可用\r\n", CMD_WARN);
    }
    term_putc('\r');
    term_putc('\n');
    term_redraw_all();

    /* 主循环 */
    int shift = 0;
    int e0 = 0;
    u64 blink_start = rdtsc_cmd();
    g_should_exit = 0;

    for (;;) {
        /* 绘制提示符 */
        term_puts_color(PROMPT, CMD_PROMPT);
        input_len = 0;
        input_cursor = 0;
        input_buf[0] = 0;
        int prompt_len = str_len(PROMPT);
        cur_col = prompt_len;
        cur_visible = 1;
        redraw_input_line();

        int cmd_done = 0;
        while (!cmd_done && !g_should_exit) {
            /* 光标闪烁 */
            if (g_tsc_per_ms) {
                u64 now = rdtsc_cmd();
                u64 elapsed = now - blink_start;
                int want_vis = (elapsed / (g_tsc_per_ms * 530)) & 1;
                if (want_vis != cur_visible) {
                    if (want_vis) term_redraw_cursor();
                    else term_clear_cursor();
                    cur_visible = want_vis;
                }
            }

            u8 st = inb(0x64);
            if (!(st & 1)) { __asm__("pause"); continue; }
            u8 data = inb(0x60);
            if (st & 0x20) continue;  /* 忽略鼠标数据 */
            u8 sc = data;

            blink_start = rdtsc_cmd();
            if (!cur_visible) { cur_visible = 1; term_redraw_cursor(); }

            if (sc == 0xE0) { e0 = 1; continue; }
            if (sc == 0x2A || sc == 0x36) { shift = 1; continue; }
            if (sc == 0xAA || sc == 0xB6) { shift = 0; continue; }

            if (sc & 0x80) {
                /* 按键释放 */
                e0 = 0;
                continue;
            }

            if (e0) {
                if (sc == 0x4B) cursor_left();
                else if (sc == 0x4D) cursor_right();
                else if (sc == 0x47) cursor_home();
                else if (sc == 0x4F) cursor_end();
                else if (sc == 0x53) { /* Delete */ }
                e0 = 0;
                continue;
            }

            if (sc == 0x1C) {
                /* Enter — 提交命令 */
                term_clear_cursor();
                input_buf[input_len] = 0;
                term_putc('\r');
                term_putc('\n');
                execute_command(input_buf);
                input_len = 0;
                input_cursor = 0;
                input_buf[0] = 0;
                cmd_done = 1;
                term_redraw_all();
                continue;
            }

            if (sc == 0x0E) { delete_char_back(); continue; }
            if (sc == 0x01) {
                /* Esc — 返回桌面 */
                logl("[cmd] esc -> return to DSK");
                g_should_exit = 1;
                cmd_done = 1;
                continue;
            }
            if (sc == 0x15 && !shift) { kill_line(); continue; }

            char c = scan_to_ascii(sc, shift);
            if (c && c >= 32 && c <= 126) {
                insert_char(c);
            }
        }

        if (g_should_exit) break;
    }

    logl("[cmd] exit");
}
