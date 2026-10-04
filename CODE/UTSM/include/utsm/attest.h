#ifndef UTSM_ATTEST_H
#define UTSM_ATTEST_H

#include <utsm/types.h>

/* ============================================================
 *  SAS-R0 启动期能力证明（boot-time feature attestation）
 *
 *  哲学：设计已定的能力，"没好"不允许带病启动——
 *  各子系统就绪时 attest_report() 打点；内核交接 DSK 前
 *  attest_check() 统一核查。FUCK [attest] require_<name>=1
 *  （默认 1）而能力未就绪 → 莲花屏 panic 带该能力的错误码。
 * ============================================================ */

/* 就绪打点。errcode 为该能力"未就绪"时的 panic 错误码。
 * 同名重复 report 覆盖 ready 标志（后到的真实探测为准）。 */
void attest_report(const char *name, const char *errcode, int ready);

/* 便捷封装：就绪打点 */
void attest_report_ready(const char *name, const char *errcode);

/* 交接 DSK 前核查：缺一项即 panic（先串口列出全部缺失项）。
 * cfg = FUCK ini_config*（用 void* 规避 ini_config 前向声明问题）。 */
void attest_check(const void *cfg);

/* 当前缺失项计数（调试/日志用） */
u32 attest_missing_count(void);

#endif /* UTSM_ATTEST_H */
