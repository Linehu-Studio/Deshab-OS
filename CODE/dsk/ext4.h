/* ext4.h — minimal full read-write ext4 driver for DSK (p2 data partition)
 *
 * 严格错误策略：任何错误（挂载失败/feature 不支持/IO 失败/校验不符）
 * 一律经 fatal(sym) 整屏 panic 停机，错误码体系 FS-E1x / FS-E2x。
 *
 * 支持范围（mkfs.ext4 由 pack_system_image.sh 以 -O
 * ^has_journal,^metadata_csum,^64bit,^resize_inode -b 4096 -I 256 创建）：
 *   - 4K blocksize、inode 256B、无 journal、无 metadata_csum、无 64bit
 *   - extent 树（depth 0/1）读写
 *   - 块位图/inode 位图分配与回收
 *   - 线性目录项查找/插入/扩展
 */
#ifndef DSK_EXT4_H
#define DSK_EXT4_H

typedef unsigned char  ext4_u8;
typedef unsigned short ext4_u16;
typedef unsigned int   ext4_u32;
typedef unsigned long long ext4_u64;

typedef int (*ext4_block_read_fn)(unsigned int index, ext4_u64 lba,
                                  ext4_u32 count, void *buf);
typedef int (*ext4_block_write_fn)(unsigned int index, ext4_u64 lba,
                                   ext4_u32 count, const void *buf);

/* 挂载 p2 ext4 分区（GPT 探测 + superblock/feature 校验）。
 * 任何失败直接调用 fatal(sym) panic。成功返回 0。 */
int ext4_mount(ext4_block_read_fn rd, ext4_block_write_fn wr,
               void (*fatal)(const char *sym), void (*logl)(const char *s),
               void (*logh)(const char *p, ext4_u64 v));

/* 按路径读文件（"/system/deshab64/FUCK"），返回内部静态缓冲。
 * 任何失败 fatal。成功返回 0 并填充 out/size。 */
int ext4_read_file(const char *path, ext4_u8 **out, ext4_u32 *size);

/* 按路径写文件（存在则覆写，不存在则创建）。
 * 写后自动回读逐字节校验，不符 fatal。成功返回 0。 */
int ext4_write_file(const char *path, const ext4_u8 *data, ext4_u32 size);

/* 启动自检：读 /system/deshab64/FUCK + 写读校验 /dsk_ext4_rw.test。 */
void ext4_boot_selftest(void);

#endif
