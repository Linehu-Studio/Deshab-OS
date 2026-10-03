/* ===================================================================
 *  sealed.c — 封缄内存极早期初始化与校验
 *
 *  链接脚本已有 .sealed section（[__sealed_start, __sealed_end)，
 *  U3 @sealed 标记），但历史上无任何代码引用它：封缄段从未建立、
 *  从未 MAC、从未校验——"封缄内存还没做好"。
 *
 *  本模块在 UTSM 极早期（utsm_init 之后、驱动/guest/DSK 之前）：
 *    1. .sealed 区为空  → panic UTSM-E01（封缄基础设施未就绪，零降级）
 *    2. 否则以 root key 为 key，BLAKE2b 对整个 .sealed 区算封缄 MAC，
 *       存入 g_sealed_mac，置 ready。
 *    3. utsm_sealed_verify() 重算比对，不符 → UTSM-E03。
 *       （供 selftest / checkpoint 路径调用；骨架阶段无固定调用方。）
 * =================================================================== */

#include <utsm/types.h>
#include <utsm/panic.h>
#include <utsm/log.h>
#include <utsm/blake2b.h>
#include <utsm/drr.h>

/* linker.ld 导出的 .sealed 段边界 */
extern u8 __sealed_start[];
extern u8 __sealed_end[];

#define UTSM_SEALED_MAC_LEN 32

static u8  g_sealed_mac[UTSM_SEALED_MAC_LEN];
static int g_sealed_ready = 0;

/* 封缄区常驻品牌落款：保证 .sealed 区非空（否则 UTSM-E01 panic），
 * 同时是封缄 MAC 保护的第一份真实数据。后续模块把机密放进
 * __attribute__((section(".sealed"))) 即自动纳入同一 MAC 域。 */
__attribute__((used, section(".sealed")))
static const char g_sealed_brand[] = "DESHAB-SEALED-MEMORY-V1-DEAICUP";

/* 极早期封缄初始化：任何不就绪都在这里 panic，绝不带病前进 */
void utsm_sealed_early_init(void) {
    u64 region = (u64)(__sealed_end - __sealed_start);
    if (region == 0) {
        /* .sealed 区为空 = 没有任何数据被真正封缄 = 基础设施未就绪 */
        panic_full("UTSM-E01 SEALED MEMORY NOT READY",
                   ".sealed section is empty: sealing infrastructure not implemented", 0);
    }
    const u64 *rk = drr_get_root_key();
    if (!rk) {
        panic_full("UTSM-E02 SEALED NO ROOT KEY",
                   "root key unavailable before sealing .sealed region", 0);
    }
    utsm_blake2b(__sealed_start, region, rk, 32, g_sealed_mac, UTSM_SEALED_MAC_LEN);
    g_sealed_ready = 1;
    log_hex64("[SEAL] .sealed region bytes=", region);
    log_info("[SEAL] sealed MAC established");
}

/* 封缄完整性校验：重算 MAC 并与 init 时的封缄比对 */
void utsm_sealed_verify(void) {
    if (!g_sealed_ready) {
        panic_full("UTSM-E04 SEALED VERIFY BEFORE INIT",
                   "utsm_sealed_verify called before utsm_sealed_early_init", 0);
    }
    u64 region = (u64)(__sealed_end - __sealed_start);
    u8 mac[UTSM_SEALED_MAC_LEN];
    const u64 *rk = drr_get_root_key();
    utsm_blake2b(__sealed_start, region, rk, 32, mac, UTSM_SEALED_MAC_LEN);
    u64 diff = 0;
    for (int i = 0; i < UTSM_SEALED_MAC_LEN; i++) diff |= (u64)(mac[i] ^ g_sealed_mac[i]);
    if (diff != 0) {
        log_hex64("[SEAL] verify diff=", diff);
        panic_full("UTSM-E03 SEALED MAC MISMATCH",
                   ".sealed region content changed without re-seal", 0);
    }
    log_info("[SEAL] sealed verify ok");
}
