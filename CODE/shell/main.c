/* Deshab Shell — 命令行交互界面
 * 接收 PS/2 键盘输入，执行内置命令。
 * 使用 Deshab "Sealed Arc" 视觉风格系统。
 */

#include "../UTSM/include/utsm/dsk.h"
#include "../UTSM/include/utsm/pe.h"
#include "../UTSM/include/utsm/linux_compat.h"

typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;
typedef long long          i64;

#define COM1 0x3F8

static __inline__ void outb(u16 port, u8 value) { __asm__ volatile("outb %0,%1"::"a"(value),"Nd"(port)); }
static __inline__ u8 inb(u16 port) { u8 v; __asm__ volatile("inb %1,%0":"=a"(v):"Nd"(port)); return v; }
static __inline__ void outl(u16 port, u32 value) { __asm__ volatile("outl %0,%1"::"a"(value),"Nd"(port)); }
static __inline__ u32 inl(u16 port) { u32 v; __asm__ volatile("inl %1,%0":"=a"(v):"Nd"(port)); return v; }

/* ---- TSC-based timing (实机要求: 用 CPU 频率计算, 不用循环) ---- */
static u64 g_tsc_per_ms = 0;

static __inline__ u64 rdtsc_shell(void) {
    u32 lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((u64)hi << 32) | lo;
}

/* 通过 PIT ch0 (16-bit mode 0) 校准 TSC ~10ms 计数 */
static void tsc_calibrate_shell(void) {
    outb(0x43, 0x30);       /* ch0, lo+hi, mode 0, binary */
    outb(0x40, 0x7c);       /* 11932 low = ~10ms */
    outb(0x40, 0x2e);       /* 11932 high */
    u64 tsc_start = rdtsc_shell();
    u16 prev = 0; u64 loops = 0;
    for (;;) {
        outb(0x43, 0x00);
        u16 cur = (u16)inb(0x40) | ((u16)inb(0x40) << 8);
        if (cur > prev && loops > 10) break;
        prev = cur; loops++;
    }
    u64 tsc_end = rdtsc_shell();
    g_tsc_per_ms = (tsc_end - tsc_start) / 10;
}

/* 串口发送等待: TSC 100us 超时, 校准失败时回退到循环 */
static void serial_wait_tx_shell(void) {
    if (g_tsc_per_ms == 0) {
        tsc_calibrate_shell();
    }
    if (g_tsc_per_ms == 0) {
        for (u32 i = 0; i < 100000; i++) {
            if (inb(COM1 + 5) & 0x20) break;
        }
        return;
    }
    u64 deadline = rdtsc_shell() + g_tsc_per_ms / 10;  /* 100us 超时 */
    while (rdtsc_shell() < deadline) {
        if (inb(COM1 + 5) & 0x20) break;
        __asm__ volatile("pause");
    }
}

static void sputc(char c) {
    serial_wait_tx_shell();
    outb(COM1, (unsigned char)c);
}
static void swrite(const char *s) { while(*s) { if(*s=='\n')sputc('\r'); sputc(*s++); } }
static void logl(const char *s) { swrite(s); swrite("\n"); }

/* ASCII 字体 — 必须在 deshab_ui.h 之前包含，因为 du_draw_char 引用 g_ascii */
#include "../firstInit/ascii_bitmaps.c"
#include "../UTSM/include/utsm/deshab_ui.h"

/* ============================================================
 *  Block 设备 + FAT32 读写（移植自 DSK，支持 cp/mv/cat/ls）
 * ============================================================ */

typedef int (*shell_block_read_fn)(u32 index, u64 lba, u32 count, void *buf);
typedef int (*shell_block_write_fn)(u32 index, u64 lba, u32 count, const void *buf);

static shell_block_read_fn  g_block_read;
static shell_block_write_fn g_block_write;

/* FAT32 BPB / 目录项结构（与 DSK 一致，packed） */
typedef struct __attribute__((packed)) {
    u8 jmp[3]; char oem[8]; u16 bps; u8 spc; u16 rsvd; u8 fc; u16 root_ent;
    u16 ts16; u8 media; u16 spf16; u16 spt; u16 heads; u32 hidden; u32 ts32;
    u32 spf; u16 flags; u16 ver; u32 root_clus; u16 fsi; u16 bkboot;
    u8 res[12]; u8 drv; u8 ntfl; u8 sig; u32 ser; char lbl[11]; char typ[8];
    u8 code[420]; u16 boot_sig;
} shell_fat32_bpb;

typedef struct __attribute__((packed)) {
    char name[11]; u8 attr; u8 ntr; u8 ctenth;
    u16 ctime; u16 cdate; u16 adate; u16 chigh;
    u16 wtime; u16 wdate; u16 clow; u32 fsize;
} shell_fat32_de;

static u32 sh_r32(const u8 *p) { return (u32)p[0]|((u32)p[1]<<8)|((u32)p[2]<<16)|((u32)p[3]<<24); }
static u16 sh_r16(const u8 *p) { return (u16)p[0]|((u16)p[1]<<8); }
static int sh_neq11(const char *a, const char *b) { for(int i=0;i<11;i++) if(a[i]!=b[i]) return 0; return 1; }

/* 批量读写缓冲区（与 DSK 对齐：256 扇区覆盖 BPB+FAT+根目录+小文件） */
static u8 g_disk[131072];      /* 256 扇区 BPB+FAT+根目录缓存 */
static u8 g_cluster[4096];     /* 单簇缓冲 */
static u8 g_fdata[262144];     /* 文件数据缓冲（最大 256KB） */
static int g_disk_loaded = 0;  /* g_disk 是否已加载 BPB+FAT */

static int sh_read_sectors(u32 lba, u32 count, u8 *out) { return g_block_read ? g_block_read(0,lba,count,out) : -1; }
static int sh_write_sectors(u32 lba, u32 count, const u8 *buf) { return g_block_write ? g_block_write(0,lba,count,buf) : -1; }

/* 确保 g_disk 已加载 BPB+FAT+根目录（256 扇区） */
static int sh_disk_load(void) {
    if (g_disk_loaded) return 0;
    if (sh_read_sectors(0, 256, g_disk) != 0) return -1;
    const shell_fat32_bpb *bpb = (const shell_fat32_bpb *)g_disk;
    if (bpb->boot_sig != 0xAA55 || bpb->bps != 512) return -2;
    if (bpb->spf == 0 || bpb->root_clus < 2) return -3;
    g_disk_loaded = 1;
    return 0;
}

/* 把 "file.txt" / "NAME" 转为 11 字符 8.3 名（大写，空格填充）
 * 返回 0 成功，-1 失败（过长或非法字符） */
static int name_to_83(const char *in, char out[11]) {
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

/* 把 11 字符 8.3 名转为可显示字符串（如 "README.TXT"） */
static void name_from_83(const char in[11], char out[13]) {
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

/* 在根目录簇链中查找 11 字符名，输出首簇号与文件大小 */
static int fat32_find_in_root(const u8 *clus, u32 clus_sectors, const char *target,
                              u32 *out_clus, u32 *out_size, u32 *out_idx) {
    const shell_fat32_de *dir = (const shell_fat32_de *)clus;
    for (u32 e = 0; e * 32 < clus_sectors * 512; e++) {
        if (dir[e].name[0] == 0) break;
        if ((u8)dir[e].name[0] == 0xE5) continue;
        if (dir[e].attr == 0x0F) continue;
        if (dir[e].attr & 0x08) continue;
        if (sh_neq11(dir[e].name, target)) {
            *out_clus = (u32)sh_r16((const u8*)&dir[e].clow) | ((u32)sh_r16((const u8*)&dir[e].chigh) << 16);
            *out_size = dir[e].fsize;
            if (out_idx) *out_idx = e;
            return 0;
        }
    }
    return -1;
}

/* 在目录簇链中查找名为 name11 的子目录（attr & 0x10），返回首簇号。
 * dir_clus: 目录起始簇号。返回 0 成功，-1 未找到。 */
static int fat32_find_dir(u32 dir_clus, const char *name11, u32 *out_clus) {
    if (sh_disk_load() != 0) return -2;
    const shell_fat32_bpb *bpb = (const shell_fat32_bpb *)g_disk;
    u32 data_lba = bpb->rsvd + (u32)bpb->fc * bpb->spf;
    u32 fat_byte_off = bpb->rsvd * 512;
    u32 spc = bpb->spc;
    u32 clus = dir_clus;
    while (clus >= 2 && clus < 0x0FFFFFF8) {
        u32 lba = data_lba + (clus - 2) * spc;
        const u8 *cb;
        if ((u64)lba * 512 + (u64)spc * 512 <= 256ULL * 512)
            cb = g_disk + (u64)lba * 512;
        else {
            if (sh_read_sectors(lba, 8, g_cluster) != 0) return -4;
            cb = g_cluster;
        }
        const shell_fat32_de *dir = (const shell_fat32_de *)cb;
        for (u32 e = 0; e * 32 < spc * 512; e++) {
            if (dir[e].name[0] == 0) goto done_dir;
            if ((u8)dir[e].name[0] == 0xE5) continue;
            if (dir[e].attr == 0x0F) continue;
            if (dir[e].attr & 0x08) continue;
            if (!(dir[e].attr & 0x10)) continue;  /* 只匹配目录 */
            if (sh_neq11(dir[e].name, name11)) {
                *out_clus = (u32)sh_r16((const u8*)&dir[e].clow)
                          | ((u32)sh_r16((const u8*)&dir[e].chigh) << 16);
                return 0;
            }
        }
        u32 fo = fat_byte_off + clus * 4;
        if (fo + 4 > sizeof(g_disk)) break;
        clus = sh_r32(g_disk + fo) & 0x0FFFFFFF;
    }
done_dir:
    return -1;
}

/* 从指定目录首簇读取文件到 g_fdata。
 * dir_clus: 目录起始簇号。name11: 8.3 名。
 * 返回 0 成功，*out_data 指向 g_fdata，*out_size 为字节数；非 0 失败。 */
static int fat32_read_file_in_dir(u32 dir_clus, const char *name11,
                                   u8 **out_data, u32 *out_size) {
    if (sh_disk_load() != 0) return -1;
    const shell_fat32_bpb *bpb = (const shell_fat32_bpb *)g_disk;
    u32 data_lba = bpb->rsvd + (u32)bpb->fc * bpb->spf;
    u32 fat_byte_off = bpb->rsvd * 512;
    u32 spc = bpb->spc;

    u32 clus = dir_clus;
    u32 found_clus = 0, found_size = 0;
    int found = 0;
    while (clus >= 2 && clus < 0x0FFFFFF8 && !found) {
        u32 lba = data_lba + (clus - 2) * spc;
        const u8 *cb;
        if ((u64)lba * 512 + (u64)spc * 512 <= 256ULL * 512)
            cb = g_disk + (u64)lba * 512;
        else {
            if (sh_read_sectors(lba, 8, g_cluster) != 0) return -4;
            cb = g_cluster;
        }
        if (fat32_find_in_root(cb, spc, name11, &found_clus, &found_size, 0) == 0) { found = 1; break; }
        u32 fo = fat_byte_off + clus * 4;
        if (fo + 4 > sizeof(g_disk)) break;
        clus = sh_r32(g_disk + fo) & 0x0FFFFFFF;
    }
    if (!found) return -5;
    if (found_size > sizeof(g_fdata)) return -6;

    u8 *dst = g_fdata; u32 remaining = found_size; u32 fc = found_clus;
    while (fc >= 2 && fc < 0x0FFFFFF8 && remaining > 0) {
        u32 fc_lba = data_lba + (fc - 2) * spc;
        u32 fc_bytes = spc * 512;
        if (fc_bytes > remaining) fc_bytes = remaining;
        const u8 *fb;
        if ((u64)fc_lba * 512 + (u64)spc * 512 <= 256ULL * 512)
            fb = g_disk + (u64)fc_lba * 512;
        else {
            if (sh_read_sectors(fc_lba, 8, g_cluster) != 0) return -7;
            fb = g_cluster;
        }
        for (u32 b = 0; b < fc_bytes; b++) dst[b] = fb[b];
        dst += fc_bytes; remaining -= fc_bytes;
        u32 fo = fat_byte_off + fc * 4;
        if (fo + 4 > sizeof(g_disk)) break;
        fc = sh_r32(g_disk + fo) & 0x0FFFFFFF;
    }
    *out_data = g_fdata; *out_size = found_size;
    return 0;
}

/* 读取根目录下指定 8.3 名文件到 g_fdata。
 * 返回 0 成功，*out_data 指向 g_fdata，*out_size 为字节数；非 0 失败。 */
static int fat32_read_root_file(const char *name11, u8 **out_data, u32 *out_size) {
    if (sh_disk_load() != 0) return -1;
    const shell_fat32_bpb *bpb = (const shell_fat32_bpb *)g_disk;
    return fat32_read_file_in_dir(bpb->root_clus, name11, out_data, out_size);
}

/* /bin 目录首簇号缓存 */
static u32 g_bin_clus = 0;
static int g_bin_checked = 0;

/* 获取 /bin 目录首簇号。返回 0 成功，-1 未找到。 */
static int get_bin_cluster(u32 *out_clus) {
    if (g_bin_checked) {
        if (g_bin_clus == 0) return -1;
        *out_clus = g_bin_clus;
        return 0;
    }
    g_bin_checked = 1;
    if (sh_disk_load() != 0) { g_bin_clus = 0; return -1; }
    const shell_fat32_bpb *bpb = (const shell_fat32_bpb *)g_disk;
    char bin83[11] = {'B','I','N',' ',' ',' ',' ',' ',' ',' ',' '};
    if (fat32_find_dir(bpb->root_clus, bin83, &g_bin_clus) != 0) {
        g_bin_clus = 0;
        return -1;
    }
    *out_clus = g_bin_clus;
    return 0;
}

/* 写入根目录下指定 8.3 名文件（存在则替换，不存在则新建）。
 * 依赖 g_disk 已加载 BPB+FAT。返回 0 成功。 */
static int fat32_write_root_file(const char *name11, const u8 *data, u32 size) {
    if (sh_disk_load() != 0) return -1;
    /* 写入前重新加载 BPB+FAT，保证与盘上一致 */
    if (sh_read_sectors(0, 256, g_disk) != 0) return -2;
    const shell_fat32_bpb *bpb = (const shell_fat32_bpb *)g_disk;
    u32 spc = bpb->spc;
    u32 fat_lba = bpb->rsvd;
    u32 fat_sectors = bpb->spf;
    u32 data_lba = bpb->rsvd + (u32)bpb->fc * bpb->spf;
    u32 fat_byte_off = bpb->rsvd * 512;
    u32 root_clus = bpb->root_clus;
    u32 cluster_bytes = spc * 512;

    u32 root_lba = data_lba + (root_clus - 2) * spc;
    u8 *root_buf = g_disk + (u64)root_lba * 512;
    u32 max_entries = cluster_bytes / 32;
    shell_fat32_de *dir = (shell_fat32_de *)root_buf;
    int free_entry = -1;
    u32 existing_clus = 0;
    int found = 0;
    u32 existing_idx = 0;
    for (u32 e = 0; e < max_entries; e++) {
        if (dir[e].name[0] == 0) { if (free_entry < 0) free_entry = (int)e; break; }
        if ((u8)dir[e].name[0] == 0xE5) { if (free_entry < 0) free_entry = (int)e; continue; }
        if (dir[e].attr == 0x0F) continue;
        if (dir[e].attr & 0x08) continue;
        if (sh_neq11(dir[e].name, name11)) {
            existing_clus = (u32)sh_r16((const u8*)&dir[e].clow) | ((u32)sh_r16((const u8*)&dir[e].chigh) << 16);
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
            if (fo + 4 > sizeof(g_disk)) break;
            cur = sh_r32(g_disk + fo) & 0x0FFFFFFF;
            count++;
        }
        while (count < clusters_needed) {
            u32 newc = 0;
            for (u32 c = 2; c < (fat_sectors * 512) / 4; c++) {
                u32 fo = fat_byte_off + c * 4;
                if (fo + 4 > sizeof(g_disk)) break;
                if ((sh_r32(g_disk + fo) & 0x0FFFFFFF) == 0) { newc = c; break; }
            }
            if (newc == 0) return -3;
            u32 pfo = fat_byte_off + prev_clus * 4;
            if (pfo + 4 <= sizeof(g_disk)) {
                g_disk[pfo] = (u8)(newc & 0xFF);
                g_disk[pfo+1] = (u8)((newc >> 8) & 0xFF);
                g_disk[pfo+2] = (u8)((newc >> 16) & 0xFF);
                g_disk[pfo+3] = (u8)((newc >> 24) & 0x0F);
            }
            prev_clus = newc;
            count++;
        }
        if (prev_clus >= 2) {
            u32 fo = fat_byte_off + prev_clus * 4;
            if (fo + 4 <= sizeof(g_disk)) {
                g_disk[fo] = 0xF8; g_disk[fo+1] = 0xFF; g_disk[fo+2] = 0xFF; g_disk[fo+3] = 0x0F;
            }
        }
        /* 释放多余旧簇 */
        u32 next = 0;
        u32 fo = fat_byte_off + prev_clus * 4;
        if (fo + 4 <= sizeof(g_disk)) next = sh_r32(g_disk + fo) & 0x0FFFFFFF;
        while (next >= 2 && next < 0x0FFFFFF8) {
            u32 nfo = fat_byte_off + next * 4;
            u32 nn = 0;
            if (nfo + 4 <= sizeof(g_disk)) nn = sh_r32(g_disk + nfo) & 0x0FFFFFFF;
            g_disk[nfo] = 0; g_disk[nfo+1] = 0; g_disk[nfo+2] = 0; g_disk[nfo+3] = 0;
            next = nn;
        }
    } else {
        if (free_entry < 0) return -4;
        for (u32 i = 0; i < clusters_needed; i++) {
            u32 newc = 0;
            for (u32 c = 2; c < (fat_sectors * 512) / 4; c++) {
                u32 fo = fat_byte_off + c * 4;
                if (fo + 4 > sizeof(g_disk)) break;
                if ((sh_r32(g_disk + fo) & 0x0FFFFFFF) == 0) { newc = c; break; }
            }
            if (newc == 0) return -3;
            if (i == 0) first_clus = newc;
            if (prev_clus >= 2) {
                u32 pfo = fat_byte_off + prev_clus * 4;
                g_disk[pfo] = (u8)(newc & 0xFF);
                g_disk[pfo+1] = (u8)((newc >> 8) & 0xFF);
                g_disk[pfo+2] = (u8)((newc >> 16) & 0xFF);
                g_disk[pfo+3] = (u8)((newc >> 24) & 0x0F);
            }
            prev_clus = newc;
        }
        if (prev_clus >= 2) {
            u32 fo = fat_byte_off + prev_clus * 4;
            g_disk[fo] = 0xF8; g_disk[fo+1] = 0xFF; g_disk[fo+2] = 0xFF; g_disk[fo+3] = 0x0F;
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
            u8 *dst = g_disk + (u64)clba * 512;
            for (u32 b = 0; b < chunk; b++) dst[b] = src[b];
            for (u32 b = chunk; b < cluster_bytes; b++) dst[b] = 0;
        } else {
            if (sh_read_sectors(clba, spc, g_cluster) != 0) return -5;
            for (u32 b = 0; b < chunk; b++) g_cluster[b] = src[b];
            for (u32 b = chunk; b < cluster_bytes; b++) g_cluster[b] = 0;
            if (sh_write_sectors(clba, spc, g_cluster) != 0) return -6;
        }
        src += chunk; remaining -= chunk; ci++;
        u32 fo = fat_byte_off + cur * 4;
        if (fo + 4 > sizeof(g_disk)) break;
        cur = sh_r32(g_disk + fo) & 0x0FFFFFFF;
    }

    /* 更新目录项 */
    u32 entry_idx = found ? existing_idx : (u32)free_entry;
    shell_fat32_de *e = &dir[entry_idx];
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
        if (sh_write_sectors(flba, fat_sectors, g_disk + (u64)flba * 512) != 0) return -7;
    }
    u32 root_dir_lba = data_lba + (root_clus - 2) * spc;
    if ((u64)root_dir_lba * 512 + cluster_bytes <= 256ULL * 512) {
        if (sh_write_sectors(root_dir_lba, spc, g_disk + (u64)root_dir_lba * 512) != 0) return -8;
    }
    cur = first_clus; ci = 0;
    while (cur >= 2 && cur < 0x0FFFFFF8 && ci < clusters_needed) {
        u32 clba = data_lba + (cur - 2) * spc;
        if ((u64)clba * 512 + cluster_bytes <= 256ULL * 512) {
            if (sh_write_sectors(clba, spc, g_disk + (u64)clba * 512) != 0) return -9;
        }
        u32 fo = fat_byte_off + cur * 4;
        if (fo + 4 > sizeof(g_disk)) break;
        cur = sh_r32(g_disk + fo) & 0x0FFFFFFF;
        ci++;
    }
    g_disk_loaded = 0;  /* 下次操作重新加载，避免使用脏缓存 */
    return 0;
}

/* 删除根目录下指定 8.3 名文件：清空簇链 + 标记目录项为 0xE5。
 * 返回 0 成功，-1 未找到，其他负值失败。 */
static int fat32_delete_root_file(const char *name11) {
    if (sh_disk_load() != 0) return -2;
    if (sh_read_sectors(0, 256, g_disk) != 0) return -3;
    const shell_fat32_bpb *bpb = (const shell_fat32_bpb *)g_disk;
    u32 spc = bpb->spc;
    u32 fat_lba = bpb->rsvd;
    u32 fat_sectors = bpb->spf;
    u32 data_lba = bpb->rsvd + (u32)bpb->fc * bpb->spf;
    u32 fat_byte_off = bpb->rsvd * 512;
    u32 root_clus = bpb->root_clus;

    u32 root_lba = data_lba + (root_clus - 2) * spc;
    u8 *root_buf = g_disk + (u64)root_lba * 512;
    u32 max_entries = (spc * 512) / 32;
    shell_fat32_de *dir = (shell_fat32_de *)root_buf;
    u32 found_idx = 0xFFFFFFFFu;
    for (u32 e = 0; e < max_entries; e++) {
        if (dir[e].name[0] == 0) break;
        if ((u8)dir[e].name[0] == 0xE5) continue;
        if (dir[e].attr == 0x0F) continue;
        if (dir[e].attr & 0x08) continue;
        if (sh_neq11(dir[e].name, name11)) { found_idx = e; break; }
    }
    if (found_idx == 0xFFFFFFFFu) return -1;

    /* 释放簇链 */
    u32 cur = (u32)sh_r16((const u8*)&dir[found_idx].clow) | ((u32)sh_r16((const u8*)&dir[found_idx].chigh) << 16);
    while (cur >= 2 && cur < 0x0FFFFFF8) {
        u32 fo = fat_byte_off + cur * 4;
        u32 next = 0;
        if (fo + 4 <= sizeof(g_disk)) next = sh_r32(g_disk + fo) & 0x0FFFFFFF;
        g_disk[fo] = 0; g_disk[fo+1] = 0; g_disk[fo+2] = 0; g_disk[fo+3] = 0;
        cur = next;
    }
    /* 标记目录项为已删除 */
    dir[found_idx].name[0] = (char)0xE5;

    /* 回写 FAT + 根目录 */
    for (u32 f = 0; f < bpb->fc; f++) {
        u32 flba = fat_lba + f * fat_sectors;
        if (sh_write_sectors(flba, fat_sectors, g_disk + (u64)flba * 512) != 0) return -4;
    }
    if (sh_write_sectors(root_lba, spc, g_disk + (u64)root_lba * 512) != 0) return -5;
    g_disk_loaded = 0;
    return 0;
}

/* 列出根目录所有文件/目录，调用回调输出 */
static int fat32_list_root(int (*emit)(const char *name, u32 size, u8 attr, void *u), void *u) {
    if (sh_disk_load() != 0) return -1;
    const shell_fat32_bpb *bpb = (const shell_fat32_bpb *)g_disk;
    u32 data_lba = bpb->rsvd + (u32)bpb->fc * bpb->spf;
    u32 fat_byte_off = bpb->rsvd * 512;
    u32 spc = bpb->spc;
    u32 clus = bpb->root_clus;
    while (clus >= 2 && clus < 0x0FFFFFF8) {
        u32 lba = data_lba + (clus - 2) * spc;
        const u8 *cb;
        if ((u64)lba * 512 + (u64)spc * 512 <= 256ULL * 512)
            cb = g_disk + (u64)lba * 512;
        else {
            if (sh_read_sectors(lba, 8, g_cluster) != 0) return -4;
            cb = g_cluster;
        }
        const shell_fat32_de *dir = (const shell_fat32_de *)cb;
        for (u32 e = 0; e * 32 < spc * 512; e++) {
            if (dir[e].name[0] == 0) goto done;
            if ((u8)dir[e].name[0] == 0xE5) continue;
            if (dir[e].attr == 0x0F) continue;
            if (dir[e].attr & 0x08) continue;
            char disp[13];
            name_from_83(dir[e].name, disp);
            if (emit(disp, dir[e].fsize, dir[e].attr, u) != 0) goto done;
        }
        u32 fo = fat_byte_off + clus * 4;
        if (fo + 4 > sizeof(g_disk)) break;
        clus = sh_r32(g_disk + fo) & 0x0FFFFFFF;
    }
done:
    return 0;
}

/* ---- 风格令牌（引用 deshab_ui.h 语义色） ---- */
#define BG_COLOR      DS_DARK_BG_PRIMARY
#define TEXT_FG       DS_DARK_TEXT_PRIMARY
#define PROMPT_FG     DS_DARK_PROMPT
#define ERROR_FG      DP_ERROR
#define OK_FG         DP_SUCCESS
#define DIM_FG        DS_DARK_TEXT_DIM
#define ACCENT_FG     DS_DARK_ACCENT
#define DECORATIVE_FG DS_DARK_DECORATIVE

/* 标题栏参数 */
#define TITLEBAR_H    28
#define TITLEBAR_BG   DP_ABYSS_800
#define TITLEBAR_FG   DS_DARK_TEXT_SECONDARY
#define TITLEBAR_ACCENT DS_DARK_ACCENT

/* 状态栏参数 */
#define STATUSBAR_H   22
#define STATUSBAR_BG  DP_ABYSS_800

/* 终端尺寸 */
#define TERM_COLS  100
#define TERM_ROWS  35
#define TERM_MAX_CHARS (TERM_COLS * TERM_ROWS)

static du_context g_ctx;
static u64 fb_a, fb_w, fb_h, fb_p;

/* 终端缓冲区 — 字符 + 颜色 */
static u8  term_ch[TERM_MAX_CHARS];
static u32 term_fg[TERM_MAX_CHARS];
static int term_w = TERM_COLS;
static int term_h = TERM_ROWS;
static int cur_col = 0;
static int cur_row = 0;
static int cur_visible = 1;

/* ---- 终端像素坐标 ---- */
static int term_start_x, term_start_y;

static void term_init_pos(void) {
    int margin_x = (int)DU_SPACE_MD;
    term_start_x = margin_x;
    /* 标题栏下方开始 */
    term_start_y = TITLEBAR_H + (int)DU_SPACE_SM;
    int avail_w = (int)fb_w - margin_x * 2;
    int avail_h = (int)fb_h - term_start_y - STATUSBAR_H - (int)DU_SPACE_SM;
    term_w = avail_w / (int)DU_ASCII_STEP;
    term_h = avail_h / (int)DU_ASCII_LINE_H;
    if (term_w < 10) term_w = 10;
    if (term_h < 5) term_h = 5;
    if (term_w > TERM_COLS) term_w = TERM_COLS;
    if (term_h > TERM_ROWS) term_h = TERM_ROWS;
}

static void term_clear(void) {
    for (int i = 0; i < TERM_MAX_CHARS; i++) {
        term_ch[i] = ' ';
        term_fg[i] = TEXT_FG;
    }
    cur_col = 0;
    cur_row = 0;
}

static void term_scroll(void) {
    for (int r = 1; r < term_h; r++) {
        for (int c = 0; c < term_w; c++) {
            int src = r * TERM_COLS + c;
            int dst = (r-1) * TERM_COLS + c;
            term_ch[dst] = term_ch[src];
            term_fg[dst] = term_fg[src];
        }
    }
    for (int c = 0; c < term_w; c++) {
        term_ch[(term_h-1) * TERM_COLS + c] = ' ';
        term_fg[(term_h-1) * TERM_COLS + c] = TEXT_FG;
    }
    cur_row = term_h - 1;
    cur_col = 0;
}

static void term_newline(void) {
    cur_col = 0;
    cur_row++;
    if (cur_row >= term_h) term_scroll();
}

/* dev_mode 串口镜像：reserved[3]=1 时终端输出同步写 COM1，便于自动化验证 */
static int g_serial_mirror = 0;

static void term_putc_color(char c, u32 color) {
    if (g_serial_mirror) { if (c == '\n') sputc('\r'); sputc(c); }
    if (c == '\n') { term_newline(); return; }
    if (c == '\r') { cur_col = 0; return; }
    if (c == '\t') {
        int spaces = 4 - (cur_col % 4);
        for (int i = 0; i < spaces; i++) term_putc_color(' ', color);
        return;
    }
    if (c < ' ' || c > '~') c = '?';
    if (cur_col >= term_w) term_newline();
    int idx = cur_row * TERM_COLS + cur_col;
    term_ch[idx] = (u8)c;
    term_fg[idx] = color;
    cur_col++;
}

static void term_putc(char c) { term_putc_color(c, TEXT_FG); }
static void term_puts_color(const char *s, u32 color) { while (*s) term_putc_color(*s++, color); }
static void term_puts(const char *s) { term_puts_color(s, TEXT_FG); }

/* ---- 标题栏渲染 ---- */
static void draw_titlebar(void) {
    du_fill_rect(&g_ctx, 0, 0, (i64)fb_w, TITLEBAR_H, TITLEBAR_BG);
    /* 底部强调线 */
    du_fill_rect(&g_ctx, 0, TITLEBAR_H - 2, (i64)fb_w, 2, TITLEBAR_ACCENT);
    /* 左侧标题文字 */
    du_draw_string(&g_ctx, "Deshab Shell",
                   (i64)DU_SPACE_MD, (TITLEBAR_H - (i64)DU_ASCII_CELL_H) / 2,
                   DS_DARK_TEXT_PRIMARY, TITLEBAR_BG, DU_ASCII_STEP);
    /* 右侧版本号 */
    du_draw_string(&g_ctx, "v0.1.0",
                   (i64)fb_w - 6 * (i64)DU_ASCII_STEP - (i64)DU_SPACE_MD,
                   (TITLEBAR_H - (i64)DU_ASCII_CELL_H) / 2,
                   DS_DARK_TEXT_DIM, TITLEBAR_BG, DU_ASCII_STEP);
}

/* ---- 状态栏渲染 ---- */
static void draw_statusbar(void) {
    i64 sy = (i64)fb_h - STATUSBAR_H;
    du_fill_rect(&g_ctx, 0, sy, (i64)fb_w, STATUSBAR_H, STATUSBAR_BG);
    /* 顶部强调线 */
    du_fill_rect(&g_ctx, 0, sy, (i64)fb_w, 1, DS_DARK_DIVIDER);
    /* 左侧：当前路径 */
    du_draw_string(&g_ctx, "/",
                   (i64)DU_SPACE_MD, sy + (STATUSBAR_H - (i64)DU_ASCII_CELL_H) / 2,
                   DS_DARK_TEXT_DIM, STATUSBAR_BG, DU_ASCII_STEP);
    /* 右侧：root@deshab */
    const char *user = "root@deshab";
    int ulen = 0;
    while (user[ulen]) ulen++;
    du_draw_string(&g_ctx, user,
                   (i64)fb_w - ulen * (i64)DU_ASCII_STEP - (i64)DU_SPACE_MD,
                   sy + (STATUSBAR_H - (i64)DU_ASCII_CELL_H) / 2,
                   DS_DARK_ACCENT, STATUSBAR_BG, DU_ASCII_STEP);
}

static void term_redraw_all(void) {
    /* 清屏背景 */
    du_fill_bg_solid(&g_ctx, BG_COLOR);
    /* 标题栏 */
    draw_titlebar();
    /* 渲染所有字符 */
    for (int r = 0; r < term_h; r++) {
        for (int c = 0; c < term_w; c++) {
            int idx = r * TERM_COLS + c;
            u8 ch = term_ch[idx];
            if (ch == ' ') continue;
            i64 x = term_start_x + (i64)c * (i64)DU_ASCII_STEP;
            i64 y = term_start_y + (i64)r * (i64)DU_ASCII_LINE_H;
            du_draw_char(&g_ctx, ch, x, y, term_fg[idx], BG_COLOR);
        }
    }
    /* 光标 */
    if (cur_visible && cur_col < term_w && cur_row < term_h) {
        i64 x = term_start_x + (i64)cur_col * (i64)DU_ASCII_STEP;
        i64 y = term_start_y + (i64)cur_row * (i64)DU_ASCII_LINE_H;
        du_fill_rect(&g_ctx, x, y + (i64)DU_ASCII_CELL_H - 3,
                     (i64)DU_ASCII_CELL_W, 2, DS_DARK_CURSOR);
    }
    /* 状态栏 */
    draw_statusbar();
}

static void term_redraw_cursor(void) {
    if (cur_visible && cur_col < term_w && cur_row < term_h) {
        i64 x = term_start_x + (i64)cur_col * (i64)DU_ASCII_STEP;
        i64 y = term_start_y + (i64)cur_row * (i64)DU_ASCII_LINE_H;
        du_fill_rect(&g_ctx, x, y + (i64)DU_ASCII_CELL_H - 3,
                     (i64)DU_ASCII_CELL_W, 2, DS_DARK_CURSOR);
    }
}

static void term_clear_cursor(void) {
    if (cur_col < term_w && cur_row < term_h) {
        i64 x = term_start_x + (i64)cur_col * (i64)DU_ASCII_STEP;
        i64 y = term_start_y + (i64)cur_row * (i64)DU_ASCII_LINE_H;
        du_fill_rect(&g_ctx, x, y + (i64)DU_ASCII_CELL_H - 3,
                     (i64)DU_ASCII_CELL_W, 2, BG_COLOR);
    }
}

/* ---- 输入缓冲 ---- */
static char input_buf[256];
static int input_len = 0;
static int input_cursor = 0;

/* ---- 命令历史（上下箭头浏览） ---- */
/* 前向声明: redraw_input_line 在下方定义 */
static void redraw_input_line(void);

#define HISTORY_MAX 32
static char g_history[HISTORY_MAX][256];
static int g_history_count = 0;   /* 已记录命令条数 (0..HISTORY_MAX) */
static int g_history_view = -1;  /* 当前浏览索引, -1 表示正在编辑新输入 */

/* 将命令推入历史。空字符串或纯空白不入历史；与最近一条相同时跳过。 */
static void history_push(const char *cmd) {
    const char *p = cmd;
    while (*p == ' ') p++;
    if (*p == 0) return;
    /* 与最近一条相同则跳过 */
    if (g_history_count > 0) {
        const char *latest = g_history[(g_history_count - 1) % HISTORY_MAX];
        const char *a = latest;
        const char *b = p;
        while (*a && *b && *a == *b) { a++; b++; }
        if (*a == 0 && *b == 0) return;
    }
    int idx = g_history_count % HISTORY_MAX;
    int i = 0;
    while (p[i] && i < 255) { g_history[idx][i] = p[i]; i++; }
    g_history[idx][i] = 0;
    if (g_history_count < HISTORY_MAX) g_history_count++;
}

/* 加载历史条目到 input_buf 并刷新显示 */
static void history_load(int idx) {
    const char *src = g_history[idx % HISTORY_MAX];
    int i = 0;
    while (src[i] && i < 255) { input_buf[i] = src[i]; i++; }
    input_buf[i] = 0;
    input_len = i;
    input_cursor = i;
    redraw_input_line();
}

/* 上箭头: 浏览更早的命令 */
static void history_prev(void) {
    if (g_history_count == 0) return;
    if (g_history_view == -1) {
        g_history_view = g_history_count - 1;
    } else if (g_history_view > 0) {
        g_history_view--;
    } else {
        return;  /* 已到达最早一条 */
    }
    history_load(g_history_view);
}

/* 下箭头: 浏览更新的命令; 超过最新则回到空输入 */
static void history_next(void) {
    if (g_history_view == -1) return;
    g_history_view++;
    if (g_history_view >= g_history_count) {
        g_history_view = -1;
        input_len = 0;
        input_cursor = 0;
        input_buf[0] = 0;
        redraw_input_line();
        return;
    }
    history_load(g_history_view);
}

/* ---- 键盘扫描码 → ASCII（Set 1） ---- */
static char scan_to_ascii(u8 sc, int shift) {
    static const char normal[58] = {
        0, 27, '1','2','3','4','5','6','7','8','9','0','-','=', 8, '\t',
        'q','w','e','r','t','y','u','i','o','p','[',']','\n',0,
        'a','s','d','f','g','h','j','k','l',';','\'', '`',0,'\\',
        'z','x','c','v','b','n','m',',','.','/',0,'*',0,' '
    };
    static const char shifted[58] = {
        0, 27, '!','@','#','$','%','^','&','*','(',')','_','+', 8, '\t',
        'Q','W','E','R','T','Y','U','I','O','P','{','}','\n',0,
        'A','S','D','F','G','H','J','K','L',':','"','~',0,'|',
        'Z','X','C','V','B','N','M','<','>','?',0,'*',0,' '
    };
    if (sc >= 58) return 0;
    return shift ? shifted[sc] : normal[sc];
}

/* ---- 内置命令 ---- */
static int str_eq(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

static void cmd_help(void) {
    term_puts_color("Deshab Shell — 内置命令列表:\n", PROMPT_FG);
    term_puts("  help          显示此帮助\n");
    term_puts("  clear         清屏\n");
    term_puts("  echo <text>   显示文本（-n 不换行, > file 重定向写盘）\n");
    term_puts("  ls / dir      列出根目录文件\n");
    term_puts("  cat <file>    显示文件内容\n");
    term_puts("  cp <s> <d>    复制文件\n");
    term_puts("  mv <s> <d>    移动/重命名文件\n");
    term_puts("  rm <file>     删除文件\n");
    term_puts("  pwd           显示当前目录\n");
    term_puts("  whoami        显示当前用户\n");
    term_puts("  id            显示用户ID\n");
    term_puts("  version/ver   显示系统版本\n");
    term_puts("  uname         显示系统名称\n");
    term_puts("  date          显示当前日期\n");
    term_puts("  about         关于 Deshab\n");
    term_puts("  pci           列出 PCI 设备\n");
    term_puts("  run <NAME.ELF> 运行根目录下的工具程序\n");
    term_puts("  linux <prog> [args] 通过双内核兼容层运行 Linux 程序\n");
    term_puts("  reboot        重启系统\n");
    term_puts("  halt          关机（停止 CPU）\n");
    term_putc('\n');
    term_puts_color("外部命令（/bin/ 目录，PATH 自动查找）:\n", DIM_FG);
    term_puts("  ping          ICMP echo 测试工具\n");
    term_puts("  curl          HTTP GET 请求工具\n");
}

static void cmd_version(void) {
    term_puts_color("Deshab OS v0.1.0\n", OK_FG);
    term_puts("  Kernel: DSK (Deshab System Kernel)\n");
    term_puts("  Architecture: x86_64\n");
    term_puts("  ABI: DKM (Deshab Kernel Module) v1\n");
    term_puts("  Bootloader: Limine\n");
    term_puts_color("  Style: Sealed Arc (Deshab UI)\n", DIM_FG);
}

static void cmd_uname(void) {
    term_puts("Deshab\n");
}

static void cmd_about(void) {
    term_puts_color("=== Deshab OS ===\n", PROMPT_FG);
    term_puts("单地址空间 Ring0 内核实验系统\n");
    term_puts("架构: UTSM + DSK + DKM 驱动模型\n");
    term_puts_color("视觉: Sealed Arc 设计语言\n", DECORATIVE_FG);
    term_puts_color("Built with Clang + lld\n", DIM_FG);
    term_puts_color("(c) 2026 Deshab Project\n", DIM_FG);
}

static void cmd_clear(void) {
    term_clear();
    term_redraw_all();
}

static void cmd_reboot(void) {
    term_puts_color("Rebooting...\n", PROMPT_FG);
    term_redraw_all();
    outb(0x64, 0xFE);
    __asm__ volatile("int $0x03");
    for(;;) __asm__("hlt");
}

static void cmd_halt(void) {
    term_puts_color("System halted.\n", PROMPT_FG);
    term_redraw_all();
    for(;;) __asm__("hlt");
}

/* echo：支持 -n（不换行）和 " > file" 重定向写盘 */
static void cmd_echo(const char *args) {
    int newline = 1;
    if (args && args[0] == '-' && args[1] == 'n' && (args[2] == ' ' || args[2] == 0)) {
        newline = 0;
        args += 2;
        while (*args == ' ') args++;
    }
    /* 检测 " > file" 重定向 */
    const char *redir = 0;
    if (args) {
        const char *p = args;
        while (*p) { if (*p == '>') { redir = p; break; } p++; }
    }
    if (redir) {
        int text_len = (int)(redir - args);
        while (text_len > 0 && args[text_len-1] == ' ') text_len--;
        const char *fname = redir + 1;
        while (*fname == ' ') fname++;
        char fnbuf[64]; int fl = 0;
        while (*fname && *fname != ' ' && fl < 63) fnbuf[fl++] = *fname++;
        fnbuf[fl] = 0;
        if (fl == 0) { term_puts_color("echo: 重定向缺少文件名\n", ERROR_FG); return; }
        char n83[11];
        if (name_to_83(fnbuf, n83) != 0) { term_puts_color("echo: 无效文件名\n", ERROR_FG); return; }
        static char echo_buf[4096];
        int el = 0;
        for (int i = 0; i < text_len && el < 4095; i++) echo_buf[el++] = args[i];
        if (newline) echo_buf[el++] = '\n';
        if (!g_block_write) { term_puts_color("echo: 无块设备写能力\n", ERROR_FG); return; }
        int rc = fat32_write_root_file(n83, (const u8 *)echo_buf, (u32)el);
        if (rc != 0) { term_puts_color("echo: 写入失败\n", ERROR_FG); return; }
        term_puts_color("echo: 已写入 ", OK_FG);
        term_puts_color(fnbuf, OK_FG);
        term_putc('\n');
        return;
    }
    if (args && *args) term_puts_color(args, TEXT_FG);
    if (newline) term_putc('\n');
}

static u8 read_rtc_reg(u8 reg);
static u8 bcd_to_bin_impl(u8 bcd);

static void cmd_date(void) {
    u8 year = bcd_to_bin_impl(read_rtc_reg(9));
    u8 month = bcd_to_bin_impl(read_rtc_reg(8));
    u8 day = bcd_to_bin_impl(read_rtc_reg(7));
    u8 hour = bcd_to_bin_impl(read_rtc_reg(4));
    u8 minute = bcd_to_bin_impl(read_rtc_reg(2));
    u8 second = bcd_to_bin_impl(read_rtc_reg(0));
    char buf[32];
    int p = 0;
    buf[p++] = '2'; buf[p++] = '0';
    buf[p++] = '0' + year / 10; buf[p++] = '0' + year % 10;
    buf[p++] = '-';
    buf[p++] = '0' + month / 10; buf[p++] = '0' + month % 10;
    buf[p++] = '-';
    buf[p++] = '0' + day / 10; buf[p++] = '0' + day % 10;
    buf[p++] = ' ';
    buf[p++] = '0' + hour / 10; buf[p++] = '0' + hour % 10;
    buf[p++] = ':';
    buf[p++] = '0' + minute / 10; buf[p++] = '0' + minute % 10;
    buf[p++] = ':';
    buf[p++] = '0' + second / 10; buf[p++] = '0' + second % 10;
    buf[p] = 0;
    term_puts(buf);
    term_putc('\n');
}

static void cmd_unknown(const char *cmd) {
    term_puts_color("未知命令: ", ERROR_FG);
    term_puts_color(cmd, ERROR_FG);
    term_putc('\n');
    term_puts_color("输入 'help' 查看可用命令\n", DIM_FG);
}

static void cmd_not_impl(const char *cmd) {
    term_puts_color(cmd, ERROR_FG);
    term_puts_color(": 此命令尚未实现\n", ERROR_FG);
}

/* ============================================================
 *  文件命令：ls / cat / cp / mv / rm（FAT32 根目录）
 * ============================================================ */

/* 从 args 解析一个空白分隔 token 到 out(最多 max-1 字符)，返回剩余 args 指针 */
static const char *parse_token(const char *args, char *out, int max) {
    int n = 0;
    while (*args == ' ') args++;
    while (*args && *args != ' ' && n < max - 1) out[n++] = *args++;
    out[n] = 0;
    return args;
}

/* u32 → 十进制字符串 */
static void u32_to_dec(char *buf, u32 v) {
    char tmp[12]; int n = 0;
    if (v == 0) { buf[0] = '0'; buf[1] = 0; return; }
    while (v && n < 11) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    for (int i = 0; i < n; i++) buf[i] = tmp[n - 1 - i];
    buf[n] = 0;
}

/* ls 回调：每行打印一个条目 */
static int ls_emit(const char *name, u32 size, u8 attr, void *u) {
    (void)u;
    if (attr & 0x10) term_puts_color("[DIR] ", ACCENT_FG);
    else             term_puts_color("      ", TEXT_FG);
    term_puts_color(name, TEXT_FG);
    /* 名字对齐填充 */
    int nl = 0; while (name[nl]) nl++;
    for (int i = nl; i < 14; i++) term_putc(' ');
    if (!(attr & 0x10)) {
        char sz[12]; u32_to_dec(sz, size);
        term_puts_color(sz, DIM_FG);
        term_puts_color(" B", DIM_FG);
    }
    term_putc('\n');
    return 0;
}

static void cmd_ls(void) {
    if (!g_block_read) { term_puts_color("ls: 无块设备\n", ERROR_FG); return; }
    if (fat32_list_root(ls_emit, 0) != 0) {
        term_puts_color("ls: 读取根目录失败\n", ERROR_FG);
    }
}

static void cmd_cat(const char *args) {
    if (!g_block_read) { term_puts_color("cat: 无块设备\n", ERROR_FG); return; }
    char fn[64];
    parse_token(args, fn, 64);
    if (fn[0] == 0) { term_puts_color("用法: cat <文件>\n", ERROR_FG); return; }
    char n83[11];
    if (name_to_83(fn, n83) != 0) { term_puts_color("cat: 无效文件名\n", ERROR_FG); return; }
    u8 *data = 0; u32 size = 0;
    if (fat32_read_root_file(n83, &data, &size) != 0) {
        term_puts_color("cat: 文件不存在\n", ERROR_FG);
        return;
    }
    for (u32 i = 0; i < size; i++) term_putc((char)data[i]);
}

static void cmd_cp(const char *args) {
    if (!g_block_read || !g_block_write) { term_puts_color("cp: 需要块设备读写能力\n", ERROR_FG); return; }
    char src[64], dst[64];
    args = parse_token(args, src, 64);
    parse_token(args, dst, 64);
    if (src[0] == 0 || dst[0] == 0) {
        term_puts_color("用法: cp <源文件> <目标文件>\n", ERROR_FG);
        return;
    }
    char s83[11], d83[11];
    if (name_to_83(src, s83) != 0) { term_puts_color("cp: 无效源文件名\n", ERROR_FG); return; }
    if (name_to_83(dst, d83) != 0) { term_puts_color("cp: 无效目标文件名\n", ERROR_FG); return; }
    u8 *data = 0; u32 size = 0;
    if (fat32_read_root_file(s83, &data, &size) != 0) {
        term_puts_color("cp: 源文件不存在或读取失败\n", ERROR_FG);
        return;
    }
    if (fat32_write_root_file(d83, data, size) != 0) {
        term_puts_color("cp: 写入目标失败\n", ERROR_FG);
        return;
    }
    term_puts_color("cp: ", OK_FG);
    term_puts_color(src, OK_FG);
    term_puts_color(" -> ", OK_FG);
    term_puts_color(dst, OK_FG);
    term_puts_color(" (", DIM_FG);
    char sz[12]; u32_to_dec(sz, size);
    term_puts_color(sz, DIM_FG);
    term_puts_color(" bytes)\n", DIM_FG);
}

static void cmd_mv(const char *args) {
    if (!g_block_read || !g_block_write) { term_puts_color("mv: 需要块设备读写能力\n", ERROR_FG); return; }
    char src[64], dst[64];
    args = parse_token(args, src, 64);
    parse_token(args, dst, 64);
    if (src[0] == 0 || dst[0] == 0) {
        term_puts_color("用法: mv <源文件> <目标文件>\n", ERROR_FG);
        return;
    }
    char s83[11], d83[11];
    if (name_to_83(src, s83) != 0) { term_puts_color("mv: 无效源文件名\n", ERROR_FG); return; }
    if (name_to_83(dst, d83) != 0) { term_puts_color("mv: 无效目标文件名\n", ERROR_FG); return; }
    u8 *data = 0; u32 size = 0;
    if (fat32_read_root_file(s83, &data, &size) != 0) {
        term_puts_color("mv: 源文件不存在或读取失败\n", ERROR_FG);
        return;
    }
    if (fat32_write_root_file(d83, data, size) != 0) {
        term_puts_color("mv: 写入目标失败\n", ERROR_FG);
        return;
    }
    /* 写入后 g_disk 缓存已失效，delete 会重新加载 BPB+FAT */
    if (fat32_delete_root_file(s83) != 0) {
        term_puts_color("mv: 警告 — 源文件删除失败，目标已写入\n", DP_WARNING);
        return;
    }
    term_puts_color("mv: ", OK_FG);
    term_puts_color(src, OK_FG);
    term_puts_color(" -> ", OK_FG);
    term_puts_color(dst, OK_FG);
    term_putc('\n');
}

static void cmd_rm(const char *args) {
    if (!g_block_write) { term_puts_color("rm: 需要块设备写能力\n", ERROR_FG); return; }
    char fn[64];
    parse_token(args, fn, 64);
    if (fn[0] == 0) { term_puts_color("用法: rm <文件>\n", ERROR_FG); return; }
    char n83[11];
    if (name_to_83(fn, n83) != 0) { term_puts_color("rm: 无效文件名\n", ERROR_FG); return; }
    int rc = fat32_delete_root_file(n83);
    if (rc == -1) { term_puts_color("rm: 文件不存在\n", ERROR_FG); return; }
    if (rc != 0) { term_puts_color("rm: 删除失败\n", ERROR_FG); return; }
    term_puts_color("rm: 已删除 ", OK_FG);
    term_puts_color(fn, OK_FG);
    term_putc('\n');
}

/* ---- PCI 设备列表 ---- */
static u32 pci_config_read(u8 bus, u8 dev, u8 func, u8 reg) {
    u32 addr = 0x80000000u | ((u32)bus << 16) | ((u32)dev << 11)
             | ((u32)func << 8) | ((u32)reg & 0xFCu);
    outl(0xCF8, addr);
    return inl(0xCFC);
}

static void hex_nibble(char *buf, u8 v) {
    v = (u8)(v & 0x0F);
    buf[0] = (char)(v < 10 ? '0' + v : 'A' + (v - 10));
}

static void u8_to_hex(char *buf, u8 v) {
    hex_nibble(buf, (u8)(v >> 4));
    hex_nibble(buf + 1, v);
    buf[2] = 0;
}

static void u16_to_hex(char *buf, u16 v) {
    hex_nibble(buf,   (u8)(v >> 12));
    hex_nibble(buf+1, (u8)(v >> 8));
    hex_nibble(buf+2, (u8)(v >> 4));
    hex_nibble(buf+3, v);
    buf[4] = 0;
}

static void u8_to_dec(char *buf, u8 v) {
    if (v >= 100) {
        buf[0] = (char)('0' + v / 100);
        buf[1] = (char)('0' + (v / 10) % 10);
        buf[2] = (char)('0' + v % 10);
        buf[3] = 0;
    } else if (v >= 10) {
        buf[0] = (char)('0' + v / 10);
        buf[1] = (char)('0' + v % 10);
        buf[2] = 0;
    } else {
        buf[0] = (char)('0' + v);
        buf[1] = 0;
    }
}

static const char *pci_vendor_name(u16 vendor) {
    switch (vendor) {
        case 0x8086: return "Intel";
        case 0x1234: return "Bochs/QEMU";
        case 0x1AF4: return "Virtio";
        case 0x168C: return "Atheros";
        case 0x10DE: return "NVIDIA";
        case 0x1002: return "AMD";
        case 0x10EC: return "Realtek";
        case 0x14E4: return "Broadcom";
        case 0x1B36: return "Red Hat";
        case 0x1022: return "AMD-x86";
        case 0x106B: return "Apple";
        default: return "";
    }
}

static const char *pci_class_name(u8 class_code) {
    switch (class_code) {
        case 0x00: return "Unclassified";
        case 0x01: return "Mass storage";
        case 0x02: return "Network";
        case 0x03: return "Display";
        case 0x04: return "Multimedia";
        case 0x05: return "Memory";
        case 0x06: return "Bridge";
        case 0x07: return "Comm";
        case 0x08: return "Sys peripheral";
        case 0x09: return "Input";
        case 0x0C: return "Serial bus";
        case 0x0D: return "Wireless";
        case 0x10: return "Encryption";
        case 0x11: return "Signal proc";
        case 0x40: return "Coprocessor";
        default: return "Other";
    }
}

static void cmd_pci(void) {
    int count = 0;
    term_puts_color("PCI 设备列表:\n", PROMPT_FG);
    for (u8 bus = 0; bus < 1; bus++) {
        for (u8 dev = 0; dev < 32; dev++) {
            u32 vd = pci_config_read(bus, dev, 0, 0x00);
            if ((vd & 0xFFFFu) == 0xFFFFu) continue;
            u32 hdr_class = pci_config_read(bus, dev, 0, 0x0C);
            u8 header_type = (u8)((hdr_class >> 16) & 0xFF);
            u8 func_count = (u8)((header_type & 0x80) ? 8 : 1);
            for (u8 func = 0; func < func_count; func++) {
                u32 vdf = pci_config_read(bus, dev, func, 0x00);
                u16 vendor = (u16)(vdf & 0xFFFFu);
                u16 device = (u16)(vdf >> 16);
                if (vendor == 0xFFFFu) continue;
                u32 cls = pci_config_read(bus, dev, func, 0x08);
                u8 rev = (u8)(cls & 0xFF);
                u8 prog_if = (u8)((cls >> 8) & 0xFF);
                u8 subclass = (u8)((cls >> 16) & 0xFF);
                u8 class_code = (u8)((cls >> 24) & 0xFF);

                char line[80];
                int p = 0;
                /* "00:00.0 " */
                u8_to_hex(line + p, bus); p += 2;
                line[p++] = ':';
                u8_to_hex(line + p, dev); p += 2;
                line[p++] = '.';
                u8_to_hex(line + p, func); p += 2;
                line[p++] = ' ';
                /* "VEN:DEV " */
                u16_to_hex(line + p, vendor); p += 4;
                line[p++] = ':';
                u16_to_hex(line + p, device); p += 4;
                line[p++] = ' ';
                /* "rev NN " */
                line[p++] = 'r'; line[p++] = 'e'; line[p++] = 'v';
                line[p++] = ' ';
                u8_to_hex(line + p, rev); p += 2;
                line[p++] = ' ';
                /* "class CC:SS:PP" */
                line[p++] = 'c'; line[p++] = 'l'; line[p++] = 'a';
                line[p++] = 's'; line[p++] = 's'; line[p++] = ' ';
                u8_to_hex(line + p, class_code); p += 2;
                line[p++] = ':';
                u8_to_hex(line + p, subclass); p += 2;
                line[p++] = ':';
                u8_to_hex(line + p, prog_if); p += 2;
                line[p++] = ' ';
                line[p++] = '[';
                const char *cn = pci_class_name(class_code);
                while (*cn) line[p++] = *cn++;
                line[p++] = ']';
                const char *vn = pci_vendor_name(vendor);
                if (*vn) {
                    line[p++] = ' ';
                    while (*vn) line[p++] = *vn++;
                }
                line[p] = 0;
                term_puts(line);
                term_putc('\n');
                count++;
            }
        }
    }
    char tail[32];
    int q = 0;
    const char *prefix = "Total: ";
    while (prefix[q]) { tail[q] = prefix[q]; q++; }
    u8_to_dec(tail + q, (u8)(count > 255 ? 255 : count));
    while (tail[q]) q++;
    const char *suffix = " device(s)\n";
    int s = 0;
    while (suffix[s]) { tail[q++] = suffix[s++]; }
    tail[q] = 0;
    term_puts_color(tail, DIM_FG);
}

static u64 g_kernel_api = 0;
static const dsk_boot_context *g_boot_ctx = 0;
static const linux_compat_service *g_lxc_svc = 0;

/* ============================================================
 *  run 命令：从 FAT32 根目录加载并跳转 PIE ELF 工具
 * ============================================================ */

#define SH_ELFCLASS64 2
#define SH_EM_X86_64 62
#define SH_PT_LOAD 1
#define SH_PT_DYNAMIC 2
#define SH_DT_RELA 7
#define SH_DT_RELASZ 8
#define SH_DT_RELAENT 9
#define SH_R_X86_64_RELATIVE 8

typedef struct { u8 ident[16]; u16 type,machine; u32 ver; u64 entry,phoff,shoff; u32 flags; u16 ehsize,phentsize,phnum,shentsize,shnum,shstrndx; } sh_elf64_ehdr;
typedef struct { u32 type,flags; u64 offset,vaddr,paddr,filesz,memsz,align; } sh_elf64_phdr;
typedef struct { i64 tag; u64 val; } sh_elf64_dyn;
typedef struct { u64 offset; u64 info; i64 addend; } sh_elf64_rela;

/* run 加载镜像缓冲（容纳工具 ELF 展开后的 memsz） */
static u8 g_run_image[262144];

static int sh_load_elf(u8 *data, void **entry_out) {
    const sh_elf64_ehdr *eh = (const sh_elf64_ehdr *)data;
    if (eh->ident[0] != 0x7F || eh->ident[4] != SH_ELFCLASS64) return -1;
    if (eh->machine != SH_EM_X86_64) return -2;
    u64 min_vaddr = ~0ULL, max_vaddr = 0;
    u32 lc = 0;
    for (u16 i = 0; i < eh->phnum; i++) {
        const sh_elf64_phdr *ph = (const sh_elf64_phdr *)(data + eh->phoff + (u64)i * eh->phentsize);
        if (ph->type != SH_PT_LOAD) continue;
        if (ph->filesz > ph->memsz) return -3;
        if (ph->vaddr < min_vaddr) min_vaddr = ph->vaddr;
        if (ph->vaddr + ph->memsz > max_vaddr) max_vaddr = ph->vaddr + ph->memsz;
        lc++;
    }
    if (!lc || min_vaddr == ~0ULL) return -4;
    u64 isize = (max_vaddr - min_vaddr + 0xFFF) & ~0xFFFULL;
    if (isize > sizeof(g_run_image)) return -5;
    u8 *image = g_run_image;
    for (u64 i = 0; i < isize; i++) image[i] = 0;
    for (u16 i = 0; i < eh->phnum; i++) {
        const sh_elf64_phdr *ph = (const sh_elf64_phdr *)(data + eh->phoff + (u64)i * eh->phentsize);
        if (ph->type != SH_PT_LOAD) continue;
        u64 off = ph->vaddr - min_vaddr;
        for (u64 b = 0; b < ph->filesz; b++) image[off + b] = data[ph->offset + b];
    }
    *entry_out = image + (eh->entry - min_vaddr);

    /* R_X86_64_RELATIVE 重定位 */
    u64 load_bias = (u64)image - min_vaddr;
    for (u16 i = 0; i < eh->phnum; i++) {
        const sh_elf64_phdr *ph = (const sh_elf64_phdr *)(data + eh->phoff + (u64)i * eh->phentsize);
        if (ph->type != SH_PT_DYNAMIC) continue;
        const sh_elf64_dyn *dyn = (const sh_elf64_dyn *)(image + (ph->vaddr - min_vaddr));
        u64 rela_off = 0, rela_sz = 0, rela_ent = 24;
        for (; dyn->tag != 0; dyn++) {
            if (dyn->tag == SH_DT_RELA) rela_off = dyn->val;
            else if (dyn->tag == SH_DT_RELASZ) rela_sz = dyn->val;
            else if (dyn->tag == SH_DT_RELAENT) rela_ent = dyn->val;
        }
        if (rela_off && rela_sz) {
            u64 cnt = rela_sz / rela_ent;
            for (u64 rr = 0; rr < cnt; rr++) {
                const sh_elf64_rela *rel = (const sh_elf64_rela *)(image + (rela_off - min_vaddr) + rr * rela_ent);
                if ((u32)(rel->info & 0xFFFFFFFF) == SH_R_X86_64_RELATIVE) {
                    u64 *slot = (u64 *)(image + (rel->offset - min_vaddr));
                    *slot = load_bias + (u64)rel->addend;
                }
            }
        }
    }
    return 0;
}

/* run NAME.ELF — 加载根目录工具，全屏执行，返回后重绘 shell */
static void cmd_run(const char *args) {
    if (!g_block_read) { term_puts_color("run: 无块设备\n", ERROR_FG); return; }
    char fn[64];
    parse_token(args, fn, 64);
    if (fn[0] == 0) { term_puts_color("用法: run <NAME.ELF>\n", ERROR_FG); return; }
    char n83[11];
    if (name_to_83(fn, n83) != 0) { term_puts_color("run: 无效文件名\n", ERROR_FG); return; }
    u8 *data = 0; u32 size = 0;
    if (fat32_read_root_file(n83, &data, &size) != 0) {
        term_puts_color("run: 文件不存在: ", ERROR_FG);
        term_puts_color(fn, ERROR_FG);
        term_putc('\n');
        return;
    }
    void *entry = 0;
    int rc = sh_load_elf(data, &entry);
    if (rc != 0) {
        term_puts_color("run: ELF 加载失败（无效格式或过大）\n", ERROR_FG);
        return;
    }
    logl("[shell] run: jumping to tool");
    void (*fn_entry)(const dsk_boot_context *) = (void (*)(const dsk_boot_context *))entry;
    fn_entry(g_boot_ctx);
    logl("[shell] run: tool returned");
    /* 工具可能改动磁盘/显示状态：失效 FAT 缓存并重绘 */
    g_disk_loaded = 0;
    term_redraw_all();
}

/* ============================================================
 *  PATH 查找：在 /bin 子目录中搜索 name.ELF 并执行
 *  返回 1 表示找到并已处理（成功或报错），0 表示未找到
 * ============================================================ */
static int path_search_and_run(const char *name, const char *args) {
    (void)args;  /* 参数保留供未来扩展 */
    if (!g_block_read) return 0;

    /* 构造 "NAME.ELF" → 8.3 名 */
    char full[64];
    int nl = 0;
    while (name[nl] && nl < 55) nl++;
    int p = 0;
    for (int i = 0; i < nl; i++) full[p++] = name[i];
    full[p++] = '.'; full[p++] = 'E'; full[p++] = 'L';
    full[p++] = 'F'; full[p] = 0;
    char n83[11];
    if (name_to_83(full, n83) != 0) return 0;

    /* 获取 /bin 目录首簇 */
    u32 bin_clus;
    if (get_bin_cluster(&bin_clus) != 0) return 0;

    /* 从 /bin 子目录读取 ELF */
    u8 *data = 0; u32 size = 0;
    if (fat32_read_file_in_dir(bin_clus, n83, &data, &size) != 0) return 0;

    /* 加载 ELF */
    void *entry = 0;
    int rc = sh_load_elf(data, &entry);
    if (rc != 0) {
        term_puts_color(name, ERROR_FG);
        term_puts_color(": ELF 加载失败\n", ERROR_FG);
        return 1;
    }

    /* 执行 */
    logl("[shell] path: running /bin/ tool");
    void (*fn_entry)(const dsk_boot_context *) = (void (*)(const dsk_boot_context *))entry;
    fn_entry(g_boot_ctx);
    logl("[shell] path: tool returned");
    g_disk_loaded = 0;
    term_redraw_all();
    return 1;
}

/* ============================================================
 *  linux 命令：通过双内核兼容层执行 Linux 程序
 *
 *  用法:
 *    linux                显示兼容层状态
 *    linux <prog> [args]  执行 /bin/<prog>（或完整路径）
 *    linux ls -la         → 执行 /bin/ls -la
 *    linux uname -a       → 执行 /bin/uname -a
 *
 *  依赖 UTSM+Linux 双内核架构: Linux guest 作为 daemon park 在 VMX
 *  guest 中,shell 通过 g_lxc_svc->exec 写 IPC 请求并 vmresume 唤醒。
 * ============================================================ */

/* 简单 token 分词：将 args 按空格切分为 argv[]，返回 argc。
 * 不支持引号转义（保持与 shell 其余部分一致的简单分词）。 */
static int split_args(const char *args, char argv[16][64], int max_argc) {
    int argc = 0;
    const char *p = args;
    while (*p && argc < max_argc) {
        while (*p == ' ') p++;
        if (*p == 0) break;
        int len = 0;
        while (*p && *p != ' ' && len < 63) {
            argv[argc][len++] = *p++;
        }
        argv[argc][len] = 0;
        argc++;
    }
    return argc;
}

static void cmd_linux(const char *args) {
    if (!g_lxc_svc) {
        term_puts_color("linux: 兼容层服务未初始化（boot context 未提供）\n", ERROR_FG);
        return;
    }
    if (!g_lxc_svc->is_available()) {
        term_puts_color("linux: 兼容层不可用（Linux guest 未驻留或 IPC 未就绪）\n", ERROR_FG);
        char status[128];
        g_lxc_svc->status(status, sizeof(status));
        term_puts_color(status, ERROR_FG);
        term_putc('\n');
        return;
    }

    /* 无参数：显示状态 */
    while (*args == ' ') args++;
    if (*args == 0) {
        term_puts_color("Linux 兼容层已就绪 (双内核 park-and-resume)\n", OK_FG);
        term_puts_color("用法: linux <程序> [参数...]\n", DIM_FG);
        term_puts_color("示例: linux ls -la / linux uname -a / linux cat /etc/hostname\n", DIM_FG);
        return;
    }

    /* 分词参数 */
    char argv_buf[16][64];
    int argc = split_args(args, argv_buf, 16);
    if (argc == 0) {
        term_puts_color("linux: 参数解析失败\n", ERROR_FG);
        return;
    }

    /* 构造程序路径：
     *   - 以 '/' 开头视为绝对路径
     *   - 否则前缀 "/bin/" */
    char path[160];
    const char *prog = argv_buf[0];
    if (prog[0] == '/') {
        int i = 0;
        while (prog[i] && i < 159) { path[i] = prog[i]; i++; }
        path[i] = 0;
    } else {
        const char *prefix = "/bin/";
        int i = 0;
        while (prefix[i]) { path[i] = prefix[i]; i++; }
        int j = 0;
        while (prog[j] && i < 159) { path[i++] = prog[j++]; }
        path[i] = 0;
    }

    /* 构造 argv 指针数组（argv[0] = 程序名） */
    const char *argv_ptrs[16];
    argv_ptrs[0] = prog;  /* argv[0] 用程序名（不带 /bin/ 前缀） */
    for (int i = 1; i < argc; i++) argv_ptrs[i] = argv_buf[i];

    /* 执行：stdout 缓冲区 */
    static char stdout_buf[8192];
    u64 stdout_len = 0;
    u64 exit_code = 0;

    logl("[shell] linux: exec ");
    logl(path);
    int rc = g_lxc_svc->exec(path, argc, argv_ptrs,
                             stdout_buf, sizeof(stdout_buf) - 1,
                             &stdout_len, &exit_code);

    if (rc != 0) {
        term_puts_color("linux: 执行失败 (code=", ERROR_FG);
        char num[16];
        int neg = rc < 0;
        unsigned int v = neg ? (unsigned int)(-rc) : (unsigned int)rc;
        int n = 0;
        if (v == 0) num[n++] = '0';
        while (v) { num[n++] = '0' + (v % 10); v /= 10; }
        if (neg) term_putc('-');
        for (int i = n - 1; i >= 0; i--) term_putc(num[i]);
        term_puts_color(")\n", ERROR_FG);
        return;
    }

    /* 输出 stdout */
    if (stdout_len > 0) {
        stdout_buf[stdout_len] = 0;
        term_puts(stdout_buf);
        /* 确保末尾换行 */
        if (stdout_buf[stdout_len - 1] != '\n') {
            term_putc('\n');
        }
    }

    /* 非零退出码提示 */
    if (exit_code != 0) {
        term_puts_color("[exit ", DIM_FG);
        char ec[16];
        unsigned int ev = (unsigned int)exit_code;
        int en = 0;
        if (ev == 0) ec[en++] = '0';
        while (ev) { ec[en++] = '0' + (ev % 10); ev /= 10; }
        for (int i = en - 1; i >= 0; i--) term_putc(ec[i]);
        term_puts_color("]\n", DIM_FG);
    }
}

static void execute_command(const char *cmd) {
    while (*cmd == ' ') cmd++;
    if (*cmd == 0) return;
    const char *args = cmd;
    while (*args && *args != ' ') args++;
    int cmd_name_len = (int)(args - cmd);
    char name[32];
    if (cmd_name_len >= 32) cmd_name_len = 31;
    for (int i = 0; i < cmd_name_len; i++) name[i] = cmd[i];
    name[cmd_name_len] = 0;
    while (*args == ' ') args++;
    if (str_eq(name, "help") || str_eq(name, "?")) cmd_help();
    else if (str_eq(name, "version") || str_eq(name, "ver")) cmd_version();
    else if (str_eq(name, "uname")) cmd_uname();
    else if (str_eq(name, "about")) cmd_about();
    else if (str_eq(name, "clear") || str_eq(name, "cls")) cmd_clear();
    else if (str_eq(name, "reboot")) cmd_reboot();
    else if (str_eq(name, "halt") || str_eq(name, "shutdown")) cmd_halt();
    else if (str_eq(name, "echo")) cmd_echo(args);
    else if (str_eq(name, "date") || str_eq(name, "time")) cmd_date();
    else if (str_eq(name, "ls") || str_eq(name, "dir")) cmd_ls();
    else if (str_eq(name, "pci")) cmd_pci();
    else if (str_eq(name, "cat")) cmd_cat(args);
    else if (str_eq(name, "cp")) cmd_cp(args);
    else if (str_eq(name, "mv")) cmd_mv(args);
    else if (str_eq(name, "rm") || str_eq(name, "del")) cmd_rm(args);
    else if (str_eq(name, "run")) cmd_run(args);
    else if (str_eq(name, "linux")) cmd_linux(args);
    else if (str_eq(name, "cd")) cmd_not_impl("cd");
    else if (str_eq(name, "pwd")) { term_puts("/\n"); }
    else if (str_eq(name, "whoami")) { term_puts("root\n"); }
    else if (str_eq(name, "id")) { term_puts("uid=0(root) gid=0(root)\n"); }
    /* ---- PATH 查找：未匹配内建时搜索 /bin/ ---- */
    else if (!path_search_and_run(name, args)) {
        cmd_unknown(name);
    }
}

/* ---- CMOS RTC 读取 ---- */
static u8 read_rtc_reg(u8 reg) {
    outb(0x70, reg);
    return inb(0x71);
}

static u8 bcd_to_bin_impl(u8 bcd) {
    return (u8)((bcd >> 4) * 10 + (bcd & 0x0F));
}

/* ---- 提示符 ---- */
static const char *PROMPT = "deshab# ";

static void draw_prompt(void) {
    term_puts_color(PROMPT, PROMPT_FG);
}

/* ---- 输入行编辑 ---- */
static void redraw_input_line(void) {
    int prompt_len = 0;
    while (PROMPT[prompt_len]) prompt_len++;
    i64 y = term_start_y + (i64)cur_row * (i64)DU_ASCII_LINE_H;
    du_fill_rect(&g_ctx, term_start_x, y,
                 (i64)term_w * (i64)DU_ASCII_STEP,
                 (i64)DU_ASCII_CELL_H, BG_COLOR);
    for (int i = 0; i < prompt_len; i++) {
        i64 x = term_start_x + (i64)i * (i64)DU_ASCII_STEP;
        du_draw_char(&g_ctx, PROMPT[i], x, y, PROMPT_FG, BG_COLOR);
    }
    for (int i = 0; i < input_len; i++) {
        i64 x = term_start_x + (i64)(prompt_len + i) * (i64)DU_ASCII_STEP;
        du_draw_char(&g_ctx, input_buf[i], x, y, TEXT_FG, BG_COLOR);
    }
    cur_col = prompt_len + input_cursor;
    term_redraw_cursor();
}

static void insert_char(char c) {
    if (input_len >= 255) return;
    for (int i = input_len; i > input_cursor; i--) {
        input_buf[i] = input_buf[i-1];
    }
    input_buf[input_cursor] = c;
    input_len++;
    input_cursor++;
    redraw_input_line();
}

static void delete_char_back(void) {
    if (input_cursor == 0) return;
    for (int i = input_cursor - 1; i < input_len - 1; i++) {
        input_buf[i] = input_buf[i+1];
    }
    input_len--;
    input_cursor--;
    input_buf[input_len] = 0;
    redraw_input_line();
}

static void delete_char_fwd(void) {
    if (input_cursor >= input_len) return;
    for (int i = input_cursor; i < input_len - 1; i++) {
        input_buf[i] = input_buf[i+1];
    }
    input_len--;
    input_buf[input_len] = 0;
    redraw_input_line();
}

static void cursor_left(void) {
    if (input_cursor > 0) {
        input_cursor--;
        term_clear_cursor();
        cur_col--;
        if (cur_col < 0) { cur_col = term_w - 1; cur_row--; }
        term_redraw_cursor();
    }
}

static void cursor_right(void) {
    if (input_cursor < input_len) {
        input_cursor++;
        term_clear_cursor();
        cur_col++;
        if (cur_col >= term_w) { cur_col = 0; cur_row++; }
        term_redraw_cursor();
    }
}

static void cursor_home(void) {
    term_clear_cursor();
    int prompt_len = 0;
    while (PROMPT[prompt_len]) prompt_len++;
    cur_col = prompt_len;
    input_cursor = 0;
    term_redraw_cursor();
}

static void cursor_end(void) {
    term_clear_cursor();
    int prompt_len = 0;
    while (PROMPT[prompt_len]) prompt_len++;
    cur_col = prompt_len + input_len;
    input_cursor = input_len;
    term_redraw_cursor();
}

static void kill_to_end(void) {
    input_len = input_cursor;
    input_buf[input_len] = 0;
    redraw_input_line();
}

static void kill_line(void) {
    input_len = 0;
    input_cursor = 0;
    input_buf[0] = 0;
    redraw_input_line();
}

/* 开发者模式:自动测试命令序列（ls/echo>/cat/cp/mv/rm）
 * 由 DSK 在 firstInit.txt 第二行=1 时触发，reserved[3]=1 */
static void run_dev_tests(void) {
    term_puts_color("=== 开发者模式:自动测试命令 ===\n", PROMPT_FG);
    term_putc('\n');

    term_puts_color("[1] ls — 列出根目录\n", ACCENT_FG);
    cmd_ls();
    term_putc('\n');

    term_puts_color("[2] echo \"Deshab dev test\" > TEST.TXT\n", ACCENT_FG);
    cmd_echo("Deshab dev test > TEST.TXT");

    term_puts_color("[3] cat TEST.TXT — 读回验证\n", ACCENT_FG);
    cmd_cat("TEST.TXT");
    term_putc('\n');

    term_puts_color("[4] cp TEST.TXT COPY.TXT\n", ACCENT_FG);
    cmd_cp("TEST.TXT COPY.TXT");

    term_puts_color("[5] ls — 确认复制\n", ACCENT_FG);
    cmd_ls();
    term_putc('\n');

    term_puts_color("[6] mv COPY.TXT MOVED.TXT\n", ACCENT_FG);
    cmd_mv("COPY.TXT MOVED.TXT");

    term_puts_color("[7] ls — 确认移动\n", ACCENT_FG);
    cmd_ls();
    term_putc('\n');

    term_puts_color("[8] rm TEST.TXT\n", ACCENT_FG);
    cmd_rm("TEST.TXT");
    term_puts_color("[9] rm MOVED.TXT — 清理\n", ACCENT_FG);
    cmd_rm("MOVED.TXT");

    term_puts_color("[10] ls — 确认清理\n", ACCENT_FG);
    cmd_ls();

    term_puts_color("[11] ping — 外部命令（/bin/ping.elf），跳过内联测试\n", ACCENT_FG);

    term_putc('\n');
    term_puts_color("=== 自动测试完成 ===\n", OK_FG);
    term_puts_color("按 Esc 返回 DSK，或继续输入命令\n", DIM_FG);
    term_putc('\n');
}

/* ---- 主入口 ---- */
__attribute__((visibility("default")))
void dsk_entry(const dsk_boot_context *ctx) {
    __asm__ volatile("cli");
    logl("[shell] boot");

    if (!ctx || ctx->magic != 0x44534B31424F4F54ULL) {
        logl("[shell] bad context");
        for(;;) __asm__("hlt");
    }

    fb_a = ctx->framebuffer_address;
    fb_w = ctx->framebuffer_width;
    fb_h = ctx->framebuffer_height;
    fb_p = ctx->framebuffer_pitch;

    /* 保存 boot 上下文（网络协议栈/run 命令使用） */
    g_kernel_api = ctx->dkm_kernel_api;
    g_boot_ctx = ctx;

    /* 获取 block 设备 read/write（从 kernel_api + 0xA8 → block_api → +16/+24） */
    {
        u64 api = ctx->dkm_kernel_api;
        u64 blk = *(u64 *)(api + 0xA8);
        g_block_read  = blk ? (shell_block_read_fn)*(u64 *)(blk + 16) : 0;
        g_block_write = blk ? (shell_block_write_fn)*(u64 *)(blk + 24) : 0;
    }
    if (g_block_read)  logl("[shell] block_read ok");
    if (g_block_write) logl("[shell] block_write ok");

    /* 获取 Linux 兼容层服务（reserved[5]）—— 双内核 park-and-resume */
    {
        u64 lxc_addr = ctx->reserved[5];
        if (lxc_addr) {
            const linux_compat_service *lxc = (const linux_compat_service *)lxc_addr;
            if (lxc->magic == LINUX_COMPAT_MAGIC) {
                g_lxc_svc = lxc;
                logl("[shell] linux_compat service ok");
            } else {
                logl("[shell] linux_compat magic mismatch");
            }
        } else {
            logl("[shell] linux_compat service not available");
        }
    }

    /* 初始化风格系统渲染上下文 */
    du_context_init(&g_ctx, fb_a, fb_w, fb_h, fb_p);

    term_init_pos();
    term_clear();
    term_redraw_all();

    /* 启动 banner — 使用风格令牌 */
    term_puts_color("=== Deshab OS v0.1.0 ===\n", ACCENT_FG);
    if (ctx->reserved[3] == 1)
        term_puts_color("开发者模式 (自动测试命令)\n", OK_FG);
    else
        term_puts_color("欢迎使用 Deshab Shell\n", OK_FG);
    term_puts_color("输入 'help' 查看可用命令\n", DIM_FG);
    term_putc('\n');

    /* 开发者模式:DSK 设 reserved[3]=1 时自动跑命令测试序列 */
    if (ctx->reserved[3] == 1) {
        g_serial_mirror = 1;
        run_dev_tests();
    }

    /* 将 banner + dev test 输出刷新到 framebuffer */
    term_redraw_all();

    /* 主循环 */
    int shift = 0;
    int e0 = 0;
    u64 blink_start = rdtsc_shell();
    for (;;) {
        draw_prompt();
        input_len = 0;
        input_cursor = 0;
        input_buf[0] = 0;
        int prompt_len = 0;
        while (PROMPT[prompt_len]) prompt_len++;
        cur_col = prompt_len;
        cur_visible = 1;
        redraw_input_line();

        int cmd_done = 0;
        while (!cmd_done) {
            /* 光标闪烁：每 ~530ms 切换一次可见性 */
            if (g_tsc_per_ms) {
                u64 now = rdtsc_shell();
                u64 elapsed = now - blink_start;
                int want_vis = (elapsed / (g_tsc_per_ms * 530)) & 1;
                if (want_vis != cur_visible) {
                    if (want_vis) term_redraw_cursor();
                    else term_clear_cursor();
                    cur_visible = want_vis;
                }
            }
            u8 st = inb(0x64);
            if (!(st & 1)) { __asm__("pause"); continue; }
            u8 data = inb(0x60);
            if (st & 0x20) continue;
            u8 sc = data;
            /* 有键盘输入时重置闪烁，让光标保持可见 */
            blink_start = rdtsc_shell();
            if (!cur_visible) { cur_visible = 1; term_redraw_cursor(); }
            if (sc == 0xE0) { e0 = 1; continue; }
            if (sc == 0x2A || sc == 0x36) { shift = 1; continue; }
            if (sc == 0xAA || sc == 0xB6) { shift = 0; continue; }
            if (sc & 0x80) {
                if (e0 && sc == 0x9C) { e0 = 0; }
                else if (e0 && sc == 0xCB) { e0 = 0; }
                else if (e0 && sc == 0xCD) { e0 = 0; }
                else if (e0 && sc == 0xC8) { e0 = 0; }
                else if (e0 && sc == 0xD0) { e0 = 0; }
                else if (e0 && sc == 0xD3) { e0 = 0; }
                else if (e0 && sc == 0xC7) { e0 = 0; }
                else if (e0 && sc == 0xCF) { e0 = 0; }
                e0 = 0;
                continue;
            }
            if (e0) {
                if (sc == 0x4B) cursor_left();
                else if (sc == 0x4D) cursor_right();
                else if (sc == 0x47) cursor_home();
                else if (sc == 0x4F) cursor_end();
                else if (sc == 0x53) delete_char_fwd();
                else if (sc == 0x48) { history_prev(); }
                else if (sc == 0x50) { history_next(); }
                e0 = 0;
                continue;
            }
            if (sc == 0x1C) {
                term_clear_cursor();
                input_buf[input_len] = 0;
                /* 提交命令前先入历史; 执行后再清空输入 */
                history_push(input_buf);
                g_history_view = -1;
                term_putc('\n');
                execute_command(input_buf);
                /* 确保命令输出后在新行绘制提示符 */
                if (cur_col != 0) {
                    term_putc('\n');
                }
                /* 刷新终端显示 */
                term_redraw_all();
                input_len = 0;
                input_cursor = 0;
                input_buf[0] = 0;
                cmd_done = 1;
                continue;
            }
            if (sc == 0x0E) { delete_char_back(); continue; }
            if (sc == 0x01) { logl("[shell] esc -> return to DSK"); return; }
            if (sc == 0x15 && !shift) { kill_line(); continue; }
            if (sc == 0x17 && !shift) { while (input_cursor > 0) delete_char_back(); continue; }
            if (sc == 0x0B && !shift) { kill_to_end(); continue; }
            char c = scan_to_ascii(sc, shift);
            if (c && c >= 32 && c <= 126) {
                insert_char(c);
            }
        }
    }
}
