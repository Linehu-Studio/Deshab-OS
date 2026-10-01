/* fat32_part.h — locate a FAT32 volume on a superfloppy or GPT ESP.
 *
 * Callers already define u8/u16/u32/u64. The read callback matches
 * dkm_block_api.read: (index, lba, count, buf).
 */
#ifndef DESHAB_FAT32_PART_H
#define DESHAB_FAT32_PART_H

/* pack_system_image.sh uses 64 sectors/cluster (32KiB) when the ESP is >= 4GiB.
 * Every FAT32 cluster buffer in DSK/UTSM/userland must cover that size. */
#ifndef FAT32_MAX_CLUSTER_BYTES
#define FAT32_MAX_CLUSTER_BYTES 32768u
#endif

typedef int (*fat32_part_read_fn)(u32 index, u64 lba, u32 count, void *buf);

static inline int fat32_part_bpb_ok(const u8 *sec) {
    u32 spf;
    if (!sec) return 0;
    if (sec[510] != 0x55 || sec[511] != 0xAA) return 0;
    if (sec[11] != 0x00 || sec[12] != 0x02) return 0; /* 512-byte sectors */
    spf = (u32)sec[36] | ((u32)sec[37] << 8) | ((u32)sec[38] << 16) | ((u32)sec[39] << 24);
    if (spf == 0) return 0;
    if (sec[13] == 0) return 0; /* sectors per cluster */
    return 1;
}

static inline u64 fat32_part_r64(const u8 *p) {
    return (u64)p[0] | ((u64)p[1] << 8) | ((u64)p[2] << 16) | ((u64)p[3] << 24)
         | ((u64)p[4] << 32) | ((u64)p[5] << 40) | ((u64)p[6] << 48) | ((u64)p[7] << 56);
}

/* EFI System Partition type GUID c12a7328-f81f-11d2-ba4b-00a0c93ec93b */
static const u8 fat32_part_esp_guid[16] = {
    0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11,
    0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b
};

/* Return the LBA of the FAT32 BPB relative to the whole disk.
 * 0 means LBA0 is already a FAT32 superfloppy (or probe failed). */
static inline u64 fat32_part_probe(fat32_part_read_fn rd, u32 index) {
    u8 sec[512];
    u32 i, ent_size;
    u64 entries_lba;
    if (!rd) return 0;
    if (rd(index, 0, 1, sec) == 0 && fat32_part_bpb_ok(sec)) return 0;
    if (rd(index, 1, 1, sec) == 0 &&
        sec[0] == 'E' && sec[1] == 'F' && sec[2] == 'I' && sec[3] == ' ' &&
        sec[4] == 'P' && sec[5] == 'A' && sec[6] == 'R' && sec[7] == 'T') {
        entries_lba = fat32_part_r64(sec + 72);
        ent_size = (u32)sec[84] | ((u32)sec[85] << 8)
                 | ((u32)sec[86] << 16) | ((u32)sec[87] << 24);
        if (ent_size == 0) ent_size = 128;
        if (entries_lba == 0) entries_lba = 2;
        if (rd(index, entries_lba, 1, sec) == 0 && ent_size >= 40 && ent_size <= 512) {
            for (i = 0; i + ent_size <= 512; i += ent_size) {
                u32 j, match = 1;
                for (j = 0; j < 16; j++) {
                    if (sec[i + j] != fat32_part_esp_guid[j]) { match = 0; break; }
                }
                if (match) return fat32_part_r64(sec + i + 32);
            }
        }
    }
    if (rd(index, 2048, 1, sec) == 0 && fat32_part_bpb_ok(sec)) return 2048;
    return 0;
}

#endif /* DESHAB_FAT32_PART_H */
