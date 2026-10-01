/* f32io.h — fat32_io.h 单编译单元封装（M0）
 *
 * fat32_io.h 是 header-only static inline 实现，含 ~4.3MB 静态缓冲
 * （f32_disk 128KB + f32_cluster 4KB + f32_data 4MB）。多模块需要 FAT32
 * 访问时若各自 include 会每 TU 复制一份缓冲——f32io.c 是唯一包含者，
 * 本头导出薄封装（_w 后缀）供其他模块调用。
 *
 * 纯字符串助手（fat32_lfn_streq_ci / fat32_lfn_ends_with_ci / f32_entry）
 * 无状态，各模块直接 include fat32_lfn.h 即可，不经此封装。
 */
#ifndef DESHAB_F32IO_H
#define DESHAB_F32IO_H

/* f32_entry / 回调签名（与 fat32_io.h 一致；f32_entry 来自 fat32_lfn.h） */
#include "../tools/fat32_lfn.h"

typedef int (*f32_list_cb_fn)(const char *name, unsigned int size,
                              unsigned char attr, void *user_data);
typedef int (*f32_list_entry_fn)(const f32_entry *e, void *user_data);

/* 初始化（对应 f32_init，注入 block read/write） */
void f32_io_init(desktop_block_read_fn rd, desktop_block_write_fn wr);

/* 根目录文件读/写/列举 */
int  f32_read_root_file_w(const char *name11, u8 **out_data, u32 *out_size);
int  f32_write_root_file_w(const char *name11, const u8 *data, u32 size);
int  f32_list_root_w(f32_list_cb_fn emit, void *user_data);

/* 目录列举 / 路径定位 / 长路径读取 */
int  f32_list_dir_w(u32 dir_clus, f32_list_entry_fn emit, void *user_data);
int  f32_find_path_dir_lfn_w(const char *path, u32 *out_clus);
int  f32_read_path_lfn_w(const char *path, u8 **out_data, u32 *out_size);

/* 流式直读到指定缓冲（绕过 f32_data 4MB 上限，zhfont 字库加载用） */
int  f32_read_path_lfn_to_w(const char *path, u8 *dst, u32 cap, u32 *out_size);

/* 8.3 名转换（纯函数，但定义于 fat32_io.h，故同样封装） */
int  f32_name_to_83_w(const char *in, char out[11]);
void f32_name_from_83_w(const char in[11], char out[13]);
int  f32_neq11_w(const char *a, const char *b);

#endif /* DESHAB_F32IO_H */
