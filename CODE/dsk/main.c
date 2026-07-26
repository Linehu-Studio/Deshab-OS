/* DSK Main Kernel — boot entry, FAT32 firstInit check, ELF loader */

#include "../UTSM/include/utsm/dsk.h"
#include "port.h"

#define COM1 0x3F8

typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;
typedef long long          i64;

typedef int (*block_read_fn)(u32 index, u64 lba, u32 count, void *buf);
typedef int (*block_write_fn)(u32 index, u64 lba, u32 count, const void *buf);

static void sputc(char c) { for(unsigned i=0;i<100000;i++){if(inb(COM1+5)&0x20)break;} outb(COM1,(u8)c); }
static void swrite(const char *s) { while(*s){if(*s=='\n')sputc('\r');sputc(*s++);} }
static void logl(const char *s) { swrite(s); swrite("\n"); }
static void logh(const char *p, u64 v) { static const char h[]="0123456789abcdef"; char b[19]; int i=0; b[i++]='0';b[i++]='x';
  for(int j=15;j>=0;j--)b[i++]=h[(v>>(j*4))&0xf];b[i]=0; swrite(p);swrite(b);swrite("\n"); }

static void *memset_dsk(void *d, int c, u64 n) { u8 *p=(u8*)d; while(n--)*p++=(u8)c; return d; }
static void *memcpy_dsk(void *d, const void *s, u64 n) { u8 *dd=(u8*)d; const u8 *ss=(const u8*)s; while(n--)*dd++=*ss++; return d; }

typedef struct __attribute__((packed)) {
    u8 jmp[3]; char oem[8]; u16 bps; u8 spc; u16 rsvd; u8 fc; u16 root_ent;
    u16 ts16; u8 media; u16 spf16; u16 spt; u16 heads; u32 hidden; u32 ts32;
    u32 spf; u16 flags; u16 ver; u32 root_clus; u16 fsi; u16 bkboot;
    u8 res[12]; u8 drv; u8 ntfl; u8 sig; u32 ser; char lbl[11]; char typ[8];
    u8 code[420]; u16 boot_sig;
} fat32_bpb;

typedef struct __attribute__((packed)) {
    char name[11]; u8 attr; u8 ntr; u8 ctenth;
    u16 ctime; u16 cdate; u16 adate; u16 chigh;
    u16 wtime; u16 wdate; u16 clow; u32 fsize;
} fat32_de;

static u32 r32(const u8 *p) { return (u32)p[0]|((u32)p[1]<<8)|((u32)p[2]<<16)|((u32)p[3]<<24); }
static u16 r16(const u8 *p) { return (u16)p[0]|((u16)p[1]<<8); }
static int neq11(const char *a, const char *b) { for(int i=0;i<11;i++) if(a[i]!=b[i]) return 0; return 1; }

#define EI_MAG0 0x7f
#define ELFCLASS64 2
#define ELFDATA2LSB 1
#define EM_X86_64 62
#define PT_LOAD 1
#define PT_DYNAMIC 2
#define DT_RELA 7
#define DT_RELASZ 8
#define DT_RELAENT 9
#define R_X86_64_RELATIVE 8
typedef struct { u8 ident[16]; u16 type,machine; u32 ver; u64 entry,phoff,shoff; u32 flags; u16 ehsize,phentsize,phnum,shentsize,shnum,shstrndx; } elf64_ehdr;
typedef struct { u32 type,flags; u64 offset,vaddr,paddr,filesz,memsz,align; } elf64_phdr;
typedef struct { i64 tag; u64 val; } elf64_dyn;
typedef struct { u64 offset; u64 info; i64 addend; } elf64_rela;

#define BG_TOP    0xFFC8E0F0u
#define BG_BOTTOM 0xFF49306Fu
#define BG_MID    0xFF8888B0u

static u64 g_fb_addr, g_fb_w, g_fb_h, g_fb_p;
static block_read_fn g_block_read;
static block_write_fn g_block_write;
static u64 g_tsc_per_ms = 0;
static u8 g_disk[131072];
static u8 g_cluster[4096];
static u8 g_fdata[262144];

static int fat32_read_sectors(u32 lba, u32 count, u8 *out) { return g_block_read ? g_block_read(0,lba,count,out) : -1; }
static int fat32_write_sectors(u32 lba, u32 count, const u8 *buf) { return g_block_write ? g_block_write(0,lba,count,buf) : -1; }

/* ---- TSC-based timing ---- */
static u64 rdtsc_dsk(void) {
    u32 lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((u64)hi << 32) | lo;
}

static void tsc_calibrate_dsk(void) {
    outb(0x43, 0x30);       /* ch0, lo+hi, mode 0, binary */
    outb(0x40, 0x7c);       /* 11932 low = ~10ms */
    outb(0x40, 0x2e);       /* 11932 high */
    u64 tsc_start = rdtsc_dsk();
    u16 prev = 0; u64 loops = 0;
    for (;;) {
        outb(0x43, 0x00);
        u16 cur = (u16)inb(0x40) | ((u16)inb(0x40) << 8);
        if (cur > prev && loops > 10) break;
        prev = cur; loops++;
    }
    u64 tsc_end = rdtsc_dsk();
    g_tsc_per_ms = (tsc_end - tsc_start) / 10;
    logh("[DSK] tsc_per_ms=", g_tsc_per_ms);
}

static u32 blend_color(u32 c1, u32 c2, u32 a) {
    u32 na = 256 - a;
    u32 r = ((c1 & 0xFF) * na + (c2 & 0xFF) * a) >> 8;
    u32 g = (((c1 >> 8) & 0xFF) * na + ((c2 >> 8) & 0xFF) * a) >> 8;
    u32 b = (((c1 >> 16) & 0xFF) * na + ((c2 >> 16) & 0xFF) * a) >> 8;
    return 0xFF000000 | (b << 16) | (g << 8) | r;
}

static u32 dsk_bg_at_y(i64 y) {
    if (g_fb_h <= 1) return BG_TOP;
    if (y < 0) y = 0;
    if ((u64)y >= g_fb_h) y = (i64)g_fb_h - 1;
    return blend_color(BG_TOP, BG_BOTTOM, (u32)(((u64)y * 255ULL) / (g_fb_h - 1)));
}

static void dsk_fill_gradient_rect(u32 *fb, i64 x, i64 y, i64 w, i64 h) {
    for (i64 r = 0; r < h; r++) {
        i64 yy = y + r;
        if (yy < 0 || (u64)yy >= g_fb_h) continue;
        u32 color = dsk_bg_at_y(yy);
        u32 *line = (u32 *)((u8 *)fb + (u64)yy * g_fb_p);
        for (i64 c = 0; c < w; c++) {
            i64 xx = x + c;
            if (xx < 0 || (u64)xx >= g_fb_w) continue;
            line[(u64)xx] = color;
        }
    }
}

/* ---- Spinner animation: comet-tail arc, clockwise, double-buffered ---- */
/* sin×127 lookup table, 256 entries (1.40625° each). cos(d) = sin(d+64). */
static const i8 g_sin_tab[256] = {
    0,3,6,9,12,16,19,22,25,28,31,34,37,40,43,46,
    49,51,54,57,60,63,65,68,71,73,76,78,81,83,85,88,
    90,92,94,96,98,100,102,104,106,107,109,111,112,113,115,116,
    117,118,120,121,122,122,123,124,125,125,126,126,126,127,127,127,
    127,127,127,127,126,126,126,125,125,124,123,122,122,121,120,118,
    117,116,115,113,112,111,109,107,106,104,102,100,98,96,94,92,
    90,88,85,83,81,78,76,73,71,68,65,63,60,57,54,51,
    49,46,43,40,37,34,31,28,25,22,19,16,12,9,6,3,
    0,-3,-6,-9,-12,-16,-19,-22,-25,-28,-31,-34,-37,-40,-43,-46,
    -49,-51,-54,-57,-60,-63,-65,-68,-71,-73,-76,-78,-81,-83,-85,-88,
    -90,-92,-94,-96,-98,-100,-102,-104,-106,-107,-109,-111,-112,-113,-115,-116,
    -117,-118,-120,-121,-122,-122,-123,-124,-125,-125,-126,-126,-126,-127,-127,-127,
    -127,-127,-127,-127,-126,-126,-126,-125,-125,-124,-123,-122,-122,-121,-120,-118,
    -117,-116,-115,-113,-112,-111,-109,-107,-106,-104,-102,-100,-98,-96,-94,-92,
    -90,-88,-85,-83,-81,-78,-76,-73,-71,-68,-65,-63,-60,-57,-54,-51,
    -49,-46,-43,-40,-37,-34,-31,-28,-25,-22,-19,-16,-12,-9,-6,-3,
};
static i64 dsk_isin(u32 deg) { return (i64)g_sin_tab[deg & 255]; }
static i64 dsk_icos(u32 deg) { return (i64)g_sin_tab[(deg + 64) & 255]; }

#define SPIN_R       64
#define SPIN_ARC     110   /* comet-tail span (of 256) ≈ 155° */
#define SPIN_THICK   3     /* half-thickness of arc band */
#define SPIN_BOX     (SPIN_R + SPIN_THICK + 2)
static u32 g_spin_bg[(SPIN_BOX*2+1) * (SPIN_BOX*2+1)];

static void spinner_save_bg(u32 *fb, i64 cx, i64 cy) {
    for (i64 dy = -SPIN_BOX; dy <= SPIN_BOX; dy++) {
        for (i64 dx = -SPIN_BOX; dx <= SPIN_BOX; dx++) {
            i64 x = cx + dx, y = cy + dy;
            u32 px = (x >= 0 && (u64)x < g_fb_w && y >= 0 && (u64)y < g_fb_h)
                   ? ((u32*)((u8*)fb + (u64)y * g_fb_p))[(u64)x]
                   : dsk_bg_at_y(y);
            g_spin_bg[(dy + SPIN_BOX) * (SPIN_BOX*2+1) + (dx + SPIN_BOX)] = px;
        }
    }
}

static void spinner_restore_bg(u32 *fb, i64 cx, i64 cy) {
    for (i64 dy = -SPIN_BOX; dy <= SPIN_BOX; dy++) {
        i64 y = cy + dy;
        if (y < 0 || (u64)y >= g_fb_h) continue;
        u32 *line = (u32*)((u8*)fb + (u64)y * g_fb_p);
        for (i64 dx = -SPIN_BOX; dx <= SPIN_BOX; dx++) {
            i64 x = cx + dx;
            if (x < 0 || (u64)x >= g_fb_w) continue;
            line[(u64)x] = g_spin_bg[(dy + SPIN_BOX) * (SPIN_BOX*2+1) + (dx + SPIN_BOX)];
        }
    }
}

/* Draw comet-tail arc: head at (base+SPIN_ARC) brightest, tail at base fading out.
 * Clockwise rotation by advancing `base`. Double-buffered via save/restore. */
static void spinner_draw(u32 *fb, i64 cx, i64 cy, u32 base, u32 head_color) {
    u32 bg_ref = dsk_bg_at_y(cy);
    for (u32 d = 0; d < SPIN_ARC; d++) {
        u32 ang = (base + d) & 255;
        i64 ic = dsk_icos(ang), is = dsk_isin(ang);
        u32 alpha = ((d + 1) * 255) / SPIN_ARC;   /* tail→head: 0→255 */
        u32 color = blend_color(bg_ref, head_color, alpha);
        for (i64 t = -SPIN_THICK; t <= SPIN_THICK; t++) {
            i64 r = SPIN_R + t;
            if (r <= 0) continue;
            i64 x = cx + (r * ic) / 127;
            i64 y = cy + (r * is) / 127;
            if (x < 0 || (u64)x >= g_fb_w || y < 0 || (u64)y >= g_fb_h) continue;
            u32 *line = (u32*)((u8*)fb + (u64)y * g_fb_p);
            line[(u64)x] = color;
        }
    }
}

static int fat32_find_entry(const u8 *clus, u32 clus_sectors, const char *target, u32 *out_clus, u32 *out_size) {
    const fat32_de *dir = (const fat32_de *)clus;
    for (u32 e=0; e*32 < clus_sectors*512; e++) {
        if (dir[e].name[0]==0) break;
        if ((u8)dir[e].name[0]==0xE5) continue;
        if (dir[e].attr==0x0F) continue;
        if (dir[e].attr & 0x08) continue;
        if (neq11(dir[e].name, target)) {
            *out_clus = r16((const u8*)&dir[e].clow);
            *out_size = dir[e].fsize;
            return 0;
        }
    }
    return -1;
}

/* find a single file in root directory by 11-char 8.3 name (avoids PIE pointer reloc) */
static int fat32_read_root_file(const char *name11, u8 **out_data, u32 *out_size) {
    u8 *disk = g_disk;
    logl("[DSK] fat32 reading 256 sectors from LBA 0");
    int st = fat32_read_sectors(0, 256, disk);
    logh("[DSK] fat32 read_sectors rc=", (u64)(i64)st);
    if (st != 0) return -1;
    const fat32_bpb *bpb = (const fat32_bpb *)disk;
    if (bpb->boot_sig != 0xAA55 || bpb->bps != 512) return -2;
    if (bpb->spf == 0 || bpb->root_clus < 2) return -3;

    u32 data_lba = bpb->rsvd + (u32)bpb->fc * bpb->spf;
    u32 fat_byte_off = bpb->rsvd * 512;
    u32 spc = bpb->spc;
    logh("[DSK] fat32 data_lba=", data_lba);

    /* walk root directory cluster chain */
    u32 clus = bpb->root_clus;
    u32 found_clus = 0, found_size = 0;
    int found = 0;
    while (clus >= 2 && clus < 0x0FFFFFF8 && !found) {
        u32 lba = data_lba + (clus - 2) * spc;
        const u8 *cb;
        if ((u64)lba * 512 + (u64)spc * 512 <= 256ULL * 512)
            cb = disk + (u64)lba * 512;
        else {
            if (fat32_read_sectors(lba, 8, g_cluster) != 0) return -4;
            cb = g_cluster;
        }
        if (fat32_find_entry(cb, spc, name11, &found_clus, &found_size) == 0) { found = 1; break; }
        u32 fo = fat_byte_off + clus * 4;
        if (fo + 4 > sizeof(g_disk)) break;
        clus = r32(disk + fo) & 0x0FFFFFFF;
    }
    if (!found) { logl("[DSK] fat32 entry not found"); return -5; }
    logh("[DSK] fat32 found_clus=", found_clus);
    logh("[DSK] fat32 found_size=", found_size);

    if (found_size > sizeof(g_fdata)) return -6;
    u8 *dst = g_fdata; u32 remaining = found_size; u32 fc = found_clus;
    while (fc >= 2 && fc < 0x0FFFFFF8 && remaining > 0) {
        u32 fc_lba = data_lba + (fc - 2) * spc;
        u32 fc_bytes = spc * 512;
        if (fc_bytes > remaining) fc_bytes = remaining;
        const u8 *fb;
        if ((u64)fc_lba * 512 + (u64)spc * 512 <= 256ULL * 512)
            fb = disk + (u64)fc_lba * 512;
        else {
            if (fat32_read_sectors(fc_lba, 8, g_cluster) != 0) return -7;
            fb = g_cluster;
        }
        for (u32 b = 0; b < fc_bytes; b++) dst[b] = fb[b];
        dst += fc_bytes; remaining -= fc_bytes;
        u32 fo = fat_byte_off + fc * 4;
        if (fo + 4 > sizeof(g_disk)) break;
        fc = r32(disk + fo) & 0x0FFFFFFF;
    }
    *out_data = g_fdata; *out_size = found_size;
    logl("[DSK] fat32 file read done");
    return 0;
}

/* Write a file to the FAT32 root directory by 11-char 8.3 name.
 * If the file exists, its content is replaced. If not, a new entry is created.
 * Uses g_disk (256-sector buffer) which must already contain the BPB+FAT+root.
 * Returns 0 on success. */
static int fat32_write_root_file(const char *name11, const u8 *data, u32 size) {
    u8 *disk = g_disk;
    const fat32_bpb *bpb = (const fat32_bpb *)disk;
    if (bpb->boot_sig != 0xAA55 || bpb->bps != 512) return -1;
    u32 spc = bpb->spc;
    u32 fat_lba = bpb->rsvd;
    u32 fat_sectors = bpb->spf;
    u32 data_lba = bpb->rsvd + (u32)bpb->fc * bpb->spf;
    u32 fat_byte_off = bpb->rsvd * 512;
    u32 root_clus = bpb->root_clus;
    u32 cluster_bytes = spc * 512;

    /* 1. Re-read BPB+FAT to ensure g_disk is fresh */
    if (fat32_read_sectors(0, 256, disk) != 0) return -2;

    /* 2. Find file entry in root directory */
    u32 root_lba = data_lba + (root_clus - 2) * spc;
    u8 *root_buf = disk + (u64)root_lba * 512;
    u32 max_entries = cluster_bytes / 32;
    fat32_de *dir = (fat32_de *)root_buf;
    int free_entry = -1;
    u32 existing_clus = 0;
    int found = 0;
    for (u32 e = 0; e < max_entries; e++) {
        if (dir[e].name[0] == 0) { if (free_entry < 0) free_entry = (int)e; break; }
        if ((u8)dir[e].name[0] == 0xE5) { if (free_entry < 0) free_entry = (int)e; continue; }
        if (dir[e].attr == 0x0F) continue;
        if (dir[e].attr & 0x08) continue;
        if (neq11(dir[e].name, name11)) {
            existing_clus = r16((const u8*)&dir[e].clow) | ((u32)r16((const u8*)&dir[e].chigh) << 16);
            found = 1;
            break;
        }
    }

    /* 3. Allocate or reuse clusters */
    u32 bytes_needed = size > 0 ? size : 1;
    u32 clusters_needed = (bytes_needed + cluster_bytes - 1) / cluster_bytes;
    u32 first_clus = 0;
    u32 prev_clus = 0;

    if (found && existing_clus >= 2) {
        /* Reuse existing chain, extend or shrink as needed */
        first_clus = existing_clus;
        u32 cur = existing_clus;
        u32 count = 0;
        while (cur >= 2 && cur < 0x0FFFFFF8 && count < clusters_needed) {
            prev_clus = cur;
            u32 fo = fat_byte_off + cur * 4;
            if (fo + 4 > sizeof(g_disk)) break;
            cur = r32(disk + fo) & 0x0FFFFFFF;
            count++;
        }
        /* If we need more clusters, allocate them */
        while (count < clusters_needed) {
            /* Find a free cluster in FAT */
            u32 newc = 0;
            for (u32 c = 2; c < (fat_sectors * 512) / 4; c++) {
                u32 fo = fat_byte_off + c * 4;
                if (fo + 4 > sizeof(g_disk)) break;
                if ((r32(disk + fo) & 0x0FFFFFFF) == 0) { newc = c; break; }
            }
            if (newc == 0) { logl("[DSK] fat32 write: no free clusters"); return -3; }
            /* Link previous cluster to new */
            u32 pfo = fat_byte_off + prev_clus * 4;
            if (pfo + 4 <= sizeof(g_disk)) {
                disk[pfo] = (u8)(newc & 0xFF);
                disk[pfo+1] = (u8)((newc >> 8) & 0xFF);
                disk[pfo+2] = (u8)((newc >> 16) & 0xFF);
                disk[pfo+3] = (u8)((newc >> 24) & 0x0F);
            }
            prev_clus = newc;
            count++;
        }
        /* Terminate the chain */
        if (prev_clus >= 2) {
            u32 fo = fat_byte_off + prev_clus * 4;
            if (fo + 4 <= sizeof(g_disk)) {
                disk[fo] = 0xF8; disk[fo+1] = 0xFF; disk[fo+2] = 0xFF; disk[fo+3] = 0x0F;
            }
        }
        /* Free excess clusters from the old chain */
        u32 next = 0;
        u32 fo = fat_byte_off + prev_clus * 4;
        if (fo + 4 <= sizeof(g_disk)) next = r32(disk + fo) & 0x0FFFFFFF;
        while (next >= 2 && next < 0x0FFFFFF8) {
            u32 nfo = fat_byte_off + next * 4;
            u32 nn = 0;
            if (nfo + 4 <= sizeof(g_disk)) nn = r32(disk + nfo) & 0x0FFFFFFF;
            disk[nfo] = 0; disk[nfo+1] = 0; disk[nfo+2] = 0; disk[nfo+3] = 0;
            next = nn;
        }
    } else {
        /* Allocate new clusters */
        if (free_entry < 0) { logl("[DSK] fat32 write: no free dir entry"); return -4; }
        for (u32 i = 0; i < clusters_needed; i++) {
            u32 newc = 0;
            for (u32 c = 2; c < (fat_sectors * 512) / 4; c++) {
                u32 fo = fat_byte_off + c * 4;
                if (fo + 4 > sizeof(g_disk)) break;
                if ((r32(disk + fo) & 0x0FFFFFFF) == 0) { newc = c; break; }
            }
            if (newc == 0) { logl("[DSK] fat32 write: no free clusters"); return -3; }
            if (i == 0) first_clus = newc;
            if (prev_clus >= 2) {
                u32 pfo = fat_byte_off + prev_clus * 4;
                disk[pfo] = (u8)(newc & 0xFF);
                disk[pfo+1] = (u8)((newc >> 8) & 0xFF);
                disk[pfo+2] = (u8)((newc >> 16) & 0xFF);
                disk[pfo+3] = (u8)((newc >> 24) & 0x0F);
            }
            prev_clus = newc;
        }
        /* Terminate chain */
        if (prev_clus >= 2) {
            u32 fo = fat_byte_off + prev_clus * 4;
            disk[fo] = 0xF8; disk[fo+1] = 0xFF; disk[fo+2] = 0xFF; disk[fo+3] = 0x0F;
        }
    }

    /* 4. Write data to clusters */
    u32 remaining = size;
    const u8 *src = data;
    u32 cur = first_clus;
    u32 ci = 0;
    while (cur >= 2 && cur < 0x0FFFFFF8 && ci < clusters_needed) {
        u32 clba = data_lba + (cur - 2) * spc;
        u32 chunk = remaining < cluster_bytes ? remaining : cluster_bytes;
        /* Read cluster into g_cluster, overlay data, write back */
        if ((u64)clba * 512 + cluster_bytes <= 256ULL * 512) {
            /* In g_disk buffer — copy data directly */
            u8 *dst = disk + (u64)clba * 512;
            for (u32 b = 0; b < chunk; b++) dst[b] = src[b];
            for (u32 b = chunk; b < cluster_bytes; b++) dst[b] = 0;
        } else {
            /* Out of buffer — use sector write */
            if (fat32_read_sectors(clba, spc, g_cluster) != 0) return -5;
            for (u32 b = 0; b < chunk; b++) g_cluster[b] = src[b];
            for (u32 b = chunk; b < cluster_bytes; b++) g_cluster[b] = 0;
            if (fat32_write_sectors(clba, spc, g_cluster) != 0) return -6;
        }
        src += chunk; remaining -= chunk; ci++;
        u32 fo = fat_byte_off + cur * 4;
        if (fo + 4 > sizeof(g_disk)) break;
        cur = r32(disk + fo) & 0x0FFFFFFF;
    }

    /* 5. Update directory entry */
    if (!found) {
        fat32_de *e = &dir[free_entry];
        for (int i = 0; i < 11; i++) e->name[i] = name11[i];
        e->attr = 0x20;  /* archive */
        e->ntr = 0; e->ctenth = 0; e->ctime = 0; e->cdate = 0;
        e->adate = 0; e->chigh = (u16)((first_clus >> 16) & 0xFFFF);
        e->wtime = 0; e->wdate = 0; e->clow = (u16)(first_clus & 0xFFFF);
    } else {
        dir[free_entry >= 0 ? (u32)free_entry : 0].chigh = (u16)((first_clus >> 16) & 0xFFFF);
        dir[free_entry >= 0 ? (u32)free_entry : 0].clow = (u16)(first_clus & 0xFFFF);
        /* Find the existing entry again to update it */
        for (u32 e2 = 0; e2 < max_entries; e2++) {
            if (neq11(dir[e2].name, name11)) {
                dir[e2].chigh = (u16)((first_clus >> 16) & 0xFFFF);
                dir[e2].clow = (u16)(first_clus & 0xFFFF);
                dir[e2].fsize = size;
                break;
            }
        }
    }
    /* Set file size in the directory entry */
    for (u32 e2 = 0; e2 < max_entries; e2++) {
        if (neq11(dir[e2].name, name11)) {
            dir[e2].fsize = size;
            break;
        }
    }

    /* 6. Write FAT + root directory + data sectors back to disk */
    /* Write FAT (both copies) */
    for (u32 f = 0; f < bpb->fc; f++) {
        u32 flba = fat_lba + f * fat_sectors;
        if (fat32_write_sectors(flba, fat_sectors, disk + (u64)flba * 512) != 0) return -7;
    }
    /* Write root directory cluster */
    u32 root_dir_lba = data_lba + (root_clus - 2) * spc;
    if ((u64)root_dir_lba * 512 + cluster_bytes <= 256ULL * 512) {
        if (fat32_write_sectors(root_dir_lba, spc, disk + (u64)root_dir_lba * 512) != 0) return -8;
    }
    /* Write data clusters that were in the g_disk buffer */
    cur = first_clus;
    ci = 0;
    while (cur >= 2 && cur < 0x0FFFFFF8 && ci < clusters_needed) {
        u32 clba = data_lba + (cur - 2) * spc;
        if ((u64)clba * 512 + cluster_bytes <= 256ULL * 512) {
            if (fat32_write_sectors(clba, spc, disk + (u64)clba * 512) != 0) return -9;
        }
        u32 fo = fat_byte_off + cur * 4;
        if (fo + 4 > sizeof(g_disk)) break;
        cur = r32(disk + fo) & 0x0FFFFFFF;
        ci++;
    }

    logl("[DSK] fat32 write: success");
    return 0;
}

static int dsk_check_firstinit(int *is_first, int *dev_mode) {
    char name[12]; /* "FIRSTINTXT" on stack — no PIE reloc */
    name[0]='F';name[1]='I';name[2]='R';name[3]='S';name[4]='T';
    name[5]='I';name[6]='N';name[7]='T';name[8]='X';name[9]='T';name[10]=0;
    u8 *data = 0; u32 size = 0;
    *dev_mode = 0;
    logl("[DSK] fat32_read_path start");
    int rc = fat32_read_root_file(name, &data, &size);
    logh("[DSK] fat32_read_path rc=", (u64)(i64)rc);
    if (rc != 0) { logl("[DSK] firstInit.txt not found"); *is_first = 1; return 0; }
    if (data[0] == '0') { logl("[DSK] firstInit=0"); *is_first = 1; }
    else { logl("[DSK] firstInit != 0"); *is_first = 0; }
    /* 解析第二行 dev_mode 标志（firstInit.txt 格式: "0|1\n0|1"） */
    u32 i = 0;
    while (i < size && data[i] != '\n') i++;
    if (i < size) {
        i++;  /* 跳过换行 */
        if (i < size && data[i] == '1') {
            *dev_mode = 1;
            logl("[DSK] dev_mode=1 (developer auto-test)");
        }
    }
    return 0;
}

static int dsk_load_elf(u8 *data, u32 size, void **entry_out) {
    (void)size;
    const elf64_ehdr *eh = (const elf64_ehdr *)data;
    if (eh->ident[0] != EI_MAG0 || eh->ident[4] != ELFCLASS64) return -1;
    if (eh->machine != EM_X86_64) return -2;
    u64 min_vaddr = ~0ULL, max_vaddr = 0;
    u32 lc = 0;
    for (u16 i = 0; i < eh->phnum; i++) {
        const elf64_phdr *ph = (const elf64_phdr *)(data + eh->phoff + (u64)i * eh->phentsize);
        if (ph->type != PT_LOAD) continue;
        if (ph->filesz > ph->memsz) return -3;
        if (ph->vaddr < min_vaddr) min_vaddr = ph->vaddr;
        if (ph->vaddr + ph->memsz > max_vaddr) max_vaddr = ph->vaddr + ph->memsz;
        lc++;
    }
    if (!lc || min_vaddr == ~0ULL) return -4;
    u64 isize = max_vaddr - min_vaddr;
    isize = (isize + 0xFFF) & ~0xFFFULL;
    /* 加载缓冲:需容纳 shell.elf(~536KB,含 FAT32 读写缓冲 BSS)。1MB 留余量。 */
    static u8 ibuf[1048576];
    u8 *image = ibuf;
    if (isize > 1048576) return -5;  /* 镜像过大，防止越界 */
    memset_dsk(image, 0, isize);
    for (u16 i = 0; i < eh->phnum; i++) {
        const elf64_phdr *ph = (const elf64_phdr *)(data + eh->phoff + (u64)i * eh->phentsize);
        if (ph->type != PT_LOAD) continue;
        u64 off = ph->vaddr - min_vaddr;
        memcpy_dsk(image + off, data + ph->offset, ph->filesz);
    }
    *entry_out = image + (eh->entry - min_vaddr);

    /* apply R_X86_64_RELATIVE relocations from PT_DYNAMIC (PIE) */
    u64 load_bias = (u64)image - min_vaddr;
    for (u16 i = 0; i < eh->phnum; i++) {
        const elf64_phdr *ph = (const elf64_phdr *)(data + eh->phoff + (u64)i * eh->phentsize);
        if (ph->type != PT_DYNAMIC) continue;
        const elf64_dyn *dyn = (const elf64_dyn *)(image + (ph->vaddr - min_vaddr));
        u64 rela_off = 0, rela_sz = 0, rela_ent = 24;
        for (; dyn->tag != 0; dyn++) {
            if (dyn->tag == DT_RELA) rela_off = dyn->val;
            else if (dyn->tag == DT_RELASZ) rela_sz = dyn->val;
            else if (dyn->tag == DT_RELAENT) rela_ent = dyn->val;
        }
        if (rela_off && rela_sz) {
            u64 n = rela_sz / rela_ent;
            logh("[DSK] elf relocs=", n);
            for (u64 r = 0; r < n; r++) {
                const elf64_rela *rel = (const elf64_rela *)(image + (rela_off - min_vaddr) + r * rela_ent);
                u32 rtype = (u32)(rel->info & 0xFFFFFFFF);
                if (rtype == R_X86_64_RELATIVE) {
                    u64 *slot = (u64 *)(image + (rel->offset - min_vaddr));
                    *slot = load_bias + (u64)rel->addend;
                }
            }
        }
    }

    logh("[DSK] elf image base=", (u64)image);
    logh("[DSK] elf entry=", (u64)*entry_out);
    return 0;
}

/* Load an ELF module from the FAT32 root by 11-char 8.3 name, parse it into
 * the shared image buffer, and jump to its entry with the boot context.
 * Returns 0 if the module ran and returned, non-zero on load failure. */
static int dsk_load_and_run(const char name11[12], const dsk_boot_context *ctx) {
    u8 *data=0; u32 size=0; void *entry=0;
    if (fat32_read_root_file(name11, &data, &size) != 0) return -1;
    if (dsk_load_elf(data, size, &entry) != 0) return -2;
    void(*fn)(const dsk_boot_context*)=(void(*)(const dsk_boot_context*))entry;
    fn(ctx);
    return 0;
}

/* Load desktop.elf and jump to it. Never returns on success. */
static void dsk_run_desktop(const dsk_boot_context *ctx) {
    char dn[12]; /* DESKTOP ELF */
    dn[0]='D';dn[1]='E';dn[2]='S';dn[3]='K';dn[4]='T';dn[5]='O';dn[6]='P';dn[7]=' ';dn[8]='E';dn[9]='L';dn[10]='F';
    if (dsk_load_and_run(dn, ctx) != 0) logl("[DSK] desktop load failed");
    for(;;)__asm__("hlt");
}

/* After FirstInit returns, persist the encrypted user.conf buffer (left in
 * ctx->reserved[0/1]) to the FAT32 root as USER.CONF, then flip firstInit.txt
 * from '0' to '1' so subsequent boots skip the wizard and go to login. */
static void dsk_persist_userconf(const dsk_boot_context *ctx) {
    dsk_boot_context *ctx_mut = (dsk_boot_context *)ctx;
    u64 conf_ptr = ctx_mut->reserved[0];
    u64 conf_sz  = ctx_mut->reserved[1];
    if (!conf_ptr || !conf_sz) {
        logl("[DSK] no conf buffer from FirstInit, skipping persist");
        return;
    }
    if (!g_block_write) {
        logl("[DSK] no block_write, cannot persist user.conf");
        return;
    }
    const u8 *conf = (const u8 *)conf_ptr;

    /* 1. Write USER.CONF (encrypted buffer as-is; login decrypts with password) */
    char cn[12]; /* USER    CON */
    cn[0]='U';cn[1]='S';cn[2]='E';cn[3]='R';cn[4]=' ';cn[5]=' ';cn[6]=' ';cn[7]=' ';cn[8]='C';cn[9]='O';cn[10]='N';
    logl("[DSK] writing USER.CONF");
    int wrc = fat32_write_root_file(cn, conf, (u32)conf_sz);
    logh("[DSK] USER.CONF write rc=", (u64)(i64)wrc);

    /* 2. Flip firstInit.txt: '0' -> '1'，保留第二行 dev_mode 标志 */
    char fn[12]; /* FIRSTINTXT */
    fn[0]='F';fn[1]='I';fn[2]='R';fn[3]='S';fn[4]='T';fn[5]='I';fn[6]='N';fn[7]='T';fn[8]='X';fn[9]='T';fn[10]=0;
    /* 读取当前 firstInit.txt 以保留 dev_mode（第二行），格式 "0|1\n0|1" */
    u8 dev_mode_byte = '0';
    u8 *fi_data = 0; u32 fi_size = 0;
    if (fat32_read_root_file(fn, &fi_data, &fi_size) == 0) {
        u32 i = 0;
        while (i < fi_size && fi_data[i] != '\n') i++;
        if (i < fi_size) { i++; if (i < fi_size) dev_mode_byte = fi_data[i]; }
    }
    u8 fi_buf[3]; fi_buf[0] = '1'; fi_buf[1] = '\n'; fi_buf[2] = dev_mode_byte;
    int frc = fat32_write_root_file(fn, fi_buf, 3);
    logh("[DSK] firstInit.txt flip rc=", (u64)(i64)frc);

    /* Clear reserved so login path doesn't see stale pointer */
    ctx_mut->reserved[0] = 0;
    ctx_mut->reserved[1] = 0;
}

/* Draw the spinning loader (comet-tail arc) on the gradient background,
 * then run the firstInit check and dispatch system component loading. */
static void dsk_spinner_and_check(const void *boot_ctx_ptr) {
    const dsk_boot_context *ctx = (const dsk_boot_context *)boot_ctx_ptr;
    { u64 api = ctx->dkm_kernel_api;
      u64 blk = *(u64 *)(api + 0xA8);
      g_block_read  = blk ? (block_read_fn)*(u64 *)(blk + 16) : 0;   /* block_api.read  */
      g_block_write = blk ? (block_write_fn)*(u64 *)(blk + 24) : 0;  /* block_api.write */ }

    g_fb_addr=ctx->framebuffer_address; g_fb_w=ctx->framebuffer_width;
    g_fb_h=ctx->framebuffer_height; g_fb_p=ctx->framebuffer_pitch;
    u32 *sfb=(u32*)(u64)g_fb_addr;
    logh("[DSK] block_read ptr=", (u64)g_block_read);

    tsc_calibrate_dsk();

    /* Gradient background + spinning loader (~1.1s, or 800ms without block dev). */
    i64 cx = (i64)g_fb_w / 2;
    i64 cy = (i64)g_fb_h / 2;
    dsk_fill_gradient_rect(sfb, 0, 0, (i64)g_fb_w, (i64)g_fb_h);
    spinner_save_bg(sfb, cx, cy);

    u32 base = 0;
    u32 head_color = 0xFFE8F2FC;   /* nearly-white light blue */
    u64 spin_start = rdtsc_dsk();
    u64 spin_budget = g_tsc_per_ms * (g_block_read ? 1100 : 800);
    while (rdtsc_dsk() - spin_start < spin_budget) {
        spinner_restore_bg(sfb, cx, cy);
        spinner_draw(sfb, cx, cy, base, head_color);
        base = (base + 6) & 255;   /* clockwise advance */
        u64 fs = rdtsc_dsk();
        while (rdtsc_dsk() - fs < g_tsc_per_ms * 33) __asm__("pause");  /* ~30fps */
    }
    spinner_restore_bg(sfb, cx, cy);

    if (!g_block_read) {
        logl("[DSK] no block device, halting");
        for(;;)__asm__("hlt");
    }

    logl("[DSK] checking firstInit...");
    int is_first = 0, dev_mode = 0;
    if (dsk_check_firstinit(&is_first, &dev_mode) != 0) {
        logl("[DSK] firstInit check failed, halting");
        for(;;)__asm__("hlt");
    }

    /* 开发者模式:加载 shell.elf 自动测试命令序列（cp/mv/echo/ls/cat/rm） */
    if (dev_mode) {
        logl("[DSK] dev mode: loading shell.elf for auto-test");
        char sn[12]; /* SHELL   ELF */
        sn[0]='S';sn[1]='H';sn[2]='E';sn[3]='L';sn[4]='L';sn[5]=' ';sn[6]=' ';sn[7]=' ';sn[8]='E';sn[9]='L';sn[10]='F';
        dsk_boot_context *ctx_mut = (dsk_boot_context *)ctx;
        ctx_mut->reserved[3] = 1;  /* dev mode flag for shell */
        if (dsk_load_and_run(sn, ctx) == 0) logl("[DSK] shell auto-test returned");
        else logl("[DSK] shell.elf not found, skipping dev test");
        ctx_mut->reserved[3] = 0;
    }

    if (!is_first) {
        logl("[DSK] firstInit=1, attempting login before desktop");

        /* Load login.elf first (into g_fdata, then dsk_load_elf copies to ibuf) */
        char ln[12]; /* LOGIN    ELF */
        ln[0]='L';ln[1]='O';ln[2]='G';ln[3]='I';ln[4]='N';ln[5]=' ';ln[6]=' ';ln[7]=' ';ln[8]='E';ln[9]='L';ln[10]='F';
        u8 *ld=0; u32 ls=0; void *le=0;
        int login_ok = 0;
        if (fat32_read_root_file(ln, &ld, &ls) == 0) {
            if (dsk_load_elf(ld, ls, &le) == 0) {
                /* Now read USER.CONF into g_fdata (login.elf already copied to ibuf) */
                char cn[12]; /* USER    CON */
                cn[0]='U';cn[1]='S';cn[2]='E';cn[3]='R';cn[4]=' ';cn[5]=' ';cn[6]=' ';cn[7]=' ';cn[8]='C';cn[9]='O';cn[10]='N';
                u8 *conf_data=0; u32 conf_size=0;
                if (fat32_read_root_file(cn, &conf_data, &conf_size) == 0) {
                    logl("[DSK] USER.CONF found, passing to login");
                    dsk_boot_context *ctx_mut = (dsk_boot_context *)ctx;
                    ctx_mut->reserved[0] = (u64)conf_data;
                    ctx_mut->reserved[1] = (u64)conf_size;
                    ctx_mut->reserved[2] = 0;  /* login success flag */

                    logl("[DSK] jumping to login");
                    void(*lentry)(const dsk_boot_context*)=(void(*)(const dsk_boot_context*))le;
                    lentry(ctx);
                    logl("[DSK] login returned");

                    if (ctx_mut->reserved[2] == 1) {
                        logl("[DSK] login success");
                        login_ok = 1;
                    } else {
                        logl("[DSK] login skipped or failed");
                    }
                } else {
                    logl("[DSK] USER.CONF not found, skipping login");
                }
            } else {
                logl("[DSK] login.elf load failed, skipping login");
            }
        } else {
            logl("[DSK] login.elf not found, skipping login");
        }
        (void)login_ok;

        logl("[DSK] loading desktop");
        dsk_run_desktop(ctx);
        return;
    }

    /* First boot: run system initializers, then FirstInit, then desktop */
    logl("[DSK] first init detected; running system initializers");

    /* mouseInit.elf — safe PS/2 mouse init */
    {   char mn[12]; /* MOUSE   ELF */
        mn[0]='M';mn[1]='O';mn[2]='U';mn[3]='S';mn[4]='E';mn[5]=' ';mn[6]=' ';mn[7]=' ';mn[8]='E';mn[9]='L';mn[10]='F';
        logl("[DSK] loading mouseInit");
        if (dsk_load_and_run(mn, ctx) == 0) logl("[DSK] mouseInit returned");
        else logl("[DSK] mouseInit not found");
    }

    /* netman.elf — network configuration */
    {   char nn[12]; /* NETMAN  ELF */
        nn[0]='N';nn[1]='E';nn[2]='T';nn[3]='M';nn[4]='A';nn[5]='N';nn[6]=' ';nn[7]=' ';nn[8]='E';nn[9]='L';nn[10]='F';
        logl("[DSK] loading netman");
        if (dsk_load_and_run(nn, ctx) == 0) logl("[DSK] netman returned");
        else logl("[DSK] netman not found");
    }

    /* FirstInit.elf — user setup wizard; returns to DSK when done */
    {   char en[12]; /* FIRSTINIT */
        en[0]='F';en[1]='I';en[2]='R';en[3]='S';en[4]='T';
        en[5]='I';en[6]='N';en[7]='I';en[8]='T';en[9]=' ';en[10]=0;
        logl("[DSK] loading FirstInit");
        if (dsk_load_and_run(en, ctx) != 0) {
            logl("[DSK] FirstInit not found, loading desktop directly");
            dsk_run_desktop(ctx);
            return;
        }
        logl("[DSK] FirstInit returned, persisting user.conf");
        dsk_persist_userconf(ctx);
        logl("[DSK] loading desktop");
        dsk_run_desktop(ctx);
    }
}

__attribute__((noreturn, visibility("default")))
void dsk_entry(const dsk_boot_context *ctx) {
    __asm__ volatile("cli");  /* disable IRQs — ps2kbd/IRQ1 & mouse/IRQ12 handlers crash during boot */
    logl("[DSK] boot");
    if(!ctx||ctx->magic!=DSK_BOOT_MAGIC){logl("[DSK] bad context");for(;;)__asm__("hlt");}
    logl("[DSK] context ok");
    dsk_spinner_and_check(ctx);
    logl("[DSK] SELFTEST PASS");
    for(;;)__asm__("hlt");
}
