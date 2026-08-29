#include <utsm/dsk.h>
#include <utsm/arena.h>
#include <utsm/dkm.h>
#include <utsm/log.h>
#include <utsm/types.h>
#include <utsm/pe.h>
#include <utsm/linux_compat.h>
#include <utsm/instr.h>
#include "../arch/x86_64/limine.h"
#include "../../tools/fat32_lfn.h"

#define DSK_PATH "/system/deshab64/deshab.elf"
#define DSK_CMDLINE "dsk:main"

#define ELF_MAGIC0 0x7f
#define ELF_MAGIC1 'E'
#define ELF_MAGIC2 'L'
#define ELF_MAGIC3 'F'
#define ELFCLASS64 2
#define ELFDATA2LSB 1
#define ET_DYN 3
#define ET_EXEC 2
#define EM_X86_64 62
#define PT_LOAD 1
#define PT_DYNAMIC 2

/* ELF Dynamic section tags */
#define DT_NULL     0
#define DT_RELA     7
#define DT_RELASZ   8
#define DT_RELAENT  9

/* ELF Relocation types */
#define R_X86_64_RELATIVE 8

typedef struct elf64_dyn {
    i64 tag;
    u64 val;
} elf64_dyn;

typedef struct elf64_rela {
    u64 offset;
    u64 info;
    i64 addend;
} elf64_rela;

void *memset(void *dst, int value, usize len);
void *memcpy(void *dst, const void *src, usize len);

extern volatile struct limine_module_request g_module_request;
extern volatile struct limine_rsdp_request g_rsdp_request;
extern volatile struct limine_framebuffer_request g_fb_request;
extern volatile struct limine_hhdm_request g_hhdm_request;

typedef struct elf64_ehdr {
    u8 ident[16];
    u16 type;
    u16 machine;
    u32 version;
    u64 entry;
    u64 phoff;
    u64 shoff;
    u32 flags;
    u16 ehsize;
    u16 phentsize;
    u16 phnum;
    u16 shentsize;
    u16 shnum;
    u16 shstrndx;
} elf64_ehdr;

typedef struct elf64_phdr {
    u32 type;
    u32 flags;
    u64 offset;
    u64 vaddr;
    u64 paddr;
    u64 filesz;
    u64 memsz;
    u64 align;
} elf64_phdr;

/* ---- minimal FAT32 parser ---- */
typedef struct __attribute__((packed)) {
    u8  jmp[3];
    char oem[8];
    u16 bytes_per_sector;
    u8  sectors_per_cluster;
    u16 reserved_sector_count;
    u8  fat_count;
    u16 root_entry_count;
    u16 total_sectors_16;
    u8  media;
    u16 sectors_per_fat_16;
    u16 sectors_per_track;
    u16 heads;
    u32 hidden_sectors;
    u32 total_sectors_32;
    u32 sectors_per_fat;
    u16 flags;
    u16 version;
    u32 root_cluster;
    u16 fsinfo_sector;
    u16 backup_boot;
    u8  reserved[12];
    u8  drive;
    u8  nt_flags;
    u8  signature;
    u32 serial;
    char label[11];
    char type[8];
    u8  code[420];
    u16 boot_sig;
} fat32_bpb;

typedef struct __attribute__((packed)) {
    char name[11];
    u8  attr;
    u8  nt_reserved;
    u8  creation_tenth;
    u16 creation_time;
    u16 creation_date;
    u16 access_date;
    u16 cluster_high;
    u16 write_time;
    u16 write_date;
    u16 cluster_low;
    u32 file_size;
} fat32_dir_entry;

static u32 fat32_read_u32(const u8 *p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static u16 fat32_read_u16(const u8 *p) {
    return (u16)p[0] | ((u16)p[1] << 8);
}

static int fat32_name11_eq(const char *n, const char *s) {
    for (int i = 0; i < 11; i++) {
        if (n[i] != s[i]) return 0;
    }
    return 1;
}

/* core helpers reused from earlier loader */
static int streq(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        if (*a != *b) return 0;
        a++;
        b++;
    }
    return *a == *b;
}

static int range_ok(u64 offset, u64 length, u64 size) {
    if (offset > size) return 0;
    if (length > size - offset) return 0;
    return 1;
}

static struct limine_file *dsk_find_module(void) {
    struct limine_module_response *rsp = g_module_request.response;
    if (!rsp) return 0;

    for (u64 i = 0; i < rsp->module_count; i++) {
        struct limine_file *file = rsp->modules[i];
        if (!file) continue;
        if (streq(file->path, DSK_PATH) || streq(file->cmdline, DSK_CMDLINE)) {
            return file;
        }
    }
    return 0;
}

/* ---- FAT32 block-provider file reader ---- */
static u8 g_dsk_fat32_disk[131072]; /* 256-sector BPB+FAT+root dir buffer (与 DSK 端一致) */
static u8 g_dsk_fat32_cluster[4096]; /* 8-sector cluster buffer */
/* 实机要求: 文件缓冲区足够容纳 deshab.elf (~1.5MB) 及 SYSTEM/lib 下的
 * 大型 DLL（shell32.dll ~8MB, uiautomationcore.dll ~4.3MB） */
static u8 g_dsk_fat32_filedata[10485760]; /* 10MB file data buffer */
/* BUG-019 修复: FAT 按需扇区读取缓冲区（仅 512 字节），
 * 当 FAT 条目偏移超出 g_dsk_fat32_disk (128KB) 时使用。 */
static u8 g_dsk_fat32_fat_sec[512];

/* BUG-019 修复: 按需读取 FAT 扇区获取下一个簇号。
 * 如果目标 FAT 条目在预读缓冲区内，直接读取；
 * 否则只读取包含该条目的单个 FAT 扇区（512 字节）。
 * fat_start: FAT 在预读缓冲区中的字节偏移（reserved_sectors * 512）。
 * clus: 当前簇号。
 * bpb: BPB 指针（用于计算 FAT 扇区 LBA）。
 * disk: 预读缓冲区基址。
 * 返回: 下一个簇号，失败返回 0x0FFFFFFF（链终止）。 */
static u32 fat32_get_next_cluster(u32 fat_start_byte, u32 clus,
                                   const fat32_bpb *bpb, const u8 *disk) {
    u32 fat_ent_off = fat_start_byte + clus * 4;
    /* 情况 1: FAT 条目在预读缓冲区内（小分区） */
    if (fat_ent_off + 4 <= sizeof(g_dsk_fat32_disk)) {
        return fat32_read_u32(disk + fat_ent_off) & 0x0FFFFFFF;
    }
    /* 情况 2: FAT 条目超出预读缓冲区（实机大分区）——按需读取单个 FAT 扇区 */
    const dkm_kernel_api *api = dkm_get_kernel_api();
    if (!api || !api->block) return 0x0FFFFFFF;
    /* FAT 条目在 FAT 表中的字节偏移（相对于 FAT 起始） */
    u32 fat_byte_off_in_fat = clus * 4;
    /* 该条目所在的 FAT 扇区号（相对于 FAT 起始） */
    u32 fat_sec_index = fat_byte_off_in_fat / 512;
    /* 条目在扇区内的字节偏移 */
    u32 ent_off_in_sec = fat_byte_off_in_fat % 512;
    /* FAT 扇区的绝对 LBA */
    u32 fat_sec_lba = bpb->reserved_sector_count + fat_sec_index;
    int st = api->block->read(0, fat_sec_lba, 1, g_dsk_fat32_fat_sec);
    if (st != 0) {
        log_error("[UTSM] FAT32: on-demand FAT sector read failed");
        log_hex64("[UTSM] FAT32: fat_sec_lba=", fat_sec_lba);
        log_hex64("[UTSM] FAT32: clus=", clus);
        return 0x0FFFFFFF;
    }
    return fat32_read_u32(g_dsk_fat32_fat_sec + ent_off_in_sec) & 0x0FFFFFFF;
}

static int dsk_load_from_block_fat32(const void **out_addr, u64 *out_size) {
    const dkm_kernel_api *api = dkm_get_kernel_api();
    if (!api || !api->block || !api->block->device_count) {
        log_error("[UTSM] FAT32: no block api available");
        return -1;
    }
    u32 count = api->block->device_count();
    if (count == 0 || api->block->sector_size(0) != 512) {
        log_error("[UTSM] FAT32: no block device or wrong sector size");
        log_hex64("[UTSM] FAT32: device_count=", count);
        return -1;
    }

    /* Read first 256 sectors: BPB + FAT + root dir */
    u8 *disk = g_dsk_fat32_disk;
    int st = api->block->read(0, 0, 256, disk);
    if (st != 0) {
        log_error("[UTSM] FAT32: BPB read failed");
        log_hex64("[UTSM] FAT32: read status=", (u64)(i64)st);
        return -1;
    }

    const fat32_bpb *bpb = (const fat32_bpb *)disk;
    if (bpb->boot_sig != 0xAA55 || bpb->bytes_per_sector != 512) {
        log_error("[UTSM] FAT32: invalid BPB signature or sector size");
        log_hex64("[UTSM] FAT32: boot_sig=", bpb->boot_sig);
        log_hex64("[UTSM] FAT32: bps=", bpb->bytes_per_sector);
        return -1;
    }

    u32 fat_start = bpb->reserved_sector_count * 512;
    u32 data_start_sec = bpb->reserved_sector_count + (u32)bpb->fat_count * bpb->sectors_per_fat;
    log_hex64("[UTSM] FAT32: fat_start_off=", fat_start);
    log_hex64("[UTSM] FAT32: data_start_sec=", data_start_sec);
    log_hex64("[UTSM] FAT32: spc=", bpb->sectors_per_cluster);
    log_hex64("[UTSM] FAT32: root_clus=", bpb->root_cluster);

    u32 clus = bpb->root_cluster;
    u32 found_clus = 0;
    u32 found_size = 0;
    const char *target = "DESHAB  ELF";

    while (clus >= 2 && clus < 0x0FFFFFF8) {
        u32 clus_lba = data_start_sec + (clus - 2) * bpb->sectors_per_cluster;
        u32 cls_sec = bpb->sectors_per_cluster;
        u8 *cb = g_dsk_fat32_cluster;
        st = api->block->read(0, clus_lba, cls_sec, cb);
        if (st != 0) {
            log_error("[UTSM] FAT32: dir cluster read failed");
            log_hex64("[UTSM] FAT32: clus=", clus);
            log_hex64("[UTSM] FAT32: lba=", clus_lba);
            return -1;
        }

        const fat32_dir_entry *dir = (const fat32_dir_entry *)cb;
        for (u32 e = 0; e * 32 < cls_sec * 512; e++) {
            if (dir[e].name[0] == 0) break;
            if ((u8)dir[e].name[0] == 0xE5) continue;
            if (dir[e].attr == 0x0F) continue;
            if (dir[e].attr & 0x08) continue;
            if (fat32_name11_eq(dir[e].name, target)) {
                found_size = dir[e].file_size;
                found_clus = fat32_read_u16((const u8 *)&dir[e].cluster_low) |
                             ((u32)fat32_read_u16((const u8 *)&dir[e].cluster_high) << 16);
                break;
            }
        }
        if (found_clus) break;
        /* BUG-019: 使用按需 FAT 读取，不再受 128KB 缓冲区限制 */
        clus = fat32_get_next_cluster(fat_start, clus, bpb, disk);
    }

    if (!found_clus || found_size == 0) {
        log_error("[UTSM] FAT32: DESHAB.ELF not found in root dir");
        return -1;
    }
    log_hex64("[UTSM] FAT32: found_clus=", found_clus);
    log_hex64("[UTSM] FAT32: found_size=", found_size);

    if (found_size > sizeof(g_dsk_fat32_filedata)) {
        log_error("[UTSM] FAT32: file exceeds 2MB buffer");
        return -1;
    }
    u8 *dst = g_dsk_fat32_filedata;
    u32 remaining = found_size;
    u32 fc = found_clus;

    while (fc >= 2 && fc < 0x0FFFFFF8 && remaining > 0) {
        u32 fc_lba = data_start_sec + (fc - 2) * bpb->sectors_per_cluster;
        u32 fc_sec = bpb->sectors_per_cluster;
        u32 fc_bytes = fc_sec * 512;
        if (fc_bytes > remaining) fc_bytes = remaining;

        u8 *chunk = g_dsk_fat32_cluster;
        st = api->block->read(0, fc_lba, fc_sec, chunk);
        if (st != 0) {
            log_error("[UTSM] FAT32: data cluster read failed");
            log_hex64("[UTSM] FAT32: fc=", fc);
            log_hex64("[UTSM] FAT32: lba=", fc_lba);
            return -1;
        }
        for (u32 b = 0; b < fc_bytes; b++) dst[b] = chunk[b];
        dst += fc_bytes;
        remaining -= fc_bytes;

        /* BUG-019: 使用按需 FAT 读取 */
        fc = fat32_get_next_cluster(fat_start, fc, bpb, disk);
    }

    /* Copy to arena for ELF loader */
    u8 *file_buf = (u8 *)kmem_alloc_aligned(found_size, 16);
    if (!file_buf) {
        log_error("[UTSM] FAT32: arena alloc failed");
        log_hex64("[UTSM] FAT32: size=", found_size);
        return -1;
    }
    for (u32 i = 0; i < found_size; i++) file_buf[i] = g_dsk_fat32_filedata[i];

    *out_addr = file_buf;
    *out_size = found_size;
    log_hex64("[UTSM] FAT32 file size=", found_size);
    return 0;
}

/* ---- FAT32 subdirectory traversal ---- */

/* 在指定起始 cluster 的目录项中查找 name11（8.3）或 long_name（LFN 长名）。
 * long_name 为 NULL 时仅按 8.3 匹配（向后兼容）。
 * is_dir=1 时只匹配子目录（attr & 0x10），is_dir=0 时只匹配文件。
 * 返回 0 成功，out_clus/out_size 返回匹配项的起始 cluster 和大小。
 * 注意：LFN 缓冲跨簇遍历持续存活，支持跨簇边界的 LFN 链。 */
static int fat32_find_in_dir(u32 start_clus, const char *name11, int is_dir,
                              u32 *out_clus, u32 *out_size, const char *long_name) {
    const dkm_kernel_api *api = dkm_get_kernel_api();
    if (!api || !api->block || !api->block->device_count) return -1;

    u8 *disk = g_dsk_fat32_disk;
    const fat32_bpb *bpb = (const fat32_bpb *)disk;
    if (bpb->boot_sig != 0xAA55 || bpb->bytes_per_sector != 512) return -2;

    u32 fat_start = bpb->reserved_sector_count * 512;
    u32 data_start_sec = bpb->reserved_sector_count + (u32)bpb->fat_count * bpb->sectors_per_fat;
    u32 spc = bpb->sectors_per_cluster;

    /* LFN 缓冲必须在簇循环外声明：跨簇 LFN 链需要持续存活 */
    fat32_lfn_buf lfn;
    fat32_lfn_init(&lfn);

    u32 clus = start_clus;
    while (clus >= 2 && clus < 0x0FFFFFF8) {
        u32 clus_lba = data_start_sec + (clus - 2) * spc;
        u8 *cb = g_dsk_fat32_cluster;
        int st = api->block->read(0, clus_lba, spc, cb);
        if (st != 0) return -3;

        const u8 *entries = (const u8 *)cb;
        u32 entry_count = (spc * 512) / 32;
        for (u32 e = 0; e < entry_count; e++) {
            const u8 *entry = entries + e * 32;
            if (entry[0] == 0) break;
            if ((u8)entry[0] == 0xE5) {
                fat32_lfn_init(&lfn);
                continue;
            }
            const fat32_dir_entry *de = (const fat32_dir_entry *)entry;
            int is_short = fat32_lfn_process(&lfn, entry);
            if (!is_short) continue;  /* LFN 片段 */
            if (de->attr & 0x08) {
                fat32_lfn_init(&lfn);
                continue;  /* 卷标 */
            }
            int entry_is_dir = (de->attr & 0x10) ? 1 : 0;
            if (entry_is_dir != is_dir) {
                fat32_lfn_init(&lfn);
                continue;
            }
            /* 匹配检查：先 8.3 短名，再 LFN 长名 */
            int matched = 0;
            if (name11 && fat32_name11_eq(de->name, name11)) {
                matched = 1;
            } else if (long_name) {
                if (lfn.valid) {
                    char ascii[FAT32_LFN_MAX];
                    int n = fat32_lfn_to_ascii(&lfn, ascii, sizeof(ascii));
                    if (n >= 0 && fat32_lfn_streq_ci(ascii, long_name)) matched = 1;
                }
                if (!matched) {
                    char short_disp[13];
                    fat32_lfn_short_to_str((const u8 *)de->name, short_disp);
                    if (fat32_lfn_streq_ci(short_disp, long_name)) matched = 1;
                }
            }
            if (matched) {
                *out_clus = fat32_read_u16((const u8 *)&de->cluster_low) |
                            ((u32)fat32_read_u16((const u8 *)&de->cluster_high) << 16);
                *out_size = de->file_size;
                return 0;
            }
            fat32_lfn_init(&lfn);
        }

        /* 跟随 FAT 链 — BUG-019: 使用按需 FAT 读取 */
        clus = fat32_get_next_cluster(fat_start, clus, bpb, disk);
    }
    return -4;  /* 未找到 */
}

/* 按路径查找文件并读取内容。
 * path: '/' 分隔的路径。支持两种格式：
 *   1) 8.3 路径（如 "SYSTEM  /DESHAB64 /FUCK    "，每段恰好 11 字符）
 *   2) 长名路径（如 "lib/libtest.so"）
 * 同时保留 8.3 短名（补空格）和原始长名传给 fat32_find_in_dir。
 * out_data: 返回在 g_dsk_fat32_filedata 中的文件数据指针。
 * out_size: 返回文件大小。
 * 返回 0 成功。 */
int fat32_read_path(const char *path, u8 **out_data, u32 *out_size) {
    const dkm_kernel_api *api = dkm_get_kernel_api();
    if (!api || !api->block || !api->block->device_count) return -1;

    u8 *disk = g_dsk_fat32_disk;
    /* 确保 BPB 缓冲区是新鲜的 */
    if (api->block->read(0, 0, 256, disk) != 0) return -2;

    const fat32_bpb *bpb = (const fat32_bpb *)disk;
    if (bpb->boot_sig != 0xAA55 || bpb->bytes_per_sector != 512) return -3;

    u32 fat_start = bpb->reserved_sector_count * 512;
    u32 data_start_sec = bpb->reserved_sector_count + (u32)bpb->fat_count * bpb->sectors_per_fat;
    u32 spc = bpb->sectors_per_cluster;

    /* 解析路径组件 */
    u32 pos = 0;
    u32 path_len = 0;
    while (path[path_len]) path_len++;

    u32 cur_clus = bpb->root_cluster;  /* 从根目录开始 */

    while (pos < path_len) {
        /* 提取一个路径组件（到下一个 '/' 或结尾），同时保留原始串用于 LFN 匹配 */
        char comp[12];   /* 11 字符 8.3 名 + \0 */
        char orig[260];  /* 原始长名 */
        u32 ci = 0, oi = 0;
        while (pos < path_len && path[pos] != '/') {
            if (ci < 11) comp[ci++] = path[pos];
            if (oi + 1 < sizeof(orig)) orig[oi++] = path[pos];
            pos++;
        }
        /* 不足 11 字符则补空格 */
        while (ci < 11) comp[ci++] = ' ';
        comp[11] = 0;
        orig[oi] = 0;

        /* 跳过 '/' */
        if (pos < path_len && path[pos] == '/') pos++;

        /* 判断是否为最后一个组件 */
        int is_last = (pos >= path_len);

        if (is_last) {
            /* 最后一级：查找文件 */
            u32 found_clus = 0, found_size = 0;
            int rc = fat32_find_in_dir(cur_clus, comp, 0, &found_clus, &found_size,
                                        (oi > 0 && oi != 11) ? orig : 0);
            if (rc != 0) return -4;

            if (found_size > sizeof(g_dsk_fat32_filedata)) return -5;

            /* 读取文件数据 */
            u8 *dst = g_dsk_fat32_filedata;
            u32 remaining = found_size;
            u32 fc = found_clus;
            while (fc >= 2 && fc < 0x0FFFFFF8 && remaining > 0) {
                u32 fc_lba = data_start_sec + (fc - 2) * spc;
                u32 fc_bytes = spc * 512;
                if (fc_bytes > remaining) fc_bytes = remaining;
                u8 *chunk = g_dsk_fat32_cluster;
                if (api->block->read(0, fc_lba, spc, chunk) != 0) return -6;
                for (u32 b = 0; b < fc_bytes; b++) dst[b] = chunk[b];
                dst += fc_bytes;
                remaining -= fc_bytes;
                /* BUG-019: 使用按需 FAT 读取 */
                fc = fat32_get_next_cluster(fat_start, fc, bpb, disk);
            }

            *out_data = g_dsk_fat32_filedata;
            *out_size = found_size;
            return 0;
        } else {
            /* 中间级：查找子目录 */
            u32 sub_clus = 0, sub_size = 0;
            int rc = fat32_find_in_dir(cur_clus, comp, 1, &sub_clus, &sub_size,
                                        (oi > 0 && oi != 11) ? orig : 0);
            if (rc != 0) return -7;
            cur_clus = sub_clus;
        }
    }

    return -8;  /* 路径为空或无效 */
}

/* ---- FAT32 写入（实机启动日志落盘，2026-08 移植自 DSK fat32_write_*） ----
 * 相比 DSK 版本的两点增强：
 *   1) FAT 条目支持按需读-改-写（超出 256 扇区预读缓冲时逐扇区读写，
 *      并同步镜像到所有 FAT 副本），不再受小分区限制。
 *   2) 目录扫描跨簇 + 目录满时自动扩展，文件链支持多簇（日志可达 128KB）。
 * 注意：与 DSK 一致，不更新 FSInfo（free count 会过期，测试工具可接受）。 */

/* 读取 FAT 表条目（28 位），支持按需读扇区。 */
static u32 fat32_get_cluster_entry(u32 fat_byte_off, u32 clus,
                                   const fat32_bpb *bpb, const u8 *disk) {
    u32 ent_off = fat_byte_off + clus * 4;
    if (ent_off + 4 <= sizeof(g_dsk_fat32_disk)) {
        return fat32_read_u32(disk + ent_off) & 0x0FFFFFFF;
    }
    return fat32_get_next_cluster(fat_byte_off, clus, bpb, disk);
}

/* 写 FAT 表条目。缓冲内条目直接改内存（同步镜像副本 1），
 * 缓冲外条目按 FAT 副本逐个读-改-写扇区。返回 0 成功。 */
static int fat32_set_cluster_entry(u32 fat_byte_off, u32 fat_sectors, u32 clus,
                                   u32 value, const fat32_bpb *bpb, u8 *disk) {
    u32 ent_off = fat_byte_off + clus * 4;
    if (ent_off + 4 <= sizeof(g_dsk_fat32_disk)) {
        disk[ent_off]     = (u8)(value & 0xFF);
        disk[ent_off + 1] = (u8)((value >> 8) & 0xFF);
        disk[ent_off + 2] = (u8)((value >> 16) & 0xFF);
        disk[ent_off + 3] = (u8)((disk[ent_off + 3] & 0xF0) | ((value >> 24) & 0x0F));
        /* 同步 FAT 副本 1 的缓冲镜像（若在缓冲内） */
        if (bpb->fat_count >= 2) {
            u32 e1 = ent_off + fat_sectors * 512;
            if (e1 + 4 <= sizeof(g_dsk_fat32_disk)) {
                disk[e1]     = disk[ent_off];
                disk[e1 + 1] = disk[ent_off + 1];
                disk[e1 + 2] = disk[ent_off + 2];
                disk[e1 + 3] = disk[ent_off + 3];
            }
        }
        return 0;
    }
    /* 缓冲外：逐 FAT 副本读-改-写 */
    const dkm_kernel_api *api = dkm_get_kernel_api();
    if (!api || !api->block || !api->block->read || !api->block->write) return -1;
    u32 sec_index = (clus * 4) / 512;
    u32 ent_in_sec = (clus * 4) % 512;
    u32 copies = bpb->fat_count;
    if (copies > 4) copies = 4;
    for (u32 f = 0; f < copies; f++) {
        u32 sec_lba = bpb->reserved_sector_count + f * fat_sectors + sec_index;
        if (api->block->read(0, sec_lba, 1, g_dsk_fat32_fat_sec) != 0) return -2;
        g_dsk_fat32_fat_sec[ent_in_sec]     = (u8)(value & 0xFF);
        g_dsk_fat32_fat_sec[ent_in_sec + 1] = (u8)((value >> 8) & 0xFF);
        g_dsk_fat32_fat_sec[ent_in_sec + 2] = (u8)((value >> 16) & 0xFF);
        g_dsk_fat32_fat_sec[ent_in_sec + 3] = (u8)((g_dsk_fat32_fat_sec[ent_in_sec + 3] & 0xF0) | ((value >> 24) & 0x0F));
        if (api->block->write(0, sec_lba, 1, g_dsk_fat32_fat_sec) != 0) return -3;
    }
    return 0;
}

/* 顺序扫描 FAT 找空闲簇（缓冲内直读，缓冲外按 8 扇区批量读）。
 * from_clus: 起始簇号（建议传入上次分配结果+1，避免反复扫描）。
 * 返回空闲簇号，失败返回 0。 */
static u32 fat32_find_free_cluster(u32 fat_lba, u32 fat_sectors, u32 from_clus) {
    const dkm_kernel_api *api = dkm_get_kernel_api();
    u8 *disk = g_dsk_fat32_disk;
    u32 total_entries = (fat_sectors * 512) / 4;
    u32 fat_byte_off = fat_lba * 512;
    u32 c = from_clus;
    while (c < total_entries) {
        u32 ent_off = fat_byte_off + c * 4;
        if (ent_off + 4 <= sizeof(g_dsk_fat32_disk)) {
            if ((fat32_read_u32(disk + ent_off) & 0x0FFFFFFF) == 0) return c;
            c++;
        } else {
            u32 sec_index = (c * 4) / 512;
            u32 sec_lba = fat_lba + sec_index;
            u32 secs = 8;
            if (sec_lba + secs > fat_lba + fat_sectors) secs = fat_lba + fat_sectors - sec_lba;
            if (api->block->read(0, sec_lba, secs, g_dsk_fat32_cluster) != 0) return 0;
            u32 first_in_chunk = sec_index * 128;
            u32 last_in_chunk = (sec_index + secs) * 128;
            for (u32 cc = (c < first_in_chunk ? first_in_chunk : c);
                 cc < last_in_chunk && cc < total_entries; cc++) {
                if ((fat32_read_u32(g_dsk_fat32_cluster + (cc - first_in_chunk) * 4) & 0x0FFFFFFF) == 0)
                    return cc;
            }
            c = last_in_chunk;
        }
    }
    return 0;
}

/* 分配并链接 needed 个簇，全部标记 EOC。scan_from 记录扫描游标。
 * 返回 0 成功；first/last 返回链首/链尾。 */
static int fat32_alloc_clusters(const fat32_bpb *bpb, u32 fat_lba,
                                u32 fat_sectors, u32 needed,
                                u32 *scan_from, u32 *first, u32 *last) {
    u32 prev = 0;
    u32 fc = 0, lc = 0;
    u32 cur_scan = *scan_from;
    for (u32 i = 0; i < needed; i++) {
        u32 nc = fat32_find_free_cluster(fat_lba, fat_sectors, cur_scan);
        if (nc == 0) return -1;
        cur_scan = nc + 1;
        if (fat32_set_cluster_entry(bpb->reserved_sector_count * 512, fat_sectors,
                                    nc, 0x0FFFFFFF, bpb, g_dsk_fat32_disk) != 0)
            return -2;
        if (prev) {
            if (fat32_set_cluster_entry(bpb->reserved_sector_count * 512, fat_sectors,
                                        prev, nc, bpb, g_dsk_fat32_disk) != 0)
                return -3;
        } else {
            fc = nc;
        }
        prev = nc;
        lc = nc;
    }
    *scan_from = cur_scan;
    *first = fc;
    *last = lc;
    return 0;
}

/* 在指定目录（dir_clus 起始簇）写入文件（8.3 名，11 字符）。
 * 存在则覆盖（复用旧链/扩展/截断），否则新建。支持多簇目录与多簇文件。
 * data 必须指向可寻址静态内存（如 log 缓冲）。返回 0 成功。 */
static int fat32_write_file_in_dir(u32 dir_clus, const char *name11,
                                   const u8 *data, u32 size) {
    const dkm_kernel_api *api = dkm_get_kernel_api();
    if (!api || !api->block || !api->block->read || !api->block->write) return -1;
    u8 *disk = g_dsk_fat32_disk;
    if (api->block->read(0, 0, 256, disk) != 0) return -2;
    const fat32_bpb *bpb = (const fat32_bpb *)disk;
    if (bpb->boot_sig != 0xAA55 || bpb->bytes_per_sector != 512) return -3;

    u32 spc = bpb->sectors_per_cluster;
    u32 fat_lba = bpb->reserved_sector_count;
    u32 fat_sectors = bpb->sectors_per_fat;
    u32 data_lba = bpb->reserved_sector_count + (u32)bpb->fat_count * bpb->sectors_per_fat;
    u32 fat_byte_off = fat_lba * 512;
    u32 cluster_bytes = spc * 512;

    /* ---- 1. 扫描目录链：找同名条目或空闲槽 ---- */
    u32 cur = dir_clus;
    u32 last_dir_clus = dir_clus;
    u32 entry_clus = 0;      /* 目录项所在目录簇 */
    u32 entry_slot = 0;
    int found = 0;           /* 同名文件已存在 */
    u32 old_first_clus = 0;
    int done = 0;
    while (cur >= 2 && cur < 0x0FFFFFF8) {
        last_dir_clus = cur;
        u32 clus_lba = data_lba + (cur - 2) * spc;
        if (api->block->read(0, clus_lba, spc, g_dsk_fat32_cluster) != 0) return -4;
        u32 me = cluster_bytes / 32;
        const fat32_dir_entry *d = (const fat32_dir_entry *)g_dsk_fat32_cluster;
        for (u32 e = 0; e < me; e++) {
            if (d[e].name[0] == 0) {           /* 目录结束标记 */
                if (entry_clus == 0) { entry_clus = cur; entry_slot = e; }
                done = 1;
                break;
            }
            if ((u8)d[e].name[0] == 0xE5) {    /* 已删除项，可复用 */
                if (entry_clus == 0) { entry_clus = cur; entry_slot = e; }
                continue;
            }
            if (d[e].attr == 0x0F || (d[e].attr & 0x08)) continue;
            if (fat32_name11_eq(d[e].name, name11)) {
                found = 1;
                entry_clus = cur;
                entry_slot = e;
                old_first_clus = fat32_read_u16((const u8 *)&d[e].cluster_low) |
                                 ((u32)fat32_read_u16((const u8 *)&d[e].cluster_high) << 16);
                done = 1;
                break;
            }
        }
        if (done) break;
        cur = fat32_get_cluster_entry(fat_byte_off, cur, bpb, disk);
    }

    /* ---- 2. 目录满时扩展一个簇 ---- */
    if (entry_clus == 0) {
        u32 scan_from = 2;
        u32 nc = fat32_find_free_cluster(fat_lba, fat_sectors, scan_from);
        if (nc == 0) return -9;
        if (fat32_set_cluster_entry(fat_byte_off, fat_sectors, nc, 0x0FFFFFFF,
                                    bpb, disk) != 0)
            return -10;
        if (fat32_set_cluster_entry(fat_byte_off, fat_sectors, last_dir_clus, nc,
                                    bpb, disk) != 0)
            return -11;
        for (u32 b = 0; b < cluster_bytes; b++) g_dsk_fat32_cluster[b] = 0;
        u32 nlba = data_lba + (nc - 2) * spc;
        if (api->block->write(0, nlba, spc, g_dsk_fat32_cluster) != 0) return -12;
        entry_clus = nc;
        entry_slot = 0;
    }

    /* ---- 3. 文件簇链：复用/扩展/截断 ---- */
    u32 needed = (size + cluster_bytes - 1) / cluster_bytes;
    if (needed == 0) needed = 1;
    u32 first = 0;
    u32 scan_from = 2;

    if (found && old_first_clus >= 2) {
        /* 走旧链，收集前 needed 个簇 */
        u32 c = old_first_clus;
        u32 cnt = 0;
        u32 last = 0;
        u32 tail = 0;
        while (c >= 2 && c < 0x0FFFFFF8 && cnt < needed) {
            last = c;
            cnt++;
            c = fat32_get_cluster_entry(fat_byte_off, c, bpb, disk);
        }
        first = old_first_clus;
        if (cnt == needed) {
            /* 截断：last 指向 EOC，释放旧链尾部 */
            tail = fat32_get_cluster_entry(fat_byte_off, last, bpb, disk);
            if (fat32_set_cluster_entry(fat_byte_off, fat_sectors, last, 0x0FFFFFFF,
                                        bpb, disk) != 0)
                return -13;
            while (tail >= 2 && tail < 0x0FFFFFF8) {
                u32 nn = fat32_get_cluster_entry(fat_byte_off, tail, bpb, disk);
                if (fat32_set_cluster_entry(fat_byte_off, fat_sectors, tail, 0,
                                            bpb, disk) != 0)
                    return -14;
                tail = nn;
            }
        } else {
            /* 旧链不足：last 之后补簇 */
            u32 need_more = needed - cnt;
            u32 nf = 0, nl = 0;
            if (fat32_alloc_clusters(bpb, fat_lba, fat_sectors, need_more,
                                     &scan_from, &nf, &nl) != 0)
                return -15;
            if (fat32_set_cluster_entry(fat_byte_off, fat_sectors, last, nf,
                                        bpb, disk) != 0)
                return -16;
        }
    } else {
        u32 nf = 0, nl = 0;
        if (fat32_alloc_clusters(bpb, fat_lba, fat_sectors, needed,
                                 &scan_from, &nf, &nl) != 0)
            return -17;
        first = nf;
    }

    /* ---- 4. 写数据簇 ---- */
    {
        u32 c = first;
        u32 remaining = size;
        u32 off = 0;
        while (c >= 2 && c < 0x0FFFFFF8 && remaining > 0) {
            u32 lba = data_lba + (c - 2) * spc;
            if (remaining >= cluster_bytes) {
                if (api->block->write(0, lba, spc, data + off) != 0) return -18;
            } else {
                u8 *cb = g_dsk_fat32_cluster;
                for (u32 b = 0; b < cluster_bytes; b++) cb[b] = 0;
                for (u32 b = 0; b < remaining; b++) cb[b] = data[off + b];
                if (api->block->write(0, lba, spc, cb) != 0) return -19;
            }
            if (remaining >= cluster_bytes) {
                off += cluster_bytes;
                remaining -= cluster_bytes;
            } else {
                off += remaining;
                remaining = 0;
            }
            c = fat32_get_cluster_entry(fat_byte_off, c, bpb, disk);
        }
    }

    /* ---- 5. 更新目录项 ---- */
    {
        u32 elba = data_lba + (entry_clus - 2) * spc;
        if (api->block->read(0, elba, spc, g_dsk_fat32_cluster) != 0) return -20;
        fat32_dir_entry *dir = (fat32_dir_entry *)g_dsk_fat32_cluster;
        fat32_dir_entry *e = &dir[entry_slot];
        if (!found) {
            for (int i = 0; i < 11; i++) e->name[i] = name11[i];
            e->attr = 0x20;                 /* archive */
            e->nt_reserved = 0;
            e->creation_tenth = 0;
            e->creation_time = 0;
            e->creation_date = 0;
            e->access_date = 0;
            e->write_time = 0;
            e->write_date = 0;
        }
        e->cluster_high = (u16)((first >> 16) & 0xFFFF);
        e->cluster_low  = (u16)(first & 0xFFFF);
        e->file_size = size;
        if (api->block->write(0, elba, spc, g_dsk_fat32_cluster) != 0) return -21;
    }

    /* ---- 6. 回写 FAT：缓冲内区间按副本刷回（缓冲外条目已即时写盘） ---- */
    {
        u32 copies = bpb->fat_count;
        if (copies > 4) copies = 4;
        for (u32 f = 0; f < copies; f++) {
            u32 flba = fat_lba + f * fat_sectors;
            if (flba >= 256) continue;
            u32 ns = fat_sectors;
            if (flba + ns > 256) ns = 256 - flba;
            if (ns == 0) continue;
            if (api->block->write(0, flba, ns, disk + (u64)flba * 512) != 0) return -22;
        }
    }

    log_info("[UTSM] fat32 write: ok");
    return 0;
}

/* 在根目录写入文件（8.3 名）。 */
static int fat32_write_root_file(const char *name11, const u8 *data, u32 size) {
    const dkm_kernel_api *api = dkm_get_kernel_api();
    if (!api || !api->block || !api->block->read) return -1;
    u8 *disk = g_dsk_fat32_disk;
    if (api->block->read(0, 0, 256, disk) != 0) return -2;
    const fat32_bpb *bpb = (const fat32_bpb *)disk;
    if (bpb->boot_sig != 0xAA55) return -3;
    return fat32_write_file_in_dir(bpb->root_cluster, name11, data, size);
}

/* 按 8.3 路径（每段 11 字符，'/' 分隔）定位子目录，返回起始簇，失败返回 0。 */
static u32 fat32_find_dir_path(const char *path) {
    const dkm_kernel_api *api = dkm_get_kernel_api();
    if (!api || !api->block || !api->block->read) return 0;
    u8 *disk = g_dsk_fat32_disk;
    if (api->block->read(0, 0, 256, disk) != 0) return 0;
    const fat32_bpb *bpb = (const fat32_bpb *)disk;
    if (bpb->boot_sig != 0xAA55 || bpb->bytes_per_sector != 512) return 0;

    u32 cur = bpb->root_cluster;
    u32 pos = 0;
    while (path[pos]) {
        char comp[12];
        u32 ci = 0;
        while (path[pos] && path[pos] != '/') {
            if (ci < 11) comp[ci++] = path[pos];
            pos++;
        }
        while (ci < 11) comp[ci++] = ' ';
        comp[11] = 0;
        if (path[pos] == '/') pos++;
        if (comp[0] == ' ') break;   /* 空段防呆 */
        u32 sub = 0, sz = 0;
        if (fat32_find_in_dir(cur, comp, 1, &sub, &sz, 0) != 0) return 0;
        cur = sub;
    }
    return cur;
}

/* 实机启动日志落盘：写 FAT32 文件。
 * 优先写 SYSTEM/DESHAB64/DEV/BOOTLOG.TXT；DEV 目录缺失时回退根目录 BOOTLOG.TXT。
 * 相比旧原始扇区方案不再覆盖 GPT header/entries，主机可直接读取该文件。
 * 返回 0 成功。 */
int utsm_bootlog_write(const u8 *data, u32 size) {
    const dkm_kernel_api *api = dkm_get_kernel_api();
    if (!api || !api->block || !api->block->device_count) return -1;
    if (api->block->device_count() == 0) return -2;
    if (!api->block->write) return -3;
    if (!data || size == 0) return -4;

    static const char name11[11] = {'B','O','O','T','L','O','G',' ','T','X','T'};
    u32 dev_clus = fat32_find_dir_path("SYSTEM  /DESHAB64 /DEV     ");
    if (dev_clus >= 2)
        return fat32_write_file_in_dir(dev_clus, name11, data, size);
    return fat32_write_root_file(name11, data, size);
}

/* ---- ELF checks ---- */
static int dsk_check_elf(const void *address, u64 size) {
    if (!address || size < sizeof(elf64_ehdr)) return -1;
    const elf64_ehdr *eh = (const elf64_ehdr *)address;
    if (eh->ident[0] != ELF_MAGIC0 || eh->ident[1] != ELF_MAGIC1 ||
        eh->ident[2] != ELF_MAGIC2 || eh->ident[3] != ELF_MAGIC3) return -2;
    if (eh->ident[4] != ELFCLASS64 || eh->ident[5] != ELFDATA2LSB) return -3;
    if (eh->machine != EM_X86_64) return -4;
    if (eh->type != ET_DYN && eh->type != ET_EXEC) return -5;
    if (eh->phoff == 0 || eh->phnum == 0 || eh->phentsize < sizeof(elf64_phdr)) return -6;
    if (!range_ok(eh->phoff, (u64)eh->phnum * eh->phentsize, size)) return -7;
    return 0;
}

static int dsk_load_elf_image(const void *address, u64 size, dsk_entry_fn *entry_out) {
    const elf64_ehdr *eh = (const elf64_ehdr *)address;
    const u8 *base = (const u8 *)address;

    u64 min_vaddr = ~0ULL;
    u64 max_vaddr = 0;
    u64 max_align = 0x1000;
    u32 load_count = 0;

    /* PT_DYNAMIC 的位置（PIE 重定位用） */
    u64 dyn_off = 0;
    u64 dyn_filesz = 0;
    int has_dyn = 0;

    for (u16 i = 0; i < eh->phnum; i++) {
        const elf64_phdr *ph = (const elf64_phdr *)(base + eh->phoff + (u64)i * eh->phentsize);
        if (ph->type == PT_LOAD) {
            if (ph->filesz > ph->memsz) return -1;
            if (!range_ok(ph->offset, ph->filesz, size)) return -2;
            if (ph->vaddr < min_vaddr) min_vaddr = ph->vaddr;
            if (ph->vaddr + ph->memsz > max_vaddr) max_vaddr = ph->vaddr + ph->memsz;
            if (ph->align > max_align) max_align = ph->align;
            load_count++;
        } else if (ph->type == PT_DYNAMIC) {
            if (!range_ok(ph->offset, ph->filesz, size)) return -2;
            dyn_off = ph->offset;
            dyn_filesz = ph->filesz;
            has_dyn = 1;
        }
    }

    if (load_count == 0 || min_vaddr == ~0ULL || max_vaddr <= min_vaddr) return -3;
    if (eh->entry < min_vaddr || eh->entry >= max_vaddr) return -4;

    u64 image_size = utsm_align_up_u64(max_vaddr - min_vaddr, 0x1000);
    u8 *image = (u8 *)kmem_alloc_aligned(image_size, max_align);
    if (!image) return -5;
    memset(image, 0, image_size);

    for (u16 i = 0; i < eh->phnum; i++) {
        const elf64_phdr *ph = (const elf64_phdr *)(base + eh->phoff + (u64)i * eh->phentsize);
        if (ph->type != PT_LOAD) continue;
        u64 dst_off = ph->vaddr - min_vaddr;
        if (dst_off + ph->memsz > image_size) return -6;
        memcpy(image + dst_off, base + ph->offset, ph->filesz);
    }

    /* PIE 重定位: 处理 PT_DYNAMIC → DT_RELA → R_X86_64_RELATIVE
     * load_bias = image 实际加载地址 - ELF 预期最小 vaddr
     * 对每个 R_X86_64_RELATIVE: slot = load_bias + addend */
    if (has_dyn && eh->type == ET_DYN) {
        u64 load_bias = (u64)image - min_vaddr;
        const elf64_dyn *dyn = (const elf64_dyn *)(base + dyn_off);
        u64 dyn_count = dyn_filesz / sizeof(elf64_dyn);

        u64 rela_off = 0, rela_sz = 0, rela_ent = sizeof(elf64_rela);

        for (u64 d = 0; d < dyn_count; d++) {
            if (dyn[d].tag == DT_NULL) break;
            if (dyn[d].tag == DT_RELA)    rela_off = dyn[d].val;
            if (dyn[d].tag == DT_RELASZ)  rela_sz  = dyn[d].val;
            if (dyn[d].tag == DT_RELAENT) rela_ent = dyn[d].val;
        }

        if (rela_off != 0 && rela_sz != 0 && rela_ent != 0) {
            /* rela_off 是相对于文件开头的 vaddr，需转换为 image 内偏移 */
            if (rela_off >= min_vaddr && rela_off < min_vaddr + image_size) {
                u64 rela_count = rela_sz / rela_ent;
                for (u64 r = 0; r < rela_count; r++) {
                    const elf64_rela *rel = (const elf64_rela *)(
                        image + (rela_off - min_vaddr) + r * rela_ent);
                    u32 rtype = (u32)rel->info;
                    if (rtype == R_X86_64_RELATIVE) {
                        if (rel->offset < min_vaddr || rel->offset >= min_vaddr + image_size) continue;
                        u64 *slot = (u64 *)(image + (rel->offset - min_vaddr));
                        *slot = load_bias + (u64)rel->addend;
                    }
                }
            }
            log_info("[UTSM] PIE reloc applied");
            log_hex64("[UTSM] load_bias=", load_bias);
            log_hex64("[UTSM] rela_count=", rela_sz / rela_ent);
        }
    }

    *entry_out = (dsk_entry_fn)(image + (eh->entry - min_vaddr));
    log_hex64("[UTSM] DSK image base=", (u64)image);
    log_hex64("[UTSM] DSK image size=", image_size);
    log_hex64("[UTSM] DSK entry=", (u64)*entry_out);
    return 0;
}

static void dsk_fill_boot_context(dsk_boot_context *ctx) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->magic = DSK_BOOT_MAGIC;
    ctx->abi_version = DSK_BOOT_ABI_VERSION;
    ctx->size = sizeof(*ctx);
    ctx->flags = DSK_BOOT_FLAG_FROM_UTSM | DSK_BOOT_FLAG_DKM_READY | DSK_BOOT_FLAG_FAT32_PATH;

    if (g_hhdm_request.response) {
        ctx->hhdm_offset = g_hhdm_request.response->offset;
    }
    if (g_rsdp_request.response) {
        ctx->rsdp_address = (u64)g_rsdp_request.response->address;
    }
    if (g_fb_request.response && g_fb_request.response->framebuffer_count > 0) {
        struct limine_framebuffer *fb = g_fb_request.response->framebuffers[0];
        ctx->framebuffer_address = (u64)fb->address;
        ctx->framebuffer_width = fb->width;
        ctx->framebuffer_height = fb->height;
        ctx->framebuffer_pitch = fb->pitch;
        ctx->framebuffer_bpp = fb->bpp;
    }
    ctx->boot_modules_response = (u64)g_module_request.response;
    ctx->dkm_kernel_api = (u64)dkm_get_kernel_api();

    /* PE 兼容层服务（PE32+ 原生 + PE32 解释器） */
    const pe_service *pe = pe_get_service();
    if (pe && pe->magic == PE_SERVICE_MAGIC) {
        ctx->reserved[4] = (u64)pe;
    }

    /* Linux 兼容层服务（依赖双内核 park-and-resume + IPC） */
    const linux_compat_service *lxc = linux_compat_get_service();
    if (lxc && lxc->magic == LINUX_COMPAT_MAGIC) {
        ctx->reserved[5] = (u64)lxc;
    }

    /* Probe 探测缓冲信息（供 shell $probe 命令读取） */
    if (g_instr_enabled) {
        probe_info *pi = (probe_info *)kmem_alloc_aligned(sizeof(probe_info), 8);
        if (pi) {
            pi->magic = PROBE_INFO_MAGIC;
            pi->version = 1;
            pi->probe_buf_addr = (u64)probe_buf_address();
            pi->probe_cap = probe_capacity();
            pi->probe_head = probe_head_value();
            pi->probe_count = probe_count();
            pi->probe_dropped = probe_dropped_count();
            pi->instr_enabled = g_instr_enabled;
            pi->instr_probe_enable = g_instr_probe_enable;
            ctx->reserved[6] = (u64)pi;
            ctx->flags |= DSK_BOOT_FLAG_PROBE_INFO;
        }
    }

    u64 rsp;
    __asm__ volatile ("movq %%rsp, %0" : "=r"(rsp));
    ctx->kernel_stack_top = rsp;
}

int dsk_load_and_jump(void) {
    log_info("[UTSM] loading DSK");
    log_info(DSK_PATH);

    const void *data = 0;
    u64 data_size = 0;

    /* 1) try FAT32 block provider */
    int fat32_ok = dsk_load_from_block_fat32(&data, &data_size);
    if (fat32_ok == 0) {
        log_info("[UTSM] DSK loaded from FAT32 block provider");
    } else {
        /* 2) fallback: Limine boot module */
        log_info("[UTSM] FAT32 block path unavailable; trying Limine module");
        struct limine_file *file = dsk_find_module();
        if (!file) {
            log_error("[UTSM] DSK module not found (neither FAT32 nor Limine)");
            return -1;
        }
        data = file->address;
        data_size = file->size;
        log_hex64("[UTSM] DSK module size=", data_size);
    }

    int status = dsk_check_elf(data, data_size);
    if (status != 0) {
        log_error("[UTSM] DSK ELF rejected");
        log_hex64("[UTSM] DSK ELF status=", (u64)(i64)status);
        return status;
    }

    dsk_entry_fn entry = 0;
    status = dsk_load_elf_image(data, data_size, &entry);
    if (status != 0) {
        log_error("[UTSM] DSK load failed");
        log_hex64("[UTSM] DSK load status=", (u64)(i64)status);
        return status;
    }

    dsk_boot_context *ctx = (dsk_boot_context *)kmem_alloc_aligned(sizeof(dsk_boot_context), 16);
    if (!ctx) {
        log_error("[UTSM] DSK context allocation failed");
        return -1;
    }
    dsk_fill_boot_context(ctx);

    /* 实机安全: 跳转前 cli → mask PIC → 重置 IDT → NMI off。
     * DSK 加载的 ELF 映像可能覆盖 UTSM 的驱动代码段,
     * 如果 UTSM 注册的 IRQ handler 所在内存被覆盖,
     * 中断触发时跳转到无效地址 → 三重故障重启。
     *
     * P0 修复: 1) 先 cli 禁用中断，消除 PIC mask 两步操作之间的中断竞争窗口
     *          2) mask PIC 确保不会有硬件中断到达
     *          3) 设置 LAPIC TPR=0xFF 屏蔽所有通过 IOAPIC 路由的中断
     *          4) 将 IDT 所有条目替换为安全 halt stub，防止 DSK 运行时
     *             过期的 UTSM handler 被异常/spurious IRQ 触发
     *          5) 禁用 NMI */
    {
        extern void outb(u16 port, u8 value);
        extern u8 inb(u16 port);

        /* 1) 先禁用中断——关键修复！原来缺少此步，
         *    outb(0xA1,0xFF) 和 outb(0x21,0xFF) 之间有中断窗口 */
        __asm__ volatile("cli");

        /* 2) mask PIC — 此时 IF=0，安全操作 */
        outb(0xA1, 0xFF);  /* PIC2: mask IRQ8-15 */
        outb(0x21, 0xFF);  /* PIC1: mask IRQ0-7  */

        /* 3) BUG-020 修复: 设置 LAPIC TPR=0xFF 屏蔽所有 IOAPIC 路由中断。
         *    实机多核平台中断通常经 IOAPIC 而非 PIC，PIC mask 不影响
         *    IOAPIC redirection entry。通过 IA32_APIC_BASE MSR 读取 LAPIC
         *    基地址，写入 TPR（偏移 0x80）=0xFF 可在 LAPIC 层面屏蔽所有
         *    可屏蔽中断（低于 0xFF 优先级），确保 DSK 运行期间不会有
         *    IOAPIC 路由的 IRQ 到达。HHDM 映射后 LAPIC MMIO 可访问。 */
        {
            u32 msr_lo, msr_hi;
            __asm__ volatile("rdmsr" : "=a"(msr_lo), "=d"(msr_hi) : "c"(0x1B));
            u64 lapic_phys = ((u64)msr_hi << 32) | (msr_lo & 0xFFFFF000ULL);
            if (lapic_phys && (msr_lo & (1 << 11))) {
                /* LAPIC 全局启用时（bit 11）才操作 TPR */
                extern volatile struct limine_hhdm_request g_hhdm_request;
                u64 hhdm_off = g_hhdm_request.response
                             ? g_hhdm_request.response->offset : 0;
                if (hhdm_off) {
                    u64 lapic_vaddr = lapic_phys + hhdm_off;
                    *(volatile u32 *)(lapic_vaddr + 0x80) = 0xFF;
                }
            }
        }

        /* 4) BUG-20260801-005: 换装 DSK 诊断 IDT（替代原静默 halt_all）。
         *    vector 0-31 → UTSM isr_stub（idt_handler 打完整异常现场后
         *    安全停机），vector 32-255 保持 halt stub 防御 spurious IRQ7/15。
         *    SAS-R0 下 UTSM .text 常驻内存，DSK 阶段异常投递路径已验证
         *    可用（BUG-20260729-013 登记行为）；原 halt_all 把 #PF/#GP
         *    无声吞掉，DSK 阶段零诊断。 */
        extern void idt_install_dsk_diag(void);
        idt_install_dsk_diag();

        /* 5) NMI off */
        outb(0x70, inb(0x70) | 0x80);
    }

    log_hex64("[UTSM] DSK context magic=", ctx->magic);
    log_hex64("[UTSM] DSK context api=", ctx->dkm_kernel_api);
    log_hex64("[UTSM] DSK context fb=", ctx->framebuffer_address);
    log_info("[UTSM] jumping to DSK");
    entry(ctx);

    log_error("[UTSM] DSK returned unexpectedly");
    return -1;
}
