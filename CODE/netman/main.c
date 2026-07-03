/* netman — Deshab network manager initializer
 * 读取 /system/deshab64/network/conf/conf.conf（SATA 早期镜像里为 root 目录 NETCONF.CNF），
 * 解析网络配置；枚举内核 netdev 注册表；对具备收发能力的接口通过 e1000 真实收发
 * 跑最小 DHCP 客户端拿到 IP。
 */

#include "../UTSM/include/utsm/dsk.h"

typedef signed char        i8;
typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;

typedef int (*block_read_fn)(u32 index, u64 lba, u32 count, void *buf);
typedef u32 (*net_device_count_fn)(void);
typedef int (*net_device_info_fn)(u32 index, void *out);
typedef int (*net_tx_fn)(u32 index, const void *packet, u32 length);
typedef int (*net_rx_poll_fn)(u32 index, void *buffer, u32 capacity, u32 *out_length);

#define DKM_NET_F_LINK_UP  (1u << 0)
#define DKM_NET_F_TX_READY (1u << 1)
#define DKM_NET_F_RX_READY (1u << 2)

typedef struct {
    const char *name;
    u8 mac[6];
    u32 flags;
} nm_net_device_info;

#define COM1 0x3F8

static inline void outb(u16 port, u8 value) { __asm__ volatile("outb %0,%1"::"a"(value),"Nd"(port)); }
static inline u8 inb(u16 port) { u8 v; __asm__ volatile("inb %1,%0":"=a"(v):"Nd"(port)); return v; }

static void sputc(char c) {
    for (u32 i=0;i<100000;i++) if (inb(COM1+5)&0x20) break;
    outb(COM1,(u8)c);
}
static void swrite(const char *s) { while(*s){ if(*s=='\n') sputc('\r'); sputc(*s++);} }
static void logl(const char *s) { swrite(s); swrite("\n"); }

static void log_hex(const char *p, u64 v) {
    static const char h[]="0123456789abcdef";
    char b[19]; int i=0; b[i++]='0'; b[i++]='x';
    for(int j=15;j>=0;j--) b[i++]=h[(v>>(j*4))&0xf];
    b[i]=0; swrite(p); swrite(b); swrite("\n");
}

static void *memset_nm(void *d, int c, u64 n) { u8 *p=(u8*)d; while(n--)*p++=(u8)c; return d; }
static int streqn(const char *a, const char *b, u32 n) { for(u32 i=0;i<n;i++) if(a[i]!=b[i]) return 0; return 1; }
static int neq11(const char *a, const char *b) { for(int i=0;i<11;i++) if(a[i]!=b[i]) return 0; return 1; }
static int str_eq(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b) return 0;
        a++;
        b++;
    }
    return *a == 0 && *b == 0;
}
static u16 r16(const u8 *p) { return (u16)p[0]|((u16)p[1]<<8); }
static u32 r32(const u8 *p) { return (u32)p[0]|((u32)p[1]<<8)|((u32)p[2]<<16)|((u32)p[3]<<24); }

#pragma pack(push,1)
typedef struct {
    u8 jmp[3]; char oem[8]; u16 bps; u8 spc; u16 rsvd; u8 fc; u16 root_ent;
    u16 ts16; u8 media; u16 spf16; u16 spt; u16 heads; u32 hidden; u32 ts32;
    u32 spf; u16 flags; u16 ver; u32 root_clus; u16 fsi; u16 bkboot;
    u8 res[12]; u8 drv; u8 ntfl; u8 sig; u32 ser; char lbl[11]; char typ[8];
    u8 code[420]; u16 boot_sig;
} fat32_bpb;

typedef struct {
    char name[11]; u8 attr; u8 ntr; u8 ctenth;
    u16 ctime; u16 cdate; u16 adate; u16 chigh;
    u16 wtime; u16 wdate; u16 clow; u32 fsize;
} fat32_de;
#pragma pack(pop)

static block_read_fn g_block_read;
static net_device_count_fn g_net_device_count;
static net_device_info_fn g_net_device_info;
static net_tx_fn g_net_tx;
static net_rx_poll_fn g_net_rx_poll;
static u8 g_disk[131072];
static u8 g_cluster[4096];
static char g_conf[4096];

static int read_sectors(u32 lba, u32 count, u8 *out) {
    return g_block_read ? g_block_read(0, lba, count, out) : -1;
}

static int find_entry(const u8 *clus, u32 clus_sectors, const char *target, u32 *out_clus, u32 *out_size) {
    const fat32_de *dir = (const fat32_de *)clus;
    for (u32 e=0; e*32 < clus_sectors*512; e++) {
        if (dir[e].name[0]==0) break;
        if ((u8)dir[e].name[0]==0xE5) continue;
        if (dir[e].attr==0x0F) continue;
        if (dir[e].attr & 0x08) continue;
        if (neq11(dir[e].name, target)) {
            *out_clus = ((u32)r16((const u8*)&dir[e].chigh)<<16) | r16((const u8*)&dir[e].clow);
            *out_size = dir[e].fsize;
            return 0;
        }
    }
    return -1;
}

static int fat32_read_root_file(const char *name11, char *out, u32 out_cap, u32 *out_size) {
    if (read_sectors(0, 256, g_disk) != 0) return -1;
    const fat32_bpb *bpb = (const fat32_bpb *)g_disk;
    if (bpb->boot_sig != 0xAA55 || bpb->bps != 512 || bpb->spf == 0) return -2;
    u32 data_lba = bpb->rsvd + (u32)bpb->fc * bpb->spf;
    u32 fat_byte_off = bpb->rsvd * 512;
    u32 spc = bpb->spc;
    u32 clus = bpb->root_clus;
    u32 found_clus=0, found_size=0;
    int found=0;
    while (clus >= 2 && clus < 0x0FFFFFF8 && !found) {
        u32 lba = data_lba + (clus - 2) * spc;
        const u8 *cb;
        if ((u64)lba * 512 + (u64)spc * 512 <= 256ULL * 512) cb = g_disk + (u64)lba * 512;
        else { if (read_sectors(lba, spc, g_cluster) != 0) return -3; cb = g_cluster; }
        if (find_entry(cb, spc, name11, &found_clus, &found_size) == 0) { found=1; break; }
        u32 fo = fat_byte_off + clus * 4;
        if (fo + 4 > sizeof(g_disk)) break;
        clus = r32(g_disk + fo) & 0x0FFFFFFF;
    }
    if (!found) return -4;
    if (found_size + 1 > out_cap) return -5;
    u32 remaining = found_size;
    u32 fc = found_clus;
    char *dst = out;
    while (fc >= 2 && fc < 0x0FFFFFF8 && remaining > 0) {
        u32 lba = data_lba + (fc - 2) * spc;
        u32 bytes = spc * 512;
        if (bytes > remaining) bytes = remaining;
        if (read_sectors(lba, spc, g_cluster) != 0) return -6;
        for (u32 i=0;i<bytes;i++) dst[i]=(char)g_cluster[i];
        dst += bytes; remaining -= bytes;
        u32 fo = fat_byte_off + fc * 4;
        if (fo + 4 > sizeof(g_disk)) break;
        fc = r32(g_disk + fo) & 0x0FFFFFFF;
    }
    out[found_size] = 0;
    *out_size = found_size;
    return 0;
}

typedef struct {
    char mode[16];
    char device[16];
    char name[64];
    char password[64];
    char ip[16];
    char dns[16];
} net_conf;

static void copy_value(char *dst, u32 cap, const char *src, u32 len) {
    if (!cap) return;
    u32 n = len < cap-1 ? len : cap-1;
    for (u32 i=0;i<n;i++) dst[i]=src[i];
    dst[n]=0;
}

static void parse_conf(const char *txt, net_conf *cfg) {
    copy_value(cfg->mode, sizeof(cfg->mode), "dhcp", 4);
    copy_value(cfg->device, sizeof(cfg->device), "auto", 4);
    copy_value(cfg->ip, sizeof(cfg->ip), "dhcp", 4);
    copy_value(cfg->dns, sizeof(cfg->dns), "auto", 4);
    cfg->name[0]=0; cfg->password[0]=0;
    const char *p = txt;
    while (*p) {
        const char *line = p;
        u32 len = 0;
        while (p[len] && p[len]!='\n' && p[len]!='\r') len++;
        const char *eq = 0;
        for (u32 i=0;i<len;i++) if (line[i]=='=') { eq = line + i; break; }
        if (eq) {
            u32 klen = (u32)(eq - line);
            const char *v = eq + 1;
            u32 vlen = len - klen - 1;
            if (klen==12 && streqn(line,"network.mode",12)) copy_value(cfg->mode,sizeof(cfg->mode),v,vlen);
            else if (klen==14 && streqn(line,"network.device",14)) copy_value(cfg->device,sizeof(cfg->device),v,vlen);
            else if (klen==12 && streqn(line,"network.name",12)) copy_value(cfg->name,sizeof(cfg->name),v,vlen);
            else if (klen==16 && streqn(line,"network.password",16)) copy_value(cfg->password,sizeof(cfg->password),v,vlen);
            else if (klen==10 && streqn(line,"network.ip",10)) copy_value(cfg->ip,sizeof(cfg->ip),v,vlen);
            else if (klen==11 && streqn(line,"network.dns",11)) copy_value(cfg->dns,sizeof(cfg->dns),v,vlen);
        }
        p += len;
        while (*p=='\n' || *p=='\r') p++;
    }
}

static void log_cfg(const net_conf *cfg) {
    logl("[netman] config loaded");
    swrite("[netman] mode="); logl(cfg->mode);
    swrite("[netman] device="); logl(cfg->device);
    swrite("[netman] name="); logl(cfg->name[0] ? cfg->name : "<empty>");
    swrite("[netman] ip="); logl(cfg->ip);
    swrite("[netman] dns="); logl(cfg->dns);
    u32 pwlen=0; while(cfg->password[pwlen]) pwlen++;
    log_hex("[netman] password length=", pwlen);
}

static void log_mac(const u8 mac[6]) {
    static const char h[]="0123456789abcdef";
    char b[18];
    int p=0;
    for (int i=0;i<6;i++) {
        if (i) b[p++]=':';
        b[p++]=h[(mac[i]>>4)&0xf];
        b[p++]=h[mac[i]&0xf];
    }
    b[p]=0;
    swrite(" mac=");
    swrite(b);
}

/* ---- 最小 DHCP 客户端：Ethernet + IPv4 + UDP，通过 e1000 真实收发 ---- */

static u8 g_pktbuf[2048];
static u8 g_rxbuf[2048];
static u8 g_local_mac[6];
static u32 g_lease_ip;
static u32 g_server_ip;
static u32 g_gateway_ip;
static u32 g_dns_ip;
static u32 g_dhcp_xid = 0x21030201u;

/* 累加和，用于 IPv4 头部校验 */
static u16 ip_checksum(const u8 *data, u32 len) {
    u32 sum = 0;
    for (u32 i=0;i+1<len;i+=2) sum += (u32)((data[i]<<8)|data[i+1]);
    if (len & 1) sum += (u32)(data[len-1]<<8);
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return (u16)(~sum);
}

static u16 udp_checksum_ipv4(const u8 *ip, const u8 *udp, u32 udp_len) {
    u32 sum = 0;
    for (u32 i=12;i<20;i+=2) sum += (u32)((ip[i]<<8)|ip[i+1]);
    sum += 17;
    sum += udp_len;
    for (u32 i=0;i+1<udp_len;i+=2) sum += (u32)((udp[i]<<8)|udp[i+1]);
    if (udp_len & 1) sum += (u32)(udp[udp_len-1]<<8);
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    u16 out = (u16)(~sum);
    return out ? out : 0xffff;
}

static void delay_spin(u32 loops) {
    for (volatile u32 i=0;i<loops;i++) { __asm__ volatile("pause"); }
}

/* 构造并发送一个 DHCP 报文。type: 1=DISCOVER 3=REQUEST */
static int dhcp_send(u32 index, u8 type, u32 requested_ip, u32 server_ip) {
    memset_nm(g_pktbuf, 0, sizeof(g_pktbuf));
    u8 *p = g_pktbuf;

    /* Ethernet (14) */
    for (int i=0;i<6;i++) p[i] = 0xff;                 /* dst broadcast */
    for (int i=0;i<6;i++) p[6+i] = g_local_mac[i];
    p[12]=0x08; p[13]=0x00;                            /* IPv4 */

    u8 *ip = p + 14;
    u8 *udp = ip + 20;
    u8 *dhcp = udp + 8;

    /* DHCP/bootp */
    dhcp[0]=1; dhcp[1]=1; dhcp[2]=6; dhcp[3]=0;
    dhcp[4]=(u8)(g_dhcp_xid>>24); dhcp[5]=(u8)(g_dhcp_xid>>16);
    dhcp[6]=(u8)(g_dhcp_xid>>8);  dhcp[7]=(u8)(g_dhcp_xid);
    dhcp[10]=0x80; dhcp[11]=0x00;                      /* broadcast flag */
    for (int i=0;i<6;i++) dhcp[28+i]=g_local_mac[i];   /* chaddr */
    dhcp[236]=99; dhcp[237]=130; dhcp[238]=83; dhcp[239]=99; /* magic cookie */

    u32 opt = 240;
    dhcp[opt++]=53; dhcp[opt++]=1; dhcp[opt++]=type;
    if (type == 3) {
        dhcp[opt++]=50; dhcp[opt++]=4;
        dhcp[opt++]=(u8)(requested_ip>>24); dhcp[opt++]=(u8)(requested_ip>>16);
        dhcp[opt++]=(u8)(requested_ip>>8);  dhcp[opt++]=(u8)(requested_ip);
        dhcp[opt++]=54; dhcp[opt++]=4;
        dhcp[opt++]=(u8)(server_ip>>24); dhcp[opt++]=(u8)(server_ip>>16);
        dhcp[opt++]=(u8)(server_ip>>8);  dhcp[opt++]=(u8)(server_ip);
    }
    dhcp[opt++]=55; dhcp[opt++]=3; dhcp[opt++]=1; dhcp[opt++]=3; dhcp[opt++]=6;
    dhcp[opt++]=255;

    u32 dhcp_len = opt;
    if (dhcp_len < 548) dhcp_len = 548;
    u32 udp_len = 8 + dhcp_len;
    u32 ip_len = 20 + udp_len;

    /* UDP (8) */
    udp[0]=0x00; udp[1]=68; udp[2]=0x00; udp[3]=67;
    udp[4]=(u8)(udp_len>>8); udp[5]=(u8)(udp_len);
    udp[6]=0; udp[7]=0;

    /* IPv4 (20) */
    ip[0]=0x45; ip[1]=0x00;
    ip[2]=(u8)(ip_len>>8); ip[3]=(u8)(ip_len);
    ip[4]=0; ip[5]=0; ip[6]=0x00; ip[7]=0x00;
    ip[8]=64; ip[9]=17; ip[10]=0; ip[11]=0;
    ip[12]=0;ip[13]=0;ip[14]=0;ip[15]=0;
    ip[16]=255;ip[17]=255;ip[18]=255;ip[19]=255;
    u16 ipck = ip_checksum(ip, 20);
    ip[10]=(u8)(ipck>>8); ip[11]=(u8)(ipck);

    u16 udpck = udp_checksum_ipv4(ip, udp, udp_len);
    udp[6]=(u8)(udpck>>8); udp[7]=(u8)(udpck);

    u32 frame_len = 14 + ip_len;
    if (frame_len < 60) frame_len = 60;
    return g_net_tx(index, g_pktbuf, frame_len);
}

/* 解析收到的帧，若为本机 xid 的 DHCP 回复则返回 message type。 */
static int dhcp_parse(const u8 *frame, u32 len, u32 *out_yiaddr, u32 *out_server, u32 *out_router, u32 *out_dns) {
    if (len < 14+20+8+240) return -1;
    if (frame[12]!=0x08 || frame[13]!=0x00) return -1;
    const u8 *ip = frame + 14;
    if ((ip[0]>>4)!=4) return -1;
    u32 ihl = (ip[0]&0xf)*4;
    if (ihl < 20 || len < 14 + ihl + 8 + 240) return -1;
    u16 ip_total = (u16)((ip[2]<<8)|ip[3]);
    if (ip_total < ihl + 8 + 240 || len < 14 + ip_total) return -1;
    if (ip[9]!=17) return -1;
    const u8 *udp = ip + ihl;
    u16 udp_len = (u16)((udp[4]<<8)|udp[5]);
    if (udp_len < 8 + 240 || udp_len > ip_total - ihl) return -1;
    u16 dport = (u16)((udp[2]<<8)|udp[3]);
    if (dport != 68) return -1;
    const u8 *dhcp = udp + 8;
    u32 xid = ((u32)dhcp[4]<<24)|((u32)dhcp[5]<<16)|((u32)dhcp[6]<<8)|dhcp[7];
    if (xid != g_dhcp_xid) return -1;
    u32 yi = ((u32)dhcp[16]<<24)|((u32)dhcp[17]<<16)|((u32)dhcp[18]<<8)|dhcp[19];
    if (!(dhcp[236]==99&&dhcp[237]==130&&dhcp[238]==83&&dhcp[239]==99)) return -1;
    u32 o = 240;
    u8 msg_type = 0;
    u32 server = 0;
    u32 router = 0;
    u32 dns = 0;
    u32 cap = udp_len - 8;
    while (o < cap) {
        u8 code = dhcp[o++];
        if (code == 255) break;
        if (code == 0) continue;
        if (o >= cap) break;
        u8 l = dhcp[o++];
        if (o + l > cap) break;
        if (code == 53 && l >= 1) msg_type = dhcp[o];
        if (code == 54 && l >= 4)
            server = ((u32)dhcp[o]<<24)|((u32)dhcp[o+1]<<16)|((u32)dhcp[o+2]<<8)|dhcp[o+3];
        if (code == 3 && l >= 4)
            router = ((u32)dhcp[o]<<24)|((u32)dhcp[o+1]<<16)|((u32)dhcp[o+2]<<8)|dhcp[o+3];
        if (code == 6 && l >= 4)
            dns = ((u32)dhcp[o]<<24)|((u32)dhcp[o+1]<<16)|((u32)dhcp[o+2]<<8)|dhcp[o+3];
        o += l;
    }
    *out_yiaddr = yi;
    *out_server = server;
    if (out_router) *out_router = router;
    if (out_dns) *out_dns = dns;
    return msg_type;
}

static void log_ipv4(const char *pre, u32 ip_hostorder) {
    char b[16]; int p=0;
    for (int s=3;s>=0;s--) {
        u32 oct = (ip_hostorder >> (s*8)) & 0xff;
        if (oct==0) { b[p++]='0'; }
        else {
            int started=0;
            for (u32 d=100; d>=1; d/=10) {
                u32 dig=(oct/d)%10;
                if (dig||started){ b[p++]=(char)('0'+dig); started=1; }
                if (d==1) break;
            }
        }
        if (s) b[p++]='.';
    }
    b[p]=0;
    swrite(pre); swrite(b); swrite("\n");
}

/* 对指定 netdev 跑一次 DHCP DISCOVER/OFFER/REQUEST/ACK。成功返回 0。 */
static int dhcp_run(u32 index, const u8 mac[6]) {
    if (!g_net_tx || !g_net_rx_poll) { logl("[netman] DHCP: net tx/rx api missing"); return -1; }
    for (int i=0;i<6;i++) g_local_mac[i]=mac[i];
    g_lease_ip = 0; g_server_ip = 0; g_gateway_ip = 0; g_dns_ip = 0;

    u32 offer_ip = 0, offer_srv = 0, offer_router = 0, offer_dns = 0;
    int got_offer = 0;
    u32 rx_frames = 0;
    for (u32 round=0; round<4 && !got_offer; round++) {
        logl("[netman] DHCP: sending DISCOVER");
        int txrc = dhcp_send(index, 1, 0, 0);
        log_hex("[netman] DHCP DISCOVER tx rc=", (u64)(i8)txrc);
        if (txrc != 0) { logl("[netman] DHCP: DISCOVER tx failed"); return -1; }

        for (u32 attempt=0; attempt<120000 && !got_offer; attempt++) {
            u32 rlen = 0;
            int rc = g_net_rx_poll(index, g_rxbuf, sizeof(g_rxbuf), &rlen);
            if (rc == 0 && rlen > 0) {
                rx_frames++;
                int t = dhcp_parse(g_rxbuf, rlen, &offer_ip, &offer_srv, &offer_router, &offer_dns);
                if (t == 2) { got_offer = 1; break; }
            }
            delay_spin(300);
        }
    }
    log_hex("[netman] DHCP rx frames seen=", rx_frames);
    if (!got_offer) { logl("[netman] DHCP: no OFFER received"); return -2; }
    log_ipv4("[netman] DHCP OFFER ip=", offer_ip);

    u32 ack_ip=0, ack_srv=0, ack_router=0, ack_dns=0;
    int got_ack=0;
    for (u32 round=0; round<4 && !got_ack; round++) {
        logl("[netman] DHCP: sending REQUEST");
        if (dhcp_send(index, 3, offer_ip, offer_srv) != 0) { logl("[netman] DHCP: REQUEST tx failed"); return -3; }

        for (u32 attempt=0; attempt<120000 && !got_ack; attempt++) {
            u32 rlen=0;
            int rc = g_net_rx_poll(index, g_rxbuf, sizeof(g_rxbuf), &rlen);
            if (rc == 0 && rlen > 0) {
                int t = dhcp_parse(g_rxbuf, rlen, &ack_ip, &ack_srv, &ack_router, &ack_dns);
                if (t == 5) { got_ack=1; break; }
            }
            delay_spin(300);
        }
    }
    if (!got_ack) { logl("[netman] DHCP: no ACK received"); return -4; }

    g_lease_ip = ack_ip; g_server_ip = ack_srv;
    g_gateway_ip = ack_router ? ack_router : (offer_router ? offer_router : 0x0a000202u);
    g_dns_ip = ack_dns ? ack_dns : (offer_dns ? offer_dns : g_gateway_ip);
    log_ipv4("[netman] DHCP ACK, leased ip=", ack_ip);
    log_ipv4("[netman] DHCP server=", ack_srv);
    log_ipv4("[netman] gateway=", g_gateway_ip);
    log_ipv4("[netman] dns=", g_dns_ip);
    logl("[netman] DHCP: connected");
    return 0;
}

static int arp_resolve_gateway(u32 index, u32 target_ip, u8 out_mac[6]) {
    memset_nm(g_pktbuf, 0, sizeof(g_pktbuf));
    u8 *p = g_pktbuf;
    for (int i=0;i<6;i++) p[i]=0xff;
    for (int i=0;i<6;i++) p[6+i]=g_local_mac[i];
    p[12]=0x08; p[13]=0x06;
    u8 *a = p + 14;
    a[0]=0x00; a[1]=0x01;
    a[2]=0x08; a[3]=0x00;
    a[4]=6; a[5]=4;
    a[6]=0x00; a[7]=0x01;
    for (int i=0;i<6;i++) a[8+i]=g_local_mac[i];
    a[14]=(u8)(g_lease_ip>>24); a[15]=(u8)(g_lease_ip>>16); a[16]=(u8)(g_lease_ip>>8); a[17]=(u8)g_lease_ip;
    a[24]=(u8)(target_ip>>24); a[25]=(u8)(target_ip>>16); a[26]=(u8)(target_ip>>8); a[27]=(u8)target_ip;
    logl("[netman] ARP: who-has gateway");
    if (g_net_tx(index, g_pktbuf, 60) != 0) return -1;
    for (u32 attempt=0; attempt<160000; attempt++) {
        u32 rlen=0;
        int rc = g_net_rx_poll(index, g_rxbuf, sizeof(g_rxbuf), &rlen);
        if (rc == 0 && rlen >= 42 && g_rxbuf[12]==0x08 && g_rxbuf[13]==0x06) {
            const u8 *r = g_rxbuf + 14;
            u16 op = (u16)((r[6]<<8)|r[7]);
            u32 spa = ((u32)r[14]<<24)|((u32)r[15]<<16)|((u32)r[16]<<8)|r[17];
            if (op == 2 && spa == target_ip) {
                for (int i=0;i<6;i++) out_mac[i]=r[8+i];
                logl("[netman] ARP: gateway resolved");
                return 0;
            }
        }
        delay_spin(300);
    }
    logl("[netman] ARP: timeout");
    return -2;
}

static int dns_query_a(u32 index, const u8 dst_mac[6], u32 dns_ip, const char *name, u32 *out_ip) {
    memset_nm(g_pktbuf, 0, sizeof(g_pktbuf));
    u8 *p = g_pktbuf;
    for (int i=0;i<6;i++) p[i]=dst_mac[i];
    for (int i=0;i<6;i++) p[6+i]=g_local_mac[i];
    p[12]=0x08; p[13]=0x00;
    u8 *ip = p + 14;
    u8 *udp = ip + 20;
    u8 *dns = udp + 8;
    dns[0]=0x12; dns[1]=0x34;
    dns[2]=0x01; dns[3]=0x00;
    dns[4]=0x00; dns[5]=0x01;
    u32 q=12;
    const char *s=name;
    while (*s) {
        const char *dot=s;
        u32 l=0;
        while (dot[l] && dot[l] != '.') l++;
        dns[q++]=(u8)l;
        for (u32 i=0;i<l;i++) dns[q++]=(u8)dot[i];
        s = dot + l;
        if (*s=='.') s++;
    }
    dns[q++]=0;
    dns[q++]=0x00; dns[q++]=0x01;
    dns[q++]=0x00; dns[q++]=0x01;
    u32 dns_len=q;
    u32 udp_len=8+dns_len;
    u32 ip_len=20+udp_len;
    udp[0]=0xC0; udp[1]=0x01;
    udp[2]=0x00; udp[3]=0x35;
    udp[4]=(u8)(udp_len>>8); udp[5]=(u8)udp_len;
    udp[6]=0; udp[7]=0;
    ip[0]=0x45; ip[1]=0;
    ip[2]=(u8)(ip_len>>8); ip[3]=(u8)ip_len;
    ip[4]=0x12; ip[5]=0x34; ip[6]=0; ip[7]=0;
    ip[8]=64; ip[9]=17; ip[10]=0; ip[11]=0;
    ip[12]=(u8)(g_lease_ip>>24); ip[13]=(u8)(g_lease_ip>>16); ip[14]=(u8)(g_lease_ip>>8); ip[15]=(u8)g_lease_ip;
    ip[16]=(u8)(dns_ip>>24); ip[17]=(u8)(dns_ip>>16); ip[18]=(u8)(dns_ip>>8); ip[19]=(u8)dns_ip;
    u16 ipck=ip_checksum(ip,20); ip[10]=(u8)(ipck>>8); ip[11]=(u8)ipck;
    u16 udpck=udp_checksum_ipv4(ip,udp,udp_len); udp[6]=(u8)(udpck>>8); udp[7]=(u8)udpck;
    logl("[netman] DNS: query example.com A");
    if (g_net_tx(index, g_pktbuf, 14+ip_len) != 0) return -1;
    for (u32 attempt=0; attempt<240000; attempt++) {
        u32 rlen=0;
        int rc=g_net_rx_poll(index,g_rxbuf,sizeof(g_rxbuf),&rlen);
        if (rc==0 && rlen>=14+20+8+12 && g_rxbuf[12]==0x08 && g_rxbuf[13]==0x00) {
            const u8 *rip=g_rxbuf+14;
            u32 ihl=(rip[0]&0xf)*4;
            if ((rip[0]>>4)!=4 || rip[9]!=17 || rlen<14+ihl+8+12) { delay_spin(300); continue; }
            const u8 *rudp=rip+ihl;
            if (rudp[2]!=0xC0 || rudp[3]!=0x01) { delay_spin(300); continue; }
            const u8 *rdns=rudp+8;
            if (rdns[0]!=0x12 || rdns[1]!=0x34) { delay_spin(300); continue; }
            u16 an=(u16)((rdns[6]<<8)|rdns[7]);
            u32 off=12;
            while (off<rlen && rdns[off]) off += 1 + rdns[off];
            off += 5;
            for (u16 ai=0; ai<an && off+12<=rlen; ai++) {
                if ((rdns[off]&0xC0)==0xC0) off += 2; else { while (off<rlen && rdns[off]) off += 1 + rdns[off]; off++; }
                if (off+10>rlen) break;
                u16 typ=(u16)((rdns[off]<<8)|rdns[off+1]);
                u16 cls=(u16)((rdns[off+2]<<8)|rdns[off+3]);
                u16 rdlen=(u16)((rdns[off+8]<<8)|rdns[off+9]);
                off += 10;
                if (typ==1 && cls==1 && rdlen==4 && off+4<=rlen) {
                    *out_ip=((u32)rdns[off]<<24)|((u32)rdns[off+1]<<16)|((u32)rdns[off+2]<<8)|rdns[off+3];
                    log_ipv4("[netman] DNS example.com=", *out_ip);
                    return 0;
                }
                off += rdlen;
            }
        }
        delay_spin(300);
    }
    logl("[netman] DNS: timeout");
    return -2;
}

static int network_full_connect(u32 index, const u8 mac[6]) {
    int rc = dhcp_run(index, mac);
    log_hex("[netman] DHCP final rc=", (u64)(i8)rc);
    if (rc != 0) return rc;
    u8 gw_mac[6];
    rc = arp_resolve_gateway(index, g_gateway_ip, gw_mac);
    log_hex("[netman] ARP final rc=", (u64)(i8)rc);
    if (rc != 0) return rc;
    u32 resolved=0;
    rc = dns_query_a(index, gw_mac, g_dns_ip, "example.com", &resolved);
    log_hex("[netman] DNS final rc=", (u64)(i8)rc);
    return rc;
}

static void query_netdevs(const net_conf *cfg) {
    if (!g_net_device_count || !g_net_device_info) {
        logl("[netman] net API not ready; configuration staged only");
        return;
    }

    u32 count = g_net_device_count();
    int dhcp_done = 0;
    int dhcp_enabled = cfg && !str_eq(cfg->mode, "disabled") && !str_eq(cfg->mode, "off") && !str_eq(cfg->ip, "static");
    log_hex("[netman] netdev_count=", count);
    for (u32 i=0;i<count;i++) {
        nm_net_device_info info;
        memset_nm(&info, 0, sizeof(info));
        int rc = g_net_device_info(i, &info);
        if (rc != 0) {
            log_hex("[netman] netdev info rc=", (u64)(i8)rc);
            continue;
        }
        swrite("[netman] netdev index=");
        log_hex("", i);
        swrite("[netman]   name=");
        swrite(info.name ? info.name : "<noname>");
        log_mac(info.mac);
        swrite(" flags=");
        log_hex("", info.flags);
        if (info.flags & DKM_NET_F_LINK_UP) logl("[netman]   link=up");
        else logl("[netman]   link=down");
        if (info.flags & DKM_NET_F_TX_READY) logl("[netman]   tx=ready");
        if (info.flags & DKM_NET_F_RX_READY) logl("[netman]   rx=ready");

        if (!dhcp_done && dhcp_enabled &&
            (info.flags & (DKM_NET_F_LINK_UP|DKM_NET_F_TX_READY|DKM_NET_F_RX_READY))
            == (DKM_NET_F_LINK_UP|DKM_NET_F_TX_READY|DKM_NET_F_RX_READY)) {
            int drc = network_full_connect(i, info.mac);
            log_hex("[netman] network final rc=", (u64)(i8)drc);
            dhcp_done = 1;
        }
    }
    if (!dhcp_enabled) logl("[netman] DHCP skipped by config");
}

__attribute__((visibility("default")))
void dsk_entry(const dsk_boot_context *ctx) {
    logl("[netman] boot");
    if (!ctx || ctx->magic != DSK_BOOT_MAGIC) { logl("[netman] bad context"); return; }
    u64 api = ctx->dkm_kernel_api;
    if (api) {
        u64 block_api = *(u64 *)(api + 0xA8);
        if (block_api) g_block_read = (block_read_fn)*(u64 *)(block_api + 16);
        u64 net_api = *(u64 *)(api + 0x48);
        log_hex("[netman] kernel_api.net=", net_api);
        if (net_api) {
            g_net_device_count = (net_device_count_fn)*(u64 *)(net_api + 8);
            g_net_device_info = (net_device_info_fn)*(u64 *)(net_api + 16);
            g_net_tx = (net_tx_fn)*(u64 *)(net_api + 24);
            g_net_rx_poll = (net_rx_poll_fn)*(u64 *)(net_api + 32);
            log_hex("[netman] net.tx ptr=", (u64)g_net_tx);
            log_hex("[netman] net.rx ptr=", (u64)g_net_rx_poll);
        }
    }
    u32 size=0;
    int rc = fat32_read_root_file("NETCONF CNF", g_conf, sizeof(g_conf), &size);
    log_hex("[netman] conf read rc=", (u64)(i8)rc);
    if (rc != 0) {
        logl("[netman] using built-in defaults");
        g_conf[0]=0;
    } else {
        log_hex("[netman] conf size=", size);
    }
    net_conf cfg;
    memset_nm(&cfg, 0, sizeof(cfg));
    parse_conf(g_conf, &cfg);
    log_cfg(&cfg);
    if (str_eq(cfg.mode, "disabled") || str_eq(cfg.mode, "off")) {
        logl("[netman] networking disabled by config");
    }
    query_netdevs(&cfg);
    logl("[netman] done");
}
