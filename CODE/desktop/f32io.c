/* f32io.c — fat32_io.h 唯一包含者（M0 模块化拆分）
 *
 * 全模块仅此 TU include fat32_io.h（4MB f32_data 等缓冲唯一副本），
 * 其余模块经 f32io.h 的 _w 封装访问。
 */
#include "desktop.h"

#define F32_DATA_BYTES (4u * 1024u * 1024u)
#include "../tools/fat32_io.h"

void f32_io_init(desktop_block_read_fn rd, desktop_block_write_fn wr) {
    f32_init((f32_block_read_fn)rd, (f32_block_write_fn)wr);
}

int f32_read_root_file_w(const char *name11, u8 **out_data, u32 *out_size) {
    return f32_read_root_file(name11, out_data, out_size);
}

int f32_write_root_file_w(const char *name11, const u8 *data, u32 size) {
    return f32_write_root_file(name11, data, size);
}

int f32_list_root_w(f32_list_cb_fn emit, void *user_data) {
    return f32_list_root((f32_list_cb)emit, user_data);
}

int f32_list_dir_w(u32 dir_clus, f32_list_entry_fn emit, void *user_data) {
    return f32_list_dir(dir_clus, (f32_list_entry_cb)emit, user_data);
}

int f32_find_path_dir_lfn_w(const char *path, u32 *out_clus) {
    return f32_find_path_dir_lfn(path, out_clus);
}

int f32_read_path_lfn_w(const char *path, u8 **out_data, u32 *out_size) {
    return f32_read_path_lfn(path, out_data, out_size);
}

int f32_read_path_lfn_to_w(const char *path, u8 *dst, u32 cap, u32 *out_size) {
    int rc = f32_read_path_lfn_to(path, dst, cap, out_size);
    if (rc != 0) slog_num("[f32io] read_to rc=", rc);   /* M1 调试：定位失败步骤 */
    return rc;
}

int f32_name_to_83_w(const char *in, char out[11]) {
    return f32_name_to_83(in, out);
}

void f32_name_from_83_w(const char in[11], char out[13]) {
    f32_name_from_83(in, out);
}

int f32_neq11_w(const char *a, const char *b) {
    return f32_neq11(a, b);
}
