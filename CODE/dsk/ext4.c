/* ext4.c — minimal full read-write ext4 driver for DSK (p2 data partition)
 * 见 ext4.h 头注释。严格错误策略：任何错误 fatal panic，零降级。 */

#include "ext4.h"

/* ---------------- IO 回调与日志 ---------------- */
static ext4_block_read_fn  rd_fn;
static ext4_block_write_fn wr_fn;
static void (*fatalf)(const char *sym);
static void (*logl_fn)(const char *s);
static void (*logh_fn)(const char *p, ext4_u64 v);

/* ---------------- 小端读取 ---------------- */
static ext4_u16 r16(const ext4_u8 *p) { return (ext4_u16)(p[0] | (p[1] << 8)); }
static ext4_u32 r32(const ext4_u8 *p) {
    return (ext4_u32)p[0] | ((ext4_u32)p[1] << 8) |
           ((ext4_u32)p[2] << 16) | ((ext4_u32)p[3] << 24);
}
static ext4_u64 r64(const ext4_u8 *p) { return (ext4_u64)r32(p) | ((ext4_u64)r32(p + 4) << 32); }
static void w32(ext4_u8 *p, ext4_u32 v) { p[0]=v&0xFF; p[1]=(v>>8)&0xFF; p[2]=(v>>16)&0xFF; p[3]=(v>>24)&0xFF; }
static void w16(ext4_u8 *p, ext4_u16 v) { p[0]=v&0xFF; p[1]=(v>>8)&0xFF; }

/* ---------------- 状态 ---------------- */
static ext4_u64 g_part_lba;        /* ext4 分区起始 LBA */
static ext4_u32 g_block_size;      /* 4096 */
static ext4_u32 g_sec_per_block;   /* 8 */
static ext4_u32 g_inodes_per_group;
static ext4_u32 g_blocks_per_group;
static ext4_u32 g_inode_size;
static ext4_u32 g_first_data_block;
static ext4_u32 g_desc_size;
static ext4_u32 g_group_count;
static ext4_u64 g_blocks_count;
static ext4_u8  g_sb[1024];        /* superblock（1024B） */
static ext4_u8  g_gdt[16384];      /* 组描述符表缓存（≤512 组 × 32B） */
#define EXT4_MAX_FILE (8u * 1024u * 1024u)
static ext4_u8  g_fdata[EXT4_MAX_FILE];   /* 读文件静态缓冲 */
static ext4_u8  g_blk[4096];       /* 元数据块 scratch（位图/目录/inode 表） */
static ext4_u8  g_blk2[4096];      /* 第二 scratch（inode 表与目录并行操作） */

#define EXT4_SUPER_MAGIC  0xEF53
#define EXT4_ROOT_INO     2

/* incompat feature 位 */
#define INCOMPAT_FILETYPE      0x0002u
#define INCOMPAT_RECOVER       0x0004u
#define INCOMPAT_META_BG       0x0010u
#define INCOMPAT_EXTENTS       0x0040u
#define INCOMPAT_64BIT         0x0080u
#define INCOMPAT_MMP           0x0100u
#define INCOMPAT_FLEX_BG       0x0200u
#define INCOMPAT_METADATA_CSUM 0x0400u
#define INCOMPAT_INLINE_DATA   0x8000u
#define INCOMPAT_SUPPORTED (INCOMPAT_FILETYPE | INCOMPAT_EXTENTS | INCOMPAT_FLEX_BG)

/* inode flags */
#define EXT4_NODUMP_FL 0x00000004u
#define EXT4_EXTENTS_FL 0x00080000u

/* ---------------- 底层 IO（失败即 fatal） ---------------- */
static void rd_sec(ext4_u64 lba, ext4_u32 cnt, void *buf) {
    if (rd_fn(0, lba, cnt, buf) != 0) fatalf("FS-E20 EXT4 BLOCK READ FAILED");
}
static void wr_sec(ext4_u64 lba, ext4_u32 cnt, const void *buf) {
    if (wr_fn(0, lba, cnt, buf) != 0) fatalf("FS-E21 EXT4 BLOCK WRITE FAILED");
}
/* 块号 → LBA */
static ext4_u64 blk_lba(ext4_u32 blk) { return g_part_lba + (ext4_u64)blk * g_sec_per_block; }
static void rd_blk(ext4_u32 blk, void *buf)  { rd_sec(blk_lba(blk), g_sec_per_block, buf); }
static void wr_blk(ext4_u32 blk, const void *buf) { wr_sec(blk_lba(blk), g_sec_per_block, buf); }

/* ---------------- GPT 探测 ext4 分区 ---------------- */
static void gpt_find_ext4(void) {
    ext4_u8 gpt[512];
    rd_sec(1, 1, gpt);
    if (gpt[0] != 'E' || gpt[1] != 'F' || gpt[2] != 'I' || gpt[3] != ' ')
        fatalf("FS-E22 GPT SIGNATURE NOT FOUND");
    ext4_u64 ent_lba = r64(gpt + 0x48);
    ext4_u32 ent_num = r32(gpt + 0x50);
    ext4_u32 ent_sz  = r32(gpt + 0x54);
    if (ent_sz < 128 || ent_num == 0 || ent_num > 128)
        fatalf("FS-E23 GPT ENTRY TABLE INVALID");
    static ext4_u8 ents[128 * 128];
    rd_sec(ent_lba, (ent_num * ent_sz + 511) / 512, ents);
    for (ext4_u32 i = 0; i < ent_num; i++) {
        const ext4_u8 *e = ents + i * ent_sz;
        static const ext4_u8 linux_guid[16] = {
            0xAF,0x3D,0xC6,0x0F, 0x83,0x84, 0x72,0x47,
            0x8E,0x79, 0x3D,0x69,0xD8,0x47,0x7D,0xE4 };
        int is_linux = 1;
        for (int j = 0; j < 16; j++) if (e[j] != linux_guid[j]) { is_linux = 0; break; }
        if (!is_linux) continue;
        ext4_u64 start = r64(e + 0x20);
        if (start == 0) continue;
        /* 探测 superblock 魔数（offset 1024 处 0xEF53） */
        ext4_u8 sb_probe[1024];
        rd_sec(start + 2, 2, sb_probe);
        if (r16(sb_probe + 0x38) == EXT4_SUPER_MAGIC) {
            g_part_lba = start;
            for (int j = 0; j < 1024; j++) g_sb[j] = sb_probe[j];
            return;
        }
    }
    fatalf("FS-E24 EXT4 PARTITION NOT FOUND IN GPT");
}

/* ---------------- 挂载 ---------------- */
int ext4_mount(ext4_block_read_fn rd, ext4_block_write_fn wr,
               void (*fatal)(const char *sym), void (*logl)(const char *s),
               void (*logh)(const char *p, ext4_u64 v)) {
    rd_fn = rd; wr_fn = wr; fatalf = fatal; logl_fn = logl; logh_fn = logh;
    gpt_find_ext4();
    logh_fn("[EXT4] partition LBA=", g_part_lba);
    if (r16(g_sb + 0x38) != EXT4_SUPER_MAGIC) fatalf("FS-E25 SUPERBLOCK MAGIC MISMATCH");

    ext4_u32 log_bs = r32(g_sb + 0x18);
    if (log_bs != 2) fatalf("FS-E26 UNSUPPORTED BLOCKSIZE (need 4096)");
    g_block_size = 4096; g_sec_per_block = 8;

    g_first_data_block   = r32(g_sb + 0x14);
    g_blocks_per_group   = r32(g_sb + 0x20);
    g_inodes_per_group   = r32(g_sb + 0x28);
    g_inode_size         = r16(g_sb + 0x58);
    g_desc_size          = r16(g_sb + 0xFE);   /* s_desc_size @ 0xFE（0xFA 是 hash_seed） */
    if (g_desc_size < 32) g_desc_size = 32;
    g_blocks_count       = r32(g_sb + 0x04);

    ext4_u32 incompat = r32(g_sb + 0x60);
    if (incompat & INCOMPAT_RECOVER)        fatalf("FS-E27 JOURNAL DIRTY (RECOVER SET)");
    if (incompat & (INCOMPAT_64BIT | INCOMPAT_METADATA_CSUM | INCOMPAT_MMP |
                    INCOMPAT_INLINE_DATA | INCOMPAT_META_BG))
        fatalf("FS-E28 UNSUPPORTED EXT4 FEATURE");
    if (incompat & ~INCOMPAT_SUPPORTED)     fatalf("FS-E28 UNSUPPORTED EXT4 FEATURE");

    ext4_u16 state = r16(g_sb + 0x3A);
    if (state != 1) fatalf("FS-E29 FILESYSTEM NOT CLEAN");

    if (g_inode_size != 256) fatalf("FS-E2A UNSUPPORTED INODE SIZE");
    if (g_first_data_block != 0) fatalf("FS-E2B UNEXPECTED FIRST DATA BLOCK");
    if (g_blocks_per_group != 32768) fatalf("FS-E2C UNEXPECTED BLOCKS PER GROUP");

    g_group_count = (ext4_u32)((g_blocks_count - g_first_data_block + g_blocks_per_group - 1) / g_blocks_per_group);
    if (g_group_count == 0) g_group_count = 1;
    if ((ext4_u64)g_group_count * g_desc_size > sizeof(g_gdt))
        fatalf("FS-E2D GROUP DESCRIPTOR TABLE OVERFLOW");
    /* 组描述符表位于 block 1（first_data_block==0 时） */
    rd_blk(1, g_gdt);
    logh_fn("[EXT4] groups=", g_group_count);

    /* mount 即写 sb（mnt_count++），验证写通道，失败即 panic */
    ext4_u16 mnt = r16(g_sb + 0x34);
    w16(g_sb + 0x34, (ext4_u16)(mnt + 1));
    {
        ext4_u8 sbw[1024];
        for (int i = 0; i < 1024; i++) sbw[i] = g_sb[i];
        wr_sec(g_part_lba + 2, 2, sbw);
        ext4_u8 sbr[1024];
        rd_sec(g_part_lba + 2, 2, sbr);
        if (r16(sbr + 0x34) != (ext4_u16)(mnt + 1)) fatalf("FS-E2E SB WRITE VERIFY FAILED");
    }
    logl_fn("[EXT4] mounted");
    return 0;
}

/* ---------------- 组描述符访问 ---------------- */
/* 32B 组描述符：0x00 block_bitmap, 0x04 inode_bitmap, 0x08 inode_table,
 * 0x0C free_blocks_lo(u16), 0x0E free_inodes_lo(u16), 0x10 used_dirs(u16) */
static ext4_u32 gd_block_bitmap(ext4_u32 g)  { return r32(g_gdt + (ext4_u64)g * g_desc_size + 0x00); }
static ext4_u32 gd_inode_bitmap(ext4_u32 g)  { return r32(g_gdt + (ext4_u64)g * g_desc_size + 0x04); }
static ext4_u32 gd_inode_table(ext4_u32 g)   { return r32(g_gdt + (ext4_u64)g * g_desc_size + 0x08); }
static ext4_u16 gd_free_blocks(ext4_u32 g)   { return r16(g_gdt + (ext4_u64)g * g_desc_size + 0x0C); }
static ext4_u16 gd_free_inodes(ext4_u32 g)   { return r16(g_gdt + (ext4_u64)g * g_desc_size + 0x0E); }

static void gd_set_free_blocks(ext4_u32 g, ext4_u16 v) { w16(g_gdt + (ext4_u64)g * g_desc_size + 0x0C, v); }
static void gd_set_free_inodes(ext4_u32 g, ext4_u16 v) { w16(g_gdt + (ext4_u64)g * g_desc_size + 0x0E, v); }

static void gdt_flush(void) { wr_blk(1, g_gdt); }

/* ---------------- inode 读写 ---------------- */
/* inode 结构关键偏移（256B）：
 * 0x00 mode 0x02 uid 0x04 size_lo 0x1A links 0x1C blocks_lo(512B)
 * 0x20 flags 0x28 i_block[15] 0x6C size_high */
static void read_inode(ext4_u32 ino, ext4_u8 *dst) {
    if (ino < 2) fatalf("FS-E2F BAD INODE NUMBER");
    ext4_u32 g = (ino - 1) / g_inodes_per_group;
    ext4_u32 idx = (ino - 1) % g_inodes_per_group;
    ext4_u32 table = gd_inode_table(g);
    ext4_u32 per_block = g_block_size / g_inode_size;   /* 16 */
    rd_blk(table + idx / per_block, g_blk);
    for (ext4_u32 i = 0; i < g_inode_size; i++)
        dst[i] = g_blk[(ext4_u64)(idx % per_block) * g_inode_size + i];
}
static void write_inode(ext4_u32 ino, const ext4_u8 *src) {
    if (ino < 2) fatalf("FS-E2F BAD INODE NUMBER");
    ext4_u32 g = (ino - 1) / g_inodes_per_group;
    ext4_u32 idx = (ino - 1) % g_inodes_per_group;
    ext4_u32 table = gd_inode_table(g);
    ext4_u32 per_block = g_block_size / g_inode_size;
    rd_blk(table + idx / per_block, g_blk);
    for (ext4_u32 i = 0; i < g_inode_size; i++)
        g_blk[(ext4_u64)(idx % per_block) * g_inode_size + i] = src[i];
    wr_blk(table + idx / per_block, g_blk);
}

/* ---------------- 块分配/回收 ---------------- */
static void sb_dec_free_blocks(void) {
    ext4_u32 v = r32(g_sb + 0x0C);
    if (v == 0) fatalf("FS-E30 FREE BLOCK COUNT UNDERFLOW");
    w32(g_sb + 0x0C, v - 1);
    /* sb 稍后统一 flush（写文件末尾），避免多次 1KB IO */
}
static void sb_inc_free_blocks(void) {
    w32(g_sb + 0x0C, r32(g_sb + 0x0C) + 1);
}
static void sb_flush(void) {
    ext4_u8 sbw[1024];
    for (int i = 0; i < 1024; i++) sbw[i] = g_sb[i];
    wr_sec(g_part_lba + 2, 2, sbw);
}

/* 分配 1 个数据块，返回物理块号。立即落盘位图与组描述符。 */
static ext4_u32 block_alloc(void) {
    for (ext4_u32 g = 0; g < g_group_count; g++) {
        if (gd_free_blocks(g) == 0) continue;
        rd_blk(gd_block_bitmap(g), g_blk);
        for (ext4_u32 byte = 0; byte < g_block_size; byte++) {
            if (g_blk[byte] == 0xFF) continue;
            for (int bit = 0; bit < 8; bit++) {
                if (g_blk[byte] & (1u << bit)) continue;
                ext4_u64 blk = (ext4_u64)g * g_blocks_per_group + (ext4_u64)byte * 8 + bit;
                if (blk >= g_blocks_count) fatalf("FS-E31 BLOCK ALLOC OUT OF RANGE");
                g_blk[byte] |= (ext4_u8)(1u << bit);
                wr_blk(gd_block_bitmap(g), g_blk);
                gd_set_free_blocks(g, (ext4_u16)(gd_free_blocks(g) - 1));
                gdt_flush();
                sb_dec_free_blocks();
                /* 新块需清零（目录块/文件尾块） */
                for (ext4_u32 i = 0; i < g_block_size; i++) g_blk2[i] = 0;
                wr_blk((ext4_u32)blk, g_blk2);
                return (ext4_u32)blk;
            }
        }
    }
    fatalf("FS-E32 NO FREE BLOCKS");
    return 0;
}

/* 回收 1 个块 */
static void block_free(ext4_u32 blk) {
    ext4_u32 g = (ext4_u32)(blk / g_blocks_per_group);
    ext4_u32 off = (ext4_u32)(blk % g_blocks_per_group);
    rd_blk(gd_block_bitmap(g), g_blk);
    if (!(g_blk[off / 8] & (1u << (off % 8)))) fatalf("FS-E33 DOUBLE FREE DETECTED");
    g_blk[off / 8] &= (ext4_u8)~(1u << (off % 8));
    wr_blk(gd_block_bitmap(g), g_blk);
    gd_set_free_blocks(g, (ext4_u16)(gd_free_blocks(g) + 1));
    gdt_flush();
    sb_inc_free_blocks();
}

/* ---------------- extent 树 ---------------- */
/* 根节点位于 inode i_block（0x28，60B）：
 * header 12B: magic(2) entries(2) max(2) depth(2) gen(4)
 * depth 0: leaf 项 12B: block(4) len(2) starthi(2) startlo(4)
 * depth 1: index 项 12B: block(4) leaf_lo(4) leaf_hi(2) unused(2) */
#define EXTENT_MAGIC 0xF30A

/* 遍历叶项回调：map(file_block → phys_block)。depth 0/1 支持。 */
static int extent_walk(const ext4_u8 *ino,
                       void (*cb)(void *ud, ext4_u64 file_blk, ext4_u64 phys_blk, ext4_u32 len),
                       void *ud) {
    ext4_u8 root[60];
    for (int i = 0; i < 60; i++) root[i] = ino[0x28 + i];
    if (r16(root + 0) != EXTENT_MAGIC) fatalf("FS-E34 EXTENT MAGIC INVALID");
    ext4_u16 depth = r16(root + 6);
    if (depth == 0) {
        ext4_u16 n = r16(root + 2);
        const ext4_u8 *e = root + 12;
        for (ext4_u16 i = 0; i < n; i++, e += 12) {
            ext4_u64 fb = r32(e);
            ext4_u32 len = r16(e + 4);
            if (len == 0) continue;
            ext4_u64 phys = r32(e + 8) | ((ext4_u64)r16(e + 6) << 32);
            cb(ud, fb, phys, len);
        }
        return 0;
    }
    if (depth != 1) fatalf("FS-E35 EXTENT DEPTH > 1 UNSUPPORTED");
    ext4_u16 n = r16(root + 2);
    const ext4_u8 *e = root + 12;
    for (ext4_u16 i = 0; i < n; i++, e += 12) {
        ext4_u64 leaf = r32(e + 4) | ((ext4_u64)r16(e + 8) << 32);
        rd_blk((ext4_u32)leaf, g_blk);
        if (r16(g_blk + 0) != EXTENT_MAGIC) fatalf("FS-E34 EXTENT MAGIC INVALID");
        ext4_u16 ln = r16(g_blk + 2);
        if (r16(g_blk + 6) != 0) fatalf("FS-E35 EXTENT DEPTH > 1 UNSUPPORTED");
        const ext4_u8 *le = g_blk + 12;
        for (ext4_u16 j = 0; j < ln; j++, le += 12) {
            ext4_u64 fb = r32(le);
            ext4_u32 len = r16(le + 4);
            if (len == 0) continue;
            ext4_u64 phys = r32(le + 8) | ((ext4_u64)r16(le + 6) << 32);
            cb(ud, fb, phys, len);
        }
    }
    return 0;
}

/* 回调载体：读文件 */
typedef struct { ext4_u8 *dst; ext4_u64 dst_cap; } read_ctx;
/* 优化：整段连续读（尾块超界时经 g_blk2 部分拷贝，杜绝缓冲区溢出） */
static void cb_read_seq(void *ud, ext4_u64 fb, ext4_u64 phys, ext4_u32 len) {
    read_ctx *c = (read_ctx *)ud;
    ext4_u64 start = fb * g_block_size;
    ext4_u64 bytes = (ext4_u64)len * g_block_size;
    if (start >= c->dst_cap) return;
    if (start + bytes <= c->dst_cap) {
        rd_sec(blk_lba((ext4_u32)phys), (ext4_u32)(len * g_sec_per_block),
               c->dst + start);
    } else {
        for (ext4_u32 i = 0; i < len; i++) {
            ext4_u64 off = start + (ext4_u64)i * g_block_size;
            if (off >= c->dst_cap) return;
            rd_blk((ext4_u32)(phys + i), g_blk2);
            ext4_u64 rem = c->dst_cap - off;
            if (rem > g_block_size) rem = g_block_size;
            for (ext4_u64 b = 0; b < rem; b++) c->dst[off + b] = g_blk2[b];
        }
    }
}
/* 回调载体：释放全部块 */
static void cb_free(void *ud, ext4_u64 fb, ext4_u64 phys, ext4_u32 len) {
    (void)ud; (void)fb;
    for (ext4_u32 i = 0; i < len; i++) block_free((ext4_u32)phys);
}

/* ---------------- inode 分配 ---------------- */
static ext4_u32 inode_alloc(void) {
    for (ext4_u32 g = 0; g < g_group_count; g++) {
        if (gd_free_inodes(g) == 0) continue;
        rd_blk(gd_inode_bitmap(g), g_blk);
        for (ext4_u32 byte = 0; byte < g_block_size; byte++) {
            if (g_blk[byte] == 0xFF) continue;
            for (int bit = 0; bit < 8; bit++) {
                if (g_blk[byte] & (1u << bit)) continue;
                ext4_u32 ino = g * g_inodes_per_group + (ext4_u32)byte * 8 + (ext4_u32)bit + 1;
                if (ino < 11) continue;   /* 保留 inode 1..10 */
                g_blk[byte] |= (ext4_u8)(1u << bit);
                wr_blk(gd_inode_bitmap(g), g_blk);
                gd_set_free_inodes(g, (ext4_u16)(gd_free_inodes(g) - 1));
                gdt_flush();
                w32(g_sb + 0x10, r32(g_sb + 0x10) - 1);   /* free_inodes_count */
                return ino;
            }
        }
    }
    fatalf("FS-E37 NO FREE INODES");
    return 0;
}

/* ---------------- 路径解析 ---------------- */
static ext4_u32 streq(const char *a, const char *b, ext4_u32 n) {
    for (ext4_u32 i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}

/* 目录块内查找 name。返回 inode nr（0=未找到）。
 * entry: inode(4) rec_len(2) name_len(1) ftype(1) name */
static ext4_u32 dir_block_find(const ext4_u8 *blk, const char *name, ext4_u32 name_len) {
    ext4_u32 off = 0;
    while (off + 8 <= g_block_size) {
        ext4_u32 ino = r32(blk + off);
        ext4_u16 rec = r16(blk + off + 4);
        if (rec < 8 || (off + rec) > g_block_size) fatalf("FS-E38 DIR ENTRY CORRUPT");
        if (ino != 0) {
            ext4_u8 nl = blk[off + 6];
            if (nl == name_len && streq((const char *)(blk + off + 8), name, name_len))
                return ino;
        }
        off += rec;
    }
    return 0;
}

/* 读 inode 的第 bi 个目录块（bi < ceil(size/blocksize)） */
static void dir_read_block(const ext4_u8 *ino, ext4_u32 bi, ext4_u8 *out) {
    ext4_u8 root[60];
    for (int i = 0; i < 60; i++) root[i] = ino[0x28 + i];
    if (r16(root + 0) != EXTENT_MAGIC) fatalf("FS-E34 EXTENT MAGIC INVALID");
    ext4_u16 depth = r16(root + 6);
    if (depth != 0) fatalf("FS-E39 DIR TREE DEPTH UNSUPPORTED");
    ext4_u16 n = r16(root + 2);
    const ext4_u8 *e = root + 12;
    for (ext4_u16 i = 0; i < n; i++, e += 12) {
        ext4_u64 fb = r32(e);
        ext4_u32 len = r16(e + 4);
        ext4_u64 phys = r32(e + 8) | ((ext4_u64)r16(e + 6) << 32);
        if (bi >= fb && bi < fb + len) {
            rd_blk((ext4_u32)(phys + (bi - fb)), out);
            return;
        }
    }
    fatalf("FS-E3A DIR BLOCK NOT MAPPED");
}

/* 在 dir inode 下查找子项，返回 inode nr（0=不存在） */
static ext4_u32 dir_lookup(const ext4_u8 *dir_ino, const char *name, ext4_u32 name_len) {
    ext4_u64 dsize = r32(dir_ino + 0x04);
    ext4_u32 nblk = (ext4_u32)((dsize + g_block_size - 1) / g_block_size);
    for (ext4_u32 bi = 0; bi < nblk; bi++) {
        dir_read_block(dir_ino, bi, g_blk);
        ext4_u32 hit = dir_block_find(g_blk, name, name_len);
        if (hit) return hit;
    }
    return 0;
}

/* 按路径逐级走到父目录 + 最终名。 */
static void resolve_parent(const char *path, ext4_u32 *parent_ino,
                           const char **name, ext4_u32 *name_len) {
    if (path[0] != '/') fatalf("FS-E3B PATH MUST BE ABSOLUTE");
    ext4_u32 cur = EXT4_ROOT_INO;
    const char *p = path + 1;
    /* 找最终段 */
    const char *last = p;
    for (const char *q = p; *q; q++) if (*q == '/') last = q + 1;
    ext4_u32 last_len = 0;
    while (last[last_len]) last_len++;
    if (last_len == 0 || last_len > 255) fatalf("FS-E3C BAD FINAL NAME");
    /* 逐级遍历前面的段 */
    while (p < last) {
        ext4_u32 seg = 0;
        while (p[seg] && p[seg] != '/') seg++;
        if (seg == 0) { p++; continue; }
        ext4_u8 ino_buf[256];
        read_inode(cur, ino_buf);
        ext4_u32 next = dir_lookup(ino_buf, p, seg);
        if (!next) fatalf("FS-E3D PATH COMPONENT NOT FOUND");
        cur = next;
        p += seg + 1;
    }
    *parent_ino = cur;
    *name = last;
    *name_len = last_len;
}

/* ---------------- 读文件 ---------------- */
int ext4_read_file(const char *path, ext4_u8 **out, ext4_u32 *size) {
    ext4_u32 parent; const char *name; ext4_u32 name_len;
    resolve_parent(path, &parent, &name, &name_len);
    ext4_u8 dir_ino[256];
    read_inode(parent, dir_ino);
    ext4_u32 target = dir_lookup(dir_ino, name, name_len);
    if (!target) fatalf("FS-E3E FILE NOT FOUND");
    ext4_u8 ino[256];
    read_inode(target, ino);
    ext4_u16 mode = r16(ino + 0x00);
    if ((mode & 0xF000) != 0x8000) fatalf("FS-E3F NOT A REGULAR FILE");
    ext4_u64 fsize = r32(ino + 0x04) | ((ext4_u64)r32(ino + 0x6C) << 32);
    if (fsize > EXT4_MAX_FILE) fatalf("FS-E36 FILE EXCEEDS READ BUFFER");
    read_ctx rc; rc.dst = g_fdata; rc.dst_cap = fsize;
    extent_walk(ino, cb_read_seq, &rc);
    *out = g_fdata;
    *size = (ext4_u32)fsize;
    return 0;
}

/* ---------------- 目录项插入 ---------------- */
/* 在 dir inode 的现有块中找空隙插入。返回 0 成功；1 无空隙（需扩块）。 */
static int dir_insert_existing(ext4_u8 *dir_ino, const char *name, ext4_u32 name_len, ext4_u32 target) {
    ext4_u64 dsize = r32(dir_ino + 0x04);
    ext4_u32 nblk = (ext4_u32)((dsize + g_block_size - 1) / g_block_size);
    ext4_u32 need = (8 + name_len + 3) & ~3u;
    for (ext4_u32 bi = 0; bi < nblk; bi++) {
        dir_read_block(dir_ino, bi, g_blk);
        ext4_u32 off = 0;
        while (off + 8 <= g_block_size) {
            ext4_u32 ino = r32(g_blk + off);
            ext4_u16 rec = r16(g_blk + off + 4);
            if (rec < 8 || (off + rec) > g_block_size) fatalf("FS-E38 DIR ENTRY CORRUPT");
            if (ino == 0 && rec >= need) {
                /* 空洞直接占用 */
                w32(g_blk + off, target);
                g_blk[off + 6] = (ext4_u8)name_len;
                g_blk[off + 7] = 1;   /* REGULAR FILE */
                for (ext4_u32 i = 0; i < name_len; i++) g_blk[off + 8 + i] = (ext4_u8)name[i];
                wr_blk_dir:;
                ext4_u8 root[60];
                for (int i = 0; i < 60; i++) root[i] = dir_ino[0x28 + i];
                /* 找 bi 对应物理块写回 */
                const ext4_u8 *e = root + 12;
                ext4_u16 n = r16(root + 2);
                for (ext4_u16 i = 0; i < n; i++, e += 12) {
                    ext4_u64 fb = r32(e);
                    ext4_u32 len = r16(e + 4);
                    ext4_u64 phys = r32(e + 8) | ((ext4_u64)r16(e + 6) << 32);
                    if (bi >= fb && bi < fb + len) { wr_blk((ext4_u32)(phys + (bi - fb)), g_blk); return 0; }
                }
                fatalf("FS-E3A DIR BLOCK NOT MAPPED");
            }
            if (off + rec == g_block_size) {
                /* 尾项收缩插入 */
                ext4_u8 nl = g_blk[off + 6];
                ext4_u32 min_rec = (8 + nl + 3) & ~3u;
                if (rec - min_rec >= need) {
                    ext4_u32 new_off = off + min_rec;
                    w16(g_blk + off + 4, (ext4_u16)min_rec);   /* rec_len 在 +4 */
                    w32(g_blk + new_off, target);
                    w16(g_blk + new_off + 4, (ext4_u16)(rec - min_rec));
                    g_blk[new_off + 6] = (ext4_u8)name_len;
                    g_blk[new_off + 7] = 1;
                    for (ext4_u32 i = 0; i < name_len; i++) g_blk[new_off + 8 + i] = (ext4_u8)name[i];
                    goto wr_blk_dir;
                }
                break;
            }
            off += rec;
        }
    }
    return 1;
}

/* 追加一个新目录块并插入条目 */
static void dir_insert_new_block(ext4_u8 *dir_ino, const char *name, ext4_u32 name_len, ext4_u32 target) {
    /* 扩展 extent：depth 0 且 <4 项时直接追加；否则 fatal（目录应远小于 4 块） */
    ext4_u8 root[60];
    for (int i = 0; i < 60; i++) root[i] = dir_ino[0x28 + i];
    if (r16(root + 0) != EXTENT_MAGIC) fatalf("FS-E34 EXTENT MAGIC INVALID");
    if (r16(root + 6) != 0) fatalf("FS-E39 DIR TREE DEPTH UNSUPPORTED");
    ext4_u16 n = r16(root + 2);
    ext4_u16 max = r16(root + 4);
    if (n >= max || n >= 4) fatalf("FS-E40 DIR EXTENT TABLE FULL");
    ext4_u32 phys = block_alloc();
    ext4_u64 dsize = r32(dir_ino + 0x04);
    /* 追加 extent 项：file block = dsize/4096，len 1 */
    w32(root + 12 + (ext4_u64)n * 12, (ext4_u32)(dsize / g_block_size));
    w16(root + 12 + (ext4_u64)n * 12 + 4, 1);
    w16(root + 12 + (ext4_u64)n * 12 + 6, 0);
    w32(root + 12 + (ext4_u64)n * 12 + 8, phys);
    w16(root + 12 + (ext4_u64)n * 12 + 10, 0);
    w16(root + 2, (ext4_u16)(n + 1));
    for (int i = 0; i < 60; i++) dir_ino[0x28 + i] = root[i];
    /* 更新 dir 尺寸与块计数 */
    dsize += g_block_size;
    w32(dir_ino + 0x04, (ext4_u32)dsize);
    w32(dir_ino + 0x1C, r32(dir_ino + 0x1C) + g_sec_per_block);
    /* 构造新目录块 */
    for (ext4_u32 i = 0; i < g_block_size; i++) g_blk[i] = 0;
    w32(g_blk + 0, target);
    w16(g_blk + 4, (ext4_u16)g_block_size);
    g_blk[6] = (ext4_u8)name_len;
    g_blk[7] = 1;
    for (ext4_u32 i = 0; i < name_len; i++) g_blk[8 + i] = (ext4_u8)name[i];
    /* used_dirs 不变（非目录）。写 inode 由调用方统一完成 */
    wr_blk(phys, g_blk);
}

/* ---------------- 写文件（覆写/创建） ---------------- */
int ext4_write_file(const char *path, const ext4_u8 *data, ext4_u32 size) {
    ext4_u32 parent; const char *name; ext4_u32 name_len;
    resolve_parent(path, &parent, &name, &name_len);
    ext4_u8 dir_ino[256];
    read_inode(parent, dir_ino);
    ext4_u32 target = dir_lookup(dir_ino, name, name_len);
    int existed = (target != 0);

    ext4_u8 ino[256];
    if (target) {
        read_inode(target, ino);
        ext4_u16 mode = r16(ino + 0x00);
        if ((mode & 0xF000) != 0x8000) fatalf("FS-E3F NOT A REGULAR FILE");
        /* 释放旧 extent 叶块（depth 1） */
        {
            ext4_u8 root0[60];
            for (int i = 0; i < 60; i++) root0[i] = ino[0x28 + i];
            if (r16(root0 + 0) != EXTENT_MAGIC) fatalf("FS-E34 EXTENT MAGIC INVALID");
            if (r16(root0 + 6) == 1) {
                ext4_u16 nn = r16(root0 + 2);
                for (ext4_u16 i = 0; i < nn; i++) {
                    ext4_u64 leaf = r32(root0 + 12 + i * 12 + 4) |
                                    ((ext4_u64)r16(root0 + 12 + i * 12 + 8) << 32);
                    block_free((ext4_u32)leaf);
                }
            }
        }
        /* 释放旧数据块 */
        extent_walk(ino, cb_free, 0);
    } else {
        target = inode_alloc();
        for (ext4_u32 i = 0; i < g_inode_size; i++) ino[i] = 0;
        w16(ino + 0x00, 0x8000 | 0644);       /* regular, rw-r--r-- */
        w16(ino + 0x1A, 1);                    /* links */
        w32(ino + 0x20, EXT4_EXTENTS_FL);
        w32(ino + 0x64, 0xDE5AAB1E);           /* generation 品牌落款 */
    }

    /* 构造 extent 根（depth 0/1） */
    ext4_u32 nblk = (size + g_block_size - 1) / g_block_size;
    if ((ext4_u64)nblk * g_block_size > EXT4_MAX_FILE) fatalf("FS-E36 FILE EXCEEDS READ BUFFER");
    /* 收集分配的块号（最大 8MB/4K = 2048 块） */
    static ext4_u32 alloc_map[2048];
    for (ext4_u32 i = 0; i < nblk; i++) alloc_map[i] = block_alloc();
    /* 写数据块 */
    for (ext4_u32 i = 0; i < nblk; i++) {
        for (ext4_u32 b = 0; b < g_block_size; b++) {
            ext4_u64 off = (ext4_u64)i * g_block_size + b;
            g_blk[b] = (off < size) ? data[off] : 0;
        }
        wr_blk(alloc_map[i], g_blk);
    }
    /* 生成 extent 叶项（每 extent ≤ 32768 块，此处 ≤2048） */
    ext4_u8 root[60];
    for (int i = 0; i < 60; i++) root[i] = 0;
    w16(root + 0, EXTENT_MAGIC);
    w16(root + 4, 4);          /* max = 4（根内联） */
    w16(root + 6, 0);          /* depth 0 */
    ext4_u16 ne = 0;
    ext4_u32 i = 0;
    if (nblk > 0) {
        while (i < nblk) {
            if (ne >= 4) {
                /* >4 extents：需要 depth 1。把现有叶项压入一个叶块，继续分配。 */
                /* 分配叶块，把 4 项写入 */
                ext4_u32 leaf = block_alloc();
                ext4_u8 lfb[4096];
                for (ext4_u32 b = 0; b < g_block_size; b++) lfb[b] = 0;
                w16(lfb + 0, EXTENT_MAGIC);
                w16(lfb + 2, ne);
                w16(lfb + 4, (ext4_u16)((g_block_size - 12) / 12));
                w16(lfb + 6, 0);
                for (ext4_u16 j = 0; j < ne; j++) {
                    const ext4_u8 *src = root + 12 + j * 12;
                    ext4_u8 *dst = lfb + 12 + j * 12;
                    for (int k = 0; k < 12; k++) dst[k] = src[k];
                }
                wr_blk(leaf, lfb);
                /* root 变 depth1：保留 index[0] = 旧叶；继续向 alloc_map 后段收集 */
                /* 这里简化：一次性深度1重建（见下），直接跳出 */
                static ext4_u32 leaf_blocks[8];
                static ext4_u16 leaf_counts[8];
                int nleaf = 0;
                leaf_blocks[nleaf] = leaf; leaf_counts[nleaf] = ne; nleaf++;
                /* 继续把剩余块打成叶块，每块 ≤340 项 */
                ext4_u16 cur_cnt = ne;
                ext4_u32 cur_start = i;   /* 前 4 项 extent 已覆盖 file blocks [0, i) */
                (void)cur_cnt;
                while (cur_start < nblk) {
                    ext4_u32 cnt = nblk - cur_start;
                    if (cnt > (g_block_size - 12) / 12) cnt = (g_block_size - 12) / 12;
                    ext4_u32 lf = block_alloc();
                    ext4_u8 lb[4096];
                    for (ext4_u32 b = 0; b < g_block_size; b++) lb[b] = 0;
                    w16(lb + 0, EXTENT_MAGIC);
                    w16(lb + 2, (ext4_u16)cnt);
                    w16(lb + 4, (ext4_u16)((g_block_size - 12) / 12));
                    w16(lb + 6, 0);
                    for (ext4_u32 j = 0; j < cnt; j++) {
                        ext4_u8 *ee = lb + 12 + j * 12;
                        w32(ee, cur_start + j);            /* file block */
                        w16(ee + 4, 1);                    /* len 1 */
                        w16(ee + 6, 0);
                        w32(ee + 8, alloc_map[cur_start + j]);
                    }
                    wr_blk(lf, lb);
                    if (nleaf >= 8) fatalf("FS-E41 TOO MANY EXTENT LEAVES");
                    leaf_blocks[nleaf] = lf; leaf_counts[nleaf] = (ext4_u16)cnt; nleaf++;
                    cur_start += cnt;
                }
                /* 重建根为 depth 1 */
                for (int b = 0; b < 60; b++) root[b] = 0;
                w16(root + 0, EXTENT_MAGIC);
                w16(root + 2, (ext4_u16)nleaf);
                w16(root + 4, 4);
                w16(root + 6, 1);
                ext4_u64 fb_cur = 0;
                for (int l = 0; l < nleaf; l++) {
                    ext4_u8 *ie = root + 12 + l * 12;
                    w32(ie, (ext4_u32)fb_cur);
                    w32(ie + 4, leaf_blocks[l]);
                    w16(ie + 8, 0);
                    fb_cur += leaf_counts[l];
                }
                goto root_done;
            }
            /* 找连续 run */
            ext4_u32 run = 1;
            while (i + run < nblk && run < 32768 &&
                   alloc_map[i + run] == alloc_map[i] + run) run++;
            ext4_u8 *ee = root + 12 + ne * 12;
            w32(ee, i);
            w16(ee + 4, (ext4_u16)run);
            w16(ee + 6, 0);
            w32(ee + 8, alloc_map[i]);
            ne++;
            i += run;
        }
    }
    w16(root + 2, ne);
root_done:
    for (int b = 0; b < 60; b++) ino[0x28 + b] = root[b];
    w32(ino + 0x04, size);                       /* size_lo */
    w32(ino + 0x6C, 0);                          /* size_high */
    w32(ino + 0x1C, (ext4_u32)((ext4_u64)nblk * g_sec_per_block));  /* blocks(512B) */
    w32(ino + 0x10, 0xDE5A0000u);                /* mtime 品牌纪年 */
    write_inode(target, ino);

    /* 新文件：插入目录项（target 为新分配 inode 且原查找时不存在） */
    if (!existed) {
        if (dir_insert_existing(dir_ino, name, name_len, target) != 0)
            dir_insert_new_block(dir_ino, name, name_len, target);
        write_inode(parent, dir_ino);
    }

    sb_flush();
    logh_fn("[EXT4] wrote blocks=", nblk);

    /* 写后回读校验 */
    ext4_u8 *vdata; ext4_u32 vsize;
    ext4_read_file(path, &vdata, &vsize);
    if (vsize != size) fatalf("FS-E42 WRITE VERIFY SIZE MISMATCH");
    for (ext4_u32 b = 0; b < size; b++)
        if (vdata[b] != data[b]) fatalf("FS-E42 WRITE VERIFY DATA MISMATCH");
    return 0;
}

/* ---------------- 启动自检 ---------------- */
void ext4_boot_selftest(void) {
    /* 读测试：p2 上的 FUCK 配置副本 */
    ext4_u8 *d; ext4_u32 n;
    ext4_read_file("/system/deshab64/FUCK", &d, &n);
    if (n == 0) fatalf("FS-E43 SELFTEST FUCK READ EMPTY");
    logh_fn("[EXT4] selftest FUCK bytes=", n);
    /* 写读校验测试 */
    static const ext4_u8 msg[] = "DESHAB EXT4 RW OK - DEAICUP\n";
    ext4_write_file("/dsk_ext4_rw.test", msg, sizeof(msg) - 1);
    logl_fn("[EXT4] selftest PASS");
}
