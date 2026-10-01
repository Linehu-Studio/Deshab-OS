/* textures.h — 裸 RGBA 纹理绘制（M2）
 *
 * 纹理由 gen_textures.py 生成（.rgba = 无头 RGBA 字节流，R,G,B,A），
 * 运行时经 FAT32 流式读入固定地址，tex_draw_scaled 双线性缩放 + alpha 混合。
 */
#ifndef DESHAB_TEXTURES_H
#define DESHAB_TEXTURES_H

#include "../UTSM/include/utsm/deshab_ui.h"

typedef unsigned char      tex_u8;
typedef unsigned int       tex_u32;
typedef unsigned long long tex_u64;
typedef long long          tex_i64;

/* 开始菜单 logo 缓存区（zhfont24 之后、WAPP 之前，1MB） */
#define SMLOGO_ADDR  0x8700000ULL
#define SMLOGO_MAX   (1024u * 1024u)
#define SMLOGO_W     216
#define SMLOGO_H     210

/* 启动背景（boot 屏专用，读入 PE surface 暂存区，启动期独占） */
#define BOOTBG_W  889
#define BOOTBG_H  500
#define BOOTLOGO_W  157
#define BOOTLOGO_H  157

/* 双线性缩放绘制 RGBA 纹理（src-over alpha 混合）。
 * dim: 0..256 亮度衰减（256 = 原亮度；boot 背景用 ~150 压暗保证文字对比）。 */
void tex_draw_scaled(du_context *ctx, const tex_u8 *rgba, int sw, int sh,
                     int x, int y, int w, int h, int dim);

#endif /* DESHAB_TEXTURES_H */
