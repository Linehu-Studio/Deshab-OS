/* ===================================================================
 *  brand.c — 五期 E 系列：品牌体验骨架
 *
 *  设计已定、实现未做的品牌项，全部立骨架 + 调用即莲花 panic。
 *  骨架不接线：当前启动走静态 Logo（DSK），接线上屏是后续任务。
 *
 *  E1 启动莲花动画：静态 Logo → 分瓣绽放动画（时间轴设计见
 *     docs/ROADMAP_INSANE.md E1）。实现前调用即 panic。
 *  E2 假想时光键彩蛋：特定按键序列触发"时光倒流"演出（回放最近
 *     一段帧缓冲历史，DRR checkpoint 帧源）。实现前调用即 panic。
 * =================================================================== */

#include <utsm/types.h>
#include <utsm/panic.h>
#include <utsm/log.h>

/* E1: 启动莲花动画（brand-E 系列） */
void brand_lotus_boot_anim(u32 hold_ms) {
    (void)hold_ms;
    log_error("[BRAND] lotus boot animation requested");
    panic_full("BRAND-E01 LOTUS BOOT ANIM NOT IMPLEMENTED",
               "E1 petal-bloom boot animation: designed, not implemented", 0);
}

/* E2: 假想时光键彩蛋（Ctrl+Alt+Shift+T 时光倒流演出） */
void brand_time_egg_trigger(void) {
    log_error("[BRAND] time egg triggered");
    panic_full("BRAND-E02 TIME EGG NOT IMPLEMENTED",
               "E2 imaginary time-key easter egg: designed, not implemented", 0);
}
