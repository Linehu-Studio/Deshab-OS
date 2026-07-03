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
static u8 g_disk[131072];
static u8 g_cluster[4096];
static u8 g_fdata[262144];

static int fat32_read_sectors(u32 lba, u32 count, u8 *out) { return g_block_read ? g_block_read(0,lba,count,out) : -1; }

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

static int dsk_check_firstinit(int *is_first) {
    char name[12]; /* "FIRSTINTXT" on stack — no PIE reloc */
    name[0]='F';name[1]='I';name[2]='R';name[3]='S';name[4]='T';
    name[5]='I';name[6]='N';name[7]='T';name[8]='X';name[9]='T';name[10]=0;
    u8 *data = 0; u32 size = 0;
    logl("[DSK] fat32_read_path start");
    int rc = fat32_read_root_file(name, &data, &size);
    logh("[DSK] fat32_read_path rc=", (u64)(i64)rc);
    if (rc != 0) { logl("[DSK] firstInit.txt not found"); *is_first = 1; return 0; }
    if (data[0] == '0') { logl("[DSK] firstInit=0"); *is_first = 1; return 0; }
    logl("[DSK] firstInit != 0"); *is_first = 0; return 0;
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
    static u8 ibuf[262144];
    u8 *image = ibuf;
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

static void dsk_spinner_and_check(u32 *fb_arg, i64 cx, i64 cy, i64 r, i64 thk,
                                   const u32 arc_colors[4], u32 bg, const void *boot_ctx_ptr) {
    (void)bg;
    (void)fb_arg;
    const dsk_boot_context *ctx = (const dsk_boot_context *)boot_ctx_ptr;
    { u64 api = ctx->dkm_kernel_api;
      g_block_read = (block_read_fn)*(u64 *)(api + 0xA8);
      if (g_block_read) g_block_read = (block_read_fn)*(u64 *)((u64)g_block_read + 16); }

#define SPR 128
    static u32 sprite[SPR*SPR];
    i64 sp=SPR*4, scx=SPR/2, scy=SPR/2, bx=cx-scx, by=cy-scy;
    i64 frame=0, base=0; int checked=0; void *fe_entry=0;

    g_fb_addr=ctx->framebuffer_address; g_fb_w=ctx->framebuffer_width;
    g_fb_h=ctx->framebuffer_height; g_fb_p=ctx->framebuffer_pitch;
    u32 *sfb=(u32*)(u64)g_fb_addr;
    dsk_fill_gradient_rect(sfb, 0, 0, (i64)g_fb_w, (i64)g_fb_h);
    u64 sw=g_fb_w, sh=g_fb_h, spv=g_fb_p;
    logh("[DSK] block_read ptr=", (u64)g_block_read);
    logl("[DSK] entering spinner loop");

    while (1) {
        g_fb_w=SPR; g_fb_h=SPR; g_fb_p=sp;
        /* per-row gradient so sprite interior matches background */
        static u32 row_bg[SPR];
        for (i64 y0=0; y0<SPR; y0++) {
            i64 ry = by + y0;
            if (ry < 0) ry = 0;
            if ((u64)ry >= sh) ry = (i64)sh - 1;
            row_bg[y0] = blend_color(BG_TOP, BG_BOTTOM, (u32)(((u64)ry * 255ULL) / (sh - 1)));
            u32 *ln=&sprite[y0*SPR]; for(i64 x0=0;x0<SPR;x0++) ln[x0]=row_bg[y0];
        }
        {
            i64 ro=r+thk, ri=r-thk;
            for (i64 dq=0; dq<4; dq++) {
                i64 sa=base+dq*90, sw2=82; u32 clr=arc_colors[dq];
                for(i64 dy=-ro-1;dy<=ro+1;dy++){i64 yy=scy+dy; if(yy<0||yy>=SPR)continue;
                u32 sbg=row_bg[yy];
                for(i64 dx=-ro-1;dx<=ro+1;dx++){i64 xx=scx+dx; if(xx<0||xx>=SPR)continue;
                  i64 d2=dx*dx+dy*dy; if(d2<ri*ri-2||d2>(ro+1)*(ro+1)+2)continue;
                  i64 ang; if(dx==0)ang=dy<0?90:270;
                  else{i64 ax=dx<0?-dx:dx,ay=dy<0?-dy:dy,a0=ax>ay?ay*45/ax:90-ax*45/(ay?ay:1);
                       if(dx>=0&&dy<=0)ang=a0;else if(dx<0&&dy<=0)ang=180-a0;
                       else if(dx<0&&dy>0)ang=180+a0;else ang=360-a0;}
                  i64 a=ang-sa; if(a<0)a+=360; if((u64)a>(u64)sw2)continue;
                  u32 fade; if((u64)a<6)fade=(u32)(a*255/6);else if((u64)a>(u64)(sw2-6))fade=(u32)((sw2-a)*255/6);else fade=255;
                  u64 vv=(u64)d2; u64 xr=vv,yr=(xr+1)>>1; while(yr<xr){xr=yr;yr=(yr+vv/yr)>>1;} i64 dist=(i64)xr;
                  u32 eff=((sbg&0xFF)*(256-fade)+(clr&0xFF)*fade)>>8;
                  u32 ge=(((sbg>>8)&0xFF)*(256-fade)+((clr>>8)&0xFF)*fade)>>8;
                  u32 be=(((sbg>>16)&0xFF)*(256-fade)+((clr>>16)&0xFF)*fade)>>8;
                  eff=0xFF000000|(be<<16)|(ge<<8)|eff;
                  if(dist>=ri&&dist<=ro)sprite[(u64)yy*SPR+(u64)xx]=eff;
                }}
            }
            i64 ri2=r-8;
            for(i64 dy=-ri2-2;dy<=ri2+2;dy++){i64 yy2=scy+dy; if(yy2<0||yy2>=SPR)continue;
            u32 sbg2=row_bg[yy2];
            for(i64 dx=-ri2-2;dx<=ri2+2;dx++){i64 xx2=scx+dx; if(xx2<0||xx2>=SPR)continue;
              i64 d2=dx*dx+dy*dy;
              if(d2<=ri2*ri2){
                u32 ic=((sbg2&0xFF)*176+0xD0*80)>>8;
                u32 ig=(((sbg2>>8)&0xFF)*176+0xF0*80)>>8;
                u32 ib=(((sbg2>>16)&0xFF)*176+0xFF*80)>>8;
                sprite[(u64)yy2*SPR+(u64)xx2]=0xFF000000|(ib<<16)|(ig<<8)|ic;
              }
            }}
        }
        g_fb_w=sw; g_fb_h=sh; g_fb_p=spv;
        dsk_fill_gradient_rect(sfb, bx, by, SPR, SPR);
        for(i64 sr=0;sr<SPR;sr++){i64 sy=by+sr; if(sy<0||(u64)sy>=g_fb_h)continue;
          u32 *dst=(u32*)((u8*)sfb+(u64)sy*g_fb_p)+bx; u32 *src=&sprite[sr*SPR];
          for(i64 sc=0;sc<SPR;sc++){i64 sx=bx+sc; if(sx<0||(u64)sx>=g_fb_w)continue; dst[sc]=src[sc];}}
        for(volatile u32 d=0;d<72000;d++)__asm__("pause");
        base-=2; if(base<0)base+=360; frame++;

        if(frame>=100&&!checked){checked=1;  /* was 540, reduced for test */
            logl("[DSK] checking firstInit...");
            if(g_block_read){logl("[DSK] have block read, calling dsk_check_firstinit");
                int is_first=0;
                if(dsk_check_firstinit(&is_first)==0){
                    if(is_first){
                        logl("[DSK] first init detected; running system initializers");

                        /* DSK owns system component initialization. FirstInit is only user setup. */
                        char mn[12]; /* MOUSE   ELF */
                        mn[0]='M';mn[1]='O';mn[2]='U';mn[3]='S';mn[4]='E';mn[5]=' ';mn[6]=' ';mn[7]=' ';mn[8]='E';mn[9]='L';mn[10]='F';
                        u8 *md=0; u32 ms=0; void *me=0;
                        if(fat32_read_root_file(mn,&md,&ms)==0){
                            if(dsk_load_elf(md,ms,&me)==0){
                                logl("[DSK] jumping to mouseInit");
                                void(*mentry)(const dsk_boot_context*)=(void(*)(const dsk_boot_context*))me;
                                mentry(ctx);
                                logl("[DSK] mouseInit returned");
                            }
                        }

                        char nn[12]; /* NETMAN  ELF */
                        nn[0]='N';nn[1]='E';nn[2]='T';nn[3]='M';nn[4]='A';nn[5]='N';nn[6]=' ';nn[7]=' ';nn[8]='E';nn[9]='L';nn[10]='F';
                        u8 *nd=0; u32 ns=0; void *ne=0;
                        if(fat32_read_root_file(nn,&nd,&ns)==0){
                            if(dsk_load_elf(nd,ns,&ne)==0){
                                logl("[DSK] jumping to netman");
                                void(*nentry)(const dsk_boot_context*)=(void(*)(const dsk_boot_context*))ne;
                                nentry(ctx);
                                logl("[DSK] netman returned");
                            }
                        }

                        logl("[DSK] loading FirstInit.elf");
                        char en[12]; /* FIRSTINIT */
                        en[0]='F';en[1]='I';en[2]='R';en[3]='S';en[4]='T';
                        en[5]='I';en[6]='N';en[7]='I';en[8]='T';en[9]=' ';en[10]=0;
                        u8 *fd=0; u32 fs=0;
                        if(fat32_read_root_file(en,&fd,&fs)==0){
                            if(dsk_load_elf(fd,fs,&fe_entry)==0) logl("[DSK] FirstInit.elf loaded");
                        }
                    } else {
                        logl("[DSK] firstInit=1, skipping user setup, loading shell directly");
                    }
                }
            }
        }
        if(fe_entry&&frame>150){logl("[DSK] jumping to FirstInit");  /* was 600 */
            void(*entry)(const dsk_boot_context*)=(void(*)(const dsk_boot_context*))fe_entry;
            entry(ctx);
            logl("[DSK] FirstInit returned, loading shell");
            /* FirstInit 返回后加载 shell */
            char sh[12]; /* SHELL   ELF */
            sh[0]='S';sh[1]='H';sh[2]='E';sh[3]='L';sh[4]='L';sh[5]=' ';sh[6]=' ';sh[7]=' ';sh[8]='E';sh[9]='L';sh[10]='F';
            u8 *sd=0; u32 ss=0; void *se=0;
            if(fat32_read_root_file(sh,&sd,&ss)==0){
                if(dsk_load_elf(sd,ss,&se)==0){
                    logl("[DSK] jumping to shell");
                    void(*sentry)(const dsk_boot_context*)=(void(*)(const dsk_boot_context*))se;
                    sentry(ctx);
                }
            }
            for(;;)__asm__("hlt");
        }
        /* 非首次启动：直接加载 shell */
        if(checked&&!fe_entry&&frame>150){
            logl("[DSK] loading shell directly (non-first boot)");
            char sh[12]; /* SHELL   ELF */
            sh[0]='S';sh[1]='H';sh[2]='E';sh[3]='L';sh[4]='L';sh[5]=' ';sh[6]=' ';sh[7]=' ';sh[8]='E';sh[9]='L';sh[10]='F';
            u8 *sd=0; u32 ss=0; void *se=0;
            if(fat32_read_root_file(sh,&sd,&ss)==0){
                if(dsk_load_elf(sd,ss,&se)==0){
                    logl("[DSK] jumping to shell");
                    void(*sentry)(const dsk_boot_context*)=(void(*)(const dsk_boot_context*))se;
                    sentry(ctx);
                }
            }
            for(;;)__asm__("hlt");
        }
        if(!g_block_read&&frame>800){logl("[DSK] no block device, skipping firstInit");break;}
        if(checked&&!fe_entry&&frame>800){logl("[DSK] firstInit not found, continuing normal boot");break;}
    }
}

__attribute__((noreturn, visibility("default")))
void dsk_entry(const dsk_boot_context *ctx) {
    __asm__ volatile("cli");  /* disable IRQs — ps2kbd/IRQ1 & mouse/IRQ12 handlers crash during spinner */
    logl("[DSK] boot");
    if(!ctx||ctx->magic!=DSK_BOOT_MAGIC){logl("[DSK] bad context");for(;;)__asm__("hlt");}
    logl("[DSK] context ok");
    g_fb_addr=ctx->framebuffer_address; g_fb_w=ctx->framebuffer_width;
    g_fb_h=ctx->framebuffer_height; g_fb_p=ctx->framebuffer_pitch;
    u32 bg=BG_MID, arcs[4]={0xFF4488CC,0xFF2266AA,0xFF66AAEE,0xFF88CCFF};
    i64 cx=(i64)g_fb_w/2,cy=(i64)g_fb_h/2,r=48,thk=4;
    dsk_spinner_and_check((u32*)(u64)g_fb_addr,cx,cy,r,thk,arcs,bg,ctx);
    logl("[DSK] SELFTEST PASS");
    for(;;)__asm__("hlt");
}
