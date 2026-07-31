#include <utsm/utsm.h>
#include <utsm/segment.h>
#include <utsm/pckc.h>
#include <utsm/process.h>
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
