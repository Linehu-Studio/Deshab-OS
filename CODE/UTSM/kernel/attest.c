/* attest.c — SAS-R0 启动期能力证明（boot-time feature attestation）
 *
 * "设计已定的能力没好 = 启动即 panic"，不是"调用才 panic"。
 * 各子系统在初始化完成点 attest_report()；内核交接 DSK 前
 * attest_check() 统一核查 FUCK [attest] 的 require_<name>。
 *
 * require 默认值 = 1（必备）：能力没好就必须 panic，绝不带病前进。
 * 仅当某构建环境明确不具备该能力（如 QEMU 无 wlan）时，才在
 * FUCK [attest] 显式写 require_<name>=0——这是设计内白名单，
 * 必须在 CLAUDE.md §3 有对应条目。
 */
#include <utsm/attest.h>
#include <utsm/panic.h>
#include <utsm/log.h>
#include "../kernel/ini_parser.h"

#define ATTEST_MAX_FEATURES 32
#define ATTEST_NAME_LEN 28
#define ATTEST_CODE_LEN 20

typedef struct {
    char name[ATTEST_NAME_LEN];
    char errcode[ATTEST_CODE_LEN];
    int ready;
    int reported;
} attest_feature;

static attest_feature g_features[ATTEST_MAX_FEATURES];
static u32 g_feature_count;

static attest_feature *attest_find(const char *name) {
    for (u32 i = 0; i < g_feature_count; i++) {
        const char *a = g_features[i].name;
        const char *b = name;
        while (*a && *b && *a == *b) { a++; b++; }
        if (*a == *b) return &g_features[i];
    }
    return 0;
}

void attest_report(const char *name, const char *errcode, int ready) {
    if (!name || !errcode) return;
    attest_feature *f = attest_find(name);
    if (!f) {
        if (g_feature_count >= ATTEST_MAX_FEATURES) {
            log_error("[ATTEST] feature table full, dropping");
            log_error(name);
            return;
        }
        f = &g_features[g_feature_count++];
        /* 手工 strncpy（freestanding） */
        u32 i = 0;
        for (; name[i] && i < ATTEST_NAME_LEN - 1; i++) f->name[i] = name[i];
        f->name[i] = 0;
        i = 0;
        for (; errcode[i] && i < ATTEST_CODE_LEN - 1; i++) f->errcode[i] = errcode[i];
        f->errcode[i] = 0;
        f->reported = 0;
    }
    f->ready = ready ? 1 : 0;
    f->reported = 1;
}

void attest_report_ready(const char *name, const char *errcode) {
    attest_report(name, errcode, 1);
}

u32 attest_missing_count(void) {
    u32 n = 0;
    for (u32 i = 0; i < g_feature_count; i++)
        if (!g_features[i].ready) n++;
    return n;
}

void attest_check(const void *cfg) {
    log_info("[ATTEST] boot-time capability check begin");

    /* 先串口列出全部缺失项（panic 屏只显示第一个，日志要全） */
    u32 missing = 0;
    for (u32 i = 0; i < g_feature_count; i++) {
        const attest_feature *f = &g_features[i];
        if (f->ready) continue;
        missing++;
        log_error("[ATTEST] MISSING:");
        log_error(f->name);
        log_error(f->errcode);
    }
    log_hex64("[ATTEST] features=", g_feature_count);
    log_hex64("[ATTEST] missing=", missing);

    if (missing == 0) {
        log_info("[ATTEST] all capabilities ready");
        return;
    }

    /* 逐项判断是否必备（require_<name> 默认 1）：
     * 全部缺失项中只要有一项必备 → panic（错误码取第一个必备缺失项） */
    const attest_feature *first_required_missing = 0;
    for (u32 i = 0; i < g_feature_count; i++) {
        const attest_feature *f = &g_features[i];
        if (f->ready) continue;
        char key[48];
        u32 p = 0;
        const char *pre = "require_";
        while (*pre) key[p++] = *pre++;
        u32 j = 0;
        for (; f->name[j] && p < sizeof(key) - 1; j++) key[p++] = f->name[j];
        key[p] = 0;
        /* ini key 小写；name 约定即小写（register 侧保证） */
        if (ini_get_bool(cfg, "attest", key, 1)) {
            first_required_missing = f;
            break;
        }
    }

    if (first_required_missing) {
        panic_full(first_required_missing->errcode,
                   "required capability not ready at boot (attestation)", 0);
    }

    /* 全部缺失项均被 FUCK 显式豁免（设计内白名单），放行 */
    log_warn("[ATTEST] missing capabilities exempted by FUCK [attest]");
}
