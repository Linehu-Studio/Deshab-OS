/* zhfont_test.c — zhfont 渲染冒烟测试（Windows/Linux 本机编译运行，非裸机）
 *
 * 验证：
 *   1. .dbf 附加 + UTF-8 解码 + zh_string_width 一致性
 *   2. zh_draw_char 单字渲染与 dbf 字形逐像素一致（fg=白 bg=黑 时
 *      每通道值 == 字形灰度字节）
 *   3. zh_draw_string 混排推进正确、缺字回退空框
 *
 * 用法：clang zhfont_test.c zhfont.c ascii_font.c -I../UTSM/include -I. -o zhfont_test
 *       ./zhfont_test ../font/simhei_16.dbf
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ascii_font.h"
#include "zhfont.h"
#include "../font/dbf.h"

#define FB_W 480
#define FB_H 120
static unsigned int fb[FB_W * FB_H];

static int fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); fail = 1; } \
} while (0)

/* 与 zhfont 相同的 dbf 查找（测试侧独立实现） */
static const unsigned char *ref_glyph(const unsigned char *font, unsigned int cp,
                                      int *w, int *h) {
    const dbf_header *hdr = (const dbf_header *)font;
    const dbf_index_entry *idx = (const dbf_index_entry *)(font + 14);
    int lo = 0, hi = (int)hdr->count - 1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if ((unsigned)idx[m].codepoint == cp) {
            *w = idx[m].width; *h = hdr->height;
            return font + idx[m].offset;
        }
        if ((unsigned)idx[m].codepoint < cp) lo = m + 1; else hi = m - 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    const char *path = (argc > 1) ? argv[1] : "../font/simhei_16.dbf";
    FILE *f = fopen(path, "rb");
    if (!f) { printf("cannot open %s\n", path); return 1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = malloc(sz);
    if (fread(buf, 1, sz, f) != (size_t)sz) { printf("read fail\n"); return 1; }
    fclose(f);

    zhfont_attach16(buf);
    CHECK(zhfont_ok(), "attach16 failed");

    du_context ctx;
    du_context_init(&ctx, (unsigned long long)(size_t)fb, FB_W, FB_H, FB_W * 4);
    du_fill_rect(&ctx, 0, 0, FB_W, FB_H, 0xFF000000u);

    /* --- 1. 单字渲染逐像素比对：中(0x4E2D) 文(0x6587) --- */
    unsigned int cps[2] = { 0x4E2D, 0x6587 };
    for (int k = 0; k < 2; k++) {
        int w, h;
        const unsigned char *g = ref_glyph(buf, cps[k], &w, &h);
        CHECK(g != 0, "ref_glyph missing");
        memset(fb, 0, sizeof(fb));
        du_fill_rect(&ctx, 0, 0, FB_W, FB_H, 0xFF000000u);
        int adv = zh_draw_char(&ctx, cps[k], 10, 5, 0xFFFFFFFFu);
        CHECK(adv == w, "advance mismatch");
        int diff = 0;
        for (int r = 0; r < h; r++)
            for (int c = 0; c < w; c++) {
                unsigned int px = fb[(5 + r) * FB_W + (10 + c)];
                unsigned int chan = px & 0xFF;   /* bg 黑 fg 白 → 通道值 == du_blend 量化后的 alpha */
                unsigned int a = g[r * w + c];
                unsigned int expect = (a == 255) ? 255 : ((a * 255u) >> 8);
                if (chan != expect) diff++;
            }
        CHECK(diff == 0, "pixel mismatch vs dbf glyph");
        printf("glyph U+%04X: %dx%d advance=%d pixel-diff=%d\n",
               cps[k], w, h, adv, diff);
    }

    /* --- 2. UTF-8 解码 --- */
    {
        const char *s = "\xe4\xb8\xad";   /* 中 */
        CHECK(zh_utf8_decode(&s) == 0x4E2D, "utf8 decode 中");
        CHECK(*s == 0, "utf8 advanced past end");
        const char *bad = "\xff";
        CHECK(zh_utf8_decode(&bad) == 0xFFFFFFFFu, "utf8 invalid byte");
    }

    /* --- 3. 字符串宽度一致性 --- */
    {
        const char *s = "Deshab \xe6\xa1\x8c\xe9\x9d\xa2\xe7\xb3\xbb\xe7\xbb\x9f";
        int w1 = zh_string_width(s, 0);
        memset(fb, 0, sizeof(fb));
        du_fill_rect(&ctx, 0, 0, FB_W, FB_H, 0xFF000000u);
        int w2 = zh_draw_string(&ctx, s, 2, 2, 0xFFFFFFFFu, 0);
        printf("width: zh_string_width=%d zh_draw_string=%d\n", w1, w2);
        CHECK(w1 == w2, "string width mismatch");
        CHECK(w1 > 0, "zero width");
    }

    /* --- 4. 缺字回退：U+0391 希腊字母（GB2312 无，dbf 应缺字画框） --- */
    {
        memset(fb, 0, sizeof(fb));
        du_fill_rect(&ctx, 0, 0, FB_W, FB_H, 0xFF000000u);
        zh_draw_char(&ctx, 0x0391, 4, 4, 0xFFFFFFFFu);
        /* 空框：边框至少有非零像素 */
        int ink = 0;
        for (int i = 0; i < FB_W * FB_H; i++) if (fb[i] & 0xFF) ink++;
        CHECK(ink > 8, "missing-glyph fallback box not drawn");
        printf("missing-glyph fallback: ink=%d px\n", ink);
    }

    if (fail) { printf("RESULT: FAIL\n"); return 1; }
    printf("RESULT: PASS\n");
    return 0;
}
