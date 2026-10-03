#include <utsm/utrw.h>
#include <utsm/segment.h>
#include <utsm/pckc.h>
#include <utsm/drr.h>
#include <utsm/log.h>
#include <utsm/panic.h>

/* ===================================================================
 *  U2: UTRW Slow Path 真分支
 *
 *  "快柜空着，慢柜要你先念 UUID 咒语——念错也给你件，只是晚一拍。"
 *
 *  Fast Path（read.c/write.c）失败后进入本分支，按 reason 分派：
 *    KEY_MISS     -> PCKC 重派生密钥，成功即 OK（可重试）
 *    EPOCH        -> 刷新 cap.epoch（out_cap 回传），OK（可重试）
 *    STALE_CAP    -> generation 不匹配，不可恢复，原样返回
 *    MAC_FAILED   -> PCKC 失效 + DRR fault（回滚路径接管），返回原错误
 *    POISONED     -> DRR fault 留痕，返回原错误（段已坏）
 *    RETRY        -> seqlock 忙，无恢复动作，返回 RETRY
 *
 *  返回 UTSM_OK 表示"已恢复，Fast Path 可立即重试"；
 *  其余返回值为不可恢复错误，调用方不得对同一 cap 继续访问。
 *
 *  Slow 路径有审计日志（log_warn）——慢要慢得可解释。
 * =================================================================== */

/* 轻量校验（绕过 epoch 检查）：EPOCH/KEY_MISS 恢复前确认段其余属性仍然合法 */
static int slow_validate_ignoring_epoch(utsm_capability cap, u32 required_rights,
                                        u64 offset, u64 len,
                                        utsm_segment_desc **out_desc) {
    utsm_segment_desc *desc = utsm_get_segment(cap.segment_slot);
    if (!desc) return UTSM_ERR_INVALID;
    if (desc->state != UTSM_SEG_ACTIVE) {
        return (desc->state == UTSM_SEG_POISONED) ? UTSM_ERR_POISONED : UTSM_ERR_STATE;
    }
    if (desc->generation != cap.generation) return UTSM_ERR_STALE_CAP;
    if ((cap.rights & required_rights) != required_rights) return UTSM_ERR_ACCESS;
    if (offset > desc->cipher_length || len > desc->cipher_length ||
        offset + len > desc->cipher_length) return UTSM_ERR_BOUNDS;
    *out_desc = desc;
    return UTSM_OK;
}

int utsm_slow_path(utsm_capability cap, u32 rights, u64 offset, u64 len,
                   int reason, utsm_capability *out_cap) {
    switch (reason) {
    case UTSM_ERR_KEY_MISS: {
        /* 咒语没背熟：PCKC 失效后重派生 */
        utsm_segment_desc *desc = NULL;
        int st = slow_validate_ignoring_epoch(cap, rights, offset, len, &desc);
        if (st != UTSM_OK) return st;
        utsm_pckc_invalidate_slot(cap.segment_slot);
        utsm_key_material key;
        st = utsm_pckc_get_or_derive(cap.segment_slot, desc, &key);
        if (st != UTSM_OK) {
            log_warn("[UTRW] slow: KEY_MISS re-derive failed");
            panic_full("UTRW-E01 KEY_MISS RE-DERIVE FAILED",
                       "utsm_slow_path: KEY_MISS key re-derivation failed", 0);
            return st;
        }
        log_warn("[UTRW] slow: KEY_MISS -> key re-derived (retry)");
        return UTSM_OK;
    }

    case UTSM_ERR_EPOCH: {
        /* 纪元过期：cap.epoch 落后于段 key_epoch —— 刷新后可重试 */
        utsm_segment_desc *desc = NULL;
        int st = slow_validate_ignoring_epoch(cap, rights, offset, len, &desc);
        if (st != UTSM_OK) return st;
        if (out_cap) {
            *out_cap = cap;
            out_cap->epoch = (u32)desc->key_epoch;
        }
        log_warn("[UTRW] slow: EPOCH expired -> cap refreshed (retry)");
        return UTSM_OK;
    }

    case UTSM_ERR_STALE_CAP:
        /* generation 不匹配：凭证指向已销毁的段世代，不可恢复 */
        log_warn("[UTRW] slow: STALE_CAP -> permanent");
        return reason;

    case UTSM_ERR_MAC_FAILED:
        /* 页 MAC 校验失败：疑似静默损坏 —— 交 DRR（回滚）处置 */
        log_warn("[UTRW] slow: MAC_FAILED -> DRR fault reported");
        utsm_pckc_invalidate_slot(cap.segment_slot);
        drr_report_fault("utrw slow path: page MAC verification failed");
        return reason;

    case UTSM_ERR_POISONED:
        /* 段已中毒：留痕后原样拒绝 */
        log_warn("[UTRW] slow: POISONED -> DRR notified, access denied");
        drr_report_fault("utrw slow path: access to poisoned segment");
        return reason;

    case UTSM_ERR_RETRY:
        /* seqlock 忙（writer 正在写）：无恢复动作，调用方自旋重试 */
        return UTSM_ERR_RETRY;

    default:
        log_warn("[UTRW] slow: unhandled reason -> pass through");
        return reason;
    }
}
