/* Deshab Bash — 全屏终端应用
 * PS/2 键盘输入，命令行编辑，内置命令。
 * Esc 键退出返回 DSK。
 */

#include "../../firstInit/ascii_bitmaps.c"
#include "../desktop_app.h"

/* ---- 终端参数 ---- */
#define TERM_COLS  120
#define TERM_ROWS  40
#define TERM_MAX_CHARS (TERM_COLS * TERM_ROWS)
#define CHAR_STEP  12
#define CHAR_H     18
#define PROMPT_STR "deshab# "

static da_app_context g_ac;

/* 终端缓冲区 */
static u8  term_ch[TERM_MAX_CHARS];
static u32 term_fg[TERM_MAX_CHARS];
static int term_w = TERM_COLS;
static int term_h = TERM_ROWS;
static int cur_col = 0;
static int cur_row = 0;

/* 输入缓冲 */
static char input_buf[256];
static int input_len = 0;
static int input_cursor = 0;

/* 命令历史 */
#define HIST_SIZE 16
static char history[HIST_SIZE][256];
static int hist_count = 0;
static int hist_idx = 0;

/* 坐标 */
static int term_x, term_y;
static int content_h;

static void term_init_layout(void) {
    term_x = 8;
    term_y = DA_TITLEBAR_H + 4;
    int avail_w = (int)g_ac.fb_w - 16;
    int avail_h = (int)g_ac.fb_h - term_y - DA_STATUSBAR_H - 8;
    term_w = avail_w / CHAR_STEP;
    term_h = avail_h / CHAR_H;
    if (term_w < 10) term_w = 10;
    if (term_h < 5) term_h = 5;
    if (term_w > TERM_COLS) term_w = TERM_COLS;
    if (term_h > TERM_ROWS) term_h = TERM_ROWS;
    content_h = term_h * CHAR_H;
}

static void term_clear(void) {
    for (int i = 0; i < TERM_MAX_CHARS; i++) {
        term_ch[i] = ' ';
        term_fg[i] = DA_TEXT_PRIMARY;
    }
    cur_col = 0; cur_row = 0;
}

static void term_scroll(void) {
    for (int r = 1; r < term_h; r++) {
        for (int c = 0; c < term_w; c++) {
            term_ch[(r-1)*TERM_COLS+c] = term_ch[r*TERM_COLS+c];
            term_fg[(r-1)*TERM_COLS+c] = term_fg[r*TERM_COLS+c];
        }
    }
    for (int c = 0; c < term_w; c++) {
        term_ch[(term_h-1)*TERM_COLS+c] = ' ';
        term_fg[(term_h-1)*TERM_COLS+c] = DA_TEXT_PRIMARY;
    }
    cur_row = term_h - 1; cur_col = 0;
}

static void term_newline(void) {
    cur_col = 0; cur_row++;
    if (cur_row >= term_h) term_scroll();
}

static void term_putc_color(char c, u32 color) {
    if (c == '\n') { term_newline(); return; }
    if (c == '\r') { cur_col = 0; return; }
    if (c == '\t') { int sp = 4 - (cur_col % 4); for (int i = 0; i < sp; i++) term_putc_color(' ', color); return; }
    if (c < ' ' || c > '~') c = '?';
    if (cur_col >= term_w) term_newline();
    int idx = cur_row * TERM_COLS + cur_col;
    term_ch[idx] = (u8)c; term_fg[idx] = color;
    cur_col++;
}

static void term_putc(char c) { term_putc_color(c, DA_TEXT_PRIMARY); }
static void term_puts_color(const char *s, u32 color) { while (*s) term_putc_color(*s++, color); }
static void term_puts(const char *s) { term_puts_color(s, DA_TEXT_PRIMARY); }

/* ---- 绘制 ---- */
static void redraw_all(void) {
    da_fill_bg(&g_ac, DA_BG_PRIMARY);
    da_draw_titlebar(&g_ac, "Deshab Bash", (i64)g_ac.fb_w);
    /* 终端字符 */
    for (int r = 0; r < term_h; r++) {
        for (int c = 0; c < term_w; c++) {
            int idx = r * TERM_COLS + c;
            if (term_ch[idx] == ' ') continue;
            da_draw_char(&g_ac, term_ch[idx],
                         term_x + c * CHAR_STEP, term_y + r * CHAR_H,
                         term_fg[idx], DA_BG_PRIMARY);
        }
    }
    /* 光标 */
    if (cur_col < term_w && cur_row < term_h) {
        da_fill_rect(&g_ac, term_x + cur_col * CHAR_STEP,
                     term_y + cur_row * CHAR_H + CHAR_H - 3, 11, 2, DA_ACCENT);
    }
    /* 状态栏 */
    char status[64];
    int p = 0;
    const char *s1 = "/  root@deshab  ";
    while (*s1) status[p++] = *s1++;
    u8 hh, mm; da_rtc_time(&hh, &mm);
    status[p++] = '0' + hh/10; status[p++] = '0' + hh%10;
    status[p++] = ':'; status[p++] = '0' + mm/10; status[p++] = '0' + mm%10;
    status[p] = 0;
    da_draw_statusbar(&g_ac, status, (i64)g_ac.fb_w, (i64)g_ac.fb_h);
}

/* ---- 内置命令 ---- */
static int str_eq(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

static void cmd_help(void) {
    term_puts_color("Deshab Bash - 内置命令:\n", DA_PROMPT_COLOR);
    term_puts("  help        显示此帮助\n");
    term_puts("  clear       清屏\n");
    term_puts("  echo <text> 显示文本\n");
    term_puts("  version     显示系统版本\n");
    term_puts("  ver         同 version\n");
    term_puts("  uname       显示系统名称\n");
    term_puts("  date        显示当前日期时间\n");
    term_puts("  about       关于 Deshab\n");
    term_puts("  reboot      重启系统\n");
    term_puts("  halt        关机\n");
    term_puts("  pwd         显示当前目录\n");
    term_puts("  whoami      显示当前用户\n");
    term_puts("  id          显示用户ID\n");
    term_puts("  ls          列出根目录文件\n");
    term_puts("  cat <file>  显示文件内容\n");
}

static void cmd_version(void) {
    term_puts_color("Deshab OS v0.1.0\n", DA_SUCCESS);
    term_puts("  Kernel: DSK (Deshab System Kernel)\n");
    term_puts("  Architecture: x86_64\n");
    term_puts("  ABI: DKM v1\n");
    term_puts("  Bootloader: Limine\n");
}

static void cmd_uname(void) { term_puts("Deshab\n"); }

static void cmd_about(void) {
    term_puts_color("=== Deshab OS ===\n", DA_PROMPT_COLOR);
    term_puts("单地址空间 Ring0 内核实验系统\n");
    term_puts("架构: UTSM + DSK + DKM\n");
    term_puts_color("Built with Clang + lld\n", DA_TEXT_DIM);
}

static void cmd_clear(void) { term_clear(); }

static void cmd_reboot(void) {
    term_puts_color("Rebooting...\n", DA_PROMPT_COLOR);
    redraw_all();
    outb(0x64, 0xFE);
    for(;;) __asm__("hlt");
}

static void cmd_halt(void) {
    term_puts_color("System halted.\n", DA_PROMPT_COLOR);
    redraw_all();
    for(;;) __asm__("hlt");
}

static void cmd_echo(const char *args) {
    if (args && *args) term_puts_color(args, DA_TEXT_PRIMARY);
    term_putc('\n');
}

static void cmd_date(void) {
    u8 year, month, day, hour, minute, second;
    outb(0x70, 9); year = inb(0x71);
    outb(0x70, 8); month = inb(0x71);
    outb(0x70, 7); day = inb(0x71);
    outb(0x70, 4); hour = inb(0x71);
    outb(0x70, 2); minute = inb(0x71);
    outb(0x70, 0); second = inb(0x71);
    year = (u8)((year>>4)*10+(year&0xF));
    month = (u8)((month>>4)*10+(month&0xF));
    day = (u8)((day>>4)*10+(day&0xF));
    hour = (u8)((hour>>4)*10+(hour&0xF));
    minute = (u8)((minute>>4)*10+(minute&0xF));
    second = (u8)((second>>4)*10+(second&0xF));
    char buf[32]; int p = 0;
    buf[p++]='2'; buf[p++]='0'; buf[p++]='0'+year/10; buf[p++]='0'+year%10; buf[p++]='-';
    buf[p++]='0'+month/10; buf[p++]='0'+month%10; buf[p++]='-';
    buf[p++]='0'+day/10; buf[p++]='0'+day%10; buf[p++]=' ';
    buf[p++]='0'+hour/10; buf[p++]='0'+hour%10; buf[p++]=':';
    buf[p++]='0'+minute/10; buf[p++]='0'+minute%10; buf[p++]=':';
    buf[p++]='0'+second/10; buf[p++]='0'+second%10; buf[p]=0;
    term_puts(buf); term_putc('\n');
}

static void cmd_ls(void) {
    if (!g_ac.block_read) {
        term_puts_color("ls: 无块设备可用\n", DA_ERROR);
        return;
    }
    /* 读取 FAT32 根目录 — 简化版：读取 BPB 获取根目录区 */
    u8 bpb[512];
    if (g_ac.block_read(0, 0, 1, bpb) != 0) {
        term_puts_color("ls: 读取 BPB 失败\n", DA_ERROR);
        return;
    }
    /* 检查 FAT32 签名 */
    if (bpb[510] != 0x55 || bpb[511] != 0xAA) {
        term_puts_color("ls: 无有效 FAT32 分区\n", DA_ERROR);
        return;
    }
    u32 root_cluster = *(u32 *)(bpb + 44);
    u16 bytes_per_sector = *(u16 *)(bpb + 11);
    u8  sectors_per_cluster = bpb[13];
    u32 fat_start = *(u16 *)(bpb + 14);
    u32 data_start = fat_start + (*(u16 *)(bpb + 22)) * 2;
    (void)bytes_per_sector;
    (void)sectors_per_cluster;
    (void)data_start;

    /* 读取根目录簇 — 简化：只读取第一个簇 */
    u8 dir_buf[8192];
    u32 cluster = root_cluster;
    u32 lba = data_start + (cluster - 2) * sectors_per_cluster;
    if (g_ac.block_read(0, lba, sectors_per_cluster, dir_buf) != 0) {
        term_puts_color("ls: 读取根目录失败\n", DA_ERROR);
        return;
    }
    /* 解析目录项 */
    int found = 0;
    for (int i = 0; i < (int)(sectors_per_cluster * 512); i += 32) {
        u8 first = dir_buf[i];
        if (first == 0x00) break;
        if (first == 0xE5) continue;
        if (dir_buf[i+11] & 0x08) continue; /* 卷标 */
        if (dir_buf[i+11] & 0x10) {
            /* 目录 */
            term_puts_color("[DIR]  ", DA_ACCENT);
        } else {
            term_puts_color("       ", DA_TEXT_PRIMARY);
        }
        /* 文件名（8.3） */
        for (int j = 0; j < 8; j++) {
            if (dir_buf[i+j] == ' ') break;
            term_putc((char)dir_buf[i+j]);
        }
        if (dir_buf[i+8] != ' ') {
            term_putc('.');
            for (int j = 8; j < 11; j++) {
                if (dir_buf[i+j] == ' ') break;
                term_putc((char)dir_buf[i+j]);
            }
        }
        term_putc('\n');
        found++;
    }
    if (found == 0) term_puts_color("(empty)\n", DA_TEXT_DIM);
}

static void cmd_cat(const char *args) {
    if (!args || !*args) {
        term_puts_color("cat: 需要文件名参数\n", DA_ERROR);
        return;
    }
    if (!g_ac.block_read) {
        term_puts_color("cat: 无块设备可用\n", DA_ERROR);
        return;
    }
    term_puts_color("cat: 文件读取暂未完整实现\n", DA_WARNING);
}

static void execute_command(const char *cmd) {
    while (*cmd == ' ') cmd++;
    if (*cmd == 0) return;
    const char *args = cmd;
    while (*args && *args != ' ') args++;
    char name[32];
    int nlen = (int)(args - cmd);
    if (nlen >= 32) nlen = 31;
    for (int i = 0; i < nlen; i++) name[i] = cmd[i];
    name[nlen] = 0;
    while (*args == ' ') args++;

    if (str_eq(name,"help")||str_eq(name,"?")) cmd_help();
    else if (str_eq(name,"version")||str_eq(name,"ver")) cmd_version();
    else if (str_eq(name,"uname")) cmd_uname();
    else if (str_eq(name,"about")) cmd_about();
    else if (str_eq(name,"clear")||str_eq(name,"cls")) cmd_clear();
    else if (str_eq(name,"reboot")) cmd_reboot();
    else if (str_eq(name,"halt")||str_eq(name,"shutdown")) cmd_halt();
    else if (str_eq(name,"echo")) cmd_echo(args);
    else if (str_eq(name,"date")||str_eq(name,"time")) cmd_date();
    else if (str_eq(name,"ls")||str_eq(name,"dir")) cmd_ls();
    else if (str_eq(name,"cat")) cmd_cat(args);
    else if (str_eq(name,"pwd")) { term_puts("/\n"); }
    else if (str_eq(name,"whoami")) { term_puts("root\n"); }
    else if (str_eq(name,"id")) { term_puts("uid=0(root) gid=0(root)\n"); }
    else {
        term_puts_color("未知命令: ", DA_ERROR);
        term_puts_color(name, DA_ERROR);
        term_putc('\n');
        term_puts_color("输入 'help' 查看可用命令\n", DA_TEXT_DIM);
    }
}

static void add_history(const char *cmd) {
    if (hist_count < HIST_SIZE) {
        int i;
        for (i = 0; cmd[i] && i < 255; i++) history[hist_count][i] = cmd[i];
        history[hist_count][i] = 0;
        hist_count++;
    }
    hist_idx = hist_count;
}

/* ---- 输入行重绘 ---- */
static void redraw_input_line(void) {
    int plen = 0;
    while (PROMPT_STR[plen]) plen++;
    i64 y = term_y + (i64)cur_row * CHAR_H;
    da_fill_rect(&g_ac, term_x, y, (i64)term_w * CHAR_STEP, CHAR_H, DA_BG_PRIMARY);
    for (int i = 0; i < plen; i++)
        da_draw_char(&g_ac, PROMPT_STR[i], term_x + i * CHAR_STEP, y, DA_PROMPT_COLOR, DA_BG_PRIMARY);
    for (int i = 0; i < input_len; i++)
        da_draw_char(&g_ac, input_buf[i], term_x + (plen + i) * CHAR_STEP, y, DA_TEXT_PRIMARY, DA_BG_PRIMARY);
    cur_col = plen + input_cursor;
    /* 光标 */
    da_fill_rect(&g_ac, term_x + cur_col * CHAR_STEP, y + CHAR_H - 3, 11, 2, DA_ACCENT);
}

/* ---- 主入口 ---- */
__attribute__((visibility("default")))
void dsk_entry(const da_boot_context *ctx) {
    __asm__ volatile("cli");
    da_slog("bash", "boot");

    if (!ctx || ctx->magic != DA_BOOT_MAGIC) {
        da_slog("bash", "bad context");
        for(;;) __asm__("hlt");
    }

    da_init(&g_ac, ctx);
    term_init_layout();
    term_clear();

    /* 启动 banner */
    term_puts_color("=== Deshab Bash v0.1 ===\n", DA_ACCENT);
    term_puts_color("欢迎使用 Deshab 终端\n", DA_SUCCESS);
    term_puts_color("输入 'help' 查看可用命令 | Esc 退出\n", DA_TEXT_DIM);
    term_putc('\n');

    redraw_all();

    int shift = 0, e0 = 0;
    for (;;) {
        /* 绘制提示符 */
        term_puts_color(PROMPT_STR, DA_PROMPT_COLOR);
        input_len = 0; input_cursor = 0; input_buf[0] = 0;
        int plen = 0; while (PROMPT_STR[plen]) plen++;
        cur_col = plen;
        redraw_all();

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
            if (sc & 0x80) { e0 = 0; continue; }

            /* Esc 退出 */
            if (sc == 0x01 && !e0) {
                da_slog("bash", "exit");
                return;
            }

            if (e0) {
                if (sc == 0x4B) { if (input_cursor > 0) input_cursor--; }
                else if (sc == 0x4D) { if (input_cursor < input_len) input_cursor++; }
                else if (sc == 0x47) { input_cursor = 0; }
                else if (sc == 0x4F) { input_cursor = input_len; }
                else if (sc == 0x48) { /* 上箭头 - 历史 */
                    if (hist_idx > 0) {
                        hist_idx--;
                        int i; for (i = 0; history[hist_idx][i] && i < 255; i++) input_buf[i] = history[hist_idx][i];
                        input_len = i; input_cursor = i; input_buf[i] = 0;
                    }
                }
                else if (sc == 0x50) { /* 下箭头 - 历史 */
                    if (hist_idx < hist_count - 1) {
                        hist_idx++;
                        int i; for (i = 0; history[hist_idx][i] && i < 255; i++) input_buf[i] = history[hist_idx][i];
                        input_len = i; input_cursor = i; input_buf[i] = 0;
                    } else if (hist_idx < hist_count) {
                        hist_idx = hist_count;
                        input_len = 0; input_cursor = 0; input_buf[0] = 0;
                    }
                }
                e0 = 0;
                redraw_input_line();
                continue;
            }

            if (sc == 0x1C) { /* Enter */
                input_buf[input_len] = 0;
                term_putc('\n');
                if (input_len > 0) add_history(input_buf);
                execute_command(input_buf);
                cmd_done = 1;
                continue;
            }
            if (sc == 0x0E) { /* Backspace */
                if (input_cursor > 0) {
                    for (int i = input_cursor - 1; i < input_len - 1; i++)
                        input_buf[i] = input_buf[i+1];
                    input_len--; input_cursor--;
                    input_buf[input_len] = 0;
                }
                redraw_input_line();
                continue;
            }
            char c = da_scan_to_ascii(sc, shift);
            if (c && c >= 32 && c <= 126 && input_len < 255) {
                for (int i = input_len; i > input_cursor; i--)
                    input_buf[i] = input_buf[i-1];
                input_buf[input_cursor++] = c;
                input_len++;
                redraw_input_line();
            }
        }
        redraw_all();
    }
}
