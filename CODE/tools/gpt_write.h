/* gpt_write.h — Deshab 共享 GPT 写库（static inline，无库）
 *
 * 前置条件：包含者已定义 u8/u16/u32/u64/i64 基础类型。
 * 通过 gpt_init() 注入 block_read/block_write 函数指针（签名与 f32 库一致）。
 *
 * 能力：
 *   gpt_probe()       — 读 LBA1 校验 "EFI PART" 签名 + header CRC（轻量探测）
 *   gpt_load()        — 加载 GPT 头 + 分区表到内部缓冲，并做 CRC 校验
 *   gpt_find_esp()    — 查找 ESP 分区（type GUID c12a7328-...），输出起始/结束 LBA
 *   gpt_find_part()   — 按任意 type GUID 查找分区
 *   gpt_update_esp()  — 新增/更新 ESP 分区项（改起始/结束 LBA）
 *   gpt_commit()      — 重算 entries CRC + 双 header CRC，写主表/备份表/主头/备份头
 *   gpt_blank_disk()  — 全新初始化：protective MBR + 空 GPT（主+备份，无分区）
 *
 * 与 build.ps1（New-GptHeader / New-GptFat32Image）字节兼容：
 *   LBA0 protective MBR（type 0xEE），LBA1 主头，LBA2 主表（128×128B = 32 扇区），
 *   备份表 lastLba-32，备份头 lastLba。GUID 按 .NET Guid.ToByteArray()
 *   little-endian 编码（前三组小端、后两组原序）。
 *   CRC32 为 IEEE 802.3（init 0xFFFFFFFF / poly 0xEDB88320 / xorout 0xFFFFFFFF），
 *   与 build.ps1 Get-Crc32 逐位实现等价。
 *
 * 缓冲：包含者需提供 ~16.5KB BSS（1 扇区头缓冲 + 32 扇区分区表缓冲）。
 *
 * 典型流程（FirstInit 写盘 / 系统格式化）：
 *   gpt_init(block_read, block_write);
 *   if (gpt_probe() != 0) gpt_blank_disk(total_sectors, NULL);  // 空盘初始化
 *   gpt_load();
 *   gpt_update_esp(2048, total_sectors - 34);                   // 或扩容
 *   gpt_commit();
 */

#ifndef DESHAB_GPT_WRITE_H
#define DESHAB_GPT_WRITE_H

/* ---- block 设备函数类型（与 fat32_io.h 签名一致，index 固定 0） ---- */
typedef int (*gpt_block_read_fn)(u32 index, u64 lba, u32 count, void *buf);
typedef int (*gpt_block_write_fn)(u32 index, u64 lba, u32 count, const void *buf);

static gpt_block_read_fn  gpt_blk_read  = 0;
static gpt_block_write_fn gpt_blk_write = 0;

/* ---- GPT 常量（与 build.ps1 一致） ---- */
#define GPT_SIG_BYTES  8
#define GPT_ENTRIES    128u
#define GPT_ENTRY_SIZE 128u
#define GPT_ENTRY_SECTORS 32u        /* 128×128 / 512 */
#define GPT_FIRST_USABLE  34u        /* 2 + 32 */
#define GPT_HEADER_SIZE   92u
#define GPT_REVISION      0x00010000u
#define GPT_MBR_PART_OFF  446        /* protective MBR 分区项偏移 */

/* ---- GPT 头（512B，build.ps1 Set-Le* 布局） ---- */
typedef struct __attribute__((packed)) {
    u8  sig[8];         /* "EFI PART" */
    u32 revision;       /* 0x00010000 */
    u32 header_size;    /* 92 */
    u32 header_crc;
    u32 reserved0;
    u64 current_lba;    /* 主头=1，备份头=lastLba */
    u64 backup_lba;
    u64 first_usable;   /* 34 */
    u64 last_usable;    /* lastLba - 33 */
    u8  disk_guid[16];
    u64 entries_lba;    /* 主表=2，备份表=lastLba-32 */
    u32 entries_count;  /* 128 */
    u32 entries_size;   /* 128 */
    u32 entries_crc;
    u8  reserved[420];
} gpt_header;           /* 512B */

/* ---- 分区项（128B） ---- */
typedef struct __attribute__((packed)) {
    u8  type_guid[16];  /* 全 0 = 空槽 */
    u8  uniq_guid[16];
    u64 first_lba;
    u64 last_lba;
    u64 attrs;
    u16 name[36];       /* UTF-16LE，未用部分为 0 */
} gpt_part_entry;

/* ESP 类型 GUID：c12a7328-f81f-11d2-ba4b-00a0c93ec93b
 * （.NET Guid.ToByteArray() 编码：前三组小端、后两组原序） */
static const u8 gpt_guid_esp[16] = {
    0x28, 0x73, 0x2a, 0xc1,   /* c12a7328 */
    0x1f, 0xf8,               /* f81f */
    0xd2, 0x11,               /* 11d2 */
    0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b
};

/* ---- 内部缓冲（每包含者独立） ---- */
static u8 gpt_sec[512];                    /* 单扇区（头/MBR）缓冲 */
static u8 gpt_tbl[GPT_ENTRY_SECTORS * 512]; /* 分区表缓冲（32 扇区） */
static u64 gpt_disk_sectors = 0;           /* 磁盘总扇区数（探测后已知） */

/* ---- 初始化：注入 block_read/block_write 函数指针 ---- */
static inline void gpt_init(gpt_block_read_fn rd, gpt_block_write_fn wr) {
    gpt_blk_read = rd;
    gpt_blk_write = wr;
    gpt_disk_sectors = 0;
}

static inline int gpt_read_sectors(u64 lba, u32 count, u8 *out) {
    return gpt_blk_read ? gpt_blk_read(0, lba, count, out) : -1;
}
static inline int gpt_write_sectors(u64 lba, u32 count, const u8 *buf) {
    return gpt_blk_write ? gpt_blk_write(0, lba, count, buf) : -1;
}

/* ---- 小工具（无库约束：手写循环替代 mem*） ---- */
static inline void gpt_mem_set(u8 *p, u8 v, u32 n) { for (u32 i = 0; i < n; i++) p[i] = v; }
static inline void gpt_mem_cpy(u8 *d, const u8 *s, u32 n) { for (u32 i = 0; i < n; i++) d[i] = s[i]; }
static inline int  gpt_mem_eq(const u8 *a, const u8 *b, u32 n) {
    for (u32 i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}
static inline u32 gpt_r32(const u8 *p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}
static inline void gpt_w16(u8 *p, u16 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static inline void gpt_w32(u8 *p, u32 v) {
    p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24);
}
static inline void gpt_w64(u8 *p, u64 v) {
    gpt_w32(p, (u32)v); gpt_w32(p + 4, (u32)(v >> 32));
}

/* ---- CRC32（IEEE 802.3，与 build.ps1 Get-Crc32 逐位实现等价；表驱动） ---- */
static u32 gpt_crc_tab[256];
static int  gpt_crc_ready = 0;

static inline void gpt_crc_init(void) {
    if (gpt_crc_ready) return;
    for (u32 i = 0; i < 256; i++) {
        u32 c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (c >> 1) ^ 0xEDB88320u : (c >> 1);
        gpt_crc_tab[i] = c;
    }
    gpt_crc_ready = 1;
}

static inline u32 gpt_crc32(const u8 *p, u32 n) {
    u32 crc = 0xFFFFFFFFu;
    for (u32 i = 0; i < n; i++)
        crc = (crc >> 8) ^ gpt_crc_tab[(crc ^ p[i]) & 0xFFu];
    return crc ^ 0xFFFFFFFFu;
}

/* ---- GUID 工具 ---- */

/* "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" → 16B（.NET ToByteArray 编码）
 * 返回 0 成功，-1 格式错误 */
static inline int gpt_guid_from_str(const char *s, u8 out[16]) {
    static const int hex[256] = { ['0'] = 0, ['1'] = 1, ['2'] = 2, ['3'] = 3,
        ['4'] = 4, ['5'] = 5, ['6'] = 6, ['7'] = 7, ['8'] = 8, ['9'] = 9,
        ['a'] = 10, ['b'] = 11, ['c'] = 12, ['d'] = 13, ['e'] = 14, ['f'] = 15,
        ['A'] = 10, ['B'] = 11, ['C'] = 12, ['D'] = 13, ['E'] = 14, ['F'] = 15 };
    /* 位置表：字符串下标 → 字节下标（1=前四组反序，0=后两组原序） */
    static const struct { int str; int byte; int rev; } map[16] = {
        { 0, 3, 1 }, { 2, 2, 1 }, { 4, 1, 1 }, { 6, 0, 1 },
        { 9, 5, 1 }, { 11, 4, 1 },
        { 14, 7, 1 }, { 16, 6, 1 },
        { 19, 8, 0 }, { 21, 9, 0 }, { 23, 10, 0 }, { 25, 11, 0 },
        { 28, 12, 0 }, { 30, 13, 0 }, { 32, 14, 0 }, { 34, 15, 0 },
    };
    if (s[8] != '-' || s[13] != '-' || s[18] != '-' || s[23] != '-') return -1;
    for (int i = 0; i < 16; i++) {
        int hi = hex[(u8)s[map[i].str]];
        int lo = hex[(u8)s[map[i].str + 1]];
        if (hi < 0 || lo < 0) return -1;
        (void)0;
    }
    for (int i = 0; i < 16; i++)
        out[map[i].byte] = (u8)((hex[(u8)s[map[i].str]] << 4) | hex[(u8)s[map[i].str + 1]]);
    return 0;
}

/* 16B → "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"（与 .NET Guid.ToString() 同序） */
static inline void gpt_guid_to_str(const u8 g[16], char out[37]) {
    static const char *hd = "0123456789abcdef";
    static const struct { int byte; int rev; } om[16] = {
        { 3, 1 }, { 2, 1 }, { 1, 1 }, { 0, 1 },
        { 5, 1 }, { 4, 1 },
        { 7, 1 }, { 6, 1 },
        { 8, 0 }, { 9, 0 }, { 10, 0 }, { 11, 0 },
        { 12, 0 }, { 13, 0 }, { 14, 0 }, { 15, 0 },
    };
    for (int i = 0; i < 16; i++) {
        int b = om[i].byte;
        out[i * 2] = hd[g[b] >> 4];
        out[i * 2 + 1] = hd[g[b] & 0xF];
        if (i == 3 || i == 5 || i == 7 || i == 9) { out[i * 2 + 2] = '-'; }
    }
    out[36] = 0;
}

/* ---- GPT 头构建 / 校验 ---- */

/* 计算头 CRC：前 92 字节、header_crc 字段按 0 参与 */
static inline u32 gpt_header_crc(const gpt_header *h) {
    u8 tmp[92];
    gpt_mem_cpy(tmp, (const u8 *)h, 92);
    gpt_w32(tmp + 16, 0);
    return gpt_crc32(tmp, 92);
}

/* 构建头（不落盘）：entries_crc 由调用者预先算好 */
static inline void gpt_header_build(gpt_header *h, u64 current_lba, u64 backup_lba,
                                    u64 first_usable, u64 last_usable,
                                    const u8 disk_guid[16],
                                    u64 entries_lba, u32 entries_crc) {
    static const u8 sig[8] = { 'E', 'F', 'I', ' ', 'P', 'A', 'R', 'T' };
    gpt_mem_set((u8 *)h, 0, sizeof(*h));
    gpt_mem_cpy(h->sig, sig, 8);
    h->revision    = GPT_REVISION;
    h->header_size = GPT_HEADER_SIZE;
    h->current_lba = current_lba;
    h->backup_lba  = backup_lba;
    h->first_usable = first_usable;
    h->last_usable  = last_usable;
    if (disk_guid) gpt_mem_cpy(h->disk_guid, disk_guid, 16);
    h->entries_lba   = entries_lba;
    h->entries_count = GPT_ENTRIES;
    h->entries_size  = GPT_ENTRY_SIZE;
    h->entries_crc   = entries_crc;
    h->header_crc    = gpt_header_crc(h);
}

/* 校验 LBA 处的 GPT 头（签名/大小/current_lba/CRC），返回 0 有效 */
static inline int gpt_header_validate(const gpt_header *h, u64 expect_lba) {
    static const u8 sig[8] = { 'E', 'F', 'I', ' ', 'P', 'A', 'R', 'T' };
    if (!gpt_mem_eq(h->sig, sig, 8)) return -1;
    if (h->revision != GPT_REVISION || h->header_size < GPT_HEADER_SIZE) return -2;
    if (h->current_lba != expect_lba) return -3;
    if (gpt_header_crc(h) != h->header_crc) return -4;
    return 0;
}

/* ---- 分区表操作（内存缓冲） ---- */
static inline gpt_part_entry *gpt_part(u32 idx) {
    return (gpt_part_entry *)(gpt_tbl + idx * GPT_ENTRY_SIZE);
}

/* 分区表 CRC（覆盖全部 32 扇区） */
static inline u32 gpt_entries_crc(void) {
    return gpt_crc32(gpt_tbl, sizeof(gpt_tbl));
}

static inline int gpt_part_empty(const gpt_part_entry *e) {
    return gpt_mem_eq(e->type_guid, (const u8 *)gpt_guid_esp /* 任意非 0 引用 */, 0) &&
           gpt_mem_eq(e->type_guid, e->type_guid, 0); /* 恒真占位，防 -Wunused */
}
static inline int gpt_part_is_free(const gpt_part_entry *e) {
    /* type_guid 全 0 = 空槽 */
    static const u8 zero16[16] = { 0 };
    return gpt_mem_eq(e->type_guid, zero16, 16);
}

/* 查找第一个 type_guid 匹配的分区（含全 0 停止），返回索引或 -1 */
static inline int gpt_find_part_type(const u8 type_guid[16], u64 *out_first, u64 *out_last) {
    for (u32 i = 0; i < GPT_ENTRIES; i++) {
        const gpt_part_entry *e = gpt_part(i);
        if (gpt_part_is_free(e)) break;
        if (gpt_mem_eq(e->type_guid, type_guid, 16)) {
            if (out_first) *out_first = e->first_lba;
            if (out_last)  *out_last  = e->last_lba;
            return (int)i;
        }
    }
    return -1;
}

/* 查找 ESP 分区，返回索引或 -1 */
static inline int gpt_find_esp(u64 *out_first, u64 *out_last) {
    return gpt_find_part_type(gpt_guid_esp, out_first, out_last);
}

/* 分区名写入（ASCII → UTF-16LE，最多 36 字符） */
static inline void gpt_part_set_name(gpt_part_entry *e, const char *ascii) {
    u32 i = 0;
    for (; i < 36 && ascii[i]; i++) gpt_w16((u8 *)&e->name[i], (u16)ascii[i]);
    for (; i < 36; i++) gpt_w16((u8 *)&e->name[i], 0);
}

/* 填充分区项到指定槽位（返回 0 成功，-1 槽位越界/非空） */
static inline int gpt_part_fill(u32 idx, const u8 type_guid[16], const u8 uniq_guid[16],
                                u64 first_lba, u64 last_lba, u64 attrs, const char *name) {
    if (idx >= GPT_ENTRIES) return -1;
    gpt_part_entry *e = gpt_part(idx);
    if (!gpt_part_is_free(e)) return -2;
    gpt_mem_cpy(e->type_guid, type_guid, 16);
    if (uniq_guid) gpt_mem_cpy(e->uniq_guid, uniq_guid, 16);
    else gpt_mem_set(e->uniq_guid, 0, 16);
    e->first_lba = first_lba;
    e->last_lba  = last_lba;
    e->attrs     = attrs;
    gpt_part_set_name(e, name);
    return 0;
}

/* ---- 落盘 / 探测 ---- */

/* 探测 LBA1 是否为有效 GPT（轻量，不加载分区表） */
static inline int gpt_probe(void) {
    if (gpt_read_sectors(1, 1, gpt_sec) != 0) return -1;
    return gpt_header_validate((const gpt_header *)gpt_sec, 1);
}

/* 加载主头 + 主分区表到缓冲；校验头/表 CRC。total_sectors 为 0 时跳过盘尾推导 */
static inline int gpt_load(u64 total_sectors) {
    if (gpt_read_sectors(1, 1, gpt_sec) != 0) return -1;
    const gpt_header *h = (const gpt_header *)gpt_sec;
    if (gpt_header_validate(h, 1) != 0) return -2;
    if (h->entries_lba != 2 || h->entries_count != GPT_ENTRIES) return -3;
    if (gpt_read_sectors(2, GPT_ENTRY_SECTORS, gpt_tbl) != 0) return -4;
    if (gpt_entries_crc() != h->entries_crc) return -5;
    gpt_disk_sectors = total_sectors;
    return 0;
}

/* 全新初始化：protective MBR + 空 GPT（主+备份）。disk_guid 可 NULL（全 0）。
 * total_sectors 必须 >= 70（34 + 32 表 + 2 头）。 */
static inline int gpt_blank_disk(u64 total_sectors, const u8 disk_guid[16]) {
    if (total_sectors < GPT_FIRST_USABLE + GPT_ENTRY_SECTORS + 2) return -1;

    /* LBA0 protective MBR（build.ps1 布局） */
    gpt_mem_set(gpt_sec, 0, 512);
    gpt_sec[GPT_MBR_PART_OFF + 4] = 0xEE;             /* type */
    gpt_w32(gpt_sec + GPT_MBR_PART_OFF + 8, 1);       /* start LBA */
    u64 prot = total_sectors - 1;
    if (prot > 0xFFFFFFFFu) prot = 0xFFFFFFFFu;
    gpt_w32(gpt_sec + GPT_MBR_PART_OFF + 12, (u32)prot); /* sectors */
    gpt_sec[510] = 0x55; gpt_sec[511] = 0xAA;
    if (gpt_write_sectors(0, 1, gpt_sec) != 0) return -2;

    /* 空分区表 */
    gpt_mem_set(gpt_tbl, 0, sizeof(gpt_tbl));
    u32 entries_crc = gpt_crc32(gpt_tbl, sizeof(gpt_tbl));

    /* 主头（LBA1）+ 备份头（lastLba） */
    u64 last_lba   = total_sectors - 1;
    u64 last_usable = last_lba - GPT_ENTRY_SECTORS - 1;
    gpt_header ph, bh;
    gpt_header_build(&ph, 1, last_lba, GPT_FIRST_USABLE, last_usable,
                     disk_guid, 2, entries_crc);
    gpt_header_build(&bh, last_lba, 1, GPT_FIRST_USABLE, last_usable,
                     disk_guid, last_lba - GPT_ENTRY_SECTORS, entries_crc);

    if (gpt_write_sectors(1, 1, (const u8 *)&ph) != 0) return -3;
    if (gpt_write_sectors(2, GPT_ENTRY_SECTORS, gpt_tbl) != 0) return -4;
    if (gpt_write_sectors(last_lba - GPT_ENTRY_SECTORS, GPT_ENTRY_SECTORS, gpt_tbl) != 0) return -5;
    if (gpt_write_sectors(last_lba, 1, (const u8 *)&bh) != 0) return -6;

    gpt_disk_sectors = total_sectors;
    return 0;
}

/* 新增/更新 ESP 分区项（内存缓冲，需 gpt_commit 落盘）：
 * 已有 ESP 则更新范围，否则填第一个空槽。返回槽位索引或负错误码 */
static inline int gpt_update_esp(u64 first_lba, u64 last_lba,
                                 const u8 uniq_guid[16], const char *name) {
    static const u8 zero16[16] = { 0 };
    int idx = gpt_find_part_type(gpt_guid_esp, 0, 0);
    if (idx >= 0) {
        gpt_part_entry *e = gpt_part((u32)idx);
        e->first_lba = first_lba;
        e->last_lba  = last_lba;
        if (uniq_guid) gpt_mem_cpy(e->uniq_guid, uniq_guid, 16);
        if (name) gpt_part_set_name(e, name);
        return idx;
    }
    /* 找第一个空槽（类型全 0） */
    for (u32 i = 0; i < GPT_ENTRIES; i++) {
        if (gpt_part_is_free(gpt_part(i)))
            return gpt_part_fill(i, gpt_guid_esp, uniq_guid ? uniq_guid : zero16,
                                 first_lba, last_lba, 0, name ? name : "Deshab ESP");
    }
    return -3; /* 表满 */
}

/* 提交：重算 entries CRC + 双头 CRC，写主表/备份表/主头/备份头（幂等） */
static inline int gpt_commit(void) {
    u64 last_lba = 0;
    gpt_header h;
    /* 读当前主头，取 last_usable / disk_guid / backup 布局 */
    if (gpt_read_sectors(1, 1, gpt_sec) != 0) return -1;
    gpt_mem_cpy((u8 *)&h, gpt_sec, sizeof(h));
    if (gpt_header_validate(&h, 1) != 0) return -2;
    last_lba = h.backup_lba;

    u32 entries_crc = gpt_entries_crc();

    gpt_header_build(&h, 1, last_lba, h.first_usable, h.last_usable,
                     h.disk_guid, 2, entries_crc);
    gpt_header bh;
    gpt_header_build(&bh, last_lba, 1, h.first_usable, h.last_usable,
                     h.disk_guid, last_lba - GPT_ENTRY_SECTORS, entries_crc);

    if (gpt_write_sectors(2, GPT_ENTRY_SECTORS, gpt_tbl) != 0) return -3;
    if (gpt_write_sectors(1, 1, (const u8 *)&h) != 0) return -4;
    if (gpt_write_sectors(last_lba - GPT_ENTRY_SECTORS, GPT_ENTRY_SECTORS, gpt_tbl) != 0) return -5;
    if (gpt_write_sectors(last_lba, 1, (const u8 *)&bh) != 0) return -6;
    return 0;
}

#endif /* DESHAB_GPT_WRITE_H */