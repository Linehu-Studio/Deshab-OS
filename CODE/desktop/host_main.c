/* host_main.c — Deshab 桌面 Linux SDL2 宿主
 *
 * 仅在 -DDESHAB_HOST 编译时链接。提供：
 *   - SDL2 窗口 + streaming texture（模拟 framebuffer，g_host_backbuf）
 *   - 鼠标/键盘事件队列（模拟 PS/2，供 host_mouse_poll/host_kb_poll 消费）
 *   - deshab.img GPT ESP 文件 block 后端（模拟 kernel_api block provider）
 *   - 单调时钟 / 当天秒数（替代 rdtsc / RTC CMOS）
 *
 * main() 构造 dsk_boot_context 后调用 dsk_entry()，进入桌面主循环。
 * 主循环每帧末尾调用 host_pump_and_present() 上传 back buffer 并 pump 事件。
 */
#include "host_main.h"

#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>

#include <utsm/dsk.h>

/* ---- 全局缓冲（供 desktop/main.c 经 extern 使用） ---- */
u8 *g_host_sprite  = NULL;
u8 *g_host_backbuf = NULL;

/* ---- SDL 状态 ---- */
static SDL_Window   *g_win     = NULL;
static SDL_Renderer *g_rend    = NULL;
static SDL_Texture  *g_tex     = NULL;
static int g_fb_w = 1280, g_fb_h = 720;

/* ---- 鼠标状态（SDL 事件累积，host_mouse_poll 消费） ---- */
static struct {
    int x, y;          /* 绝对坐标 */
    int btn;           /* bit0=左 bit1=右 */
    int prev_btn;      /* 上次 poll 时的按键状态 */
    int changed;       /* 自上次 poll 以来有变化 */
} g_mse = { 400, 300, 0, 0, 0 };

/* ---- 键盘扫描码队列（SDL 键事件 → set-1 字节序列） ---- */
#define KB_QSZ 64
static u8  g_kb_q[KB_QSZ];
static int g_kb_head = 0, g_kb_tail = 0;

static void kb_push(u8 b) {
    int nxt = (g_kb_tail + 1) % KB_QSZ;
    if (nxt == g_kb_head) return;          /* 满，丢弃 */
    g_kb_q[g_kb_tail] = b;
    g_kb_tail = nxt;
}

/* ---- 日志 ---- */
void host_log(const char *s) { fputs(s, stderr); fflush(stderr); }
void host_log_char(char c)   { fputc(c, stderr); fflush(stderr); }

/* ---- 时间 ---- */
u64 host_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000ULL + (u64)ts.tv_nsec / 1000ULL;
}

int host_rtc_seconds(void) {
    time_t t = time(NULL);
    struct tm *lt = localtime(&t);
    return lt->tm_hour * 3600 + lt->tm_min * 60 + lt->tm_sec;
}

/* ---- 鼠标 ---- */
int host_mouse_poll(int *px, int *py, int *pbtn,
                    int *p_left_pressed, int *p_left_released,
                    int *p_right_pressed, int max_x, int max_y) {
    if (!g_mse.changed && g_mse.btn == g_mse.prev_btn) return 0;
    g_mse.changed = 0;
    int btn  = g_mse.btn;
    int prev = g_mse.prev_btn;
    g_mse.prev_btn = btn;
    int x = g_mse.x, y = g_mse.y;
    if (x < 0) x = 0; if (y < 0) y = 0;
    if (x > max_x) x = max_x; if (y > max_y) y = max_y;
    *px = x; *py = y; *pbtn = btn;
    *p_left_pressed  = (btn & 1) && !(prev & 1);
    *p_left_released = !(btn & 1) && (prev & 1);
    *p_right_pressed = (btn & 2) && !(prev & 2);
    return 1;
}

/* ---- 键盘 ---- */
int host_kb_poll(u8 *out_sc) {
    if (g_kb_head == g_kb_tail) return 0;
    *out_sc = g_kb_q[g_kb_head];
    g_kb_head = (g_kb_head + 1) % KB_QSZ;
    return 1;
}

/* SDL_Scancode → PS/2 set-1 make 码（单字节；E0 前缀键返回 0xFF 表示需 E0） */
static u8 sdl_to_set1(SDL_Scancode sc, int *e0) {
    *e0 = 0;
    switch (sc) {
    case SDL_SCANCODE_A: return 0x1E; case SDL_SCANCODE_B: return 0x30;
    case SDL_SCANCODE_C: return 0x2E; case SDL_SCANCODE_D: return 0x20;
    case SDL_SCANCODE_E: return 0x12; case SDL_SCANCODE_F: return 0x21;
    case SDL_SCANCODE_G: return 0x22; case SDL_SCANCODE_H: return 0x23;
    case SDL_SCANCODE_I: return 0x17; case SDL_SCANCODE_J: return 0x24;
    case SDL_SCANCODE_K: return 0x25; case SDL_SCANCODE_L: return 0x26;
    case SDL_SCANCODE_M: return 0x32; case SDL_SCANCODE_N: return 0x31;
    case SDL_SCANCODE_O: return 0x18; case SDL_SCANCODE_P: return 0x19;
    case SDL_SCANCODE_Q: return 0x10; case SDL_SCANCODE_R: return 0x13;
    case SDL_SCANCODE_S: return 0x1F; case SDL_SCANCODE_T: return 0x14;
    case SDL_SCANCODE_U: return 0x16; case SDL_SCANCODE_V: return 0x2F;
    case SDL_SCANCODE_W: return 0x11; case SDL_SCANCODE_X: return 0x2D;
    case SDL_SCANCODE_Y: return 0x15; case SDL_SCANCODE_Z: return 0x2C;
    case SDL_SCANCODE_1: return 0x02; case SDL_SCANCODE_2: return 0x03;
    case SDL_SCANCODE_3: return 0x04; case SDL_SCANCODE_4: return 0x05;
    case SDL_SCANCODE_5: return 0x06; case SDL_SCANCODE_6: return 0x07;
    case SDL_SCANCODE_7: return 0x08; case SDL_SCANCODE_8: return 0x09;
    case SDL_SCANCODE_9: return 0x0A; case SDL_SCANCODE_0: return 0x0B;
    case SDL_SCANCODE_RETURN:     return 0x1C;
    case SDL_SCANCODE_BACKSPACE:  return 0x0E;
    case SDL_SCANCODE_TAB:        return 0x0F;
    case SDL_SCANCODE_SPACE:      return 0x39;
    case SDL_SCANCODE_ESCAPE:     return 0x01;
    case SDL_SCANCODE_LSHIFT: return 0x2A; case SDL_SCANCODE_RSHIFT: return 0x36;
    case SDL_SCANCODE_LCTRL:  return 0x1D; case SDL_SCANCODE_RCTRL:  return 0x1D;
    case SDL_SCANCODE_LALT:   return 0x38; case SDL_SCANCODE_RALT:   return 0x38;
    case SDL_SCANCODE_CAPSLOCK:   return 0x3A;
    case SDL_SCANCODE_MINUS:      return 0x0C;
    case SDL_SCANCODE_EQUALS:     return 0x0D;
    case SDL_SCANCODE_LEFTBRACKET:  return 0x1A;
    case SDL_SCANCODE_RIGHTBRACKET: return 0x1B;
    case SDL_SCANCODE_SEMICOLON:  return 0x27;
    case SDL_SCANCODE_APOSTROPHE: return 0x28;
    case SDL_SCANCODE_GRAVE:      return 0x29;
    case SDL_SCANCODE_BACKSLASH:  return 0x2B;
    case SDL_SCANCODE_COMMA:  return 0x33;
    case SDL_SCANCODE_PERIOD: return 0x34;
    case SDL_SCANCODE_SLASH:  return 0x35;
    case SDL_SCANCODE_LEFT:  *e0 = 1; return 0x4B;
    case SDL_SCANCODE_RIGHT: *e0 = 1; return 0x4D;
    case SDL_SCANCODE_UP:    *e0 = 1; return 0x48;
    case SDL_SCANCODE_DOWN:  *e0 = 1; return 0x50;
    default: return 0;
    }
}

static void handle_key_event(SDL_Scancode sc, int down) {
    int e0 = 0;
    u8 code = sdl_to_set1(sc, &e0);
    if (!code) return;
    if (e0) kb_push(0xE0);
    kb_push(down ? code : (u8)(code | 0x80));
}

/* ---- 帧末 pump + present ---- */
int host_pump_and_present(void) {
    int quit = 0;
    /* 上传 back buffer 到 SDL texture 并 present */
    if (g_tex && g_host_backbuf) {
        SDL_UpdateTexture(g_tex, NULL, g_host_backbuf, g_fb_w * 4);
        SDL_RenderClear(g_rend);
        SDL_RenderCopy(g_rend, g_tex, NULL, NULL);
        SDL_RenderPresent(g_rend);
    }
    /* pump 事件 */
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
        case SDL_QUIT:
            quit = 1;
            break;
        case SDL_MOUSEMOTION:
            g_mse.x = ev.motion.x;
            g_mse.y = ev.motion.y;
            g_mse.changed = 1;
            break;
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP: {
            int down = (ev.type == SDL_MOUSEBUTTONDOWN);
            if (ev.button.button == SDL_BUTTON_LEFT) {
                if (down) g_mse.btn |= 1; else g_mse.btn &= ~1;
                g_mse.changed = 1;
            } else if (ev.button.button == SDL_BUTTON_RIGHT) {
                if (down) g_mse.btn |= 2; else g_mse.btn &= ~2;
                g_mse.changed = 1;
            }
            break;
        }
        case SDL_KEYDOWN:
        case SDL_KEYUP: {
            /* 忽略重复事件（SDL_KEYDOWN 带 repeat>0） */
            if (ev.key.repeat) break;
            handle_key_event(ev.key.keysym.scancode, ev.type == SDL_KEYDOWN);
            break;
        }
        }
    }
    return quit;
}

/* ---- block 后端：deshab.img GPT ESP 文件 ---- */
static int   g_img_fd = -1;
static u64   g_esp_byte_off = 0;   /* ESP 分区起始字节偏移 */

/* 读取 img LBA1 GPT header，定位 ESP 分区起始 LBA。失败回退 2048。 */
static u64 find_esp_offset(int fd) {
    u8 hdr[512];
    if (pread(fd, hdr, 512, 512) != 512) return 2048 * 512;
    /* GPT signature "EFI PART" */
    if (memcmp(hdr, "EFI PART", 8) != 0) return 2048 * 512;
    u64 entries_lba = 0;
    u32 entry_count = 0, entry_size = 0;
    memcpy(&entries_lba, hdr + 72, 8);
    memcpy(&entry_count, hdr + 80, 4);
    memcpy(&entry_size,  hdr + 84, 4);
    if (entry_size < 56 || entry_count == 0 || entry_count > 256) return 2048 * 512;
    /* ESP 类型 GUID（混合字节序）：c12a7328-f81f-11d2-ba4b-00a0c93ec93b */
    static const u8 esp_guid[16] = {
        0x28,0x73,0x2a,0xc1, 0x1f,0xf8, 0xd2,0x11,
        0xba,0x4b, 0x00,0xa0,0xc9,0x3e,0xc9,0x3b
    };
    u8 *entries = (u8 *)malloc((u64)entry_count * entry_size);
    if (!entries) return 2048 * 512;
    u64 entries_byte = entries_lba * 512;
    ssize_t got = pread(fd, entries, (u64)entry_count * entry_size, entries_byte);
    if (got <= 0) { free(entries); return 2048 * 512; }
    u64 esp_lba = 0;
    int found = 0;
    for (u32 i = 0; i < entry_count; i++) {
        u8 *e = entries + (u64)i * entry_size;
        if (memcmp(e, esp_guid, 16) == 0) {
            memcpy(&esp_lba, e + 32, 8);
            found = 1;
            break;
        }
    }
    free(entries);
    return found ? (esp_lba * 512) : (2048 * 512);
}

/* block_read/write：desktop 期望 int (u32 index, u64 lba, u32 count, void* buf)
 * index 固定 0；扇区 512 字节；偏移 = esp_byte_off + lba*512。 */
static int host_block_read(u32 index, u64 lba, u32 count, void *buf) {
    (void)index;
    if (g_img_fd < 0) return -1;
    u64 off = g_esp_byte_off + lba * 512ULL;
    u64 total = (u64)count * 512ULL;
    u8 *p = (u8 *)buf;
    while (total > 0) {
        ssize_t n = pread(g_img_fd, p, total, off);
        if (n <= 0) return -1;
        p += n; off += (u64)n; total -= (u64)n;
    }
    return 0;
}

static int host_block_write(u32 index, u64 lba, u32 count, const void *buf) {
    (void)index;
    if (g_img_fd < 0) return -1;
    u64 off = g_esp_byte_off + lba * 512ULL;
    u64 total = (u64)count * 512ULL;
    const u8 *p = (const u8 *)buf;
    while (total > 0) {
        ssize_t n = pwrite(g_img_fd, p, total, off);
        if (n <= 0) return -1;
        p += n; off += (u64)n; total -= (u64)n;
    }
    return 0;
}

/* ---- 伪造 kernel_api：让 dsk_entry 现有 block 获取路径零改动 ----
 * desktop/main.c: u64 blk = *(u64*)(ctx->dkm_kernel_api + 0xA8);
 *                 g_block_read  = *(u64*)(blk + 16);
 *                 g_block_write = *(u64*)(blk + 24);
 * 故 host_kernel_api 偏移 0xA8 处填 &host_block_api；
 * host_block_api[2] = read, [3] = write（偏移 16/24）。 */
static u64 host_block_api[8];
static u8  host_kernel_api[0xB0 + 8];

static void build_fake_kernel_api(void) {
    memset(host_kernel_api, 0, sizeof(host_kernel_api));
    memset(host_block_api, 0, sizeof(host_block_api));
    host_block_api[2] = (u64)(uintptr_t)host_block_read;
    host_block_api[3] = (u64)(uintptr_t)host_block_write;
    *(u64 *)(host_kernel_api + 0xA8) = (u64)(uintptr_t)host_block_api;
}

/* ---- main ---- */
int main(int argc, char **argv) {
    const char *img_path = "../../ISO/deshab.img";
    int width = 1280, height = 720;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--img") == 0 && i + 1 < argc) { img_path = argv[++i]; continue; }
        if (strcmp(argv[i], "--width") == 0 && i + 1 < argc)  { width  = atoi(argv[++i]); continue; }
        if (strcmp(argv[i], "--height") == 0 && i + 1 < argc) { height = atoi(argv[++i]); continue; }
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            fprintf(stderr, "usage: desktop_host [--img PATH] [--width W] [--height H]\n");
            return 0;
        }
    }
    g_fb_w = width; g_fb_h = height;

    /* 打开镜像 */
    g_img_fd = open(img_path, O_RDWR);
    if (g_img_fd < 0) {
        /* 只读回退 */
        g_img_fd = open(img_path, O_RDONLY);
        if (g_img_fd < 0) {
            fprintf(stderr, "[host] cannot open %s: ", img_path);
            perror("");
            return 1;
        }
        fprintf(stderr, "[host] warning: %s opened read-only, FAT32 writes will fail\n", img_path);
    }
    g_esp_byte_off = find_esp_offset(g_img_fd);
    fprintf(stderr, "[host] img=%s esp_off=%llu width=%d height=%d\n",
            img_path, (unsigned long long)g_esp_byte_off, width, height);

    /* SDL */
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "[host] SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    g_win = SDL_CreateWindow("Deshab Desktop (Linux host)",
                             SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                             width, height, SDL_WINDOW_SHOWN);
    if (!g_win) { fprintf(stderr, "[host] SDL_CreateWindow: %s\n", SDL_GetError()); return 1; }
    g_rend = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!g_rend) g_rend = SDL_CreateRenderer(g_win, -1, 0);
    if (!g_rend) { fprintf(stderr, "[host] SDL_CreateRenderer: %s\n", SDL_GetError()); return 1; }
    g_tex = SDL_CreateTexture(g_rend, SDL_PIXELFORMAT_ARGB8888,
                              SDL_TEXTUREACCESS_STREAMING, width, height);
    if (!g_tex) { fprintf(stderr, "[host] SDL_CreateTexture: %s\n", SDL_GetError()); return 1; }

    /* back buffer + sprite buffer */
    size_t fbsz = (size_t)width * height * 4;
    g_host_backbuf = (u8 *)malloc(fbsz);
    g_host_sprite  = (u8 *)malloc(fbsz);
    if (!g_host_backbuf || !g_host_sprite) {
        fprintf(stderr, "[host] alloc buffer failed\n");
        return 1;
    }
    memset(g_host_backbuf, 0, fbsz);
    memset(g_host_sprite, 0, fbsz);

    build_fake_kernel_api();

    /* 构造 dsk_boot_context */
    dsk_boot_context ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.magic                = DSK_BOOT_MAGIC;
    ctx.abi_version          = DSK_BOOT_ABI_VERSION;
    ctx.size                 = sizeof(ctx);
    ctx.framebuffer_address  = (u64)(uintptr_t)g_host_backbuf;
    ctx.framebuffer_width    = (u64)width;
    ctx.framebuffer_height   = (u64)height;
    ctx.framebuffer_pitch    = (u64)width * 4;
    ctx.framebuffer_bpp      = 32;
    ctx.dkm_kernel_api       = (u64)(uintptr_t)host_kernel_api;
    /* reserved[5]=0：无 linux_compat 服务，桌面侧 g_lxc_svc 保持 NULL */

    fprintf(stderr, "[host] entering dsk_entry\n");
    dsk_entry(&ctx);
    fprintf(stderr, "[host] dsk_entry returned, exiting\n");

    SDL_DestroyTexture(g_tex);
    SDL_DestroyRenderer(g_rend);
    SDL_DestroyWindow(g_win);
    SDL_Quit();
    if (g_img_fd >= 0) close(g_img_fd);
    free(g_host_backbuf);
    free(g_host_sprite);
    return 0;
}
