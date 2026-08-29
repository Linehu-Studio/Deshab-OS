/* fat32_write.h - Deshab 共享 FAT32 写库（static inline，无库）
 *
 * 前置条件：包含者已定义 u8/u16/u32/u64/i64 基础类型。
 * 通过 f32w_init() 注入 block_read/block_write 函数指针（签名与 f32_/gpt_ 库一致）。
 *
 * 与 fat32_io.h 的分工：
 *   fat32_io.h  - 8.3 短名读写，依赖 256 扇区 BPB+FAT+根目录缓存窗口
 *   fat32_write.h - LFN 长名创建 / mkdir / 目录簇链扩展，FAT 按需单扇区访问
 *
 * 本库解决 fat32_io.h 的三个写缺口（大 ESP 场景，2GB: spc=8, FAT=4086 扇区,
 * 根目录在分区 LBA 8204，均远超 256 扇区窗口）：
 *   1. LFN 生成：VFAT LFN 链（每项 13 UTF-16 字符，逆序存储，0x40 标志 +
 *      短名 checksum），配套 ~N 数字尾巴的 8.3 短名生成与唯一化
 *   2. mkdir：分配目录簇 + 写入 "." / ".." 项 + 父目录插入 LFN 条目
 *   3. 目录簇链扩展：目录槽位不足时自动分配新簇并接入链尾（槽位可跨簇）
 *
 * FAT 访问策略：按需读改写单个 FAT 扇区（含缓存），修改后立即回写所有 FAT
 * 副本（BPB->fc 份）。不依赖任何批量缓存窗口，任意簇号/FAT 偏移均可访问。
 *
 * 用法：
 *   f32w_init(block_read, block_write)                - 初始化（注入块设备）
 *   f32w_write_file(dir_clus, "user.conf", d, n)      - 目录中写/替换长名文件
 *   f32w_mkdir(dir_clus, "MyDir", &out_clus)          - 创建子目录（LFN）
 *   f32w_resolve_dir("system/deshab64", &clus)        - 按路径定位目录簇
 *   f32w_write_path("system/deshab64/user.conf", d, n)- 按路径写文件（LFN）
 *   f32w_mkdir_path("system/新建目录", &clus)          - 按路径建目录
 *   f32w_find(dir_clus, name, want_dir, ...)          - 按长名查找（LFN 优先）
 *
 * 缓冲区：每包含者约 9KB BSS（512 BPB + 512 FAT 扇区 + 簇缓冲 ×2）。
 * 与 fat32_io.h 可同时包含（前缀 f32w_ / f32_ 不冲突），但二者状态独立。
 */

#ifndef DESHAB_FAT32_WRITE_H
#define DESHAB_FAT32_WRITE_H

#include "fat32_lfn.h"

/* ---- block 设备函数类型（与 fat32_io.h / gpt_write.h 一致：index 固定 0） ---- */
typedef int (*f32w_block_read_fn)(u32 index, u64 lba, u32 count, void *buf);
typedef int (*f32w_block_write_fn)(u32 index, u64 lba, u32 count, const void *buf);

static f32w_block_read_fn  f32w_blk_read  = 0;
static f32w_block_write_fn f32w_blk_write = 0;

/* ---- 目录簇缓冲大小（含 data 暂存缓冲）。build.ps1 mkfs spc=8 -> 4096。
 * 包含者可在 include 前 #define F32W_CLUS_BYTES 覆盖（须为 512 的倍数）。 */
#ifndef F32W_CLUS_BYTES
#define F32W_CLUS_BYTES 4096
#endif

#define F32W_EOC      0x0FFFFFFFu
#define F32W_EOC_VAL  0x0FFFFFFFu   /* 写入 FAT 的 EOC 值（高 4 位保留位置 0x0Fxxxxxx 已含） */

/* ---- BPB（512B，与 f32_bpb 布局一致；独立定义避免依赖 fat32_io.h） ---- */
typedef struct __attribute__((packed)) {
    u8 jmp[3]; char oem[8]; u16 bps; u8 spc; u16 rsvd; u8 fc; u16 root_ent;
    u16 ts16; u8 media; u16 spf16; u16 spt; u16 heads; u32 hidden; u32 ts32;
    u32 spf; u16 flags; u16 ver; u32 root_clus; u16 fsi; u16 bkboot;
    u8 res[12]; u8 drv; u8 ntfl; u8 sig; u32 ser; char lbl[11]; char typ[8];
    u8 code[420]; u16 boot_sig;
} f32w_bpb_t;

typedef struct __attribute__((packed)) {
    char name[11]; u8 attr; u8 ntr; u8 ctenth;
    u16 ctime; u16 cdate; u16 adate; u16 chigh;
    u16 wtime; u16 wdate; u16 clow; u32 fsize;
} f32w_dirent_t;

/* ---- 挂载上下文（f32w_bpb_load 填充） ---- */
typedef struct {
    u32 spc;          /* 每簇扇区数 */
    u32 clus_bytes;   /* spc * 512 */
    u32 fat_lba;      /* FAT 副本 0 起始 LBA（= rsvd） */
    u32 fat_copies;   /* FAT 副本数（BPB->fc，实际至少 2） */
    u32 fat_sectors;  /* 单份 FAT 扇区数（BPB->spf） */
    u32 data_lba;     /* 数据区起始 LBA */
    u32 root_clus;    /* 根目录首簇 */
    u32 fat_entries;  /* FAT 项数上界 = spf*512/4 */
    int loaded;
} f32w_ctx;

static f32w_ctx f32w_c;

/* ---- 内部缓冲（每包含者独立） ---- */
static u8  f32w_sec[512];            /* BPB / FSInfo 扇区缓冲 */
static u8  f32w_fat[512];            /* FAT 单扇区缓存 */
static u32 f32w_fat_sec_no = 0xFFFFFFFFu;  /* 缓存的 FAT 扇区号（FAT 内偏移） */
static u8  f32w_dir[F32W_CLUS_BYTES];      /* 目录簇缓存（槽位扫描/条目写入） */
static u32 f32w_dir_no = 0;          /* 缓存的目录簇号（0=无，簇号最小为 2） */
static int f32w_dir_dirty = 0;
static u8  f32w_stage[F32W_CLUS_BYTES];    /* 数据写入暂存（尾簇零填充） */
static u32 f32w_alloc_hint = 2;      /* 空闲簇分配起点提示 */

/* ---- 初始化：注入 block_read/block_write 函数指针 ---- */
static inline void f32w_init(f32w_block_read_fn rd, f32w_block_write_fn wr) {
    f32w_blk_read = rd;
    f32w_blk_write = wr;
    f32w_c.loaded = 0;
    f32w_fat_sec_no = 0xFFFFFFFFu;
    f32w_dir_no = 0;
    f32w_dir_dirty = 0;
    f32w_alloc_hint = 2;
}

static inline int f32w_read_sectors(u32 lba, u32 count, u8 *out) {
    return f32w_blk_read ? f32w_blk_read(0, lba, count, out) : -1;
}
static inline int f32w_write_sectors(u32 lba, u32 count, const u8 *buf) {
    return f32w_blk_write ? f32w_blk_write(0, lba, count, buf) : -1;
}

/* ---- 小工具（无库约束：手写循环替代 mem*） ---- */
static inline void f32w_mem_set(u8 *p, u8 v, u32 n) { for (u32 i = 0; i < n; i++) p[i] = v; }
static inline u32 f32w_r32(const u8 *p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}
static inline u16 f32w_r16(const u8 *p) { return (u16)p[0] | ((u16)p[1] << 8); }

/* ---- BPB 加载 / 挂载上下文 ---- */
static inline int f32w_bpb_load(void) {
    if (f32w_c.loaded) return 0;
    if (f32w_read_sectors(0, 1, f32w_sec) != 0) return -1;
    const f32w_bpb_t *b = (const f32w_bpb_t *)f32w_sec;
    if (b->boot_sig != 0xAA55 || b->bps != 512) return -2;
    if (b->spf == 0 || b->root_clus < 2 || b->spc == 0) return -3;
    u32 fc = b->fc ? b->fc : 1;
    u32 cb = (u32)b->spc * 512;
    if (cb > F32W_CLUS_BYTES) return -4;   /* 簇超出缓冲，包含者应调大 F32W_CLUS_BYTES */
    f32w_c.spc = b->spc;
    f32w_c.clus_bytes = cb;
    f32w_c.fat_lba = b->rsvd;
    f32w_c.fat_copies = fc;
    f32w_c.fat_sectors = b->spf;
    f32w_c.data_lba = b->rsvd + fc * b->spf;
    f32w_c.root_clus = b->root_clus;
    f32w_c.fat_entries = b->spf * 512 / 4;
    f32w_c.loaded = 1;
    f32w_fat_sec_no = 0xFFFFFFFFu;   /* 新挂载，作废 FAT 缓存 */
    return 0;
}

static inline u32 f32w_clus_lba(u32 clus) {
    return f32w_c.data_lba + (clus - 2) * f32w_c.spc;
}

/* ---- FAT 按需访问（单扇区缓存 + 全副本回写） ---- */

/* 读 FAT 项。返回 0 成功，*out 为 28 位簇值。 */
static inline int f32w_fat_get(u32 clus, u32 *out) {
    if (clus < 2 || clus >= f32w_c.fat_entries) return -1;
    u32 sec = clus >> 7;            /* 每扇区 128 项 */
    u32 off = (clus & 127) * 4;
    if (sec != f32w_fat_sec_no) {
        if (f32w_read_sectors(f32w_c.fat_lba + sec, 1, f32w_fat) != 0) return -2;
        f32w_fat_sec_no = sec;
    }
    *out = f32w_r32(f32w_fat + off) & 0x0FFFFFFF;
    return 0;
}

/* 写 FAT 项（含 28 位掩码），立即回写所有 FAT 副本对应扇区。 */
static inline int f32w_fat_set(u32 clus, u32 val) {
    if (clus < 2 || clus >= f32w_c.fat_entries) return -1;
    u32 sec = clus >> 7;
    u32 off = (clus & 127) * 4;
    if (sec != f32w_fat_sec_no) {
        if (f32w_read_sectors(f32w_c.fat_lba + sec, 1, f32w_fat) != 0) return -2;
        f32w_fat_sec_no = sec;
    }
    u32 old = f32w_r32(f32w_fat + off);
    val &= 0x0FFFFFFF;
    f32w_fat[off]     = (u8)(val & 0xFF);
    f32w_fat[off + 1] = (u8)((val >> 8) & 0xFF);
    f32w_fat[off + 2] = (u8)((val >> 16) & 0xFF);
    f32w_fat[off + 3] = (u8)((old & 0xF0) | ((val >> 24) & 0x0F));
    for (u32 f = 0; f < f32w_c.fat_copies; f++) {
        u32 lba = f32w_c.fat_lba + f * f32w_c.fat_sectors + sec;
        if (f32w_write_sectors(lba, 1, f32w_fat) != 0) return -3;
    }
    return 0;
}

/* 分配一个空闲簇（线性扫描 + 起点提示轮转）。返回簇号，0=已满/失败。 */
static inline u32 f32w_alloc_free(void) {
    u32 total = f32w_c.fat_entries > 2 ? f32w_c.fat_entries - 2 : 0;
    if (total == 0) return 0;
    u32 start = (f32w_alloc_hint >= 2 && f32w_alloc_hint < f32w_c.fat_entries)
                ? f32w_alloc_hint : 2;
    for (u32 i = 0; i < total; i++) {
        u32 c = 2 + ((start - 2 + i) % total);
        u32 v = 0;
        if (f32w_fat_get(c, &v) != 0) return 0;
        if (v == 0) {
            f32w_alloc_hint = c + 1;
            return c;
        }
    }
    return 0;
}

/* 释放整条簇链（逐项清 0）。带环保护。 */
static inline int f32w_free_chain(u32 clus) {
    u32 cur = clus;
    u64 guard = 0;
    while (cur >= 2 && cur < 0x0FFFFFF8) {
        if (++guard > (u64)f32w_c.fat_entries + 2) return -1;  /* FAT 成环，损坏 */
        u32 next = 0;
        if (f32w_fat_get(cur, &next) != 0) return -2;
        if (f32w_fat_set(cur, 0) != 0) return -3;
        cur = next;
    }
    return 0;
}

/* ---- 目录簇缓存（槽位扫描 / 条目写入共用） ---- */
static inline int f32w_dir_flush(void) {
    if (!f32w_dir_dirty || f32w_dir_no == 0) { f32w_dir_dirty = 0; return 0; }
    if (f32w_write_sectors(f32w_clus_lba(f32w_dir_no), f32w_c.spc, f32w_dir) != 0) return -1;
    f32w_dir_dirty = 0;
    return 0;
}

static inline int f32w_dir_load(u32 clus) {
    if (f32w_dir_no == clus && clus != 0) return 0;
    if (f32w_dir_flush() != 0) return -1;
    if (f32w_read_sectors(f32w_clus_lba(clus), f32w_c.spc, f32w_dir) != 0) return -2;
    f32w_dir_no = clus;
    return 0;
}

/* 在 (clus, idx) 写入 32 字节目录项。 */
static inline int f32w_entry_write(u32 clus, u32 idx, const u8 *e32) {
    if (idx * 32 + 32 > f32w_c.clus_bytes) return -1;
    if (f32w_dir_load(clus) != 0) return -2;
    for (int i = 0; i < 32; i++) f32w_dir[idx * 32 + i] = e32[i];
    f32w_dir_dirty = 1;
    return 0;
}

/* 槽位步进：同簇内 idx+1，越界则沿 FAT 链进入下一簇（run 保证连续）。 */
static inline int f32w_slot_step(u32 *clus, u32 *idx) {
    (*idx)++;
    if ((*idx) * 32 >= f32w_c.clus_bytes) {
        u32 next = 0;
        if (f32w_fat_get(*clus, &next) != 0) return -1;
        if (next < 2 || next >= 0x0FFFFFF8) return -1;
        *clus = next;
        *idx = 0;
    }
    return 0;
}

/* ---- 8.3 短名生成（含 ~N 唯一化） ---- */

static inline char f32w_up(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }

/* 合法 8.3 短名字符（已转大写后）：字母/数字/!#$%&'()-@^_`{}~ */
static inline int f32w_char_ok_83(char c) {
    if (c >= 'A' && c <= 'Z') return 1;
    if (c >= '0' && c <= '9') return 1;
    if (c == '!' || c == '#' || c == '$' || c == '%' || c == '&' || c == '\'') return 1;
    if (c == '(' || c == ')' || c == '-' || c == '@' || c == '^' || c == '_') return 1;
    if (c == '`' || c == '{' || c == '}' || c == '~') return 1;
    return 0;
}

/* 名字长度（手动 strlen）。 */
static inline u32 f32w_strlen(const char *s) {
    u32 n = 0;
    while (s[n]) n++;
    return n;
}

/* 生成长名对应的 8.3 短名（未唯一化）。
 *   name11   : 输出 11 字符短名（大写、空格填充；非法字符转 '_'，超长截断）
 *   needs_lfn: 输出 1=长名无法无损表示为 8.3（需写 LFN 链 + 后续 ~N 唯一化）
 * 返回 0 成功，-1 名字非法（空/过长/含非 ASCII/纯点）。 */
static inline int f32w_make_short(const char *name, char name11[11], int *needs_lfn) {
    u32 len = f32w_strlen(name);
    if (len == 0 || len > 255) return -1;
    u32 last_dot = 0xFFFFFFFFu;
    u32 dot_count = 0;
    int all_upper_valid = 1;
    for (u32 i = 0; i < len; i++) {
        char c = name[i];
        if ((u8)c < 0x20 || (u8)c >= 0x7F || c == '/') return -1;
        if (c == '.') { last_dot = i; dot_count++; }
        if (c >= 'a' && c <= 'z') all_upper_valid = 0;
        else if (!f32w_char_ok_83(f32w_up(c))) all_upper_valid = 0;
    }
    /* 纯点 / 尾点 / 首空格：非法（简化处理，与 Windows 行为近似） */
    int non_dot = 0;
    for (u32 i = 0; i < len; i++) if (name[i] != '.') { non_dot = 1; break; }
    if (!non_dot || name[len - 1] == '.' || name[0] == ' ') return -1;

    u32 base_end = (last_dot == 0xFFFFFFFFu) ? len : last_dot;
    u32 base_len = base_end;
    u32 ext_len = (last_dot == 0xFFFFFFFFu) ? 0 : len - last_dot - 1;

    *needs_lfn = !(all_upper_valid && base_len <= 8 && ext_len <= 3 && dot_count <= 1);

    for (int i = 0; i < 11; i++) name11[i] = ' ';
    u32 bi = 0;
    for (u32 i = 0; i < base_len && bi < 8; i++) {
        char c = f32w_up(name[i]);
        if (!f32w_char_ok_83(c)) c = '_';
        name11[bi++] = c;
    }
    if (bi == 0) name11[bi++] = '_';   /* 空基名（如 ".config"）用 '_' 兜底 */
    u32 ei = 0;
    if (last_dot != 0xFFFFFFFFu) {
        for (u32 i = 0; i < ext_len && ei < 3; i++) {
            char c = f32w_up(name[last_dot + 1 + i]);
            if (!f32w_char_ok_83(c)) c = '_';
            name11[8 + ei++] = c;
        }
    }
    return 0;
}

/* 目录中是否已存在指定 11 字符短名（仅比较短名项）。1=存在，0=不存在，-1=IO 错。 */
static inline int f32w_dir_short_exists(u32 dir_clus, const char name11[11]) {
    u32 dclus = dir_clus;
    u64 guard = 0;
    while (dclus >= 2 && dclus < 0x0FFFFFF8) {
        if (++guard > (u64)f32w_c.fat_entries + 2) return -1;
        if (f32w_dir_load(dclus) != 0) return -1;
        u32 max_e = f32w_c.clus_bytes / 32;
        for (u32 e = 0; e < max_e; e++) {
            const u8 *ent = f32w_dir + e * 32;
            if (ent[0] == 0x00) return 0;            /* 目录结尾 */
            if (ent[0] == 0xE5) continue;
            if (ent[11] == 0x0F) continue;           /* LFN 项 */
            if (ent[11] & 0x08) continue;            /* 卷标 */
            int same = 1;
            for (int i = 0; i < 11; i++)
                if (ent[i] != (u8)name11[i]) { same = 0; break; }
            if (same) return 1;
        }
        u32 next = 0;
        if (f32w_fat_get(dclus, &next) != 0) return -1;
        dclus = next;
    }
    return 0;
}

/* 生成长名对应的唯一 8.3 短名（lossy 时追加 ~1..~999999 数字尾巴）。
 * 返回 0 成功，负值失败。 */
static inline int f32w_make_unique_short(u32 dir_clus, const char *name, char name11[11]) {
    int needs_lfn = 0;
    if (f32w_make_short(name, name11, &needs_lfn) != 0) return -1;
    if (!needs_lfn) return 0;   /* 无损 8.3，同名冲突由 f32w_find 语义覆盖 */
    /* 提取 make_short 产生的 base / ext，枚举 BASE~N 直到唯一 */
    char base[8];
    u32 base_len = 0;
    for (u32 i = 0; i < 8 && name11[i] != ' '; i++) base[base_len++] = name11[i];
    if (base_len == 0) { base[0] = '_'; base_len = 1; }
    for (u32 n = 1; n <= 999999u; n++) {
        char digits[8];
        int dn = 0;
        u32 v = n;
        while (v) { digits[dn++] = (char)('0' + (v % 10)); v /= 10; }
        u32 bl = (dn + 1 >= 8) ? 1 : 8 - dn - 1;   /* 保留 '~'+数字 的空间 */
        if (base_len < bl) bl = base_len;
        for (u32 i = 0; i < 8; i++) name11[i] = ' ';
        for (u32 i = 0; i < bl; i++) name11[i] = base[i];
        u32 p = bl;
        name11[p++] = '~';
        for (int i = dn - 1; i >= 0; i--) name11[p++] = digits[i];
        int ex = f32w_dir_short_exists(dir_clus, name11);
        if (ex < 0) return -2;
        if (ex == 0) return 0;
    }
    return -3;   /* 序号耗尽 */
}

/* ---- LFN 条目生成 ---- */

/* 13 个 UTF-16 槽位在 32 字节目录项内的字节偏移（name1[5] name2[6] name3[2]） */
static inline int f32w_lfn_slot_off(int slot) {
    if (slot < 5) return 1 + slot * 2;
    if (slot < 11) return 14 + (slot - 5) * 2;
    return 28 + (slot - 11) * 2;
}

/* 构造第 ordinal 个 LFN 片段项（ordinal 1..total，total 为链长）。
 * name 为 ASCII 长名，len 为字符数。out32 接收 32 字节编码。
 * 最高序号项（ordinal==total）带 0x40 标志，含 0x0000 终止 + 0xFFFF 填充。
 * 返回 0 成功。 */
static inline int f32w_build_lfn_entry(int ordinal, int total, const char *name,
                                       u32 len, u8 cksum, u8 *out32) {
    f32w_mem_set(out32, 0, 32);
    /* 全槽位预填 0xFFFF */
    for (int s = 0; s < 13; s++) {
        int off = f32w_lfn_slot_off(s);
        out32[off] = 0xFF;
        out32[off + 1] = 0xFF;
    }
    u32 base = (u32)(ordinal - 1) * 13;
    u32 cnt = (len - base) < 13 ? (len - base) : 13;
    for (u32 j = 0; j < cnt; j++) {
        int off = f32w_lfn_slot_off((int)j);
        u16 c = (u16)(u8)name[base + j];   /* ASCII -> UTF-16 */
        out32[off] = (u8)(c & 0xFF);
        out32[off + 1] = (u8)(c >> 8);
    }
    if (ordinal == total && cnt < 13) {
        int off = f32w_lfn_slot_off((int)cnt);   /* 0x0000 终止符 */
        out32[off] = 0;
        out32[off + 1] = 0;
    }
    out32[0] = (u8)(ordinal | (ordinal == total ? 0x40 : 0x00));
    out32[11] = 0x0F;   /* ATTR_LONG_NAME */
    out32[12] = 0;      /* type */
    out32[13] = cksum;
    out32[26] = 0;      /* 首簇号必须为 0 */
    out32[27] = 0;
    return 0;
}

/* 构造 8.3 短名项。 */
static inline void f32w_build_short_entry(const char name11[11], u8 attr,
                                          u32 clus, u32 size, u8 *out32) {
    f32w_mem_set(out32, 0, 32);
    for (int i = 0; i < 11; i++) out32[i] = (u8)name11[i];
    out32[11] = attr;
    /* 时间戳字段保持 0（与 fat32_io.h 写路径一致） */
    out32[21] = (u8)((clus >> 16) & 0xFF);   /* chigh */
    out32[20] = (u8)((clus >> 24) & 0xFF);
    out32[26] = (u8)(clus & 0xFF);           /* clow */
    out32[27] = (u8)((clus >> 8) & 0xFF);
    out32[28] = (u8)(size & 0xFF);           /* fsize */
    out32[29] = (u8)((size >> 8) & 0xFF);
    out32[30] = (u8)((size >> 16) & 0xFF);
    out32[31] = (u8)((size >> 24) & 0xFF);
}

/* ---- 目录槽位分配（含簇链扩展，槽位可跨簇） ---- */

/* 在目录簇链中找 count 个连续空闲槽位（0x00/0xE5）。链尾不足时自动分配
 * 新簇：FAT 接链 + 新簇清零 + 落盘，槽位 run 跨簇继续累计。
 *   out_clus/out_idx : run 首槽位位置（簇号 + 簇内项索引）
 * 返回 0 成功，负值失败（-3=磁盘满）。 */
static inline int f32w_dir_alloc_slots(u32 dir_clus, u32 count,
                                       u32 *out_clus, u32 *out_idx) {
    u32 run_clus = 0, run_idx = 0, run = 0;
    u32 dclus = dir_clus;
    u64 guard = 0;
    while (dclus >= 2 && dclus < 0x0FFFFFF8) {
        if (++guard > (u64)f32w_c.fat_entries + 2) return -1;   /* FAT 成环 */
        if (f32w_dir_load(dclus) != 0) return -2;
        u32 max_e = f32w_c.clus_bytes / 32;
        for (u32 e = 0; e < max_e; e++) {
            u8 first = f32w_dir[e * 32];
            if (first == 0x00 || first == 0xE5) {
                if (run == 0) { run_clus = dclus; run_idx = e; }
                run++;
                if (run >= count) {
                    *out_clus = run_clus;
                    *out_idx = run_idx;
                    return 0;
                }
            } else {
                run = 0;
            }
        }
        u32 next = 0;
        if (f32w_fat_get(dclus, &next) != 0) return -2;
        if (next >= 2 && next < 0x0FFFFFF8) { dclus = next; continue; }
        /* 链尾且 run 不足：扩展一个新簇接入链尾 */
        u32 nc = f32w_alloc_free();
        if (nc == 0) return -3;
        if (f32w_fat_set(dclus, nc) != 0) return -4;
        if (f32w_fat_set(nc, F32W_EOC_VAL) != 0) return -4;
        if (f32w_dir_flush() != 0) return -5;
        f32w_mem_set(f32w_dir, 0, f32w_c.clus_bytes);   /* 新簇全 0 */
        f32w_dir_no = nc;
        f32w_dir_dirty = 1;   /* 脏标记使 flush 落盘清零内容 */
        if (f32w_dir_flush() != 0) return -5;
        dclus = nc;
        /* run 跨簇继续累计（新簇全 0x00，下一轮循环继续计数） */
    }
    return -6;
}

/* ---- 按长名查找（LFN 优先，8.3 回退；大小写不敏感） ---- */

/* 在目录中查找 name（长名或 8.3 显示名）。
 *   want_dir      : -1 任意，0 只匹配文件，1 只匹配目录
 *   out_clus/size : 目标首簇 / 大小（可 NULL）
 *   out_ent_clus/idx : 短名项位置（可 NULL，用于原地更新）
 *   out_attr      : 目录项属性（可 NULL）
 * 返回 0 找到，-1 未找到，其他负值 IO 错。LFN 缓冲跨簇存活（链可跨簇）。 */
static inline int f32w_find(u32 dir_clus, const char *name, int want_dir,
                            u32 *out_clus, u32 *out_size,
                            u32 *out_ent_clus, u32 *out_ent_idx, u8 *out_attr) {
    if (dir_clus == 0) dir_clus = f32w_c.root_clus;
    u32 dclus = dir_clus;
    u64 guard = 0;
    fat32_lfn_buf lfn;
    fat32_lfn_init(&lfn);
    while (dclus >= 2 && dclus < 0x0FFFFFF8) {
        if (++guard > (u64)f32w_c.fat_entries + 2) return -2;
        if (f32w_dir_load(dclus) != 0) return -3;
        u32 max_e = f32w_c.clus_bytes / 32;
        for (u32 e = 0; e < max_e; e++) {
            const u8 *ent = f32w_dir + e * 32;
            if (ent[0] == 0x00) return -1;            /* 目录结尾，未找到 */
            if (ent[0] == 0xE5) { fat32_lfn_init(&lfn); continue; }
            const f32w_dirent_t *de = (const f32w_dirent_t *)ent;
            int is_short = fat32_lfn_process(&lfn, ent);
            if (!is_short) continue;
            if (de->attr & 0x08) { fat32_lfn_init(&lfn); continue; }   /* 卷标 */
            if (de->name[0] == '.' && (de->name[1] == ' ' || de->name[1] == '.')) {
                fat32_lfn_init(&lfn);   /* "." / ".." */
                continue;
            }
            int entry_is_dir = (de->attr & 0x10) ? 1 : 0;
            if (want_dir >= 0 && entry_is_dir != want_dir) {
                fat32_lfn_init(&lfn);
                continue;
            }
            int matched = 0;
            if (lfn.valid) {
                char ascii[FAT32_LFN_MAX];
                if (fat32_lfn_to_ascii(&lfn, ascii, sizeof(ascii)) >= 0 &&
                    fat32_lfn_streq_ci(ascii, name)) matched = 1;
            }
            if (!matched) {
                char disp[13];
                fat32_lfn_short_to_str((const u8 *)de->name, disp);
                if (fat32_lfn_streq_ci(disp, name)) matched = 1;
            }
            if (matched) {
                u32 fc = (u32)f32w_r16((const u8 *)&de->clow) |
                         ((u32)f32w_r16((const u8 *)&de->chigh) << 16);
                if (out_clus) *out_clus = fc;
                if (out_size) *out_size = de->fsize;
                if (out_ent_clus) *out_ent_clus = dclus;
                if (out_ent_idx) *out_ent_idx = e;
                if (out_attr) *out_attr = de->attr;
                return 0;
            }
            fat32_lfn_init(&lfn);
        }
        u32 next = 0;
        if (f32w_fat_get(dclus, &next) != 0) return -3;
        dclus = next;
    }
    return -1;
}

/* ---- 文件数据簇链写入（新建 / 复用扩展 / 截断收缩） ---- */

/* 写 size 字节数据。have>=2 时复用现有链（按需扩展或截断并释放多余簇），
 * have<2 时全新分配。*out_first 输出结果首簇（size==0 时为 0）。
 * 返回 0 成功，负值失败。 */
static inline int f32w_data_write(u32 have, const u8 *data, u32 size, u32 *out_first) {
    u32 cb = f32w_c.clus_bytes;
    u32 need = size ? (size + cb - 1) / cb : 0;

    if (need == 0) {
        if (have >= 2) {
            if (f32w_free_chain(have) != 0) return -1;
        }
        *out_first = 0;
        return 0;
    }

    u32 first = 0, prev = 0, count = 0;
    if (have >= 2) {
        first = have;
        u32 cur = have;
        while (cur >= 2 && cur < 0x0FFFFFF8 && count < need) {
            prev = cur;
            u32 next = 0;
            if (f32w_fat_get(cur, &next) != 0) return -2;
            cur = next;
            count++;
        }
        while (count < need) {                       /* 现有链不足：扩展 */
            u32 nc = f32w_alloc_free();
            if (nc == 0) return -3;
            if (f32w_fat_set(nc, F32W_EOC_VAL) != 0) return -4;
            if (f32w_fat_set(prev, nc) != 0) return -4;
            prev = nc;
            count++;
        }
        if (cur >= 2 && cur < 0x0FFFFFF8) {          /* 现有链过长：截断 */
            if (f32w_fat_set(prev, F32W_EOC_VAL) != 0) return -4;
            if (f32w_free_chain(cur) != 0) return -5;
        }
    } else {
        for (u32 i = 0; i < need; i++) {             /* 全新分配 */
            u32 nc = f32w_alloc_free();
            if (nc == 0) return -3;
            if (f32w_fat_set(nc, F32W_EOC_VAL) != 0) return -4;
            if (prev) {
                if (f32w_fat_set(prev, nc) != 0) return -4;
            } else {
                first = nc;
            }
            prev = nc;
        }
        count = need;
    }

    /* 写数据：整簇直写源缓冲，尾簇经暂存缓冲零填充 */
    u32 remaining = size;
    const u8 *src = data;
    u32 c = first;
    for (u32 i = 0; i < count && remaining > 0; i++) {
        u32 chunk = remaining < cb ? remaining : cb;
        u32 lba = f32w_clus_lba(c);
        if (chunk == cb) {
            if (f32w_write_sectors(lba, f32w_c.spc, src) != 0) return -6;
        } else {
            f32w_mem_set(f32w_stage, 0, cb);
            for (u32 b = 0; b < chunk; b++) f32w_stage[b] = src[b];
            if (f32w_write_sectors(lba, f32w_c.spc, f32w_stage) != 0) return -6;
        }
        src += chunk;
        remaining -= chunk;
        if (i + 1 < count) {
            u32 next = 0;
            if (f32w_fat_get(c, &next) != 0) return -2;
            c = next;
        }
    }
    *out_first = first;
    return 0;
}

/* ---- FSInfo 置未知（写操作后调用，best-effort） ----
 * 分配/释放后空闲簇数不再准确，置 0xFFFFFFFF（未知）避免 UEFI/Windows
 * 依赖陈旧计数。忽略失败（无 FSInfo 的镜像不受影响）。 */
static inline void f32w_fsinfo_unknown(void) {
    if (!f32w_c.loaded) return;
    u32 fsi_lba = 0;
    {
        /* 重新读 BPB 拿 FSInfo 扇区号（f32w_sec 此后未被覆盖过也可直接用） */
        const f32w_bpb_t *b = (const f32w_bpb_t *)f32w_sec;
        if (b->boot_sig != 0xAA55) return;
        fsi_lba = b->fsi;
    }
    if (fsi_lba == 0 || fsi_lba == 0xFFFF) return;
    u8 sec[512];
    if (f32w_read_sectors(fsi_lba, 1, sec) != 0) return;
    sec[488] = 0xFF; sec[489] = 0xFF; sec[490] = 0xFF; sec[491] = 0xFF;
    sec[492] = 0xFF; sec[493] = 0xFF; sec[494] = 0xFF; sec[495] = 0xFF;
    (void)f32w_write_sectors(fsi_lba, 1, sec);
}

/* ---- 写入 LFN 链 + 短名项（dir_alloc_slots 已定位 run 起点） ---- */
static inline int f32w_insert_entries(u32 dir_clus, const char *name,
                                      const char name11[11], u32 clus, u32 size, u8 attr) {
    u32 len = f32w_strlen(name);
    int needs_lfn = 0;
    /* needs_lfn 与调用方 make_unique_short 判定一致：重算（无损则短名回退匹配） */
    {
        char tmp11[11];
        if (f32w_make_short(name, tmp11, &needs_lfn) != 0) return -1;
    }
    u32 nlfn = needs_lfn ? (len + 12) / 13 : 0;
    u32 sc = 0, si = 0;
    if (f32w_dir_alloc_slots(dir_clus, nlfn + 1, &sc, &si) != 0) return -2;
    if (needs_lfn) {
        u8 ck = fat32_lfn_checksum((const u8 *)name11);
        for (int k = (int)nlfn; k >= 1; k--) {   /* 逆序：最高序号最先落盘 */
            u8 e[32];
            f32w_build_lfn_entry(k, (int)nlfn, name, len, ck, e);
            if (f32w_entry_write(sc, si, e) != 0) return -3;
            if (k > 1 && f32w_slot_step(&sc, &si) != 0) return -4;
        }
        if (nlfn > 0 && f32w_slot_step(&sc, &si) != 0) return -4;
    }
    u8 se[32];
    f32w_build_short_entry(name11, attr, clus, size, se);
    if (f32w_entry_write(sc, si, se) != 0) return -3;
    if (f32w_dir_flush() != 0) return -5;
    return 0;
}

/* ================= 公共 API ================= */

/* 在目录中写/替换长名文件（LFN 创建 + 短名 ~N 唯一化 + FAT 链管理）。
 *   dir_clus : 目标目录首簇（0=根目录）
 *   name     : 长文件名（ASCII；非 8.3 无损表示时自动生成 LFN 链）
 *   data/size: 文件内容（size 可为 0，簇链释放、大小清 0）
 * 同名存在：文件原地替换内容（保留原目录项/LFN），目录返回 -20。
 * 返回 0 成功，负值失败。 */
static inline int f32w_write_file(u32 dir_clus, const char *name,
                                  const u8 *data, u32 size) {
    if (f32w_bpb_load() != 0) return -1;
    if (!f32w_blk_write) return -2;
    if (!name || (!data && size)) return -3;
    if (dir_clus == 0) dir_clus = f32w_c.root_clus;

    u32 e_clus = 0, e_size = 0, ent_clus = 0, ent_idx = 0;
    u8 e_attr = 0;
    int fr = f32w_find(dir_clus, name, -1, &e_clus, &e_size, &ent_clus, &ent_idx, &e_attr);
    if (fr == 0) {
        if (e_attr & 0x10) return -20;   /* 同名目录已存在 */
        /* 原地替换：复用/扩展/截断原簇链，仅更新短名项 clus/size */
        u32 first = 0;
        if (f32w_data_write(e_clus, data, size, &first) != 0) return -4;
        if (f32w_dir_load(ent_clus) != 0) return -5;
        u8 *ent = f32w_dir + ent_idx * 32;
        ent[20] = (u8)((first >> 24) & 0xFF);   /* chigh */
        ent[21] = (u8)((first >> 16) & 0xFF);
        ent[26] = (u8)(first & 0xFF);           /* clow */
        ent[27] = (u8)((first >> 8) & 0xFF);
        ent[28] = (u8)(size & 0xFF);            /* fsize */
        ent[29] = (u8)((size >> 8) & 0xFF);
        ent[30] = (u8)((size >> 16) & 0xFF);
        ent[31] = (u8)((size >> 24) & 0xFF);
        f32w_dir_dirty = 1;
        if (f32w_dir_flush() != 0) return -5;
        f32w_fsinfo_unknown();
        return 0;
    }

    /* 新建：唯一短名 -> 数据链 -> 目录槽位（含链扩展）-> LFN+短名项 */
    char name11[11];
    if (f32w_make_unique_short(dir_clus, name, name11) != 0) return -6;
    u32 first = 0;
    if (f32w_data_write(0, data, size, &first) != 0) return -7;
    if (f32w_insert_entries(dir_clus, name, name11, first, size, 0x20) != 0) return -8;
    f32w_fsinfo_unknown();
    return 0;
}

/* 在目录中创建子目录（LFN + "." / ".." 项 + 父目录插入条目）。
 *   parent_clus: 父目录首簇（0=根目录）
 *   out_clus    : 输出新目录首簇（可 NULL）
 * 返回 0 成功，-20 同名已存在，其他负值失败。 */
static inline int f32w_mkdir(u32 parent_clus, const char *name, u32 *out_clus) {
    if (f32w_bpb_load() != 0) return -1;
    if (!f32w_blk_write) return -2;
    if (!name) return -3;
    if (parent_clus == 0) parent_clus = f32w_c.root_clus;

    int fr = f32w_find(parent_clus, name, -1, 0, 0, 0, 0, 0);
    if (fr == 0) return -20;   /* 同名已存在 */

    char name11[11];
    if (f32w_make_unique_short(parent_clus, name, name11) != 0) return -6;

    /* 分配目录簇并初始化 "." / ".." */
    u32 nc = f32w_alloc_free();
    if (nc == 0) return -21;
    if (f32w_fat_set(nc, F32W_EOC_VAL) != 0) return -22;
    if (f32w_dir_flush() != 0) return -23;
    f32w_mem_set(f32w_dir, 0, f32w_c.clus_bytes);
    {
        u8 dot[32], dotdot[32];
        char n_self[11]  = {'.',' ',' ',' ',' ',' ',' ',' ',' ',' ',' '};
        char n_up[11]    = {'.','.',' ',' ',' ',' ',' ',' ',' ',' ',' '};
        u32 parent_link = (parent_clus == f32w_c.root_clus) ? 0 : parent_clus;
        f32w_build_short_entry(n_self, 0x10, nc, 0, dot);
        f32w_build_short_entry(n_up, 0x10, parent_link, 0, dotdot);
        for (int i = 0; i < 32; i++) { f32w_dir[i] = dot[i]; f32w_dir[32 + i] = dotdot[i]; }
    }
    f32w_dir_no = nc;
    f32w_dir_dirty = 1;
    if (f32w_dir_flush() != 0) return -23;

    /* 父目录插入条目（含链扩展） */
    if (f32w_insert_entries(parent_clus, name, name11, nc, 0, 0x10) != 0) return -8;
    f32w_fsinfo_unknown();
    if (out_clus) *out_clus = nc;
    return 0;
}

/* ---- 路径辅助（'/' 分隔，每段长名；中间目录必须已存在） ---- */

/* 定位路径目录簇。空路径/"'/'" 返回根目录。 */
static inline int f32w_resolve_dir(const char *path, u32 *out_clus) {
    if (!path || !out_clus) return -1;
    if (f32w_bpb_load() != 0) return -2;
    u32 cur = f32w_c.root_clus;
    u32 pos = 0;
    u32 path_len = f32w_strlen(path);
    if (path_len == 0 || (path_len == 1 && path[0] == '/')) {
        *out_clus = cur;
        return 0;
    }
    while (pos < path_len) {
        while (pos < path_len && path[pos] == '/') pos++;
        if (pos >= path_len) break;
        char comp[260];
        u32 ci = 0;
        while (pos < path_len && path[pos] != '/' && ci + 1 < sizeof(comp))
            comp[ci++] = path[pos++];
        comp[ci] = 0;
        if (ci == 0) continue;
        u32 sub = 0;
        if (f32w_find(cur, comp, 1, &sub, 0, 0, 0, 0) != 0) return -3;
        cur = sub;
    }
    *out_clus = cur;
    return 0;
}

/* 按路径写文件（父目录必须已存在）。返回 0 成功，负值失败。 */
static inline int f32w_write_path(const char *path, const u8 *data, u32 size) {
    if (!path) return -1;
    if (f32w_bpb_load() != 0) return -2;
    u32 path_len = f32w_strlen(path);
    u32 last_slash = 0xFFFFFFFFu;
    for (u32 i = 0; i < path_len; i++)
        if (path[i] == '/') last_slash = i;
    const char *fname = (last_slash == 0xFFFFFFFFu) ? path : path + last_slash + 1;
    u32 dir_clus = 0;
    if (last_slash == 0xFFFFFFFFu) {
        dir_clus = f32w_c.root_clus;
    } else {
        char dirpath[260];
        u32 dn = last_slash;
        if (dn >= sizeof(dirpath)) return -3;
        for (u32 i = 0; i < dn; i++) dirpath[i] = path[i];
        dirpath[dn] = 0;
        if (f32w_resolve_dir(dirpath, &dir_clus) != 0) return -4;
    }
    return f32w_write_file(dir_clus, fname, data, size);
}

/* 按路径创建目录（仅最后一段被创建，中间目录必须已存在）。返回 0 成功。 */
static inline int f32w_mkdir_path(const char *path, u32 *out_clus) {
    if (!path) return -1;
    if (f32w_bpb_load() != 0) return -2;
    u32 path_len = f32w_strlen(path);
    if (path_len == 0) return -3;
    /* 去尾部 '/' */
    while (path_len > 0 && path[path_len - 1] == '/') path_len--;
    if (path_len == 0) return -3;
    u32 last_slash = 0xFFFFFFFFu;
    for (u32 i = 0; i < path_len; i++)
        if (path[i] == '/') last_slash = i;
    const char *dname = (last_slash == 0xFFFFFFFFu) ? path : path + last_slash + 1;
    if ((u32)(dname - path) >= path_len) return -3;   /* 防御：末段为空 */
    u32 parent_clus = 0;
    if (last_slash == 0xFFFFFFFFu) {
        parent_clus = f32w_c.root_clus;
    } else {
        char dirpath[260];
        u32 dn = last_slash;
        if (dn >= sizeof(dirpath)) return -4;
        for (u32 i = 0; i < dn; i++) dirpath[i] = path[i];
        dirpath[dn] = 0;
        if (f32w_resolve_dir(dirpath, &parent_clus) != 0) return -5;
    }
    return f32w_mkdir(parent_clus, dname, out_clus);
}

#endif /* DESHAB_FAT32_WRITE_H */