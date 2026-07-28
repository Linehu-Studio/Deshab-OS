/* Deshab Browser — 全屏浏览器
 * 接入 net_stack.h 共享协议栈，实现 HTTP GET 与简单内容显示。
 * 地址栏输入 URL（如 http://10.0.2.2:8000/ 或 10.0.2.2），
 * Enter 发起请求，内容区显示 HTTP 响应（分离 header/body，移除 HTML 标签）。
 * Esc 退出。
 */

#include "../../firstInit/ascii_bitmaps.c"
#include "../desktop_app.h"
#include "../net_stack.h"

#define CHAR_STEP  12
#define CHAR_H     18
#define URL_BUF_SIZE   128
#define CONTENT_BUF_SIZE 32768
#define DISPLAY_LINES   200

/* 前向声明（do_fetch 引用 redraw_all_imm 和 br_ns_init） */
static void redraw_all_imm(void);
static int br_ns_init(void);

static da_app_context g_ac;
static da_cursor g_cursor;
static da_mouse g_mouse;

/* 地址栏 */
static char url_buf[URL_BUF_SIZE] = "http://10.0.2.2:8000/";
static int url_len = 22;
static int url_focused = 1;  /* 启动时地址栏聚焦 */

/* 内容缓冲 */
static u8   content_buf[CONTENT_BUF_SIZE];
static u32  content_len = 0;
static int  content_scroll = 0;
static int  fetching = 0;       /* 1=正在请求 */
static char status_msg[64] = "Ready";

/* ---- 状态消息 ---- */
static void br_set_status(const char *s) {
    int i = 0;
    while (*s && i < 63) status_msg[i++] = *s++;
    status_msg[i] = 0;
}

/* ---- HTTP 请求（复制 shell cmd_curl 核心逻辑） ---- */
static void do_fetch(void) {
    if (url_len == 0) { br_set_status("Empty URL"); return; }

    br_set_status("Initializing net...");
    redraw_all_imm();

    if (br_ns_init() != 0) {
        br_set_status("Net init failed");
        return;
    }

    /* 解析 URL: [http://]host[:port][/path] */
    const char *p = url_buf;
    if (p[0]=='h'&&p[1]=='t'&&p[2]=='t'&&p[3]=='p'&&p[4]==':'&&p[5]=='/'&&p[6]=='/') p += 7;

    char host[64]; int hl = 0;
    while (*p && *p != ':' && *p != '/' && hl < 63) host[hl++] = *p++;
    host[hl] = 0;
    if (!hl) { br_set_status("Bad host"); return; }

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

    /* DNS 解析 */
    br_set_status("Resolving...");
    redraw_all_imm();
    u32 ip;
    if (ns_dns_resolve(host, &ip) != 0) {
        br_set_status("DNS failed");
        return;
    }

    /* TCP 连接 */
    br_set_status("Connecting...");
    redraw_all_imm();
    if (ns_tcp_connect(ip, port, 3000) != 0) {
        br_set_status("TCP connect failed");
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

    br_set_status("Sending request...");
    redraw_all_imm();
    if (ns_tcp_send((const u8 *)req, (u32)r) != 0) {
        br_set_status("Send failed");
        ns_tcp_close();
        return;
    }

    br_set_status("Receiving...");
    redraw_all_imm();
    int n = ns_tcp_recv(0, 0, 5000);
    ns_tcp_close();

    if (n < 0) { br_set_status("Connection reset"); return; }
    if (n == 0) { br_set_status("No response (timeout)"); return; }

    /* 复制响应到 content_buf */
    u32 total = ns_tcp_rx_len;
    u32 copy = (total < CONTENT_BUF_SIZE) ? total : CONTENT_BUF_SIZE;
    for (u32 i = 0; i < copy; i++) content_buf[i] = ns_tcp_rx[i];
    content_len = copy;
    content_scroll = 0;

    char sm[32]; int sp = 0;
    const char *ok = "OK "; while (*ok && sp < 30) sm[sp++] = *ok++;
    char nb[12]; ns_u32_dec(nb, copy);
    for (int i = 0; nb[i] && sp < 30; i++) sm[sp++] = nb[i];
    sm[sp] = 0;
    br_set_status(sm);
}

/* ---- 极简 HTML 文本提取：移除 <...> 标签，保留纯文本 ---- */
static int html_to_text(const u8 *in, u32 in_len, u8 *out, u32 out_cap) {
    u32 oi = 0;
    int in_tag = 0;
    int prev_space = 0;
    for (u32 i = 0; i < in_len && oi < out_cap - 1; i++) {
        u8 c = in[i];
        if (c == '<') { in_tag = 1; continue; }
        if (c == '>') { in_tag = 0; continue; }
        if (in_tag) continue;
        /* 跳过 HTTP header（找到 \r\n\r\n 后才开始） */
        /* 将 &nbsp; 等实体简化为空格 */
        if (c == '\r') continue;
        if (c == '\t') c = ' ';
        /* 连续空格压缩 */
        if (c == ' ') {
            if (prev_space) continue;
            prev_space = 1;
        } else {
            prev_space = 0;
        }
        out[oi++] = c;
    }
    out[oi] = 0;
    return (int)oi;
}

/* ---- 立即重绘（无鼠标光标保存/恢复，用于 fetching 过程） ---- */
static void redraw_all_imm(void) {
    da_fill_bg(&g_ac, DA_BG_PRIMARY);
    da_draw_titlebar(&g_ac, "Browser", (i64)g_ac.fb_w);

    /* 地址栏 */
    int bar_y = DA_TITLEBAR_H + 6;
    int bar_h = 28;
    int bar_x = 12;
    int bar_w = (int)g_ac.fb_w - 24;
    u32 bar_bg = url_focused ? DA_BG_TERTIARY : DA_BG_SECONDARY;
    da_fill_rounded_rect(&g_ac, bar_x, bar_y, bar_w, bar_h, bar_bg, 6);
    da_rect_outline(&g_ac, bar_x, bar_y, bar_w, bar_h, url_focused ? DA_ACCENT : DA_BORDER, 6);

    /* URL 文本 + 光标 */
    int url_x = bar_x + 12;
    da_draw_string(&g_ac, url_buf, url_x, bar_y + 5, DA_TEXT_PRIMARY, bar_bg, CHAR_STEP);
    if (url_focused) {
        int cx = url_x + url_len * CHAR_STEP;
        da_fill_rect(&g_ac, cx, bar_y + 5, 2, CHAR_H, DA_ACCENT_LIGHT);
    }

    /* 提示 */
    int hint_y = bar_y + bar_h + 4;
    const char *hint = "Enter: fetch  Esc: exit  Up/Down: scroll";
    da_draw_string(&g_ac, hint, 12, hint_y, DA_TEXT_DIM, DA_BG_PRIMARY, CHAR_STEP);

    /* 内容区 */
    int content_y = hint_y + CHAR_H + 4;
    int content_x = 12;
    int content_w = (int)g_ac.fb_w - 24;
    int content_h = (int)g_ac.fb_h - content_y - DA_STATUSBAR_H - 8;
    da_fill_rounded_rect(&g_ac, content_x, content_y, content_w, content_h, DA_BG_SECONDARY, 4);
    da_rect_outline(&g_ac, content_x, content_y, content_w, content_h, DA_BORDER, 4);

    /* 内容显示 */
    if (content_len > 0) {
        /* 提取 body（跳过 HTTP header） */
        u32 body_off = 0;
        for (u32 i = 0; i + 3 < content_len; i++) {
            if (content_buf[i]=='\r' && content_buf[i+1]=='\n' &&
                content_buf[i+2]=='\r' && content_buf[i+3]=='\n') {
                body_off = i + 4;
                break;
            }
        }
        /* HTML 转纯文本 */
        static u8 plain[CONTENT_BUF_SIZE];
        int plain_len = html_to_text(content_buf + body_off, content_len - body_off, plain, CONTENT_BUF_SIZE);

        /* 按行显示 */
        int visible_cols = (content_w - 16) / CHAR_STEP;
        int visible_rows = (content_h - 8) / CHAR_H;
        int y = content_y + 4;
        int pos = 0;
        int line = 0;
        /* 跳过 scroll 行 */
        int skip = content_scroll;
        while (pos < plain_len && skip > 0) {
            if (plain[pos] == '\n') skip--;
            pos++;
        }
        while (pos < plain_len && line < visible_rows) {
            int ls = pos;
            while (pos < plain_len && plain[pos] != '\n' && pos - ls < visible_cols) pos++;
            int llen = pos - ls;
            for (int i = 0; i < llen; i++) {
                char c = (char)plain[ls + i];
                if (c < ' ' || c > '~') c = '.';
                char buf[2]; buf[0] = c; buf[1] = 0;
                da_draw_string(&g_ac, buf, content_x + 8 + i * CHAR_STEP, y, DA_TEXT_PRIMARY, DA_BG_SECONDARY, CHAR_STEP);
            }
            y += CHAR_H;
            line++;
            if (pos < plain_len && plain[pos] == '\n') pos++;
        }
    } else if (!fetching) {
        const char *empty = "No content. Enter URL and press Enter to fetch.";
        da_draw_string(&g_ac, empty, content_x + 16, content_y + 16, DA_TEXT_DIM, DA_BG_SECONDARY, CHAR_STEP);
    }

    /* 状态栏 */
    char status[96]; int p = 0;
    const char *s1 = "Browser | "; while (*s1 && p < 95) status[p++] = *s1++;
    for (int i = 0; status_msg[i] && p < 95; i++) status[p++] = status_msg[i];
    status[p] = 0;
    da_draw_statusbar(&g_ac, status, (i64)g_ac.fb_w, (i64)g_ac.fb_h);
}

static void redraw_all(void) {
    redraw_all_imm();
    da_cursor_save(&g_ac, &g_cursor);
    da_cursor_draw(&g_ac, &g_cursor, DA_CURSOR_COLOR);
}

/* ---- net_stack.h 的 ns_init 需要 dkm_kernel_api ---- */
/* 重写 ns_init 以从 da_app_context 获取 kernel_api */
static int br_ns_init(void) {
    if (ns_ready) return 0;
    u64 dkm_kernel_api = (u64)g_ac.kernel_api;
    if (!dkm_kernel_api) return -1;
    return ns_init(dkm_kernel_api);
}

__attribute__((visibility("default")))
void dsk_entry(const da_boot_context *ctx) {
    __asm__ volatile("cli");
    da_slog("browser", "boot");

    if (!ctx || ctx->magic != DA_BOOT_MAGIC) {
        da_slog("browser", "bad context");
        for(;;) __asm__("hlt");
    }

    da_init(&g_ac, ctx);
    da_cursor_init(&g_cursor, (i64)g_ac.fb_w, (i64)g_ac.fb_h);
    da_mouse_init();

    /* 初始化网络协议栈 */
    br_ns_init();

    br_set_status("Ready");
    redraw_all();

    int e0 = 0, shift = 0;
    for (;;) {
        int need_redraw = 0;

        /* 鼠标轮询 */
        if (da_mouse_poll(&g_mouse, &g_cursor, (i64)g_ac.fb_w, (i64)g_ac.fb_h)) {
            need_redraw = 1;
            /* 点击地址栏聚焦 */
            int bar_y = DA_TITLEBAR_H + 6;
            int bar_h = 28;
            if (g_cursor.btn &&
                g_cursor.my >= bar_y && g_cursor.my < bar_y + bar_h &&
                g_cursor.mx >= 12 && g_cursor.mx < (int)g_ac.fb_w - 12) {
                url_focused = 1;
                need_redraw = 1;
            }
        }

        /* 键盘轮询 */
        u8 st = inb(0x64);
        if ((st & 1) && !(st & 0x20)) {
            u8 data = inb(0x60);
            u8 sc = data;
            if (sc == 0xE0) { e0 = 1; goto skip; }
            if (sc == 0x2A || sc == 0x36) { shift = 1; goto skip; }
            if (sc == 0xAA || sc == 0xB6) { shift = 0; goto skip; }
            if (sc & 0x80) { e0 = 0; goto skip; }

            if (sc == 0x01) { /* Esc */
                da_slog("browser", "exit");
                return;
            }

            /* 地址栏输入模式 */
            if (url_focused) {
                if (sc == 0x1C) { /* Enter - 发起请求 */
                    url_focused = 0;
                    fetching = 1;
                    br_set_status("Fetching...");
                    redraw_all();
                    do_fetch();
                    fetching = 0;
                    need_redraw = 1;
                    goto skip;
                }
                if (sc == 0x0E) { /* Backspace */
                    if (url_len > 0) { url_len--; url_buf[url_len] = 0; }
                    need_redraw = 1;
                    goto skip;
                }
                char c = da_scan_to_ascii(sc, shift);
                if (c && c >= 32 && c <= 126 && url_len < URL_BUF_SIZE - 1) {
                    /* 小写转大写便于 URL 输入（除协议头） */
                    url_buf[url_len++] = c;
                    url_buf[url_len] = 0;
                    need_redraw = 1;
                }
                goto skip;
            }

            /* 内容浏览模式 */
            if (e0) {
                if (sc == 0x48) { if (content_scroll > 0) content_scroll--; need_redraw = 1; }
                else if (sc == 0x50) { content_scroll++; need_redraw = 1; }
                e0 = 0;
                goto skip;
            }
            if (sc == 0x49) { content_scroll -= 10; if (content_scroll < 0) content_scroll = 0; need_redraw = 1; }
            else if (sc == 0x51) { content_scroll += 10; need_redraw = 1; }
            else if (sc == 0x1C) { url_focused = 1; need_redraw = 1; } /* Enter 回到地址栏 */
        }
    skip:
        if (need_redraw) {
            da_cursor_restore(&g_ac, &g_cursor);
            redraw_all();
        }
        __asm__("pause");
    }
}
