/* ===================================================================
 *  pkg.c — 包管理器骨架（Pacman 五阶段方案，docs/STATUS.md）
 *
 *  设计已定（五阶段：清单 → 仓库索引 → 差分下载 → 安装事务 →
 *  回滚钩子接 DRR），实现未动。建立代码骨架：调用即莲花 panic，
 *  零降级零静默。
 * =================================================================== */

#include <utsm/types.h>
#include <utsm/panic.h>
#include <utsm/log.h>

/* 阶段1: 从仓库清单解析包元数据（名称/版本/依赖/文件清单） */
int pkg_query(const char *name) {
    (void)name;
    log_error("[PKG] query requested");
    panic_full("PKG-E01 PKG QUERY NOT IMPLEMENTED",
               "pacman-style package query: designed (5-phase), not implemented", 0);
    return -1;
}

/* 阶段2+3: 仓库索引 + 差分下载 */
int pkg_install(const char *name) {
    (void)name;
    log_error("[PKG] install requested");
    panic_full("PKG-E02 PKG INSTALL NOT IMPLEMENTED",
               "package install transaction: designed (5-phase), not implemented", 0);
    return -1;
}

/* 阶段4+5: 安装事务 + DRR 回滚钩子 */
int pkg_remove(const char *name) {
    (void)name;
    log_error("[PKG] remove requested");
    panic_full("PKG-E03 PKG REMOVE NOT IMPLEMENTED",
               "package removal + DRR rollback hook: designed (5-phase), not implemented", 0);
    return -1;
}
