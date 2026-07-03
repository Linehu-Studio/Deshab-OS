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

/* ---- Root key（最终应从安全启动链获取） ---- */
static const u64 g_root_key[4] = {
    0x4452525554534D31ULL,  /* "DRRUTSM1" */
    0xFEEDFACECAFEF00DULL,
    0x0123456789ABCDEFULL,
    0xFEDCBA9876543210ULL
};

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
    g_drr.root_key = g_root_key;
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
