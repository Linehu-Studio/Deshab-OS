/* Deshab curl — 终端风格 HTTP 客户端
 * 命令行输入 URL，发送 HTTP/1.0 GET 请求并打印响应（net_stack.h 共享协议栈）。
 * Esc 键退出返回 DSK。
 */

#include "../../firstInit/ascii_bitmaps.c"
#include "../desktop_app.h"
#include "../net_stack.h"

/* ---- 终端参数 ---- */
#define TERM_COLS  120
#define TERM_ROWS  40
#define TERM_MAX_CHARS (TERM_COLS * TERM_ROWS)
#define CHAR_STEP  12
#define CHAR_H     18
#define PROMPT_STR "curl> "

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
    da_draw_titlebar(&g_ac, "curl", (i64)g_ac.fb_w);
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
    const char *s1 = "HTTP client | Esc 退出  ";
    while (*s1) status[p++] = *s1++;
    u8 hh, mm; da_rtc_time(&hh, &mm);
    status[p++] = '0' + hh/10; status[p++] = '0' + hh%10;
    status[p++] = ':'; status[p++] = '0' + mm/10; status[p++] = '0' + mm%10;
    status[p] = 0;
    da_draw_statusbar(&g_ac, status, (i64)g_ac.fb_w, (i64)g_ac.fb_h);
}

/* ---- HTTP GET 实现（net_stack.h） ---- */
static int g_ns_status = 0;  /* 0=未尝试, 1=就绪, <0=失败 */

static int curl_net_ready(void) {
    if (g_ns_status == 1) return 1;
    if (g_ns_status < 0) return 0;
    int rc = ns_init((u64)g_ac.kernel_api);
    g_ns_status = (rc == 0) ? 1 : -1;
    if (rc != 0) {
        term_puts_color("  [错误] 网络初始化失败，无可用网卡或 net API\n", DA_ERROR);
        return 0;
    }
    return 1;
}

static void do_http_get(const char *url) {
    if (!curl_net_ready()) return;

    const char *p = url;
    if (p[0]=='h'&&p[1]=='t'&&p[2]=='t'&&p[3]=='p'&&p[4]==':'&&p[5]=='/'&&p[6]=='/') p += 7;

    char host[128]; int hl = 0;
    while (*p && *p != ':' && *p != '/' && hl < 127) host[hl++] = *p++;
    host[hl] = 0;
    if (!hl) { term_puts_color("  [错误] 缺少主机名\n", DA_ERROR); return; }

    u16 port = 80;
    if (*p == ':') {
        p++;
        u32 pv = 0; int digits = 0;
        while (*p >= '0' && *p <= '9') { pv = pv * 10 + (u32)(*p - '0'); p++; digits++; }
        if (digits && pv <= 65535) port = (u16)pv;
    }
    char path[128]; int pl = 0;
    if (*p == '/') {
        while (*p && *p != ' ' && pl < 127) path[pl++] = *p++;
    }
    if (!pl) { path[0] = '/'; pl = 1; }
    path[pl] = 0;

    term_puts_color("GET ", DA_PROMPT_COLOR);
    term_puts_color(url, DA_ACCENT_LIGHT);
    term_putc('\n');

    /* 解析主机 */
    term_puts_color("  -> 解析 ", DA_TEXT_DIM);
    term_puts_color(host, DA_TEXT_DIM);
    term_puts_color("...\n", DA_TEXT_DIM);
    redraw_all();
    u32 ip;
    if (ns_dns_resolve(host, &ip) != 0) {
        term_puts_color("  [错误] DNS 解析失败\n", DA_ERROR);
        return;
    }
    char ipstr[24]; ns_fmt_ip(ip, ipstr);
    term_puts_color("  -> ", DA_TEXT_DIM);
    term_puts_color(ipstr, DA_TEXT_DIM);
    term_putc('\n');

    /* TCP 连接 */
    term_puts_color("  -> TCP 连接...\n", DA_TEXT_DIM);
    redraw_all();
    if (ns_tcp_connect(ip, port, 2000) != 0) {
        term_puts_color("  [错误] TCP 连接失败（超时/RST）\n", DA_ERROR);
        return;
    }

    /* 构造 HTTP/1.0 请求 */
    static char req[512];
    int r = 0;
    const char *m = "GET "; while (*m) req[r++] = *m++;
    for (int i = 0; i < pl; i++) req[r++] = path[i];
    m = " HTTP/1.0\r\nHost: "; while (*m) req[r++] = *m++;
    for (int i = 0; i < hl; i++) req[r++] = host[i];
    m = "\r\nConnection: close\r\n\r\n"; while (*m) req[r++] = *m++;
    req[r] = 0;

    term_puts_color("  -> 已连接，发送请求\n", DA_TEXT_DIM);
    redraw_all();
    if (ns_tcp_send((const u8 *)req, (u32)r) != 0) {
        term_puts_color("  [错误] 发送失败\n", DA_ERROR);
        ns_tcp_close();
        return;
    }

    /* 收至 FIN */
    term_puts_color("  -> 等待响应...\n", DA_TEXT_DIM);
    redraw_all();
    int n = ns_tcp_recv(0, 0, 5000);
    ns_tcp_close();
    if (n < 0) {
        term_puts_color("  [错误] 连接被重置\n", DA_ERROR);
        return;
    }
    if (n == 0) {
        term_puts_color("  [错误] 无响应数据（超时）\n", DA_ERROR);
        return;
    }

    term_puts_color("  -> 收到 ", DA_SUCCESS);
    char nb[12]; ns_u32_dec(nb, (u32)n);
    term_puts_color(nb, DA_SUCCESS);
    term_puts_color(" 字节\n\n", DA_SUCCESS);

    /* 打印 status line + body */
    u32 total = ns_tcp_rx_len;
    u32 show = (u32)n;
    for (u32 i = 0; i < show; i++) term_putc((char)ns_tcp_rx[i]);
    if (total > show) {
        term_puts_color("\n[... 已截断，共 ", DA_TEXT_DIM);
        ns_u32_dec(nb, total);
        term_puts_color(nb, DA_TEXT_DIM);
        term_puts_color(" 字节]\n", DA_TEXT_DIM);
    }
    term_putc('\n');
}

/* ---- 内置命令 ---- */
static int str_eq(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

static void cmd_help(void) {
    term_puts_color("curl — 命令行 HTTP 客户端\n", DA_PROMPT_COLOR);
    term_puts("  <url>        发送 HTTP GET 请求\n");
    term_puts("  help         显示此帮助\n");
    term_puts("  clear        清屏\n");
    term_puts("  version      显示版本\n");
    term_puts("  Esc          退出\n");
}

static void cmd_clear(void) { term_clear(); }

static void cmd_version(void) {
    term_puts_color("Deshab curl v1.0\n", DA_SUCCESS);
    term_puts_color("  HTTP/1.0 GET via net_stack (e1000 + slirp)\n", DA_TEXT_DIM);
}

static void execute_command(const char *cmd) {
    while (*cmd == ' ') cmd++;
    if (*cmd == 0) return;

    if (str_eq(cmd, "help") || str_eq(cmd, "?")) { cmd_help(); return; }
    if (str_eq(cmd, "clear") || str_eq(cmd, "cls")) { cmd_clear(); return; }
    if (str_eq(cmd, "version") || str_eq(cmd, "ver")) { cmd_version(); return; }

    /* 非内置命令视为 URL */
    if (cmd[0] == 'h' && cmd[1] == 't' && cmd[2] == 't' && cmd[3] == 'p') {
        do_http_get(cmd);
    } else {
        term_puts_color("无效 URL: ", DA_ERROR);
        term_puts_color(cmd, DA_ERROR);
        term_putc('\n');
        term_puts_color("请输入完整 URL (http://...)\n", DA_TEXT_DIM);
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
    da_fill_rect(&g_ac, term_x + cur_col * CHAR_STEP, y + CHAR_H - 3, 11, 2, DA_ACCENT);
}

/* ---- 主入口 ---- */
__attribute__((visibility("default")))
void dsk_entry(const da_boot_context *ctx) {
    __asm__ volatile("cli");
    da_slog("curl", "boot");

    if (!ctx || ctx->magic != DA_BOOT_MAGIC) {
        da_slog("curl", "bad context");
        for(;;) __asm__("hlt");
    }

    da_init(&g_ac, ctx);
    term_init_layout();
    term_clear();

    /* 启动 banner */
    term_puts_color("=== Deshab curl v1.0 ===\n", DA_ACCENT);
    term_puts_color("HTTP 命令行客户端\n", DA_SUCCESS);
    term_puts_color("输入 URL 发送 GET 请求 | 输入 'help' 查看帮助 | Esc 退出\n", DA_TEXT_DIM);
    term_putc('\n');

    redraw_all();

    int shift = 0, e0 = 0;
    for (;;) {
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
                da_slog("curl", "exit");
                return;
            }

            if (e0) {
                if (sc == 0x4B) { if (input_cursor > 0) input_cursor--; }
                else if (sc == 0x4D) { if (input_cursor < input_len) input_cursor++; }
                else if (sc == 0x47) { input_cursor = 0; }
                else if (sc == 0x4F) { input_cursor = input_len; }
                else if (sc == 0x48) {
                    if (hist_idx > 0) {
                        hist_idx--;
                        int i; for (i = 0; history[hist_idx][i] && i < 255; i++) input_buf[i] = history[hist_idx][i];
                        input_len = i; input_cursor = i; input_buf[i] = 0;
                    }
                }
                else if (sc == 0x50) {
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
