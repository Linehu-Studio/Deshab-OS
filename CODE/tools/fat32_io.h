/* fat32_io.h — Deshab 用户态共享 FAT32 读写库（static inline，无库）
 *
 * 前置条件：包含者已定义 u8/u16/u32/u64/i64 基础类型。
 * 通过 f32_init() 注入 block_read/block_write 函数指针（来自 dkm_kernel_api + 0xA8）。
 *
 * 能力：FAT32 根目录文件的读/写/删/列举，支持 FAT 链跟随、多簇文件。
 * 限制：仅根目录（不支持子目录，Phase 2.2 fileman 将扩展）；单文件最大 256KB。
 *
 * 用法：
 *   f32_init(block_read, block_write)         — 初始化（block_write 可为 NULL，仅读场景）
 *   f32_name_to_83("file.txt", name11)        — "file.txt" → 11 字符 8.3 名
 *   f32_read_root_file(name11, &data, &size)  — 读根目录文件到 f32_data 缓冲
 *   f32_write_root_file(name11, data, size)   — 写/替换根目录文件
 *   f32_delete_root_file(name11)              — 删除根目录文件
 *   f32_list_root(emit_cb, user_data)         — 列举根目录
 *
 * 缓冲区：每个包含本头的 .elf 独立持有 256KB g_disk + 4KB g_cluster + 256KB g_fdata（共 ~516KB BSS）。
 * 与 net_stack.h 同模式，接受 static 缓冲的代价以换取无库依赖。
 */

#ifndef DESHAB_FAT32_IO_H
#define DESHAB_FAT32_IO_H

/* ---- block 设备函数类型（与 shell/DSK 一致：index 参数固定 0） ---- */
typedef int (*f32_block_read_fn)(u32 index, u64 lba, u32 count, void *buf);
typedef int (*f32_block_write_fn)(u32 index, u64 lba, u32 count, const void *buf);

static f32_block_read_fn  f32_blk_read  = 0;
static f32_block_write_fn f32_blk_write = 0;

/* ---- FAT32 BPB / 目录项结构（packed，与 DSK/shell 一致） ---- */
typedef struct __attribute__((packed)) {
    u8 jmp[3]; char oem[8]; u16 bps; u8 spc; u16 rsvd; u8 fc; u16 root_ent;
    u16 ts16; u8 media; u16 spf16; u16 spt; u16 heads; u32 hidden; u32 ts32;
    u32 spf; u16 flags; u16 ver; u32 root_clus; u16 fsi; u16 bkboot;
    u8 res[12]; u8 drv; u8 ntfl; u8 sig; u32 ser; char lbl[11]; char typ[8];
    u8 code[420]; u16 boot_sig;
} f32_bpb;

typedef struct __attribute__((packed)) {
    char name[11]; u8 attr; u8 ntr; u8 ctenth;
    u16 ctime; u16 cdate; u16 adate; u16 chigh;
    u16 wtime; u16 wdate; u16 clow; u32 fsize;
} f32_dirent;

/* ---- 内部小工具 ---- */
static inline u32 f32_r32(const u8 *p) { return (u32)p[0]|((u32)p[1]<<8)|((u32)p[2]<<16)|((u32)p[3]<<24); }
static inline u16 f32_r16(const u8 *p) { return (u16)p[0]|((u16)p[1]<<8); }
static inline int f32_neq11(const char *a, const char *b) {
    for (int i = 0; i < 11; i++) if (a[i] != b[i]) return 0;
    return 1;
}

/* ---- 批量读写缓冲区（与 DSK/shell 对齐：256 扇区覆盖 BPB+FAT+根目录+小文件） ---- */
static u8 f32_disk[131072];      /* 256 扇区 BPB+FAT+根目录缓存 */
static u8 f32_cluster[4096];     /* 单簇缓冲 */
static u8 f32_data[262144];      /* 文件数据缓冲（最大 256KB） */
static int f32_disk_loaded = 0;  /* f32_disk 是否已加载 BPB+FAT */

/* 初始化：注入 block_read/block_write 函数指针 */
static inline void f32_init(f32_block_read_fn rd, f32_block_write_fn wr) {
    f32_blk_read = rd;
    f32_blk_write = wr;
    f32_disk_loaded = 0;
}

static inline int f32_read_sectors(u32 lba, u32 count, u8 *out) {
    return f32_blk_read ? f32_blk_read(0, lba, count, out) : -1;
}
static inline int f32_write_sectors(u32 lba, u32 count, const u8 *buf) {
    return f32_blk_write ? f32_blk_write(0, lba, count, buf) : -1;
}

/* 确保 f32_disk 已加载 BPB+FAT+根目录（256 扇区） */
static inline int f32_disk_load(void) {
    if (f32_disk_loaded) return 0;
    if (f32_read_sectors(0, 256, f32_disk) != 0) return -1;
    const f32_bpb *bpb = (const f32_bpb *)f32_disk;
    if (bpb->boot_sig != 0xAA55 || bpb->bps != 512) return -2;
    if (bpb->spf == 0 || bpb->root_clus < 2) return -3;
    f32_disk_loaded = 1;
    return 0;
}

/* ---- 8.3 名字转换 ---- */

/* "file.txt" / "NAME" → 11 字符 8.3 名（大写，空格填充）
 * 返回 0 成功，-1 失败（过长或非法字符） */
static inline int f32_name_to_83(const char *in, char out[11]) {
    for (int i = 0; i < 11; i++) out[i] = ' ';
    int inlen = 0;
    while (in[inlen] && inlen < 13) inlen++;
    if (inlen == 0 || inlen > 12) return -1;
    int dot = -1;
    for (int i = 0; i < inlen; i++) if (in[i] == '.') dot = i;
    int base_end = (dot >= 0) ? dot : inlen;
    if (base_end == 0 || base_end > 8) return -1;
    for (int j = 0; j < base_end; j++) {
        char c = in[j];
        if (c >= 'a' && c <= 'z') c -= 32;
        if (c == '.' || c == ' ' || c == '/' || c == '\\') return -1;
        out[j] = c;
    }
    if (dot >= 0) {
        int extlen = inlen - dot - 1;
        if (extlen > 3) return -1;
        for (int j = 0; j < extlen; j++) {
            char c = in[dot + 1 + j];
            if (c >= 'a' && c <= 'z') c -= 32;
            if (c == '.' || c == ' ' || c == '/' || c == '\\') return -1;
            out[8 + j] = c;
        }
    }
    return 0;
}

/* 11 字符 8.3 名 → 可显示字符串（如 "README.TXT"） */
static inline void f32_name_from_83(const char in[11], char out[13]) {
    int p = 0;
    for (int j = 0; j < 8; j++) {
        if (in[j] == ' ') break;
        out[p++] = in[j];
    }
    if (in[8] != ' ') {
        out[p++] = '.';
        for (int j = 8; j < 11; j++) {
            if (in[j] == ' ') break;
            out[p++] = in[j];
        }
    }
    out[p] = 0;
}

/* ---- 根目录文件查找 ---- */

/* 在根目录簇链中查找 11 字符名，输出首簇号与文件大小 */
static inline int f32_find_in_root(const u8 *clus, u32 clus_sectors, const char *target,
                                   u32 *out_clus, u32 *out_size, u32 *out_idx) {
    const f32_dirent *dir = (const f32_dirent *)clus;
    for (u32 e = 0; e * 32 < clus_sectors * 512; e++) {
        if (dir[e].name[0] == 0) break;
        if ((u8)dir[e].name[0] == 0xE5) continue;
        if (dir[e].attr == 0x0F) continue;
        if (dir[e].attr & 0x08) continue;
        if (f32_neq11(dir[e].name, target)) {
            *out_clus = (u32)f32_r16((const u8*)&dir[e].clow) | ((u32)f32_r16((const u8*)&dir[e].chigh) << 16);
            *out_size = dir[e].fsize;
            if (out_idx) *out_idx = e;
            return 0;
        }
    }
    return -1;
}

/* 读取根目录下指定 8.3 名文件到 f32_data。
 * 返回 0 成功，*out_data 指向 f32_data，*out_size 为字节数；非 0 失败。 */
static inline int f32_read_root_file(const char *name11, u8 **out_data, u32 *out_size) {
    if (f32_disk_load() != 0) return -1;
    const f32_bpb *bpb = (const f32_bpb *)f32_disk;
    u32 data_lba = bpb->rsvd + (u32)bpb->fc * bpb->spf;
    u32 fat_byte_off = bpb->rsvd * 512;
    u32 spc = bpb->spc;

    u32 clus = bpb->root_clus;
    u32 found_clus = 0, found_size = 0;
    int found = 0;
    while (clus >= 2 && clus < 0x0FFFFFF8 && !found) {
        u32 lba = data_lba + (clus - 2) * spc;
        const u8 *cb;
        if ((u64)lba * 512 + (u64)spc * 512 <= 256ULL * 512)
            cb = f32_disk + (u64)lba * 512;
        else {
            if (f32_read_sectors(lba, 8, f32_cluster) != 0) return -4;
            cb = f32_cluster;
        }
        if (f32_find_in_root(cb, spc, name11, &found_clus, &found_size, 0) == 0) { found = 1; break; }
        u32 fo = fat_byte_off + clus * 4;
        if (fo + 4 > sizeof(f32_disk)) break;
        clus = f32_r32(f32_disk + fo) & 0x0FFFFFFF;
    }
    if (!found) return -5;
    if (found_size > sizeof(f32_data)) return -6;

    u8 *dst = f32_data; u32 remaining = found_size; u32 fc = found_clus;
    while (fc >= 2 && fc < 0x0FFFFFF8 && remaining > 0) {
        u32 fc_lba = data_lba + (fc - 2) * spc;
        u32 fc_bytes = spc * 512;
        if (fc_bytes > remaining) fc_bytes = remaining;
        const u8 *fb;
        if ((u64)fc_lba * 512 + (u64)spc * 512 <= 256ULL * 512)
            fb = f32_disk + (u64)fc_lba * 512;
        else {
            if (f32_read_sectors(fc_lba, 8, f32_cluster) != 0) return -7;
            fb = f32_cluster;
        }
        for (u32 b = 0; b < fc_bytes; b++) dst[b] = fb[b];
        dst += fc_bytes; remaining -= fc_bytes;
        u32 fo = fat_byte_off + fc * 4;
        if (fo + 4 > sizeof(f32_disk)) break;
        fc = f32_r32(f32_disk + fo) & 0x0FFFFFFF;
    }
    *out_data = f32_data; *out_size = found_size;
    return 0;
}

/* 写入根目录下指定 8.3 名文件（存在则替换，不存在则新建）。
 * 依赖 f32_disk 已加载 BPB+FAT。返回 0 成功。 */
static inline int f32_write_root_file(const char *name11, const u8 *data, u32 size) {
    if (f32_disk_load() != 0) return -1;
    if (!f32_blk_write) return -10;
    /* 写入前重新加载 BPB+FAT，保证与盘上一致 */
    if (f32_read_sectors(0, 256, f32_disk) != 0) return -2;
    const f32_bpb *bpb = (const f32_bpb *)f32_disk;
    u32 spc = bpb->spc;
    u32 fat_lba = bpb->rsvd;
    u32 fat_sectors = bpb->spf;
    u32 data_lba = bpb->rsvd + (u32)bpb->fc * bpb->spf;
    u32 fat_byte_off = bpb->rsvd * 512;
    u32 root_clus = bpb->root_clus;
    u32 cluster_bytes = spc * 512;

    u32 root_lba = data_lba + (root_clus - 2) * spc;
    u8 *root_buf = f32_disk + (u64)root_lba * 512;
    u32 max_entries = cluster_bytes / 32;
    f32_dirent *dir = (f32_dirent *)root_buf;
    int free_entry = -1;
    u32 existing_clus = 0;
    int found = 0;
    u32 existing_idx = 0;
    for (u32 e = 0; e < max_entries; e++) {
        if (dir[e].name[0] == 0) { if (free_entry < 0) free_entry = (int)e; break; }
        if ((u8)dir[e].name[0] == 0xE5) { if (free_entry < 0) free_entry = (int)e; continue; }
        if (dir[e].attr == 0x0F) continue;
        if (dir[e].attr & 0x08) continue;
        if (f32_neq11(dir[e].name, name11)) {
            existing_clus = (u32)f32_r16((const u8*)&dir[e].clow) | ((u32)f32_r16((const u8*)&dir[e].chigh) << 16);
            found = 1; existing_idx = e;
            if (free_entry < 0) free_entry = (int)e;
            break;
        }
    }

    u32 bytes_needed = size > 0 ? size : 1;
    u32 clusters_needed = (bytes_needed + cluster_bytes - 1) / cluster_bytes;
    u32 first_clus = 0;
    u32 prev_clus = 0;

    if (found && existing_clus >= 2) {
        first_clus = existing_clus;
        u32 cur = existing_clus;
        u32 count = 0;
        while (cur >= 2 && cur < 0x0FFFFFF8 && count < clusters_needed) {
            prev_clus = cur;
            u32 fo = fat_byte_off + cur * 4;
            if (fo + 4 > sizeof(f32_disk)) break;
            cur = f32_r32(f32_disk + fo) & 0x0FFFFFFF;
            count++;
        }
        while (count < clusters_needed) {
            u32 newc = 0;
            for (u32 c = 2; c < (fat_sectors * 512) / 4; c++) {
                u32 fo = fat_byte_off + c * 4;
                if (fo + 4 > sizeof(f32_disk)) break;
                if ((f32_r32(f32_disk + fo) & 0x0FFFFFFF) == 0) { newc = c; break; }
            }
            if (newc == 0) return -3;
            u32 pfo = fat_byte_off + prev_clus * 4;
            if (pfo + 4 <= sizeof(f32_disk)) {
                f32_disk[pfo] = (u8)(newc & 0xFF);
                f32_disk[pfo+1] = (u8)((newc >> 8) & 0xFF);
                f32_disk[pfo+2] = (u8)((newc >> 16) & 0xFF);
                f32_disk[pfo+3] = (u8)((newc >> 24) & 0x0F);
            }
            prev_clus = newc;
            count++;
        }
        if (prev_clus >= 2) {
            u32 fo = fat_byte_off + prev_clus * 4;
            if (fo + 4 <= sizeof(f32_disk)) {
                f32_disk[fo] = 0xF8; f32_disk[fo+1] = 0xFF; f32_disk[fo+2] = 0xFF; f32_disk[fo+3] = 0x0F;
            }
        }
        /* 释放多余旧簇 */
        u32 next = 0;
        u32 fo = fat_byte_off + prev_clus * 4;
        if (fo + 4 <= sizeof(f32_disk)) next = f32_r32(f32_disk + fo) & 0x0FFFFFFF;
        while (next >= 2 && next < 0x0FFFFFF8) {
            u32 nfo = fat_byte_off + next * 4;
            u32 nn = 0;
            if (nfo + 4 <= sizeof(f32_disk)) nn = f32_r32(f32_disk + nfo) & 0x0FFFFFFF;
            f32_disk[nfo] = 0; f32_disk[nfo+1] = 0; f32_disk[nfo+2] = 0; f32_disk[nfo+3] = 0;
            next = nn;
        }
    } else {
        if (free_entry < 0) return -4;
        for (u32 i = 0; i < clusters_needed; i++) {
            u32 newc = 0;
            for (u32 c = 2; c < (fat_sectors * 512) / 4; c++) {
                u32 fo = fat_byte_off + c * 4;
                if (fo + 4 > sizeof(f32_disk)) break;
                if ((f32_r32(f32_disk + fo) & 0x0FFFFFFF) == 0) { newc = c; break; }
            }
            if (newc == 0) return -3;
            if (i == 0) first_clus = newc;
            if (prev_clus >= 2) {
                u32 pfo = fat_byte_off + prev_clus * 4;
                f32_disk[pfo] = (u8)(newc & 0xFF);
                f32_disk[pfo+1] = (u8)((newc >> 8) & 0xFF);
                f32_disk[pfo+2] = (u8)((newc >> 16) & 0xFF);
                f32_disk[pfo+3] = (u8)((newc >> 24) & 0x0F);
            }
            prev_clus = newc;
        }
        if (prev_clus >= 2) {
            u32 fo = fat_byte_off + prev_clus * 4;
            f32_disk[fo] = 0xF8; f32_disk[fo+1] = 0xFF; f32_disk[fo+2] = 0xFF; f32_disk[fo+3] = 0x0F;
        }
    }

    /* 写数据到簇 */
    u32 remaining = size;
    const u8 *src = data;
    u32 cur = first_clus;
    u32 ci = 0;
    while (cur >= 2 && cur < 0x0FFFFFF8 && ci < clusters_needed) {
        u32 clba = data_lba + (cur - 2) * spc;
        u32 chunk = remaining < cluster_bytes ? remaining : cluster_bytes;
        if ((u64)clba * 512 + cluster_bytes <= 256ULL * 512) {
            u8 *dst = f32_disk + (u64)clba * 512;
            for (u32 b = 0; b < chunk; b++) dst[b] = src[b];
            for (u32 b = chunk; b < cluster_bytes; b++) dst[b] = 0;
        } else {
            if (f32_read_sectors(clba, spc, f32_cluster) != 0) return -5;
            for (u32 b = 0; b < chunk; b++) f32_cluster[b] = src[b];
            for (u32 b = chunk; b < cluster_bytes; b++) f32_cluster[b] = 0;
            if (f32_write_sectors(clba, spc, f32_cluster) != 0) return -6;
        }
        src += chunk; remaining -= chunk; ci++;
        u32 fo = fat_byte_off + cur * 4;
        if (fo + 4 > sizeof(f32_disk)) break;
        cur = f32_r32(f32_disk + fo) & 0x0FFFFFFF;
    }

    /* 更新目录项 */
    u32 entry_idx = found ? existing_idx : (u32)free_entry;
    f32_dirent *e = &dir[entry_idx];
    if (!found) {
        for (int i = 0; i < 11; i++) e->name[i] = name11[i];
        e->attr = 0x20; e->ntr = 0; e->ctenth = 0; e->ctime = 0; e->cdate = 0; e->adate = 0;
    }
    e->chigh = (u16)((first_clus >> 16) & 0xFFFF);
    e->clow = (u16)(first_clus & 0xFFFF);
    e->fsize = size;

    /* 回写 FAT（两份）+ 根目录 + 缓冲区内的数据簇 */
    for (u32 f = 0; f < bpb->fc; f++) {
        u32 flba = fat_lba + f * fat_sectors;
        if (f32_write_sectors(flba, fat_sectors, f32_disk + (u64)flba * 512) != 0) return -7;
    }
    u32 root_dir_lba = data_lba + (root_clus - 2) * spc;
    if ((u64)root_dir_lba * 512 + cluster_bytes <= 256ULL * 512) {
        if (f32_write_sectors(root_dir_lba, spc, f32_disk + (u64)root_dir_lba * 512) != 0) return -8;
    }
    cur = first_clus; ci = 0;
    while (cur >= 2 && cur < 0x0FFFFFF8 && ci < clusters_needed) {
        u32 clba = data_lba + (cur - 2) * spc;
        if ((u64)clba * 512 + cluster_bytes <= 256ULL * 512) {
            if (f32_write_sectors(clba, spc, f32_disk + (u64)clba * 512) != 0) return -9;
        }
        u32 fo = fat_byte_off + cur * 4;
        if (fo + 4 > sizeof(f32_disk)) break;
        cur = f32_r32(f32_disk + fo) & 0x0FFFFFFF;
        ci++;
    }
    f32_disk_loaded = 0;  /* 下次操作重新加载，避免使用脏缓存 */
    return 0;
}

/* 删除根目录下指定 8.3 名文件：清空簇链 + 标记目录项为 0xE5。
 * 返回 0 成功，-1 未找到，其他负值失败。 */
static inline int f32_delete_root_file(const char *name11) {
    if (f32_disk_load() != 0) return -2;
    if (!f32_blk_write) return -10;
    if (f32_read_sectors(0, 256, f32_disk) != 0) return -3;
    const f32_bpb *bpb = (const f32_bpb *)f32_disk;
    u32 spc = bpb->spc;
    u32 fat_lba = bpb->rsvd;
    u32 fat_sectors = bpb->spf;
    u32 data_lba = bpb->rsvd + (u32)bpb->fc * bpb->spf;
    u32 fat_byte_off = bpb->rsvd * 512;
    u32 root_clus = bpb->root_clus;

    u32 root_lba = data_lba + (root_clus - 2) * spc;
    u8 *root_buf = f32_disk + (u64)root_lba * 512;
    u32 max_entries = (spc * 512) / 32;
    f32_dirent *dir = (f32_dirent *)root_buf;
    u32 found_idx = 0xFFFFFFFFu;
    for (u32 e = 0; e < max_entries; e++) {
        if (dir[e].name[0] == 0) break;
        if ((u8)dir[e].name[0] == 0xE5) continue;
        if (dir[e].attr == 0x0F) continue;
        if (dir[e].attr & 0x08) continue;
        if (f32_neq11(dir[e].name, name11)) { found_idx = e; break; }
    }
    if (found_idx == 0xFFFFFFFFu) return -1;

    /* 释放簇链 */
    u32 cur = (u32)f32_r16((const u8*)&dir[found_idx].clow) | ((u32)f32_r16((const u8*)&dir[found_idx].chigh) << 16);
    while (cur >= 2 && cur < 0x0FFFFFF8) {
        u32 fo = fat_byte_off + cur * 4;
        u32 next = 0;
        if (fo + 4 <= sizeof(f32_disk)) next = f32_r32(f32_disk + fo) & 0x0FFFFFFF;
        f32_disk[fo] = 0; f32_disk[fo+1] = 0; f32_disk[fo+2] = 0; f32_disk[fo+3] = 0;
        cur = next;
    }
    /* 标记目录项为已删除 */
    dir[found_idx].name[0] = (char)0xE5;

    /* 回写 FAT + 根目录 */
    for (u32 f = 0; f < bpb->fc; f++) {
        u32 flba = fat_lba + f * fat_sectors;
        if (f32_write_sectors(flba, fat_sectors, f32_disk + (u64)flba * 512) != 0) return -4;
    }
    if (f32_write_sectors(root_lba, spc, f32_disk + (u64)root_lba * 512) != 0) return -5;
    f32_disk_loaded = 0;
    return 0;
}

/* 列举根目录条目回调签名：返回 0 继续，非 0 停止 */
typedef int (*f32_list_cb)(const char *name, u32 size, u8 attr, void *user_data);

/* 列出根目录所有文件/目录，调用回调输出。
 * 回调返回非 0 立即停止。返回 0 成功（含回调提前停止），负值失败。 */
static inline int f32_list_root(f32_list_cb emit, void *user_data) {
    if (f32_disk_load() != 0) return -1;
    const f32_bpb *bpb = (const f32_bpb *)f32_disk;
    u32 data_lba = bpb->rsvd + (u32)bpb->fc * bpb->spf;
    u32 fat_byte_off = bpb->rsvd * 512;
    u32 spc = bpb->spc;
    u32 clus = bpb->root_clus;
    while (clus >= 2 && clus < 0x0FFFFFF8) {
        u32 lba = data_lba + (clus - 2) * spc;
        const u8 *cb;
        if ((u64)lba * 512 + (u64)spc * 512 <= 256ULL * 512)
            cb = f32_disk + (u64)lba * 512;
        else {
            if (f32_read_sectors(lba, 8, f32_cluster) != 0) return -4;
            cb = f32_cluster;
        }
        const f32_dirent *dir = (const f32_dirent *)cb;
        for (u32 e = 0; e * 32 < spc * 512; e++) {
            if (dir[e].name[0] == 0) goto done;
            if ((u8)dir[e].name[0] == 0xE5) continue;
            if (dir[e].attr == 0x0F) continue;
            if (dir[e].attr & 0x08) continue;
            char disp[13];
            f32_name_from_83(dir[e].name, disp);
            if (emit(disp, dir[e].fsize, dir[e].attr, user_data) != 0) goto done;
        }
        u32 fo = fat_byte_off + clus * 4;
        if (fo + 4 > sizeof(f32_disk)) break;
        clus = f32_r32(f32_disk + fo) & 0x0FFFFFFF;
    }
done:
    return 0;
}

/* ---- 子目录遍历支持（Phase 2.2 fileman 扩展） ---- */

/* 目录条目（供 fileman 等工具使用） */
typedef struct {
    char name[13];  /* 8.3 显示名 */
    u32  clus;      /* 首簇号 */
    u32  size;      /* 文件大小（目录为 0） */
    u8   attr;
    u8   is_dir;
} f32_entry;

typedef int (*f32_list_entry_cb)(const f32_entry *e, void *user_data);

/* 列举指定目录簇的内容。
 * dir_clus = 0 表示根目录（自动从 BPB 取 root_clus）。
 * 回调返回非 0 立即停止。返回 0 成功，负值失败。 */
static inline int f32_list_dir(u32 dir_clus, f32_list_entry_cb emit, void *user_data) {
    if (f32_disk_load() != 0) return -1;
    const f32_bpb *bpb = (const f32_bpb *)f32_disk;
    u32 data_lba = bpb->rsvd + (u32)bpb->fc * bpb->spf;
    u32 fat_byte_off = bpb->rsvd * 512;
    u32 spc = bpb->spc;
    u32 clus = (dir_clus == 0) ? bpb->root_clus : dir_clus;
    while (clus >= 2 && clus < 0x0FFFFFF8) {
        u32 lba = data_lba + (clus - 2) * spc;
        const u8 *cb;
        if ((u64)lba * 512 + (u64)spc * 512 <= 256ULL * 512)
            cb = f32_disk + (u64)lba * 512;
        else {
            if (f32_read_sectors(lba, 8, f32_cluster) != 0) return -4;
            cb = f32_cluster;
        }
        const f32_dirent *dir = (const f32_dirent *)cb;
        for (u32 e = 0; e * 32 < spc * 512; e++) {
            if (dir[e].name[0] == 0) goto done;
            if ((u8)dir[e].name[0] == 0xE5) continue;
            if (dir[e].attr == 0x0F) continue;
            if (dir[e].attr & 0x08) continue;
            /* 跳过 "." 和 ".." 条目 */
            if (dir[e].name[0] == '.' && (dir[e].name[1] == ' ' || dir[e].name[1] == '.'))
                continue;
            f32_entry fe;
            f32_name_from_83(dir[e].name, fe.name);
            fe.clus = (u32)f32_r16((const u8*)&dir[e].clow) | ((u32)f32_r16((const u8*)&dir[e].chigh) << 16);
            fe.size = dir[e].fsize;
            fe.attr = dir[e].attr;
            fe.is_dir = (dir[e].attr & 0x10) ? 1 : 0;
            if (emit(&fe, user_data) != 0) goto done;
        }
        u32 fo = fat_byte_off + clus * 4;
        if (fo + 4 > sizeof(f32_disk)) break;
        clus = f32_r32(f32_disk + fo) & 0x0FFFFFFF;
    }
done:
    return 0;
}

/* 按首簇号 + 大小读取文件内容到 out_buf。
 * out_buf 至少 size 字节容量。返回 0 成功，负值失败。 */
static inline int f32_read_file_by_clus(u32 clus, u32 size, u8 *out_buf, u32 buf_cap) {
    if (f32_disk_load() != 0) return -1;
    if (size > buf_cap) size = buf_cap;
    const f32_bpb *bpb = (const f32_bpb *)f32_disk;
    u32 data_lba = bpb->rsvd + (u32)bpb->fc * bpb->spf;
    u32 fat_byte_off = bpb->rsvd * 512;
    u32 spc = bpb->spc;
    u32 remaining = size;
    u8 *dst = out_buf;
    u32 fc = clus;
    while (fc >= 2 && fc < 0x0FFFFFF8 && remaining > 0) {
        u32 fc_lba = data_lba + (fc - 2) * spc;
        u32 fc_bytes = spc * 512;
        if (fc_bytes > remaining) fc_bytes = remaining;
        const u8 *fb;
        if ((u64)fc_lba * 512 + (u64)spc * 512 <= 256ULL * 512)
            fb = f32_disk + (u64)fc_lba * 512;
        else {
            if (f32_read_sectors(fc_lba, 8, f32_cluster) != 0) return -7;
            fb = f32_cluster;
        }
        for (u32 b = 0; b < fc_bytes; b++) dst[b] = fb[b];
        dst += fc_bytes; remaining -= fc_bytes;
        u32 fo = fat_byte_off + fc * 4;
        if (fo + 4 > sizeof(f32_disk)) break;
        fc = f32_r32(f32_disk + fo) & 0x0FFFFFFF;
    }
    return 0;
}

#endif /* DESHAB_FAT32_IO_H */
