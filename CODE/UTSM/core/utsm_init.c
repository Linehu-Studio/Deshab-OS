#include <utsm/utsm.h>
#include <utsm/segment.h>
#include <utsm/pckc.h>
#include <utsm/crypto.h>
#include <utsm/process.h>
#include <utsm/panic.h>
#include <utsm/log.h>
#include <utsm/config.h>

static utsm_segment_desc g_segments[UTSM_MAX_SEGMENTS];
static u64 g_generation = 1;

void utsm_init(void) {
    log_info("[UTSM] segment table reset begin");
    u32 max_seg = g_utsm_max_segments;
    if (max_seg > UTSM_MAX_SEGMENTS) max_seg = UTSM_MAX_SEGMENTS;
    for (u32 i = 0; i < max_seg; i++) {
        g_segments[i].state = UTSM_SEG_FREE;
        g_segments[i].generation = 1;
    }
    log_info("[UTSM] segment table reset ok");

    g_generation = 1;
    log_info("[UTSM] pckc init begin");
    utsm_pckc_init();
    log_info("[UTSM] pckc init ok");

    /* SAS-R0 特色自证：PCKC 启动期 key 派生往返。派生失败或密钥全零
     * = Per-CPU 密钥缓存没有真正立起来，零降级 panic（PCKC-E01）。 */
    {
        utsm_segment_desc probe;
        for (u32 i = 0; i < sizeof(probe); i++) ((u8 *)&probe)[i] = 0;
        probe.key_epoch = 1;
        utsm_key_material km;
        for (u32 i = 0; i < 4; i++) km.words[i] = 0;
        if (utsm_pckc_get_or_derive(0xFFFFFFFFu, &probe, &km) != UTSM_OK) {
            panic_full("PCKC-E01 KEY DERIVE FAILED",
                       "PCKC boot-time get_or_derive returned error", 0);
        }
        if (km.words[0] == 0 && km.words[1] == 0 && km.words[2] == 0 && km.words[3] == 0) {
            panic_full("PCKC-E01 KEY DERIVE FAILED",
                       "PCKC boot-time derive produced all-zero key material", 0);
        }
        utsm_pckc_invalidate_slot(0xFFFFFFFFu);   /* 探针段作废，不留缓存 */
        log_info("[UTSM] pckc derive roundtrip ok");
    }

    utsm_process_init();
}

utsm_segment_desc *utsm_get_segment(u32 slot) {
    if (slot >= g_utsm_max_segments || slot >= UTSM_MAX_SEGMENTS) {
        return NULL;
    }
    return &g_segments[slot];
}

u64 utsm_next_generation(void) {
    return g_generation++;
}
