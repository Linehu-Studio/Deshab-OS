/* probe.c — Deshab 环形探测缓冲管理
 *
 * 结构化事件记录系统：写入环形缓冲，支持事后回溯分析。
 * 用于记录关键事件（CPU异常、EPT violation、DMA完成等），
 * 通过 shell $probe 命令读取。
 *
 * 设计要点:
 *   - 环形缓冲容量由 FUCK debug.probe_capacity 控制（默认 4096 条）
 *   - 每条记录 40 字节 (tag:4 + tick:4 + args:4*8)
 *   - 写入时无内存分配，仅 head 指针推进
 *   - 满时覆盖最旧记录（环形覆盖）
 *   - 禁用时 probe_write 为空操作
 */
#include <utsm/instr.h>
#include <utsm/log.h>
#include <utsm/types.h>
#include <utsm/arena.h>
#include <utsm/panic.h>
#include "ini_parser.h"

/* ---- 探测缓冲状态 ---- */
static probe_entry *g_probe_buf   = 0;   /* 缓冲基地址（arena 分配） */
static u32 g_probe_cap            = 0;   /* 缓冲容量（条目数） */
static u32 g_probe_head           = 0;   /* 下一条写入位置（递增取模） */
static u32 g_probe_count         = 0;   /* 已写入总条目数 */
static u32 g_probe_dropped       = 0;   /* 因缓冲满而覆盖的条目数 */

void probe_init(const void *cfg_ptr) {
    const ini_config *cfg = (const ini_config *)cfg_ptr;

    u32 cap = (u32)ini_get_int(cfg, "debug", "probe_capacity", 4096);
    if (cap == 0) {
        g_instr_probe_enable = 0;
        return;
    }

    /* 从 arena 分配探测缓冲（对齐到 8 字节） */
    u64 size = (u64)cap * sizeof(probe_entry);
    g_probe_buf = (probe_entry *)kmem_alloc_aligned(size, 8);
    if (!g_probe_buf) {
        g_instr_probe_enable = 0;
        log_warn("[PROBE] arena alloc failed, probe disabled");
        panic_full("PRB-E01 PROBE ARENA ALLOC FAILED",
                   "probe_init: probe buffer arena allocation failed", 0);
        return;
    }

    g_probe_cap    = cap;
    g_probe_head   = 0;
    g_probe_count  = 0;
    g_probe_dropped = 0;

    log_info("[PROBE] init ok, capacity=");
    log_hex64("", g_probe_cap);
}

void probe_write(u32 tag, u64 a0, u64 a1, u64 a2, u64 a3) {
    if (!g_probe_buf || !g_probe_cap) return;

    u32 idx = g_probe_head % g_probe_cap;

    /* 环形覆盖：如果 head >= cap，说明有旧记录被覆盖 */
    if (g_probe_head >= g_probe_cap && g_probe_dropped == 0) {
        /* 仅首次覆盖时计数，避免每条都做减法 */
    }
    if (g_probe_count >= g_probe_cap) {
        g_probe_dropped++;
    }

    g_probe_buf[idx].tag     = tag;
    g_probe_buf[idx].tick    = (u32)instr_rdtsc();
    g_probe_buf[idx].args[0] = a0;
    g_probe_buf[idx].args[1] = a1;
    g_probe_buf[idx].args[2] = a2;
    g_probe_buf[idx].args[3] = a3;

    g_probe_head++;
    g_probe_count++;
}

int probe_read(u32 n, probe_entry *out) {
    if (!g_probe_buf || n == 0) return 0;

    u32 avail = (g_probe_count < g_probe_cap) ? g_probe_count : g_probe_cap;
    if (n > avail) n = avail;

    for (u32 i = 0; i < n; i++) {
        u32 src = (g_probe_head - n + i) % g_probe_cap;
        out[i] = g_probe_buf[src];
    }

    return (int)n;
}

u32 probe_count(void) {
    return g_probe_count;
}

u32 probe_capacity(void) {
    return g_probe_cap;
}

/* ---- 供 dsk_loader 读取内部状态（填充 probe_info） ---- */
probe_entry *probe_buf_address(void) {
    return g_probe_buf;
}

u32 probe_head_value(void) {
    return g_probe_head;
}

u32 probe_dropped_count(void) {
    return g_probe_dropped;
}
