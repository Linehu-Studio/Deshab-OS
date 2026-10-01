/* drr_core.c — DRR Dedicated Recovery Root 核心实现
 *
 * 替换原 drr_stub.c，提供完整的：
 *   - Emergency Pool（独立于普通堆分配器）
 *   - Recovery Log（write intent/commit 追踪）
 *   - Checkpoint A/B 双槽（CRC 校验、原子 active slot 切换）
 *   - Root key 管理
 *   - Heartbeat
 */

#include <utsm/drr.h>
#include <utsm/log.h>
#include <utsm/dma.h>
#include <utsm/segment.h>
#include <utsm/config.h>
#include <utsm/dkm.h>
#include <utsm/crypto.h>
#include <utsm/blake2b.h>

extern void outb(u16 port, u8 value);

/* ---- 内部内存操作（不依赖 libc） ---- */
static void drr_memzero(void *dst, u64 n) {
    u8 *d = (u8 *)dst;
    while (n--) *d++ = 0;
}

static void drr_memcpy(void *dst, const void *src, u64 n) {
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    while (n--) *d++ = *s++;
}

/* ---- CRC64-ECMA ---- */
static const u64 crc64_table[256] = {
    0x0000000000000000ULL, 0x42F0E1EBA5EA001AULL, 0x85E1C3D753D46D34ULL, 0xC711223CF63E6D2EULL,
    0x493366450E42ECDFULL, 0x0BC387AEABA8ECC5ULL, 0xCCD2A5925D9681EBULL, 0x8E224479F87C81F1ULL,
    0x9266CC8A1C85D9BEULL, 0xD0962D61B96FD9A4ULL, 0x17870F5D4F51B48AULL, 0x5577EEB6EABBB490ULL,
    0xDB55AACF12C73561ULL, 0x99A54B24B72D357BULL, 0x5EB4691841135855ULL, 0x1C4488F3E4F9584FULL,
    0x2644C6D0383BF54AULL, 0x64B4273B9DD1F550ULL, 0xA3A505076BEF987EULL, 0xE155E4ECCE059864ULL,
    0x6F77A09536791995ULL, 0x2D87417E9393198FULL, 0xEA96634265AD74A1ULL, 0xA86682A9C04774BBULL,
    0xB4220A5A24BE2CF4ULL, 0xF6D2EBB181542CEEULL, 0x31C3C98D776A41C0ULL, 0x73332866D28041DAULL,
    0xFD116C1F2AFCC02BULL, 0xBFE18DF48F16C031ULL, 0x78F0AFC87928AD1FULL, 0x3A004E23DCC2AD05ULL,
    0x4C898D607077A834ULL, 0x0E796C8BD19DA82EULL, 0xC9684EB723A3C500ULL, 0x8B98AF5C8649C51AULL,
    0x05BAEB257E3544EBULL, 0x474A0ACEDBDF44F1ULL, 0x805B28F22DE129DFULL, 0xC2ABC919880B29C5ULL,
    0xDEEF41EA6CF2718AULL, 0x9C1FA001C9187190ULL, 0x5B0E823D3F261CBEULL, 0x19FE63D69ACC1CA4ULL,
    0x97DC27AF62B09D55ULL, 0xD52CC644C75A9D4FULL, 0x123DE4783164F061ULL, 0x50CD0593948EF07BULL,
    0x6ACD4BB0484C5D7EULL, 0x283DAA5BECA65D64ULL, 0xEF2C88671B98304AULL, 0xADDC698CBE723050ULL,
    0x23FE2DF5460EB1A1ULL, 0x610ECC1EE3E4B1BBULL, 0xA61FEE2215DADC95ULL, 0xE4EF0FC9B030DC8FULL,
    0xF8AB873A54C984C0ULL, 0xBA5B66D1F12384DAULL, 0x7D4A44ED071DE9F4ULL, 0x3FBAA506A2F7E9EEULL,
    0xB198E17F5A8B681FULL, 0xF3680094FF616805ULL, 0x347922A8095F052BULL, 0x7689C343ACB50531ULL,
    0x98131AE0E0ED2F68ULL, 0xDAE3FB0B45072F72ULL, 0x1DF2D937B339425CULL, 0x5F0238DC16D34246ULL,
    0xD1207CA5EEAFC3B7ULL, 0x93D09D4E4B45C3ADULL, 0x54C1B172BD7BAE83ULL, 0x163150991891AE99ULL,
    0x0A75D66AFC68F6D6ULL, 0x488537815982F6CCULL, 0x8F9415BDAFBC9BE2ULL, 0xCD64F4560A569BF8ULL,
    0x4346B02FF22A1A09ULL, 0x01B651C457C01A13ULL, 0xC6A773F8A1FE773DULL, 0x8457921304147727ULL,
    0xBE57DC30D8D6DA22ULL, 0xFCA73DDB7D3CDA38ULL, 0x3BB61FE78B02B716ULL, 0x7946FE0C2EE8B70CULL,
    0xF764BA75D69436FDULL, 0xB5945B9E737E36E7ULL, 0x728579A285405BC9ULL, 0x3075984920AA5BD3ULL,
    0x2C3110BAC453039CULL, 0x6EC1F15161B90386ULL, 0xA9D0D36D97876EA8ULL, 0xEB203286326D6EB2ULL,
    0x650276FFCA11EF43ULL, 0x27F297146FFBEF59ULL, 0xE0E3B52899C58277ULL, 0xA21354C33C2F826DULL,
    0xD49A9780909A875CULL, 0x966A766B35708746ULL, 0x517B5457C34EEA68ULL, 0x138BB5BC66A4EA72ULL,
    0x9DA9F1C59ED86B83ULL, 0xDF59102E3B326B99ULL, 0x18483212CD0C06B7ULL, 0x5AB8D3F968E606ADULL,
    0x46FC5B0A8C1F5EE2ULL, 0x040CBA4129F55EF8ULL, 0xC31D98DDDFCB33D6ULL, 0x81ED79367A2133CCULL,
    0x0FCF3D4F825DB23DULL, 0x4D3FDCA427B7B227ULL, 0x8A2EFE98D189DF09ULL, 0xC8DE1F737463DF13ULL,
    0xF2DE5150A8A17216ULL, 0xB02EB0BB0D4B720CULL, 0x773F9287FB751F22ULL, 0x35CF736C5E9F1F38ULL,
    0xBBED3715A6E39EC9ULL, 0xF91DD6FE03099ED3ULL, 0x3E0CF4C2F537F3FDULL, 0x7CFC152950DDF3E7ULL,
    0x60B89DDAB424ABA8ULL, 0x22487C3111CEABB2ULL, 0xE5595E0DE7F0C69CULL, 0xA7A9BFE6421AC686ULL,
    0x298BFB9FBA664777ULL, 0x6B7B1A741F8C476DULL, 0xAC6A3848E9B22A43ULL, 0xEE9AD9A34C582A59ULL,
};

u64 drr_crc64(const void *data, u64 len) {
    const u8 *p = (const u8 *)data;
    u64 crc = 0xFFFFFFFFFFFFFFFFULL;
    for (u64 i = 0; i < len; i++) {
        crc = crc64_table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFFFFFFFFFULL;
}

/* ---- 全局 DRR 状态 ---- */
static drr_state g_drr;

/* ---- Phase 8: 快照区 / 看门狗 / 恢复行为控制 ---- */
#define DRR_SNAP_MAGIC   0x445252534E415021ULL  /* "DRRSNAP!" */
#define DRR_SNAP_VERSION 1u

typedef struct {
    u64 magic;
    u32 version;
    u32 capacity;     /* 条目容量（page0 空间决定） */
    u32 used;
    u32 generation;
    u64 crc;
} drr_snap_header;

static dkm_dma_buffer g_snap_region;
static int    g_snap_ok = 0;
static u32    g_snap_capacity = 0;
static int    g_reboot_enabled = 1;
static void (*g_sched_fault_cb)(u32 task_slot) = 0;

typedef struct {
    u32 task_slot;        /* 0xFFFFFFFF = 空闲 */
    u32 timeout_ticks;
    u64 last_kick_tick;
} drr_watchdog_entry;
static drr_watchdog_entry g_drr_wd[DRR_WATCHDOG_MAX];

static drr_snap_header *snap_header(void) {
    return (drr_snap_header *)g_snap_region.virt;
}
static drr_ckpt_page_entry *snap_entries(void) {
    return (drr_ckpt_page_entry *)((u8 *)g_snap_region.virt + 64);
}
static u8 *snap_page_ptr(u32 idx) {
    return (u8 *)g_snap_region.virt + 4096ULL * (1ULL + (u64)idx);
}

/* ---- Root key（Phase 9: RDRAND + TSC jitter 混合，boot 生成一次）
 * 生成一次后固化：drr_init 可重入（emergency reset），root key 不换，
 * 否则旧 segment key 全部失效。持久化（sealed disk region）留待后续。 ---- */
static u64 g_root_key_storage[4];
static int  g_root_key_ready = 0;
static const u64 g_root_key_fallback[4] = {
    0x4452525554534D31ULL,  /* "DRRUTSM1" */
    0xFEEDFACECAFEF00DULL,
    0x0123456789ABCDEFULL,
    0xFEDCBA9876543210ULL
};

static u64 drr_tsc(void) {
    u64 v;
    __asm__ volatile("rdtsc" : "=A"(v));
    return v;
}

static int drr_have_rdrand(void) {
    u32 a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1u), "c"(0u));
    return (int)((c >> 30) & 1u);
}

static int drr_rdrand64(u64 *out) {
    u8 ok;
    u64 v;
    __asm__ volatile("rdrand %0; setc %1" : "=r"(v), "=qm"(ok) :: "cc");
    if (!ok) return 0;
    *out = v;
    return 1;
}

static void drr_root_key_generate(void) {
    if (g_root_key_ready) {
        g_drr.root_key = g_root_key_storage;
        return;
    }
    int rdrand_ok = 1;
    if (drr_have_rdrand()) {
        for (int i = 0; i < 4; i++) {
            if (!drr_rdrand64(&g_root_key_storage[i])) {
                rdrand_ok = 0;
                break;
            }
        }
    } else {
        rdrand_ok = 0;
    }

    if (rdrand_ok) {
        /* 纵深：混入 TSC jitter，防 RDRAND 单点失效 */
        for (int i = 0; i < 4; i++) {
            g_root_key_storage[i] ^= drr_tsc() * 0x9e3779b97f4a7c15ULL;
        }
    } else {
        /* 无 RDRAND：常量基底 + 多轮 TSC jitter 混合 */
        for (int i = 0; i < 4; i++) {
            g_root_key_storage[i] = g_root_key_fallback[i];
        }
        for (int r = 0; r < 16; r++) {
            u64 j = drr_tsc();
            for (int i = 0; i < 4; i++) {
                j = (j ^ drr_tsc()) * 0x9e3779b97f4a7c15ULL;
                g_root_key_storage[i] ^= j;
                for (volatile int d = 0; d < 50; d++) { }
            }
        }
    }

    g_root_key_ready = 1;
    g_drr.root_key = g_root_key_storage;
    log_info("[DRR] root key generated");
    log_hex64("[DRR]   rdrand=", (u64)rdrand_ok);
}

/* ---- Emergency Pool 内部分配 ---- */
static void *pool_alloc_internal(u64 size, u64 alignment) {
    u64 current = (u64)&g_drr.pool[0] + g_drr.pool_offset;
    u64 aligned = utsm_align_up_u64(current, alignment);
    u64 new_offset = (aligned - (u64)&g_drr.pool[0]) + size;
    if (new_offset > g_drr.pool_limit) {
        return NULL;
    }
    g_drr.pool_offset = new_offset;
    return (void *)aligned;
}

void *drr_emergency_alloc(u64 size, u64 alignment) {
    if (!g_drr.initialized) return NULL;
    return pool_alloc_internal(size, alignment);
}

void drr_emergency_reset(void) {
    if (!g_drr.initialized) return;
    /* 保留已分配的子缓冲区，仅重置池偏移到子缓冲区之后 */
    /* 注意：此函数仅在 recovery 路径紧急情况下使用 */
    g_drr.pool_offset = 0;
    /* 重新分配子缓冲区 */
    drr_init();
}

/* ---- 初始化 ---- */
void drr_init(void) {
    log_info("[DRR] init begin");
    drr_memzero(&g_drr, sizeof(g_drr));

    g_drr.pool_offset = 0;
    g_drr.pool_limit = DRR_EMERGENCY_POOL_SIZE;
    drr_root_key_generate();
    g_drr.recovery_generation = 1;
    g_drr.heartbeat_interval = 1000;
    g_drr.active_slot = 0; /* A */

    /* 分配 Emergency Pool 子缓冲区 */
    g_drr.stack_base = (u8 *)pool_alloc_internal(DRR_EMERGENCY_STACK_SIZE, 64);
    g_drr.stack_size = DRR_EMERGENCY_STACK_SIZE;
    g_drr.stack_ptr = DRR_EMERGENCY_STACK_SIZE; /* 栈从顶部向下增长 */

    g_drr.log_base = (drr_log_entry *)pool_alloc_internal(DRR_RECOVERY_LOG_SIZE, 64);
    g_drr.log_capacity = DRR_RECOVERY_LOG_SIZE / sizeof(drr_log_entry);
    g_drr.log_count = 0;

    g_drr.ckpt_meta_buf = (drr_checkpoint_meta *)pool_alloc_internal(DRR_CKPT_META_BUF_SIZE, 64);
    g_drr.crash_buf = (u8 *)pool_alloc_internal(DRR_CRASH_BUF_SIZE, 64);
    g_drr.crash_buf_size = DRR_CRASH_BUF_SIZE;
    g_drr.meta_slab = pool_alloc_internal(DRR_META_SLAB_SIZE, 64);
    g_drr.meta_slab_offset = 0;

    /* 初始化 A/B slot */
    drr_memzero(&g_drr.ckpt_a, sizeof(drr_checkpoint_meta));
    drr_memzero(&g_drr.ckpt_b, sizeof(drr_checkpoint_meta));
    g_drr.ckpt_a.magic = DRR_CKPT_MAGIC;
    g_drr.ckpt_a.version = DRR_CKPT_VERSION;
    g_drr.ckpt_a.active = 1;
    g_drr.ckpt_b.magic = DRR_CKPT_MAGIC;
    g_drr.ckpt_b.version = DRR_CKPT_VERSION;

    g_drr.initialized = 1;

    /* ---- Phase 8: 快照区（dma 页，独立于普通堆；失败降级 metadata-only） ---- */
    g_snap_ok = 0;
    g_snap_capacity = 0;
    g_reboot_enabled = 1;
    g_sched_fault_cb = 0;
    for (int wdi = 0; wdi < DRR_WATCHDOG_MAX; wdi++) {
        g_drr_wd[wdi].task_slot = 0xFFFFFFFFu;
        g_drr_wd[wdi].timeout_ticks = 0;
        g_drr_wd[wdi].last_kick_tick = 0;
    }
    {
        /* page0 = 头 + 条目数组，其后 128 页快照数据（共 129 页） */
        dkm_dma_buffer buf;
        if (dma_alloc_pages(1 + 128, 4096, 0xFFFFFFFFULL, &buf) == 0 && buf.virt) {
            g_snap_region = buf;
            u32 cap = (u32)((4096 - 64) / sizeof(drr_ckpt_page_entry));
            drr_snap_header *h = (drr_snap_header *)buf.virt;
            drr_memzero(h, 4096);
            h->magic = DRR_SNAP_MAGIC;
            h->version = DRR_SNAP_VERSION;
            h->capacity = cap;
            h->used = 0;
            g_snap_capacity = cap;
            g_snap_ok = 1;
            log_info("[DRR] snapshot region ok");
            log_hex64("[DRR]   pages=", 1 + 128);
        } else {
            log_warn("[DRR] snapshot region unavailable (metadata-only mode)");
        }
    }

    log_info("[DRR] init ok");
}

const u64 *drr_get_root_key(void) {
    return g_drr.root_key;
}

u64 drr_recovery_generation(void) {
    return g_drr.recovery_generation;
}

/* ---- Fault 报告 ---- */
void drr_report_fault(const char *reason) {
    if (!g_drr.initialized) return;
    log_error("[DRR] FAULT: ");
    log_error(reason);
    /* 记录到 crash buffer */
    if (g_drr.crash_buf && g_drr.crash_buf_size > 0) {
        /* 简单记录：写入 reason 的前 N 字节 */
        u64 i = 0;
        const char *s = reason;
        while (*s && i < g_drr.crash_buf_size - 1) {
            g_drr.crash_buf[i++] = (u8)*s++;
        }
        g_drr.crash_buf[i] = 0;
    }
    /* 增加 recovery generation */
    g_drr.recovery_generation++;
}

/* ---- Recovery Log ---- */
void drr_log_write_intent(u32 segment_slot, u64 offset, u64 len) {
    if (!g_drr.initialized || !g_drr.log_base) return;
    if (g_drr.log_count >= g_drr.log_capacity) {
        /* 日志满，最简单的策略：丢弃最早的条目 */
        for (u32 i = 1; i < g_drr.log_count; i++) {
            g_drr.log_base[i - 1] = g_drr.log_base[i];
        }
        g_drr.log_count--;
    }
    drr_log_entry *e = &g_drr.log_base[g_drr.log_count++];
    e->segment_slot = segment_slot;
    e->flags = DRR_LOG_FLAG_INTENT;
    e->offset = offset;
    e->length = len;
    e->timestamp = g_drr.heartbeat_counter;
}

void drr_log_write_commit(u32 segment_slot, u64 offset, u64 len) {
    if (!g_drr.initialized || !g_drr.log_base) return;
    if (g_drr.log_count >= g_drr.log_capacity) {
        for (u32 i = 1; i < g_drr.log_count; i++) {
            g_drr.log_base[i - 1] = g_drr.log_base[i];
        }
        g_drr.log_count--;
    }
    drr_log_entry *e = &g_drr.log_base[g_drr.log_count++];
    e->segment_slot = segment_slot;
    e->flags = DRR_LOG_FLAG_COMMIT;
    e->offset = offset;
    e->length = len;
    e->timestamp = g_drr.heartbeat_counter;
}

/* ---- Checkpoint ---- */
int drr_checkpoint_begin(drr_checkpoint_type type) {
    if (!g_drr.initialized) return -1;

    /* 选择 inactive slot */
    drr_checkpoint_meta *target;
    if (g_drr.active_slot == 0) {
        target = &g_drr.ckpt_b;
    } else {
        target = &g_drr.ckpt_a;
    }

    target->magic = DRR_CKPT_MAGIC;
    target->version = DRR_CKPT_VERSION;
    target->active = 0; /* 写入时为 inactive */
    target->type = type;
    target->global_epoch = g_drr.recovery_generation;
    target->dirty_page_count = 0;
    target->mac_root = 0;
    target->segment_count = 0;
    target->crc = 0;

    return 0;
}

int drr_checkpoint_finish(void) {
    if (!g_drr.initialized) return -1;

    /* 写入 inactive slot 的 CRC */
    drr_checkpoint_meta *target;
    if (g_drr.active_slot == 0) {
        target = &g_drr.ckpt_b;
    } else {
        target = &g_drr.ckpt_a;
    }

    /* 计算 CRC（不含 crc 字段本身） */
    target->crc = drr_crc64(target,
        sizeof(drr_checkpoint_meta) - sizeof(u64));

    /* 原子切换 active slot */
    target->active = 1;
    /* 旧 slot 标记 inactive */
    if (g_drr.active_slot == 0) {
        g_drr.ckpt_a.active = 0;
        g_drr.active_slot = 1;
    } else {
        g_drr.ckpt_b.active = 0;
        g_drr.active_slot = 0;
    }

    /* 增加 recovery generation */
    g_drr.recovery_generation++;

    /* 清空 recovery log */
    g_drr.log_count = 0;

    return 0;
}

int drr_checkpoint_recover(drr_checkpoint_meta *out_meta) {
    if (!g_drr.initialized || !out_meta) return -1;

    /* 尝试 active slot */
    drr_checkpoint_meta *active = (g_drr.active_slot == 0) ?
        &g_drr.ckpt_a : &g_drr.ckpt_b;
    drr_checkpoint_meta *fallback = (g_drr.active_slot == 0) ?
        &g_drr.ckpt_b : &g_drr.ckpt_a;

    /* 验证 active slot */
    if (active->magic == DRR_CKPT_MAGIC && active->active == 1) {
        u64 expected_crc = active->crc;
        active->crc = 0;
        u64 actual_crc = drr_crc64(active,
            sizeof(drr_checkpoint_meta) - sizeof(u64));
        active->crc = expected_crc;
        if (actual_crc == expected_crc) {
            drr_memcpy(out_meta, active, sizeof(drr_checkpoint_meta));
            return 0;
        }
    }

    /* active 无效，尝试 fallback */
    if (fallback->magic == DRR_CKPT_MAGIC && fallback->active == 1) {
        u64 expected_crc = fallback->crc;
        fallback->crc = 0;
        u64 actual_crc = drr_crc64(fallback,
            sizeof(drr_checkpoint_meta) - sizeof(u64));
        fallback->crc = expected_crc;
        if (actual_crc == expected_crc) {
            drr_memcpy(out_meta, fallback, sizeof(drr_checkpoint_meta));
            return 0;
        }
    }

    /* 双槽都无效 */
    return -2;
}

/* ---- Heartbeat ---- */
void drr_heartbeat_tick(void) {
    if (!g_drr.initialized) return;
    g_drr.heartbeat_counter++;
}

u64 drr_heartbeat_get(void) {
    return g_drr.heartbeat_counter;
}

/* ===================================================================
 *  Phase 8: 真实脏页快照 / 页级回滚 / 看门狗 / system rollback
 * =================================================================== */

int drr_ckpt_snapshot_dirty(void) {
    if (!g_drr.initialized) return -1;
    if (!g_snap_ok) return -2;   /* metadata-only 模式不支持页快照 */

    drr_checkpoint_begin(DRR_CKPT_DIRTY_PAGE);
    drr_snap_header *h = snap_header();
    drr_ckpt_page_entry *entries = snap_entries();
    u32 used = 0;

    for (u32 slot = 0; slot < g_utsm_max_segments; slot++) {
        utsm_segment_desc *desc = utsm_get_segment(slot);
        if (!desc || desc->state != UTSM_SEG_ACTIVE) continue;
        desc->state = UTSM_SEG_CHECKPOINTING;

        for (u32 s = 0; s < desc->dirty_shard_count && desc->dirty_shards; s++) {
            utsm_dirty_shard *shard = &desc->dirty_shards[s];
            if (!shard->bitmap) continue;
            u64 words = (shard->page_count + 63) / 64;
            for (u64 w = 0; w < words && used < g_snap_capacity; w++) {
                u64 word = shard->bitmap[w];
                while (word && used < g_snap_capacity) {
                    u64 bit = (u64)__builtin_ctzll(word);
                    u64 page = w * 64 + bit;
                    u8 *src = desc->cipher_base + page * UTSM_PAGE_SIZE;
                    u8 *dst = snap_page_ptr(used);
                    drr_memcpy(dst, src, UTSM_PAGE_SIZE);
                    entries[used].seg_slot = slot;
                    entries[used].page_index = (u32)page;
                    entries[used].cipher_vaddr = (u64)src;
                    entries[used].crc = drr_crc64(dst, UTSM_PAGE_SIZE);
                    entries[used].key_epoch = desc->key_epoch;
                    entries[used].mac = utsm_page_mac(g_drr.root_key, slot, page,
                                                      desc->key_epoch, dst);
                    entries[used].region_idx = used;
                    entries[used].used = 1;
                    used++;
                    /* 快照成功才清脏位；区满时保留脏位（部分提交，下轮重试） */
                    shard->bitmap[w] &= ~(1ULL << bit);
                    if (shard->dirty_count > 0) shard->dirty_count--;
                    word &= ~(1ULL << bit);
                }
            }
            shard->last_checkpoint_epoch = g_drr.recovery_generation;
        }
        desc->state = UTSM_SEG_ACTIVE;
        desc->checkpoint_generation = g_drr.recovery_generation;
        if (used >= g_snap_capacity) break;
    }

    /* 区域头（crc 不含自身字段） */
    h->used = used;
    h->generation = (u32)g_drr.recovery_generation;
    h->crc = drr_crc64(h, sizeof(drr_snap_header) - sizeof(u64));

    /* 元数据 + A/B 原子切换（begin 已选好 inactive slot，同一表达式取回） */
    drr_checkpoint_meta *target = (g_drr.active_slot == 0) ? &g_drr.ckpt_b : &g_drr.ckpt_a;
    target->dirty_page_count = used;
    /* Phase 9: mac_root = keyed BLAKE2b 链（root_key 派生，绑定全部页 MAC） */
    {
        u8 kb[32];
        u8 chain[64];
        for (int i = 0; i < 4; i++) utsm_store64_le(kb + 8 * i, g_drr.root_key[i]);
        utsm_blake2b_ctx mc;
        utsm_blake2b_init(&mc, 64, kb, 32);
        for (u32 i = 0; i < used; i++) {
            u8 rec[8 + 8 + 8 + 8];
            utsm_store64_le(rec + 0, 0x544F4F524D535455ULL);    /* "UTSMROOT" LE */
            utsm_store64_le(rec + 8, ((u64)entries[i].seg_slot) |
                                     ((u64)entries[i].page_index << 32));
            utsm_store64_le(rec + 16, entries[i].key_epoch);
            utsm_store64_le(rec + 24, entries[i].mac);
            utsm_blake2b_update(&mc, rec, sizeof(rec));
        }
        utsm_blake2b_final(&mc, chain);
        target->mac_root = utsm_load64_le(chain);
    }
    target->segment_count = 0;
    drr_checkpoint_finish();

    log_info("[DRR] ckpt snapshot ok");
    log_hex64("[DRR]   pages=", used);
    return (int)used;
}

int drr_rollback_pages(void) {
    if (!g_drr.initialized) return -1;
    if (!g_snap_ok) return -2;
    drr_checkpoint_meta meta;
    if (drr_checkpoint_recover(&meta) != 0) return -3;
    if (meta.type != DRR_CKPT_DIRTY_PAGE) return -4;

    drr_snap_header *h = snap_header();
    if (h->magic != DRR_SNAP_MAGIC || h->version != DRR_SNAP_VERSION) return -5;
    u32 used = h->used;
    if (used > h->capacity) used = h->capacity;

    drr_ckpt_page_entry *entries = snap_entries();

    /* 触碰段先置 RECOVERING */
    for (u32 i = 0; i < used; i++) {
        utsm_segment_desc *d = utsm_get_segment(entries[i].seg_slot);
        if (d && d->state == UTSM_SEG_ACTIVE) d->state = UTSM_SEG_RECOVERING;
    }

    int failed = 0;
    u32 restored = 0;
    for (u32 i = 0; i < used; i++) {
        drr_ckpt_page_entry *e = &entries[i];
        u8 *snap = snap_page_ptr(e->region_idx);
        utsm_segment_desc *d = utsm_get_segment(e->seg_slot);
        if (drr_crc64(snap, UTSM_PAGE_SIZE) != e->crc) {
            failed = 1;
            if (d) d->state = UTSM_SEG_POISONED;
            log_error("[DRR] rollback crc fail");
            log_hex64("[DRR]   seg_slot=", e->seg_slot);
            log_hex64("[DRR]   page=", e->page_index);
            continue;
        }
        /* Phase 9: 页 MAC 校验（伪造/跨纪元重放/区域篡改在此被拒） */
        u64 mac = utsm_page_mac(g_drr.root_key, e->seg_slot, e->page_index,
                                e->key_epoch, snap);
        if (mac != e->mac) {
            failed = 1;
            if (d) d->state = UTSM_SEG_POISONED;
            log_error("[DRR] rollback mac fail");
            log_hex64("[DRR]   seg_slot=", e->seg_slot);
            log_hex64("[DRR]   page=", e->page_index);
            continue;
        }
        drr_memcpy((void *)e->cipher_vaddr, snap, UTSM_PAGE_SIZE);
        restored++;
        /* 清除该页脏位（P2 之类的回写不再被误判为未 checkpoint） */
        if (d && d->dirty_shard_count > 0 && d->dirty_shards && d->dirty_shards[0].bitmap) {
            utsm_dirty_shard *sh = &d->dirty_shards[0];
            u64 page = e->page_index;
            if (page < sh->page_count && (sh->bitmap[page / 64] & (1ULL << (page % 64)))) {
                sh->bitmap[page / 64] &= ~(1ULL << (page % 64));
                if (sh->dirty_count > 0) sh->dirty_count--;
            }
        }
        if (d && d->state == UTSM_SEG_RECOVERING) d->state = UTSM_SEG_ACTIVE;
    }

    log_info("[DRR] rollback done");
    log_hex64("[DRR]   pages=", restored);
    return failed ? -2 : (int)restored;
}

void drr_system_rollback(const char *reason) {
    log_error("[DRR] SYSTEM ROLLBACK");
    if (reason) log_error(reason);
    log_hex64("[DRR]   generation=", g_drr.recovery_generation);
    log_hex64("[DRR]   heartbeat=", g_drr.heartbeat_counter);
    log_error("[DRR] rebooting (8042 reset)");
    for (;;) {
        __asm__ volatile("cli");
        outb(0x64, 0xFE);   /* 8042 复位（A20 gate 已无关紧要） */
        for (volatile u32 i = 0; i < 1000000u; i++) { }
        __asm__ volatile("hlt");
    }
}

void drr_handle_task_fault(u32 task_slot, const char *reason) {
    log_error("[DRR] FAULT");
    if (reason) log_error(reason);
    log_hex64("[DRR]   task_slot=", task_slot);
    drr_report_fault(reason ? reason : "task fault");
    drr_log_write_intent(task_slot, 0, 0);
    int pages = drr_rollback_pages();
    log_hex64("[DRR]   rollback_pages=", (u64)(i64)pages);
    if (g_sched_fault_cb) {
        g_sched_fault_cb(task_slot);      /* 可能不返回（当前任务被杀+切换） */
        drr_log_write_commit(task_slot, 0, 0);
        return;
    }
    if (g_reboot_enabled) {
        drr_system_rollback(reason ? reason : "task fault (no sched cb)");
    }
    /* reboot disabled（selftest）：仅留日志 */
}

int drr_watchdog_register(u32 task_slot, u32 timeout_ticks) {
    if (!g_drr.initialized || timeout_ticks == 0) return -1;
    for (int i = 0; i < DRR_WATCHDOG_MAX; i++) {
        if (g_drr_wd[i].task_slot == task_slot) {
            g_drr_wd[i].timeout_ticks = timeout_ticks;
            g_drr_wd[i].last_kick_tick = g_drr.heartbeat_counter;
            return 0;
        }
    }
    for (int i = 0; i < DRR_WATCHDOG_MAX; i++) {
        if (g_drr_wd[i].task_slot == 0xFFFFFFFFu) {
            g_drr_wd[i].task_slot = task_slot;
            g_drr_wd[i].timeout_ticks = timeout_ticks;
            g_drr_wd[i].last_kick_tick = g_drr.heartbeat_counter;
            return 0;
        }
    }
    return -2;
}

int drr_watchdog_unregister(u32 task_slot) {
    for (int i = 0; i < DRR_WATCHDOG_MAX; i++) {
        if (g_drr_wd[i].task_slot == task_slot) {
            g_drr_wd[i].task_slot = 0xFFFFFFFFu;
            return 0;
        }
    }
    return -1;
}

void drr_watchdog_kick(u32 task_slot) {
    for (int i = 0; i < DRR_WATCHDOG_MAX; i++) {
        if (g_drr_wd[i].task_slot == task_slot) {
            g_drr_wd[i].last_kick_tick = g_drr.heartbeat_counter;
            return;
        }
    }
}

void drr_watchdog_check(void) {
    if (!g_drr.initialized) return;
    for (int i = 0; i < DRR_WATCHDOG_MAX; i++) {
        u32 slot = g_drr_wd[i].task_slot;
        if (slot == 0xFFFFFFFFu) continue;
        u64 now = g_drr.heartbeat_counter;
        if (now - g_drr_wd[i].last_kick_tick > g_drr_wd[i].timeout_ticks) {
            g_drr_wd[i].last_kick_tick = now;   /* 防重复触发 */
            drr_handle_task_fault(slot, "watchdog timeout");
        }
    }
}

void drr_set_reboot_enabled(int enabled) {
    g_reboot_enabled = enabled;
}

void drr_set_sched_fault_cb(void (*cb)(u32 task_slot)) {
    g_sched_fault_cb = cb;
}

/* kernel_api.drr 服务表（dkm_fill_platform_info 挂入） */
static const drr_recovery_api g_drr_api = {
    .report_fault = drr_report_fault,
    .heartbeat_get = drr_heartbeat_get,
    .emergency_alloc = drr_emergency_alloc,
    .ckpt_snapshot_dirty = drr_ckpt_snapshot_dirty,
    .rollback_pages = drr_rollback_pages
};

const drr_recovery_api *drr_get_recovery_api(void) {
    return &g_drr_api;
}
