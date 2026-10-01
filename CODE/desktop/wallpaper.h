/* wallpaper.h — 桌面壁纸（磁盘加载 + 一次性缩放进背景层缓存）
 *
 * 壁纸资产由 gen_wallpaper.py 生成：
 *   wallpaper.png  — 人看预览
 *   wallpaper.rgba — 1000×700 原始 RGBA 字节流，部署于 FAT32
 *                    system/wallpaper.rgba（避免 2.8MB 嵌入 ELF 撑大
 *                    DSK ibuf 占用，且换壁纸无需重编 desktop.elf）
 *
 * 加载路径（wallpaper_init）：
 *   FAT32 流式读 wallpaper.rgba → PE surface 暂存区（0x7800000，8MB，
 *   启动期独占，PE 应用启动时自行覆盖初始化）
 *   → 双线性缩放到背景层缓存（WALLPAPER_ADDR 0x6200000，≤7.5MB）
 *
 * 运行期 compositor/acrylic 直接读背景层缓存，零缩放开销。
 * 加载失败返回负值，调用方回退渐变背景，不阻塞启动。
 */
#ifndef DESHAB_WALLPAPER_H
#define DESHAB_WALLPAPER_H

typedef unsigned char      wp_u8;
typedef unsigned int       wp_u32;
typedef unsigned long long wp_u64;
typedef long long          wp_i64;

#define WALLPAPER_ADDR  0x6200000ULL
#define WALLPAPER_MAX   (7u * 1024u * 1024u + 512u * 1024u)   /* 7.5MB */

/* 壁纸原始数据暂存区（= PE surface 区，启动期独占） */
#define WALLPAPER_STAGING_ADDR  0x7800000ULL
#define WALLPAPER_STAGING_MAX   (8u * 1024u * 1024u)

/* FAT32 流式读取函数原型（f32io.h 的 f32_read_path_lfn_to_w） */
typedef int (*wallpaper_read_fn)(const char *path, unsigned char *dst,
                                 unsigned int cap, unsigned int *out_size);

/* 启动时调用一次：加载 + 缩放到背景层缓存（fb 尺寸）。
 * 返回 0 成功，负值失败（调用方回退渐变）。 */
int wallpaper_init(wallpaper_read_fn read_to, int fb_w, int fb_h);

/* 背景层缓存指针（成功后有效；失败返回 0） */
wp_u32 *wallpaper_layer(void);

#endif /* DESHAB_WALLPAPER_H */
