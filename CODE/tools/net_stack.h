/* net_stack.h — Deshab 用户态共享网络协议栈（static inline，无库）
 *
 * 前置条件：包含者已定义 u8/u16/u32/u64/i64 基础类型。
 * 通过 dkm_kernel_api + 0x48 的 net API 收发原始以太帧（轮询模型，无 IRQ）。
 *
 * 能力：eth II / ARP（含应答）/ IPv4 / ICMP echo / UDP DNS / TCP 最小 client。
 * 默认拓扑：QEMU slirp — guest 10.0.2.15 / gw 10.0.2.2 / dns 10.0.2.3。
 *
 * 用法：
 *   ns_init(ctx->dkm_kernel_api)         — 初始化，选第一个有线 tx/rx ready 设备
 *   ns_ping(ip, seq, timeout, &rtt, &ttl) — 单次 ICMP echo
 *   ns_dns_resolve(host, &ip)            — A 记录查询（数字 IP 直接返回）
 *   ns_tcp_connect(ip, port, timeout)
 *   ns_tcp_send(data, len)
 *   ns_tcp_recv(buf, cap, idle_timeout)  — 收至 FIN/超时，返回累计长度
 *   ns_tcp_close()
 *
 * IP 以 u32 网络序数值表示（NS_IP4(10,0,2,15)）。
 */

#ifndef DESHAB_NET_STACK_H
#define DESHAB_NET_STACK_H

/* ---- 默认网络配置（QEMU user-mode slirp） ---- */
#define NS_IP4(a,b,c,d) ((u32)((((u32)(a))<<24)|(((u32)(b))<<16)|(((u32)(c))<<8)|((u32)(d))))
#define NS_GUEST_IP   NS_IP4(10,0,2,15)
#define NS_GATEWAY_IP NS_IP4(10,0,2,2)
#define NS_DNS_IP     NS_IP4(10,0,2,3)
#define NS_NETMASK    NS_IP4(255,255,255,0)

#define NS_ETH_ARP   0x0806
#define NS_ETH_IPV4  0x0800
#define NS_IP_ICMP   1
#define NS_IP_TCP    6
#define NS_IP_UDP    17

#define NS_ARP_CACHE_SIZE 8
#define NS_TCP_RX_CAP     65536

/* dkm_net_device_desc.flags 位（与 utsm/net.h 一致） */
#define NS_NET_F_TX_READY 0x2
#define NS_NET_F_RX_READY 0x4
#define NS_NET_F_WIRELESS 0x8

/* ---- 端口 I/O / TSC（ns_ 前缀避免与包含者冲突） ---- */
static inline void ns_outb(u16 p, u8 v) { __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p)); }
static inline u8 ns_inb(u16 p) { u8 v; __asm__ volatile("inb %1,%0":"=a"(v):"Nd"(p)); return v; }
static inline u64 ns_rdtsc(void) {
    u32 lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((u64)hi << 32) | lo;
}

/* PIT ch0 ~10ms 校准 TSC（与 shell/dsk 同法） */
static u64 ns_tsc_per_ms = 0;
static inline void ns_tsc_calibrate(void) {
    if (ns_tsc_per_ms) return;
    ns_outb(0x43, 0x30);
    ns_outb(0x40, 0x7C);
    ns_outb(0x40, 0x2E);
    u64 t0 = ns_rdtsc();
    u16 prev = 0; u64 loops = 0;
    for (;;) {
        ns_outb(0x43, 0x00);
        u16 cur = (u16)ns_inb(0x40) | ((u16)ns_inb(0x40) << 8);
        if (cur > prev && loops > 10) break;
        prev = cur; loops++;
    }
    ns_tsc_per_ms = (ns_rdtsc() - t0) / 10;
    if (!ns_tsc_per_ms) ns_tsc_per_ms = 3000; /* 兜底 ~3GHz */
}

static inline int ns_deadline_reached(u64 deadline) {
    return (i64)(ns_rdtsc() - deadline) >= 0;
}

/* ---- 字节序/校验和 ---- */
static inline void ns_w16(u8 *p, u16 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }
static inline void ns_w32(u8 *p, u32 v) { p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v; }
static inline u16 ns_r16(const u8 *p) { return (u16)(((u16)p[0] << 8) | p[1]); }
static inline u32 ns_r32(const u8 *p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }

static inline u16 ns_checksum(const void *data, u32 len) {
    const u8 *p = (const u8 *)data;
    u32 sum = 0;
    while (len > 1) { sum += ((u16)p[0] << 8) | p[1]; p += 2; len -= 2; }
    if (len) sum += ((u16)p[0] << 8);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (u16)~sum;
}

static inline void ns_memcpy(void *d, const void *s, u32 n) {
    u8 *dd = (u8 *)d; const u8 *ss = (const u8 *)s;
    while (n--) *dd++ = *ss++;
}
static inline void ns_memset(void *d, u8 c, u32 n) {
    u8 *dd = (u8 *)d; while (n--) *dd++ = c;
}

/* ---- 十进制/IP 字符串 ---- */
static inline void ns_u32_dec(char *buf, u32 v) {
    char tmp[12]; int n = 0;
    if (!v) { buf[0] = '0'; buf[1] = 0; return; }
    while (v && n < 11) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    for (int i = 0; i < n; i++) buf[i] = tmp[n - 1 - i];
    buf[n] = 0;
}

static inline void ns_fmt_ip(u32 ip, char *out) {
    int p = 0;
    for (int i = 3; i >= 0; i--) {
        char d[4]; ns_u32_dec(d, (ip >> (i * 8)) & 0xFF);
        for (int j = 0; d[j]; j++) out[p++] = d[j];
        if (i) out[p++] = '.';
    }
    out[p] = 0;
}

/* 解析 "a.b.c.d" → 网络序 u32。返回 0 成功。 */
static inline int ns_parse_ip(const char *s, u32 *out) {
    u32 v = 0;
    for (int part = 0; part < 4; part++) {
        u32 n = 0; int digits = 0;
        while (*s >= '0' && *s <= '9') {
            n = n * 10 + (u32)(*s - '0');
            if (n > 255) return -1;
            s++; digits++;
        }
        if (!digits) return -1;
        v = (v << 8) | n;
        if (part < 3) {
            if (*s != '.') return -1;
            s++;
        }
    }
    if (*s && *s != ' ') return -1;
    *out = v;
    return 0;
}

/* ---- net API 绑定 ---- */
typedef int (*ns_net_tx_fn)(u32 index, const void *packet, u32 length);
typedef int (*ns_net_rx_poll_fn)(u32 index, void *buffer, u32 capacity, u32 *out_length);
typedef u32 (*ns_net_count_fn)(void);
typedef int (*ns_net_info_fn)(u32 index, void *out);

static int             ns_ready = 0;
static u32             ns_dev_index = 0;
static u8              ns_mac[6] = {0};
static ns_net_tx_fn    ns_net_tx = 0;
static ns_net_rx_poll_fn ns_net_rx_poll_fn_ptr = 0;
static u16             ns_ip_id = 1;

/* dkm_net_device_info 布局：name ptr @0, mac[6] @8, flags @16（对齐填充后 size 24） */
typedef struct __attribute__((packed)) {
    u64 name;
    u8  mac[6];
    u16 pad;
    u32 flags;
} ns_dev_info;

static inline int ns_init(u64 dkm_kernel_api) {
    if (ns_ready) return 0;
    if (!dkm_kernel_api) return -1;
    u64 net = *(u64 *)(dkm_kernel_api + 0x48);
    if (!net) return -2;
    ns_net_count_fn device_count = (ns_net_count_fn)*(u64 *)(net + 0x08);
    ns_net_info_fn  device_info  = (ns_net_info_fn)*(u64 *)(net + 0x10);
    ns_net_tx       = (ns_net_tx_fn)*(u64 *)(net + 0x18);
    ns_net_rx_poll_fn_ptr = (ns_net_rx_poll_fn)*(u64 *)(net + 0x20);
    if (!device_count || !device_info || !ns_net_tx || !ns_net_rx_poll_fn_ptr) return -3;
    u32 n = device_count();
    if (!n) return -4;

    int picked = -1;
    ns_dev_info info;
    for (u32 i = 0; i < n && i < 8; i++) {
        ns_memset(&info, 0, sizeof(info));
        if (device_info(i, &info) != 0) continue;
        if (info.flags & NS_NET_F_WIRELESS) continue;
        if ((info.flags & NS_NET_F_TX_READY) && (info.flags & NS_NET_F_RX_READY)) {
            picked = (int)i;
            for (int k = 0; k < 6; k++) ns_mac[k] = info.mac[k];
            break;
        }
    }
    if (picked < 0) {
        ns_memset(&info, 0, sizeof(info));
        if (device_info(0, &info) != 0) return -5;
        picked = 0;
        for (int k = 0; k < 6; k++) ns_mac[k] = info.mac[k];
    }
    ns_dev_index = (u32)picked;
    ns_tsc_calibrate();
    ns_ready = 1;
    return 0;
}

/* ---- 帧缓冲 ---- */
static u8 ns_txbuf[2048];
static u8 ns_rxframe[2048];

/* 发送以太帧 */
static inline int ns_send_eth(const u8 dst_mac[6], u16 ethertype, const u8 *payload, u32 len) {
    if (len + 14 > sizeof(ns_txbuf)) return -1;
    for (int i = 0; i < 6; i++) ns_txbuf[i] = dst_mac[i];
    for (int i = 0; i < 6; i++) ns_txbuf[6 + i] = ns_mac[i];
    ns_w16(ns_txbuf + 12, ethertype);
    ns_memcpy(ns_txbuf + 14, payload, len);
    u32 total = len + 14;
    if (total < 60) { /* 以太网最小帧 */
        ns_memset(ns_txbuf + total, 0, 60 - total);
        total = 60;
    }
    return ns_net_tx(ns_dev_index, ns_txbuf, total);
}

/* ---- ARP ---- */
typedef struct {
    u32 ip;
    u8  mac[6];
    u8  valid;
} ns_arp_entry;

static ns_arp_entry ns_arp_cache[NS_ARP_CACHE_SIZE];
static u8 ns_arp_reply_mac[6];
static volatile int ns_arp_reply_valid = 0;

static inline void ns_arp_cache_put(u32 ip, const u8 mac[6]) {
    int slot = -1;
    for (int i = 0; i < NS_ARP_CACHE_SIZE; i++) {
        if (ns_arp_cache[i].valid && ns_arp_cache[i].ip == ip) { slot = i; break; }
    }
    if (slot < 0) {
        for (int i = 0; i < NS_ARP_CACHE_SIZE; i++) {
            if (!ns_arp_cache[i].valid) { slot = i; break; }
        }
    }
    if (slot < 0) slot = 0;
    ns_arp_cache[slot].ip = ip;
    for (int k = 0; k < 6; k++) ns_arp_cache[slot].mac[k] = mac[k];
    ns_arp_cache[slot].valid = 1;
}

static inline int ns_arp_cache_get(u32 ip, u8 mac[6]) {
    for (int i = 0; i < NS_ARP_CACHE_SIZE; i++) {
        if (ns_arp_cache[i].valid && ns_arp_cache[i].ip == ip) {
            for (int k = 0; k < 6; k++) mac[k] = ns_arp_cache[i].mac[k];
            return 0;
        }
    }
    return -1;
}

/* 构造 ARP 包到 out（42 字节），oper 1=request 2=reply */
static inline void ns_arp_build(u8 *out, u16 oper,
                                const u8 sha[6], u32 spa,
                                const u8 tha[6], u32 tpa) {
    ns_w16(out + 0, 1);            /* htype eth */
    ns_w16(out + 2, NS_ETH_IPV4);  /* ptype */
    out[4] = 6; out[5] = 4;
    ns_w16(out + 6, oper);
    for (int i = 0; i < 6; i++) out[8 + i] = sha[i];
    ns_w32(out + 14, spa);
    for (int i = 0; i < 6; i++) out[18 + i] = tha[i];
    ns_w32(out + 24, tpa);
}

/* 处理收到的 ARP 包：应答对我们的请求，记录 reply。 */
static inline void ns_arp_handle(const u8 *pkt, u32 len) {
    if (len < 28) return;
    u16 oper = ns_r16(pkt + 6);
    u32 spa = ns_r32(pkt + 14);
    u32 tpa = ns_r32(pkt + 24);
    if (oper == 1 && tpa == NS_GUEST_IP) {
        /* 请求我们的 IP → 回复 */
        u8 reply[28];
        ns_arp_build(reply, 2, ns_mac, NS_GUEST_IP, pkt + 8, spa);
        ns_send_eth(pkt + 8, NS_ETH_ARP, reply, 28);
    } else if (oper == 2) {
        ns_arp_cache_put(spa, pkt + 8);
        for (int k = 0; k < 6; k++) ns_arp_reply_mac[k] = pkt[8 + k];
        ns_arp_reply_valid = 1;
    }
}

/* 轮询一帧；ARP 内部处理（不返回给调用者）。
 * 返回 0=收到非 ARP 帧（*out_et 为 ethertype），1=超时。 */
static inline int ns_wait_frame(u8 *buf, u32 cap, u32 *out_len, u16 *out_et, u32 timeout_ms) {
    u64 deadline = ns_rdtsc() + ns_tsc_per_ms * timeout_ms;
    for (;;) {
        u32 len = 0;
        int rc = ns_net_rx_poll_fn_ptr(ns_dev_index, buf, cap, &len);
        if (rc == 0 && len >= 14) {
            u16 et = ns_r16(buf + 12);
            if (et == NS_ETH_ARP) {
                ns_arp_handle(buf + 14, len - 14);
                /* 继续等待 */
            } else {
                *out_len = len;
                *out_et = et;
                return 0;
            }
        }
        if (ns_deadline_reached(deadline)) return 1;
        __asm__ volatile("pause");
    }
}

/* ARP 解析 ip → mac，3 次重传。返回 0 成功。 */
static inline int ns_arp_resolve(u32 ip, u8 out_mac[6]) {
    if (ns_arp_cache_get(ip, out_mac) == 0) return 0;
    static const u8 bcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    static const u8 zero_mac[6] = {0,0,0,0,0,0};
    for (int attempt = 0; attempt < 3; attempt++) {
        u8 req[28];
        ns_arp_build(req, 1, ns_mac, NS_GUEST_IP, zero_mac, ip);
        ns_arp_reply_valid = 0;
        if (ns_send_eth(bcast, NS_ETH_ARP, req, 28) != 0) continue;
        u64 deadline = ns_rdtsc() + ns_tsc_per_ms * 1000;
        while (!ns_deadline_reached(deadline)) {
            u32 len = 0;
            int rc = ns_net_rx_poll_fn_ptr(ns_dev_index, ns_rxframe, sizeof(ns_rxframe), &len);
            if (rc == 0 && len >= 14 && ns_r16(ns_rxframe + 12) == NS_ETH_ARP) {
                ns_arp_handle(ns_rxframe + 14, len - 14);
                if (ns_arp_reply_valid) {
                    u32 spa = ns_r32(ns_rxframe + 14 + 14);
                    if (spa == ip) {
                        for (int k = 0; k < 6; k++) out_mac[k] = ns_arp_reply_mac[k];
                        return 0;
                    }
                    ns_arp_reply_valid = 0;
                }
            }
            __asm__ volatile("pause");
        }
    }
    return -1;
}

/* 目标 IP 的下一跳 MAC（同子网直连，否则走网关）。返回 0 成功。 */
static inline int ns_route_mac(u32 dst_ip, u8 out_mac[6]) {
    u32 next = dst_ip;
    if ((dst_ip & NS_NETMASK) != (NS_GUEST_IP & NS_NETMASK)) next = NS_GATEWAY_IP;
    return ns_arp_resolve(next, out_mac);
}

/* ---- IPv4 发送 ---- */
static inline void ns_ipv4_build(u8 *out, u8 proto, u32 src, u32 dst, u32 payload_len) {
    out[0] = 0x45;               /* ver=4 ihl=5 */
    out[1] = 0;                  /* tos */
    ns_w16(out + 2, (u16)(20 + payload_len));
    ns_w16(out + 4, ns_ip_id++);
    ns_w16(out + 6, 0x4000);     /* DF */
    out[8] = 64;                 /* ttl */
    out[9] = proto;
    ns_w16(out + 10, 0);
    ns_w32(out + 12, src);
    ns_w32(out + 16, dst);
    ns_w16(out + 10, ns_checksum(out, 20));
}

/* ---- ICMP echo（ping） ----
 * 返回 0 成功（*out_rtt_ms / *out_ttl 有效），-1 ARP 失败，-2 超时。 */
static inline int ns_ping(u32 dst_ip, u16 seq, u32 timeout_ms, u32 *out_rtt_ms, u8 *out_ttl) {
    u8 dst_mac[6];
    if (ns_route_mac(dst_ip, dst_mac) != 0) return -1;

    u8 pkt[20 + 8 + 32];
    ns_ipv4_build(pkt, NS_IP_ICMP, NS_GUEST_IP, dst_ip, 8 + 32);
    u8 *icmp = pkt + 20;
    icmp[0] = 8;  /* echo request */
    icmp[1] = 0;
    ns_w16(icmp + 2, 0);
    ns_w16(icmp + 4, 0xD5AB);    /* identifier */
    ns_w16(icmp + 6, seq);
    for (int i = 0; i < 32; i++) icmp[8 + i] = (u8)(0x61 + (i % 26));
    ns_w16(icmp + 2, ns_checksum(icmp, 8 + 32));

    if (ns_send_eth(dst_mac, NS_ETH_IPV4, pkt, sizeof(pkt)) != 0) return -3;
    u64 t0 = ns_rdtsc();
    u64 deadline = t0 + ns_tsc_per_ms * timeout_ms;
    while (!ns_deadline_reached(deadline)) {
        u32 len = 0;
        int rc = ns_net_rx_poll_fn_ptr(ns_dev_index, ns_rxframe, sizeof(ns_rxframe), &len);
        if (rc == 0 && len >= 14) {
            u16 et = ns_r16(ns_rxframe + 12);
            if (et == NS_ETH_ARP) {
                ns_arp_handle(ns_rxframe + 14, len - 14);
            } else if (et == NS_ETH_IPV4 && len >= 14 + 20 + 8) {
                const u8 *ip = ns_rxframe + 14;
                u8 ihl = (u8)((ip[0] & 0x0F) * 4);
                if (ihl < 20 || len < 14 + ihl + 8) continue;
                if (ip[9] != NS_IP_ICMP) continue;
                const u8 *ic = ip + ihl;
                if (ic[0] == 0 && ns_r16(ic + 4) == 0xD5AB && ns_r16(ic + 6) == seq) {
                    u64 dt = ns_rdtsc() - t0;
                    if (out_rtt_ms) {
                        u32 ms = (u32)(dt / ns_tsc_per_ms);
                        *out_rtt_ms = ms;
                    }
                    if (out_ttl) *out_ttl = ip[8];
                    return 0;
                }
            }
        }
        __asm__ volatile("pause");
    }
    return -2;
}

/* ---- UDP DNS A 查询 ----
 * host 为数字 IP 时直接解析返回。返回 0 成功。 */
static inline int ns_dns_resolve(const char *host, u32 *out_ip) {
    if (ns_parse_ip(host, out_ip) == 0) return 0;

    u8 dst_mac[6];
    if (ns_route_mac(NS_DNS_IP, dst_mac) != 0) return -1;

    /* 构造查询：header + qname + qtype/qclass */
    u8 q[512];
    u16 txid = (u16)(ns_rdtsc() & 0xFFFF);
    if (!txid) txid = 0x1234;
    ns_w16(q + 0, txid);
    ns_w16(q + 2, 0x0100);   /* RD */
    ns_w16(q + 4, 1);        /* qdcount */
    ns_w16(q + 6, 0); ns_w16(q + 8, 0); ns_w16(q + 10, 0);
    u32 p = 12;
    u32 i = 0;
    while (host[i]) {
        u32 start = p++;
        u8 n = 0;
        while (host[i] && host[i] != '.') {
            if (n >= 63 || p >= 500) return -2;
            q[p++] = (u8)host[i++]; n++;
        }
        q[start] = n;
        if (host[i] == '.') i++;
    }
    q[p++] = 0;
    ns_w16(q + p, 1); p += 2;    /* A */
    ns_w16(q + p, 1); p += 2;    /* IN */
    u32 qlen = p;

    u8 pkt[20 + 8 + 512];
    ns_ipv4_build(pkt, NS_IP_UDP, NS_GUEST_IP, NS_DNS_IP, 8 + qlen);
    u8 *udp = pkt + 20;
    ns_w16(udp + 0, 49153);
    ns_w16(udp + 2, 53);
    ns_w16(udp + 4, (u16)(8 + qlen));
    ns_w16(udp + 6, 0);          /* 不校验 UDP checksum */
    ns_memcpy(udp + 8, q, qlen);

    for (int attempt = 0; attempt < 2; attempt++) {
        if (ns_send_eth(dst_mac, NS_ETH_IPV4, pkt, 20 + 8 + qlen) != 0) continue;
        u64 deadline = ns_rdtsc() + ns_tsc_per_ms * 3000;
        while (!ns_deadline_reached(deadline)) {
            u32 len = 0;
            int rc = ns_net_rx_poll_fn_ptr(ns_dev_index, ns_rxframe, sizeof(ns_rxframe), &len);
            if (rc == 0 && len >= 14) {
                u16 et = ns_r16(ns_rxframe + 12);
                if (et == NS_ETH_ARP) {
                    ns_arp_handle(ns_rxframe + 14, len - 14);
                } else if (et == NS_ETH_IPV4 && len >= 14 + 20 + 8 + 12) {
                    const u8 *ip = ns_rxframe + 14;
                    u8 ihl = (u8)((ip[0] & 0x0F) * 4);
                    if (ihl < 20 || ip[9] != NS_IP_UDP) continue;
                    const u8 *ru = ip + ihl;
                    if (ns_r16(ru + 2) != 49153) continue;
                    const u8 *d = ru + 8;
                    if (ns_r16(d + 0) != txid) continue;
                    u16 qd = ns_r16(d + 4);
                    u16 an = ns_r16(d + 6);
                    u32 off = 12;
                    /* 跳过问题段 */
                    for (u16 qq = 0; qq < qd; qq++) {
                        while (off < len) {
                            u8 b = d[off];
                            if (b == 0) { off++; break; }
                            if ((b & 0xC0) == 0xC0) { off += 2; break; }
                            off += 1 + b;
                        }
                        off += 4;
                    }
                    /* 遍历应答 RR */
                    for (u16 rr = 0; rr < an; rr++) {
                        while (off < len) { /* 跳过 name */
                            u8 b = d[off];
                            if (b == 0) { off++; break; }
                            if ((b & 0xC0) == 0xC0) { off += 2; break; }
                            off += 1 + b;
                        }
                        if (off + 10 > len) break;
                        u16 rtype = ns_r16(d + off);
                        u16 rdlen = ns_r16(d + off + 8);
                        off += 10;
                        if (rtype == 1 && rdlen == 4 && off + 4 <= len) {
                            *out_ip = ns_r32(d + off);
                            return 0;
                        }
                        off += rdlen;
                    }
                }
            }
            __asm__ volatile("pause");
        }
    }
    return -3;
}

/* ---- TCP 最小 client ---- */
#define NS_TCP_CLOSED   0
#define NS_TCP_SYN_SENT 1
#define NS_TCP_ESTAB    2
#define NS_TCP_FIN_WAIT 3

static struct {
    u32 state;
    u32 dst_ip;
    u16 dst_port;
    u16 src_port;
    u32 snd_nxt;
    u32 rcv_nxt;
    u8  dst_mac[6];
} ns_tc = {0, 0, 0, 0, 0, 0, {0}};

static u8  ns_tcp_rx[NS_TCP_RX_CAP];
static u32 ns_tcp_rx_len = 0;
static volatile int ns_tcp_got_fin = 0;
static volatile int ns_tcp_got_rst = 0;

#define NS_TCP_FIN 0x01
#define NS_TCP_SYN 0x02
#define NS_TCP_RST 0x04
#define NS_TCP_PSH 0x08
#define NS_TCP_ACK 0x10

/* 构造并发送 TCP 段（data 可为 0）。 */
static inline int ns_tcp_emit(u8 flags, const u8 *data, u32 len, u32 seq) {
    u8 pkt[20 + 20 + 1460];
    if (len > 1460) return -1;
    u32 tcp_len = 20 + len;
    ns_ipv4_build(pkt, NS_IP_TCP, NS_GUEST_IP, ns_tc.dst_ip, tcp_len);
    u8 *t = pkt + 20;
    ns_w16(t + 0, ns_tc.src_port);
    ns_w16(t + 2, ns_tc.dst_port);
    ns_w32(t + 4, seq);
    ns_w32(t + 8, (flags & NS_TCP_ACK) ? ns_tc.rcv_nxt : 0);
    t[12] = (5 << 4);            /* data offset */
    t[13] = flags;
    ns_w16(t + 14, 64240);       /* window */
    ns_w16(t + 16, 0);
    ns_w16(t + 18, 0);
    if (len) ns_memcpy(t + 20, data, len);
    /* 伪头部校验和 */
    u8 ph[12];
    ns_w32(ph + 0, NS_GUEST_IP);
    ns_w32(ph + 4, ns_tc.dst_ip);
    ph[8] = 0; ph[9] = NS_IP_TCP;
    ns_w16(ph + 10, (u16)tcp_len);
    u32 sum = 0;
    for (u32 k = 0; k < 12; k += 2) sum += ((u16)ph[k] << 8) | ph[k + 1];
    for (u32 k = 0; k + 1 < tcp_len; k += 2) sum += ((u16)t[k] << 8) | t[k + 1];
    if (tcp_len & 1) sum += ((u16)t[tcp_len - 1] << 8);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    ns_w16(t + 16, (u16)~sum);
    return ns_send_eth(ns_tc.dst_mac, NS_ETH_IPV4, pkt, 20 + tcp_len);
}

/* 处理收到的 IPv4/TCP 段。返回 1 表示属于本连接并已处理。 */
static inline int ns_tcp_process(const u8 *ip, u32 ip_len) {
    if (ip_len < 20) return 0;
    u8 ihl = (u8)((ip[0] & 0x0F) * 4);
    if (ihl < 20 || ip_len < ihl + 20) return 0;
    if (ip[9] != NS_IP_TCP) return 0;
    if (ns_r32(ip + 12) != ns_tc.dst_ip) return 0;
    const u8 *t = ip + ihl;
    if (ns_r16(t + 0) != ns_tc.dst_port) return 0;
    if (ns_r16(t + 2) != ns_tc.src_port) return 0;

    u32 seq = ns_r32(t + 4);
    u32 ack = ns_r32(t + 8);
    u8 doff = (u8)((t[12] >> 4) * 4);
    u8 flags = t[13];
    u16 ip_total = ns_r16(ip + 2);
    u32 data_len = (u32)ip_total - ihl - doff;
    const u8 *data = t + doff;

    if (flags & NS_TCP_RST) { ns_tcp_got_rst = 1; ns_tc.state = NS_TCP_CLOSED; return 1; }

    if (ns_tc.state == NS_TCP_SYN_SENT) {
        if ((flags & (NS_TCP_SYN | NS_TCP_ACK)) == (NS_TCP_SYN | NS_TCP_ACK)) {
            ns_tc.rcv_nxt = seq + 1;
            ns_tc.snd_nxt = ack;
            ns_tc.state = NS_TCP_ESTAB;
            ns_tcp_emit(NS_TCP_ACK, 0, 0, ns_tc.snd_nxt);
        }
        return 1;
    }

    if (ns_tc.state == NS_TCP_ESTAB || ns_tc.state == NS_TCP_FIN_WAIT) {
        if (data_len > 0) {
            if (seq == ns_tc.rcv_nxt) {
                u32 room = NS_TCP_RX_CAP - ns_tcp_rx_len;
                u32 take = data_len < room ? data_len : room;
                ns_memcpy(ns_tcp_rx + ns_tcp_rx_len, data, take);
                ns_tcp_rx_len += take;
                ns_tc.rcv_nxt += data_len;
                ns_tcp_emit(NS_TCP_ACK, 0, 0, ns_tc.snd_nxt);
            } else {
                /* 乱序超前或重复段：重 ACK 当前期望序号 */
                ns_tcp_emit(NS_TCP_ACK, 0, 0, ns_tc.snd_nxt);
            }
        }
        if (flags & NS_TCP_FIN) {
            if (seq + data_len == ns_tc.rcv_nxt || data_len == 0) {
                ns_tc.rcv_nxt = seq + data_len + 1;
                ns_tcp_emit(NS_TCP_ACK, 0, 0, ns_tc.snd_nxt);
                ns_tcp_got_fin = 1;
                ns_tc.state = NS_TCP_CLOSED;
            }
        }
        return 1;
    }
    return 1;
}

/* 在超时窗口内收帧并喂给 TCP；返回 0=收到本连接段，1=超时。 */
static inline int ns_tcp_wait(u32 timeout_ms) {
    u64 deadline = ns_rdtsc() + ns_tsc_per_ms * timeout_ms;
    while (!ns_deadline_reached(deadline)) {
        u32 len = 0;
        int rc = ns_net_rx_poll_fn_ptr(ns_dev_index, ns_rxframe, sizeof(ns_rxframe), &len);
        if (rc == 0 && len >= 14) {
            u16 et = ns_r16(ns_rxframe + 12);
            if (et == NS_ETH_ARP) {
                ns_arp_handle(ns_rxframe + 14, len - 14);
            } else if (et == NS_ETH_IPV4) {
                if (ns_tcp_process(ns_rxframe + 14, len - 14)) return 0;
            }
        }
        __asm__ volatile("pause");
    }
    return 1;
}

/* 返回 0 成功（state=ESTAB），-1 ARP 失败，-2 超时/RST。 */
static inline int ns_tcp_connect(u32 dst_ip, u16 dst_port, u32 timeout_ms) {
    if (ns_route_mac(dst_ip, ns_tc.dst_mac) != 0) return -1;
    ns_tc.dst_ip = dst_ip;
    ns_tc.dst_port = dst_port;
    ns_tc.src_port = (u16)(49152 + (ns_rdtsc() & 0x0FFF));
    u32 iss = (u32)ns_rdtsc();
    ns_tcp_rx_len = 0;
    ns_tcp_got_fin = 0;
    ns_tcp_got_rst = 0;

    for (int attempt = 0; attempt < 3; attempt++) {
        ns_tc.state = NS_TCP_SYN_SENT;
        if (ns_tcp_emit(NS_TCP_SYN, 0, 0, iss) != 0) continue;
        u64 deadline = ns_rdtsc() + ns_tsc_per_ms * timeout_ms;
        while (!ns_deadline_reached(deadline)) {
            if (ns_tcp_got_rst) { ns_tc.state = NS_TCP_CLOSED; return -2; }
            if (ns_tc.state == NS_TCP_ESTAB) return 0;
            ns_tcp_wait(50);
        }
    }
    ns_tc.state = NS_TCP_CLOSED;
    return -2;
}

/* 发送数据并等待 ACK（数据段可能在等待期间到达，已入缓冲）。 */
static inline int ns_tcp_send(const u8 *data, u32 len) {
    if (ns_tc.state != NS_TCP_ESTAB) return -1;
    for (int attempt = 0; attempt < 3; attempt++) {
        u32 expect = ns_tc.snd_nxt + len;
        if (ns_tcp_emit((u8)(NS_TCP_PSH | NS_TCP_ACK), data, len, ns_tc.snd_nxt) != 0) return -2;
        u64 deadline = ns_rdtsc() + ns_tsc_per_ms * 2000;
        while (!ns_deadline_reached(deadline)) {
            if (ns_tcp_got_rst) { ns_tc.state = NS_TCP_CLOSED; return -3; }
            /* 检查 snd_una 是否被 ACK 推进：ns_tcp_process 不记录 una，
             * 简单起见此处认为发出后收到任何本连接段即推进。
             * 对 HTTP/1.0 请求-响应模式足够（对端 ACK+数据一起回来）。 */
            if (ns_tcp_wait(100) == 0) {
                ns_tc.snd_nxt = expect;
                return 0;
            }
        }
    }
    return -4;
}

/* 收至 FIN 或空闲超时；把累计数据拷到 buf（最多 cap）。返回长度或负值。 */
static inline int ns_tcp_recv(u8 *buf, u32 cap, u32 idle_timeout_ms) {
    u64 idle_deadline = ns_rdtsc() + ns_tsc_per_ms * idle_timeout_ms;
    for (;;) {
        if (ns_tcp_got_rst) return -1;
        if (ns_tcp_got_fin) break;
        if (ns_deadline_reached(idle_deadline)) break;
        if (ns_tcp_wait(100) == 0) {
            idle_deadline = ns_rdtsc() + ns_tsc_per_ms * idle_timeout_ms;
        }
    }
    u32 n = ns_tcp_rx_len < cap ? ns_tcp_rx_len : cap;
    if (buf && n) ns_memcpy(buf, ns_tcp_rx, n);
    return (int)n;
}

static inline void ns_tcp_close(void) {
    if (ns_tc.state == NS_TCP_ESTAB) {
        ns_tcp_emit((u8)(NS_TCP_FIN | NS_TCP_ACK), 0, 0, ns_tc.snd_nxt);
    }
    ns_tc.state = NS_TCP_CLOSED;
}

#endif /* DESHAB_NET_STACK_H */
