/* f32dbg_test.c — f32 字库路径查找宿主复现测试（诊断 M1 zhfont -3）
 *
 * 用 deshab-dev.img 文件作 block 后端（GPT ESP @ LBA2048），
 * 直接调用 fat32_io.h 的查找逻辑，定位 simhei_16.dbf 为何找不到。
 * 用法：clang f32dbg_test.c -o f32dbg && ./f32dbg <img>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;
typedef long long          i64;

#include "../tools/fat32_io.h"

static FILE *g_img;
static u64 g_esp_off;

static int img_block_read(u32 index, u64 lba, u32 count, void *buf) {
    (void)index;
    if (_fseeki64(g_img, (long long)(g_esp_off + lba * 512), SEEK_SET) != 0) return -1;
    if (fread(buf, 512, count, g_img) != count) return -1;
    return 0;
}
static int img_block_write(u32 index, u64 lba, u32 count, const void *buf) {
    (void)index; (void)lba; (void)count; (void)buf; return -1;
}

int main(int argc, char **argv) {
    const char *path = (argc > 1) ? argv[1] : "../../ISO/deshab-dev.img";
    g_img = fopen(path, "rb");
    if (!g_img) { printf("open %s failed\n", path); return 1; }
    /* GPT ESP at LBA 2048 */
    u8 hdr[512];
    fseek(g_img, 512, SEEK_SET);
    fread(hdr, 1, 512, g_img);
    if (memcmp(hdr, "EFI PART", 8) != 0) { printf("no GPT\n"); return 1; }
    g_esp_off = 2048ULL * 512;

    f32_init(img_block_read, img_block_write);

    int rc = f32_disk_load();
    printf("f32_disk_load rc=%d\n", rc);
    if (rc != 0) return 1;

    const f32_bpb *bpb = (const f32_bpb *)f32_disk;
    printf("spc=%u rsvd=%u fc=%u spf=%u root_clus=%u\n",
           bpb->spc, bpb->rsvd, bpb->fc, bpb->spf, bpb->root_clus);

    /* 1. 目录定位 */
    u32 dclus = 0;
    rc = f32_find_path_dir_lfn("system/font", &dclus);
    printf("find_path_dir(system/font) rc=%d clus=%u\n", rc, dclus);
    if (rc != 0) return 1;

    /* 2. 在 FONT 目录中找文件（对照：LFN 与短名） */
    u32 fclus = 0, fsize = 0;
    rc = f32_find_in_dir_lfn(dclus, "simhei_16.dbf", 0, &fclus, &fsize);
    printf("find(simhei_16.dbf) rc=%d clus=%u size=%u\n", rc, fclus, fsize);
    rc = f32_find_in_dir_lfn(dclus, "SIMHEI~1.DB F", 0, &fclus, &fsize);
    printf("find(SIMHEI~1.DBF) rc=%d\n", rc);

    /* M2: 壁纸读取复现（rc=-4 路径） */
    {
        static u8 wpbuf[8u*1024u*1024u];
        u32 wpsize = 0;
        rc = f32_read_path_lfn_to("system/wallpaper.rgba", wpbuf, 2800512u, &wpsize);
        printf("read(wallpaper.rgba) rc=%d size=%u\n", rc, wpsize);
        if (rc != 0) {
            u32 c2 = 0;
            rc = f32_read_path_lfn_to("system/wallpaper.rgba", wpbuf, 4096, &c2);
            printf("  small-cap probe rc=%d size=%u\n", rc, c2);
        }
    }

    /* 3. 手工枚举 FONT 目录条目，dump LFN/短名/checksum */
    {
        u32 spc = bpb->spc;
        u32 data_lba = bpb->rsvd + (u32)bpb->fc * bpb->spf;
        u64 fat_byte_off = bpb->rsvd * 512;
        u32 clus = dclus;
        printf("--- FONT dir entries ---\n");
        while (clus >= 2 && clus < 0x0FFFFFF8) {
            u32 lba = data_lba + (clus - 2) * spc;
            const u8 *cb;
            if ((u64)lba * 512 + (u64)spc * 512 <= 256ULL * 512)
                cb = f32_disk + (u64)lba * 512;
            else {
                if (f32_read_sectors(lba, spc, f32_cluster) != 0) {
                    printf("dir cluster read failed lba=%u\n", lba);
                    return 1;
                }
                cb = f32_cluster;
            }
            u32 entry_count = (spc * 512) / 32;
            for (u32 e = 0; e < entry_count; e++) {
                const u8 *entry = cb + e * 32;
                if (entry[0] == 0) { printf("[end]\n"); goto done; }
                if ((u8)entry[0] == 0xE5) continue;
                if (entry[11] == 0x0F) {
                    /* LFN 片段：seq=c0 seq|0x40 首段 */
                    printf("LFN  seq=0x%02x name1='%.5s' name2='%.6s' name3='%.2s' cksum=0x%02x\n",
                           entry[0],
                           (const char*)(const void*)((u16*)entry + 1),
                           (const char*)(const void*)((u16*)entry + 6),
                           (const char*)(const void*)((u16*)entry + 14),
                           entry[13]);
                    continue;
                }
                char nm[12];
                memcpy(nm, entry, 11);
                nm[11] = 0;
                printf("SHORT '%s' attr=0x%02x clus=%u size=%u\n", nm, entry[11],
                       (unsigned)(entry[26] | (entry[20] << 16)),
                       (unsigned)(entry[28] | (entry[29]<<8) | (entry[30]<<16) | ((u32)entry[31]<<24)));
            }
            u32 fo = fat_byte_off + clus * 4;
            if (fo + 4 > sizeof(f32_disk)) break;
            clus = f32_r32(f32_disk + fo) & 0x0FFFFFFF;
        }
    }
done:
    return 0;
}
