/* DKM FAT32 Driver — read-only FAT32 filesystem parser
 * Stage 2, optional, depends on "vfs" and "block".
 * Reads from an embedded FAT32 test image (boot module "dkm:test_fat32").
 * Parses BPB, walks FAT, reads root dir, lists files, reads a file.
 */

#include <stdint.h>

/* ---------------------------------------------------------------
 * DKM ABI types
 * --------------------------------------------------------------- */
#define DKM_DRIVER_MAGIC 0x444B4D31u
#define DKM_ABI_VERSION  1u

typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;
typedef long long          i64;

#define NULL ((void *)0)

struct dkm_log_api {
    void (*info)(const char *msg);
    void (*warn)(const char *msg);
    void (*error)(const char *msg);
    void (*panic)(const char *msg);
};

struct dkm_block_api {
    int (*register_device)(const void *desc);
    u32 (*device_count)(void);
    int (*read)(u32 index, u64 lba, u32 count, void *buffer);
    u64 (*sector_size)(u32 index);
    const char *(*device_name)(u32 index);
};

struct dkm_kernel_api {
    u32 version; u32 size; u64 feature_bits;
    const struct dkm_log_api *log;
    const void *mem,*utsm,*irq,*pci_api,*dma,*vfs_api,*net,*timer,*drr;
    const void *rsdp_address;
    const void *fb_address;
    u64 fb_width,fb_height,fb_pitch; u16 fb_bpp;
    const void *boot_modules_response;
    int (*irq_register)(u8 irq, void *handler);
    u64 hhdm_offset;
    const struct dkm_block_api *block;
};

struct dkm_driver_handle;

struct dkm_driver_desc {
    u32 magic; u16 abi_version; u16 desc_size;
    const char *name,*version,*vendor;
    u32 driver_class,stage,flags,priority;
    const char *const *depends; u32 depends_count;
    const char *const *provides; u32 provides_count;
    u64 min_kernel_abi,feature_bits,reserved0,reserved1;
};

struct limine_file {
    u64 revision; void *address; u64 size; char *path; char *cmdline; u8 _pad[56];
};
struct limine_module_response {
    u64 revision; u64 module_count; struct limine_file **modules;
};

/* ---------------------------------------------------------------
 * FAT32 BPB
 * --------------------------------------------------------------- */
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
    /* FAT32 extended */
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

/* ---------------------------------------------------------------
 * Driver descriptor
 * --------------------------------------------------------------- */
static const char *const g_depends[] = { "vfs", "block" };
static const char *const g_provides[] = { "fat32" };

__attribute__((visibility("default"), used))
const struct dkm_driver_desc driver_desc = {
    .magic=DKM_DRIVER_MAGIC, .abi_version=DKM_ABI_VERSION,
    .desc_size=sizeof(struct dkm_driver_desc),
    .name="fat32", .version="0.1.0", .vendor="Deshab",
    .driver_class=7, .stage=2, .flags=0, .priority=0,
    .depends=g_depends, .depends_count=2,
    .provides=g_provides, .provides_count=1,
    .min_kernel_abi=1,
};

/* ---------------------------------------------------------------
 * Helpers
 * --------------------------------------------------------------- */
static const struct dkm_log_api *g_log;
static u8 g_block_image[65536];

static int fat32_bpb_valid(const u8 *disk, u64 disk_size) {
    if (!disk || disk_size < 512) return 0;
    const fat32_bpb *bpb = (const fat32_bpb *)disk;
    if (bpb->boot_sig != 0xAA55) return 0;
    if (bpb->bytes_per_sector != 512) return 0;
    if (bpb->sectors_per_cluster == 0) return 0;
    if (bpb->reserved_sector_count == 0) return 0;
    if (bpb->fat_count == 0) return 0;
    if (bpb->sectors_per_fat == 0) return 0;
    if (bpb->root_cluster < 2) return 0;
    return 1;
}

static void log_hex(const char *p, u64 v) {
    static const char h[] = "0123456789abcdef";
    char b[19]; u32 i=0; b[i++]='0'; b[i++]='x';
    for (int j=15;j>=0;j--) b[i++]=h[(v>>(j*4))&0xf];
    b[i]=0; g_log->info(p); g_log->info(b);
}

static u32 read_u32_le(const u8 *p) {
    return (u32)p[0]|((u32)p[1]<<8)|((u32)p[2]<<16)|((u32)p[3]<<24);
}

static u16 read_u16_le(const u8 *p) {
    return (u16)p[0]|((u16)p[1]<<8);
}

static int strneq(const char *a, const char *b, int n) {
    for (int i=0;i<n;i++) { if (a[i]!=b[i]) return 0; }
    return 1;
}

static void log_name11(const char *n) {
    char buf[13]; int j=0;
    for (int i=0;i<8 && n[i]!=0x20 && n[i];i++) buf[j++]=n[i];
    if (n[8]!=0x20 || (n[9]!=0x20 && n[9]!=0)) {
        buf[j++]='.';
        for (int i=8;i<11 && n[i]!=0x20 && n[i];i++) buf[j++]=n[i];
    }
    buf[j]=0; g_log->info(buf);
}

static const u8 *fat32_find_boot_module_image(const struct dkm_kernel_api *api, u64 *size_out) {
    const struct limine_module_response *rsp =
        (const struct limine_module_response *)api->boot_modules_response;
    if (!rsp) return NULL;
    for (u64 i=0;i<rsp->module_count;i++) {
        struct limine_file *f=rsp->modules[i];
        if (f&&f->cmdline&&strneq(f->cmdline,"dkm:test_fat32",13)) {
            if (size_out) *size_out=f->size;
            return (const u8*)f->address;
        }
    }
    return NULL;
}

static const u8 *fat32_select_disk_image(const struct dkm_kernel_api *api, u64 *size_out) {
    if (api->block && api->block->device_count && api->block->read && api->block->sector_size) {
        u32 count = api->block->device_count();
        log_hex("[fat32] block devices=", count);
        if (count > 0 && api->block->sector_size(0) == 512) {
            const char *name = api->block->device_name ? api->block->device_name(0) : NULL;
            g_log->info("[fat32] trying block provider");
            if (name) g_log->info(name);
            int st = api->block->read(0, 0, 128, g_block_image);
            log_hex("[fat32] block read status=", (u64)(i64)st);
            if (st == 0 && fat32_bpb_valid(g_block_image, sizeof(g_block_image))) {
                g_log->info("[fat32] using block provider image");
                if (size_out) *size_out = sizeof(g_block_image);
                return g_block_image;
            }
            g_log->warn("[fat32] block provider image is not FAT32; fallback");
        }
    }

    u64 module_size = 0;
    const u8 *module = fat32_find_boot_module_image(api, &module_size);
    if (module) {
        g_log->info("[fat32] using boot module image");
        if (size_out) *size_out = module_size;
        return module;
    }
    return NULL;
}

/* ---------------------------------------------------------------
 * Driver entry
 * --------------------------------------------------------------- */
__attribute__((visibility("default")))
int driver_init(const struct dkm_kernel_api *api,
                struct dkm_driver_handle *handle) {
    (void)handle;
    if (!api||!api->log) return -1;
    g_log=api->log;

    u64 disk_size = 0;
    const u8 *disk = fat32_select_disk_image(api, &disk_size);
    if (!disk) { g_log->warn("[fat32] no FAT32 image source"); return 0; }
    if (!fat32_bpb_valid(disk, disk_size)) { g_log->warn("[fat32] invalid FAT32 BPB"); return 0; }

    g_log->info("[fat32] parsing FAT32 image");
    const fat32_bpb *bpb=(const fat32_bpb*)disk;
    u32 bps=bpb->bytes_per_sector;
    u32 spc=bpb->sectors_per_cluster;
    u32 rsc=bpb->reserved_sector_count;
    u32 spf=bpb->sectors_per_fat;
    u32 root_clus=bpb->root_cluster;

    log_hex("[fat32] bytes/sec=",bps);
    log_hex("[fat32] sec/cluster=",spc);
    log_hex("[fat32] fat start sec=",rsc);
    log_hex("[fat32] sec/fat=",spf);
    log_hex("[fat32] root cluster=",root_clus);

    /* read root directory */
    u32 cluster_size = bps * spc;
    u32 fat_start = rsc * bps;
    u32 data_start = fat_start + spf * bps;

    g_log->info("[fat32] root dir entries:");

    u32 clus = root_clus;
    while (clus >= 2 && clus < 0x0FFFFFF8) {
        u32 clus_off = data_start + (clus - 2) * cluster_size;
        const fat32_dir_entry *dir = (const fat32_dir_entry *)(disk + clus_off);
        for (u32 e=0; e*32 < cluster_size; e++) {
            if (dir[e].name[0]==0) break;
            if ((u8)dir[e].name[0]==0xE5) continue;
            if (dir[e].attr==0x0F) continue; /* skip LFN */
            if (dir[e].attr & 0x08) continue; /* skip volume label */
            g_log->info("[fat32]   ");
            log_name11(dir[e].name);
            log_hex("[fat32]   size=",dir[e].file_size);

            /* try to read README.TXT content */
            if (strneq(dir[e].name,"README  TXT",11)) {
                u32 f_clus = read_u16_le((const u8*)&dir[e].cluster_low);
                u32 f_off = data_start + (f_clus - 2)*cluster_size;
                u32 f_size = dir[e].file_size;
                g_log->info("[fat32] --- README.TXT content ---");
                u32 n = f_size < 128 ? f_size : 128;
                char line[80]; u32 lp=0;
                for (u32 k=0;k<n;k++) {
                    u8 c = disk[f_off+k];
                    line[lp++]=c;
                    if (c=='\n'||lp>=78||k==n-1) { line[lp]=0; g_log->info("[fat32] "); g_log->info(line); lp=0; }
                }
                g_log->info("[fat32] --- end ---");
            }
        }

        /* read next cluster from FAT */
        u32 fat_ent_off = fat_start + clus * 4;
        clus = read_u32_le(disk + fat_ent_off) & 0x0FFFFFFF;
    }

    g_log->info("[fat32] driver ready");
    return 0;
}

__attribute__((visibility("default")))
int driver_exit(struct dkm_driver_handle *handle) {
    (void)handle; return 0;
}
