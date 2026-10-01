/* fat32_lfn.h — Deshab FAT32 VFAT 长文件名（LFN）解析共享库（static inline，无库）
 *
 * 前置条件：包含者已定义 u8/u16/u32 基础类型（与 fat32_io.h 一致）。
 *
 * FAT32 LFN 原理：
 *   目录项中 attr==0x0F 的是 LFN 片段项。每项含 13 个 UTF-16 LE 字符
 *   （name1[5] + name2[6] + name3[2]），按逆序存储——序号最高（带 0x40
 *   标志）的项最先出现，序号递减，最后紧跟一个 8.3 短目录项作为"主项"。
 *   短名项的 checksum（由 name11 计算）用于校验整条 LFN 链完整性。
 *
 * 用法（在目录遍历循环中）：
 *   fat32_lfn_buf lfn;
 *   fat32_lfn_init(&lfn);
 *   // 外层簇遍历循环——lfn 必须跨簇存活（LFN 链可能跨簇边界）
 *   while (遍历目录簇) {
 *       for (每个 32 字节目录项 e) {
 *           if (name[0]==0) 目录结束;
 *           if (name[0]==0xE5) { fat32_lfn_init(&lfn); continue; }
 *           int is_short = fat32_lfn_process(&lfn, entry_ptr);
 *           if (is_short) {
 *               // lfn.valid==1 时有完整 LFN，可 fat32_lfn_to_ascii 取名
 *               // 用 fat32_lfn_match 匹配长名或 8.3 短名
 *           }
 *       }
 *   }
 *
 * 注意：fat32_lfn_buf 必须在簇遍历循环**外**声明，跨簇切换时不清空，
 *       以支持跨簇边界的 LFN 链。
 */

#ifndef DESHAB_FAT32_LFN_H
#define DESHAB_FAT32_LFN_H

/* LFN 最大字符数（FAT32 规范最多 20 个 LFN 项 = 260 字符） */
#define FAT32_LFN_MAX 260

/* LFN 收集缓冲（栈/局部变量分配） */
typedef struct {
    u16 chars[FAT32_LFN_MAX];  /* UTF-16 LE 累积字符（正序） */
    int  count;                 /* 有效字符数（不含 0x0000 终止符/0xFFFF 填充） */
    u8   checksum;              /* LFN 链 checksum（来自 0x40 标志项） */
    int  valid;                 /* 1=LFN 链完整且 checksum 与短名项匹配 */
} fat32_lfn_buf;

/* 目录条目（供 fileman / linux_compat 列举使用）。 */
typedef struct {
    char name[13];
    u32  clus;
    u32  size;
    u8   attr;
    u8   is_dir;
    u8   has_lfn;
    char long_name[260];
} f32_entry;

typedef int (*f32_list_entry_cb)(const f32_entry *e, void *user_data);

/* 初始化/清空 LFN 缓冲 */
static inline void fat32_lfn_init(fat32_lfn_buf *buf) {
    buf->count = 0;
    buf->valid = 0;
    buf->checksum = 0;
    for (int i = 0; i < FAT32_LFN_MAX; i++) buf->chars[i] = 0;
}

/* 计算 8.3 短名（11 字节）的 LFN checksum。
 * 标准 FAT32 算法：cksum = ((cksum & 1) << 7) + (cksum >> 1) + name[i] */
static inline u8 fat32_lfn_checksum(const u8 name11[11]) {
    u8 cksum = 0;
    for (int i = 0; i < 11; i++) {
        cksum = (u8)(((cksum & 1) << 7) + (cksum >> 1) + name11[i]);
    }
    return cksum;
}

/* 处理一个 32 字节目录项。自动判断 LFN 项 / 短名项 / 已删除项。
 *
 * 调用方对目录遍历中的每个条目（跳过 name[0]==0 结束符后）调用此函数。
 *   - LFN 项（attr==0x0F）：收集 13 个 UTF-16 字符到 buf，返回 0
 *   - 已删除项（name[0]==0xE5）：清空 buf，返回 0
 *   - 短名项：校验 checksum，设 buf->valid，返回 1
 *
 * 返回 1=刚处理的是短名项（此时可检查 buf->valid 判断是否有有效 LFN），
 *      0=LFN 项或已删除项。
 *
 * 注意：遇到带 0x40 标志的 LFN 项（链开始）时自动重置 buf。
 *       遇到已删除项时自动清空 buf（防止残留 LFN 污染下一个文件）。 */
static inline int fat32_lfn_process(fat32_lfn_buf *buf, const u8 *entry) {
    u8 attr = entry[11];

    /* 已删除项：清空 LFN 缓冲 */
    if (entry[0] == 0xE5) {
        fat32_lfn_init(buf);
        return 0;
    }

    /* LFN 项（attr == 0x0F） */
    if (attr == 0x0F) {
        u8 seq = entry[0];
        u8 ord = (u8)(seq & 0x3F);  /* 序号 1-20 */
        if (ord == 0 || ord > 20) {
            fat32_lfn_init(buf);
            return 0;
        }
        /* 带 0x40 标志 = LFN 链开始（最高序号项），重置缓冲 */
        if (seq & 0x40) {
            int prev_cksum = buf->checksum;
            fat32_lfn_init(buf);
            buf->checksum = entry[13];
            (void)prev_cksum;
        }
        /* 提取 13 个 UTF-16 LE 字符，放到 (ord-1)*13 位置（正序） */
        int base = (ord - 1) * 13;
        if (base + 13 > FAT32_LFN_MAX) {
            fat32_lfn_init(buf);
            return 0;
        }
        /* name1: entry[1..10] = 5 个 UTF-16 LE 字符 */
        for (int i = 0; i < 5; i++) {
            u16 c = (u16)entry[1 + i * 2] | ((u16)entry[2 + i * 2] << 8);
            buf->chars[base + i] = c;
        }
        /* name2: entry[14..25] = 6 个 UTF-16 LE 字符 */
        for (int i = 0; i < 6; i++) {
            u16 c = (u16)entry[14 + i * 2] | ((u16)entry[15 + i * 2] << 8);
            buf->chars[base + 5 + i] = c;
        }
        /* name3: entry[28..31] = 2 个 UTF-16 LE 字符 */
        for (int i = 0; i < 2; i++) {
            u16 c = (u16)entry[28 + i * 2] | ((u16)entry[29 + i * 2] << 8);
            buf->chars[base + 11 + i] = c;
        }
        return 0;  /* LFN 项，还不是短名项 */
    }

    /* 短名项：校验 checksum */
    u8 cksum = fat32_lfn_checksum(entry);
    if (buf->checksum != 0 && buf->checksum == cksum) {
        /* LFN 链有效——计算实际字符数（截断于 0x0000 终止符或 0xFFFF 填充符） */
        int n = 0;
        for (int i = 0; i < FAT32_LFN_MAX; i++) {
            if (buf->chars[i] == 0x0000 || buf->chars[i] == 0xFFFF) break;
            n++;
        }
        buf->count = n;
        buf->valid = 1;
    } else {
        /* checksum 不匹配或无 LFN 链：清空，回退 8.3 */
        fat32_lfn_init(buf);
    }
    return 1;  /* 短名项处理完毕 */
}

/* 从 LFN 缓冲提取 ASCII 名（UTF-16→ASCII 缩窄，仅 BMP U+0000..U+007F）。
 * out_cap 为 out 缓冲容量（含 NUL）。
 * 返回字符串长度（不含 NUL），-1=无有效 LFN 或含非 ASCII 字符。 */
static inline int fat32_lfn_to_ascii(const fat32_lfn_buf *buf, char *out, int out_cap) {
    if (!buf->valid || buf->count == 0 || out_cap <= 0) return -1;
    int n = buf->count;
    if (n >= out_cap) n = out_cap - 1;
    for (int i = 0; i < n; i++) {
        u16 c = buf->chars[i];
        if (c > 0x7F) return -1;  /* 含非 ASCII 字符 */
        out[i] = (char)(u8)c;
    }
    out[n] = 0;
    return n;
}

/* ASCII 大小写不敏感字符串比较 */
static inline int fat32_lfn_streq_ci(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'a' && ca <= 'z') ca = (char)(ca - 32);
        if (cb >= 'a' && cb <= 'z') cb = (char)(cb - 32);
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == *b;
}

/* 判断 ASCII 字符串是否以指定后缀结尾（大小写不敏感）。
 * 例如 fat32_lfn_ends_with_ci("libtest.so", ".so") → 1 */
static inline int fat32_lfn_ends_with_ci(const char *s, const char *suffix) {
    if (!s || !suffix) return 0;
    int sl = 0, xl = 0;
    while (s[sl]) sl++;
    while (suffix[xl]) xl++;
    if (xl > sl || xl == 0) return 0;
    for (int i = 0; i < xl; i++) {
        char a = s[sl - xl + i];
        char b = suffix[i];
        if (a >= 'a' && a <= 'z') a = (char)(a - 32);
        if (b >= 'a' && b <= 'z') b = (char)(b - 32);
        if (a != b) return 0;
    }
    return 1;
}

/* 匹配目录项：优先匹配 LFN 长名（大小写不敏感），回退 8.3 短名。
 *   buf       : LFN 收集缓冲（短名项处理后）
 *   name11    : 短名项的 11 字节 8.3 名
 *   target_long: 目标长名（ASCII，可为 NULL=不匹配长名）
 *   target83  : 目标 8.3 名（11 字节，可为 NULL=不匹配 8.3）
 * 返回 1=匹配，0=不匹配。
 * FAT32 规范：文件名查找大小写不敏感。 */
static inline int fat32_lfn_match(const fat32_lfn_buf *buf, const u8 name11[11],
                                  const char *target_long, const char *target83) {
    /* 1. 优先匹配 LFN 长名 */
    if (buf->valid && buf->count > 0 && target_long) {
        char ascii[FAT32_LFN_MAX];
        int n = fat32_lfn_to_ascii(buf, ascii, sizeof(ascii));
        if (n >= 0 && fat32_lfn_streq_ci(ascii, target_long)) return 1;
    }
    /* 2. 回退 8.3 短名匹配（精确比较 11 字节） */
    if (target83 && name11) {
        for (int i = 0; i < 11; i++) {
            if (name11[i] != (u8)target83[i]) return 0;
        }
        return 1;
    }
    return 0;
}

/* 从 8.3 短名（11 字节）生成可显示 ASCII 字符串（如 "README.TXT"）。
 * 与 fat32_io.h 的 f32_name_from_83 功能相同，独立提供以避免循环依赖。 */
static inline void fat32_lfn_short_to_str(const u8 name11[11], char out[13]) {
    int p = 0;
    for (int j = 0; j < 8; j++) {
        if (name11[j] == ' ') break;
        out[p++] = (char)name11[j];
    }
    if (name11[8] != ' ') {
        out[p++] = '.';
        for (int j = 8; j < 11; j++) {
            if (name11[j] == ' ') break;
            out[p++] = (char)name11[j];
        }
    }
    out[p] = 0;
}

#endif /* DESHAB_FAT32_LFN_H */
