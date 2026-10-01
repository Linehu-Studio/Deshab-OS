/* zhfont.c — 中文位图字库加载与渲染实现
 *
 * .dbf 格式（见 ../font/dbf.h）：
 *   header 14 字节 + count*12 字节索引（按 Unicode 码点升序）+ 字形数据。
 *   索引项 data_offset 相对文件头。
 *
 * 渲染：8bpp 灰度字形逐像素 du_blend（与 ASCII 渲染同路径，无字体缓存，
 * 每屏 <500 字在性能预算内，见桌面重构计划第七节）。
 */
/* 顺序敏感：ascii_font.h 声明 g_ascii（deshab_ui.h 的 du_draw_char 引用） */
#include "ascii_font.h"
#include "zhfont.h"
#include "../font/dbf.h"

typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;
typedef long long          i64;

/* ---- 字库状态 ---- */
static zhfont_read_fn g_zh_read = 0;
static const u8 *g_zh16 = 0;
static const u8 *g_zh24 = 0;

/* ---- dbf 访问 ---- */

static const u8 *zh_dbf_get(const u8 *font, u32 cp, int *out_w, int *out_h) {
    const dbf_header *hdr = (const dbf_header *)font;
    const dbf_index_entry *idx = (const dbf_index_entry *)(font + 14);
    if (cp > 0xFFFF) return 0;
    int i = dbf_lookup((dbf_index_entry *)idx, hdr->count, (u16)cp);
    if (i < 0) return 0;
    *out_w = idx[i].width;
    *out_h = hdr->height;
    return font + idx[i].offset;
}

/* 缺字占位：1px 空框 */
static int zh_draw_missing(du_context *ctx, i64 x, i64 y, int w, int h, u32 fg) {
    du_rect_outline(ctx, x, y, w, h, fg, 1);
    return w;
}

/* ---- 公开接口 ---- */

/* 头部静态校验（magic / bpp / 尺寸字段），不依赖文件大小 */
static int zh_dbf_valid(const u8 *dst) {
    const dbf_header *hdr = (const dbf_header *)dst;
    if (!dst) return -1;
    if (hdr->magic[0] != 'D' || hdr->magic[1] != 'B' ||
        hdr->magic[2] != 'F' || hdr->magic[3] != 0x10) return -2;
    if (hdr->bpp != 8) return -3;
    if (hdr->count == 0 || hdr->height == 0 || hdr->width == 0) return -4;
    return 0;
}

static int zh_load_dbf(const char *path, u8 *dst, u32 cap) {
    u32 sz = 0;
    if (!g_zh_read) return -1;
    if (g_zh_read(path, dst, cap, &sz) != 0) return -2;
    if (sz < 14) return -3;
    int rc = zh_dbf_valid(dst);
    if (rc != 0) return rc - 10;
    /* 越界防护：索引区与最大数据项必须在文件内 */
    {
        const dbf_header *hdr = (const dbf_header *)dst;
        const dbf_index_entry *last =
            (const dbf_index_entry *)(dst + 14 + (u64)(hdr->count - 1) * 12);
        if (14 + (u64)hdr->count * 12 > sz) return -7;
        if ((u64)last->offset + last->size > sz) return -8;
    }
    return 0;
}

int zhfont_init(zhfont_read_fn read_to) {
    int rc;
    g_zh_read = read_to;
    rc = zh_load_dbf("system/font/simhei_16.dbf",
                     (u8 *)ZHFONT16_ADDR, ZHFONT16_MAX);
    if (rc != 0) {
        g_zh16 = 0;
        return rc;   /* -1=无读取器 -2=FAT32读失败 -3=过小 -7/-8=越界 -11..-13=头校验 */
    }
    g_zh16 = (const u8 *)ZHFONT16_ADDR;
    return 0;
}

int zhfont_load24(void) {
    if (g_zh24) return 0;
    if (zh_load_dbf("system/font/simhei_24.dbf",
                    (u8 *)ZHFONT24_ADDR, ZHFONT24_MAX) != 0) {
        return -1;
    }
    g_zh24 = (const u8 *)ZHFONT24_ADDR;
    return 0;
}

int zhfont_ok(void)   { return g_zh16 != 0; }
int zhfont_ok24(void) { return g_zh24 != 0; }

void zhfont_attach16(const void *dbf_data) {
    /* 校验 magic 后挂接（数据由调用方保证生命周期） */
    if (dbf_data && zh_dbf_valid((const u8 *)dbf_data) == 0) g_zh16 = (const u8 *)dbf_data;
    else g_zh16 = 0;
}

void zhfont_attach24(const void *dbf_data) {
    if (dbf_data && zh_dbf_valid((const u8 *)dbf_data) == 0) g_zh24 = (const u8 *)dbf_data;
    else g_zh24 = 0;
}

unsigned int zh_utf8_decode(const char **s) {
    const u8 *p = (const u8 *)*s;
    u8 c = *p++;
    unsigned int cp;
    int extra;
    if (c < 0x80) { *s = (const char *)p; return c; }
    else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
    else { (*s)++; return 0xFFFFFFFFu; }  /* 无效首字节 */
    for (int i = 0; i < extra; i++) {
        u8 cc = *p;
        if ((cc & 0xC0) != 0x80) { (*s)++; return 0xFFFFFFFFu; }
        cp = (cp << 6) | (cc & 0x3F);
        p++;
    }
    *s = (const char *)p;
    return cp;
}

int zh_draw_char(du_context *ctx, unsigned int codepoint,
                 i64 x, i64 y, unsigned int fg) {
    if (!g_zh16) {
        /* 字库未加载：ASCII 走 Consolas，其余画空框 */
        if (codepoint >= 0x20 && codepoint < 0x7F) {
            du_draw_char(ctx, (char)codepoint, x, y, fg, 0);
            return (int)DU_ASCII_STEP;
        }
        return 16;
    }
    if (codepoint < 0x20) return 4;   /* 控制字符：窄空隙 */
    int w, h;
    const u8 *g = zh_dbf_get(g_zh16, codepoint, &w, &h);
    if (!g) return zh_draw_missing(ctx, x, y, 16, 16, fg);
    for (int r = 0; r < h; r++) {
        for (int c = 0; c < w; c++) {
            u8 a = g[r * w + c];
            if (!a) continue;
            du_u32 bg = du_pixel_read(ctx, x + c, y + r);
            du_pixel(ctx, x + c, y + r,
                     (a == 255) ? fg : du_blend(bg, fg, a));
        }
    }
    return w;
}

int zh_string_width(const char *utf8, int size24) {
    int total = 0;
    const u8 *font = size24 ? g_zh24 : g_zh16;
    if (!utf8) return 0;
    while (*utf8) {
        unsigned int cp = zh_utf8_decode(&utf8);
        if (cp == 0xFFFFFFFFu) { total += 4; continue; }
        if (cp < 0x20) { total += 4; continue; }
        int w, h;
        const u8 *g = font ? zh_dbf_get(font, cp, &w, &h) : 0;
        if (!g) w = size24 ? 24 : 16;
        total += w + 1;   /* 1px 字间距 */
    }
    return total > 0 ? total - 1 : 0;
}

int zh_line_height(int size24) { return size24 ? 26 : 18; }

int zh_draw_string(du_context *ctx, const char *utf8,
                   i64 x, i64 y, unsigned int fg, int size24) {
    const u8 *font = size24 ? g_zh24 : g_zh16;
    i64 cx = x;
    if (!utf8) return 0;
    while (*utf8) {
        unsigned int cp = zh_utf8_decode(&utf8);
        if (cp == 0xFFFFFFFFu) { cx += 4; continue; }
        if (cp < 0x20) { cx += 4; continue; }
        if (cp == 0x20) { cx += (size24 ? 18 : 12); continue; }
        int w, h;
        const u8 *g = font ? zh_dbf_get(font, cp, &w, &h) : 0;
        if (!g) {
            cx += zh_draw_missing(ctx, cx, y, size24 ? 24 : 16,
                                  size24 ? 24 : 16, fg) + 1;
            continue;
        }
        for (int r = 0; r < h; r++) {
            for (int c = 0; c < w; c++) {
                u8 a = g[r * w + c];
                if (!a) continue;
                du_u32 bg = du_pixel_read(ctx, cx + c, y + r);
                du_pixel(ctx, cx + c, y + r,
                         (a == 255) ? fg : du_blend(bg, fg, a));
            }
        }
        cx += w + 1;
    }
    return (int)(cx - x);
}
