/* zhfont.h — Deshab 中文位图字库（.dbf）加载与渲染
 *
 * 字库文件由 CODE/font/mkfont.py 生成（Unicode BMP 码点索引、升序、
 * data_offset 相对文件头，见 CODE/font/dbf.h），部署于 FAT32
 * system/font/simhei_16.dbf（正文/菜单）与 simhei_24.dbf（标题/时钟）。
 *
 * 内存布局（identity-mapped 固定地址，无 malloc）：
 *   0x8000000  16px .dbf（≤1.8MB，zhfont_init 启动时加载）
 *   0x8300000  24px .dbf（≤3.9MB，zhfont_load24 懒加载）
 *
 * 字库缺失时 zh_draw_* 自动回退（缺字画空框），绝不阻塞启动。
 */
#ifndef DESHAB_ZHFONT_H
#define DESHAB_ZHFONT_H

#include <utsm/deshab_ui.h>

/* 固定地址与容量 */
#define ZHFONT16_ADDR  0x8000000ULL
#define ZHFONT16_MAX   (2u * 1024u * 1024u)
#define ZHFONT24_ADDR  0x8300000ULL
#define ZHFONT24_MAX   (4u * 1024u * 1024u)

/* FAT32 流式读取函数原型（由持有 fat32_io.h 的编译单元注入，
 * 即 f32_read_path_lfn_to）。 */
typedef int (*zhfont_read_fn)(const char *path, unsigned char *dst,
                              unsigned int cap, unsigned int *out_size);

/* 注入读取器并加载 16px 字库。返回 0 成功；负值失败（中文渲染回退 ASCII）。 */
int zhfont_init(zhfont_read_fn read_to);

/* 懒加载 24px 字库（开始菜单/小组件首次打开时调用）。返回 0 成功。 */
int zhfont_load24(void);

int zhfont_ok(void);     /* 16px 字库可用 */
int zhfont_ok24(void);   /* 24px 字库可用 */

/* 直接附加已加载的字库镜像（host SDL 宿主用 malloc 缓冲替代固定地址；
 * 裸机构建走 zhfont_init 固定地址路径，不使用本接口）。 */
void zhfont_attach16(const void *dbf_data);
void zhfont_attach24(const void *dbf_data);

/* UTF-8 解码：推进 *s，返回码点（BMP）。无效序列返回 0xFFFFFFFF 并跳 1 字节。 */
unsigned int zh_utf8_decode(const char **s);

/* 渲染单个 Unicode 码点（16px 字库）。返回推进像素数；未加载/缺字返回宽度 16。 */
int zh_draw_char(du_context *ctx, unsigned int codepoint,
                 long long x, long long y, unsigned int fg);

/* UTF-8 字符串渲染（纯 dbf 字体：ASCII 12px 半宽 + CJK 全宽，风格统一）。
 *   size24 : 0 = 16px 字库（正文/菜单），1 = 24px 字库（标题/时钟）
 * 返回总像素宽度。 */
int zh_draw_string(du_context *ctx, const char *utf8,
                   long long x, long long y, unsigned int fg, int size24);

/* UTF-8 字符串像素宽度（不绘制）。 */
int zh_string_width(const char *utf8, int size24);

/* 行高：16px 模式 18，24px 模式 26。 */
int zh_line_height(int size24);

#endif /* DESHAB_ZHFONT_H */
