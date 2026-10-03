#include <utsm/dkm.h>
#include <utsm/log.h>
#include <utsm/panic.h>
#include <utsm/types.h>
#include "../arch/x86_64/limine.h"
#include "../kernel/ini_parser.h"

/* 前向声明：带 FUCK [drivers] 过滤的加载入口（定义见下文） */
void dsm_load_by_manifest_ex(const ini_config *cfg);

#define DSM_MAX_DRIVERS_PER_STAGE 8
#define DSM_MAX_STAGES 4
#define DSM_MAX_DEPENDS 4
#define DSM_MAX_PROVIDES 4

typedef struct dsm_driver_entry {
    char name[32];
    char path[64];
    u32 class_id;
    u32 required;
    char depends[DSM_MAX_DEPENDS][32];
    u32 depend_count;
    char provides[DSM_MAX_PROVIDES][32];
    u32 provide_count;
    struct limine_file *boot_module;
    int loaded;
} dsm_driver_entry;

typedef struct dsm_stage_entry {
    u32 id;
    char name[16];
    u32 required;
    dsm_driver_entry drivers[DSM_MAX_DRIVERS_PER_STAGE];
    u32 driver_count;
} dsm_stage_entry;

static dsm_stage_entry g_stages[DSM_MAX_STAGES];
static u32 g_stage_count;

extern volatile struct limine_module_request g_module_request;

static int dsm_skip_ws(const char **p) {
    while (**p == ' ' || **p == '\t' || **p == '\n' || **p == '\r') { (*p)++; }
    return 0;
}

static int dsm_expect(const char **p, char c) {
    dsm_skip_ws(p);
    if (**p == c) { (*p)++; return 0; }
    return -1;
}

static int dsm_read_string(const char **p, char *out, u32 max_len) {
    dsm_skip_ws(p);
    if (**p != '"') return -1;
    (*p)++;
    u32 i = 0;
    while (**p && **p != '"' && i + 1 < max_len) {
        out[i++] = **p;
        (*p)++;
    }
    out[i] = 0;
    if (**p != '"') return -1;
    (*p)++;
    return 0;
}

static int dsm_skip_value(const char **p) {
    dsm_skip_ws(p);
    if (**p == '"') {
        char tmp[256];
        return dsm_read_string(p, tmp, sizeof(tmp));
    }
    if (**p == '{') {
        (*p)++;
        int depth = 1;
        while (depth > 0 && **p) {
            if (**p == '{') depth++;
            else if (**p == '}') depth--;
            else if (**p == '"') { (*p)++; while (**p && **p != '"') (*p)++; if (**p) (*p)++; continue; }
            if (depth > 0) (*p)++;
        }
        return 0;
    }
    if (**p == '[') {
        (*p)++;
        int depth = 1;
        while (depth > 0 && **p) {
            if (**p == '[') depth++;
            else if (**p == ']') depth--;
            else if (**p == '"') { (*p)++; while (**p && **p != '"') (*p)++; if (**p) (*p)++; continue; }
            if (depth > 0) (*p)++;
        }
        return 0;
    }
    while (**p && **p != ',' && **p != '}' && **p != ']') (*p)++;
    return 0;
}

static struct limine_file *dsm_find_boot_module(const char *path) {
    struct limine_module_response *rsp = g_module_request.response;
    if (!rsp) return 0;
    for (u64 i = 0; i < rsp->module_count; i++) {
        struct limine_file *f = rsp->modules[i];
        if (f && f->path) {
            const char *fp = f->path;
            while (*fp == '/') fp++;
            usize j = 0;
            usize k = 0;
            while (fp[j]) j++;
            while (path[k]) k++;
            if (j == k) {
                int match = 1;
                for (usize x = 0; x < j; x++) {
                    if (fp[x] != path[x]) { match = 0; break; }
                }
                if (match) return f;
            }
        }
    }
    return 0;
}

static int streq_static(const char *a, const char *b) {
    usize i = 0;
    while (a[i] && b[i]) {
        if (a[i] != b[i]) return 0;
        i++;
    }
    return a[i] == b[i];
}

static void dsm_parse_driver(const char **p, dsm_driver_entry *drv) {
    drv->depend_count = 0;
    drv->provide_count = 0;
    drv->required = 1;
    drv->boot_module = 0;
    drv->loaded = 0;
    drv->class_id = 0;

    if (dsm_expect(p, '{') != 0) return;
    while (**p && **p != '}') {
        dsm_skip_ws(p);
        if (**p == '}') break;
        char key[32];
        if (dsm_read_string(p, key, sizeof(key)) != 0) return;
        dsm_expect(p, ':');

        if (streq_static(key, "name")) {
            dsm_read_string(p, drv->name, sizeof(drv->name));
        } else if (streq_static(key, "path")) {
            dsm_read_string(p, drv->path, sizeof(drv->path));
        } else if (streq_static(key, "class")) {
            char cls[32];
            dsm_read_string(p, cls, sizeof(cls));
            if (streq_static(cls, "platform")) drv->class_id = 1;
            else if (streq_static(cls, "bus")) drv->class_id = 2;
            else if (streq_static(cls, "interrupt")) drv->class_id = 3;
            else if (streq_static(cls, "timer")) drv->class_id = 4;
            else if (streq_static(cls, "console")) drv->class_id = 5;
            else if (streq_static(cls, "storage")) drv->class_id = 6;
            else if (streq_static(cls, "fs")) drv->class_id = 7;
            else if (streq_static(cls, "fs-core")) drv->class_id = 8;
            else if (streq_static(cls, "net")) drv->class_id = 9;
            else if (streq_static(cls, "input")) drv->class_id = 10;
        } else if (streq_static(key, "required")) {
            dsm_skip_ws(p);
            if (**p == 't') { drv->required = 1; (*p) += 4; }
            else if (**p == 'f') { drv->required = 0; (*p) += 5; }
        } else if (streq_static(key, "depends")) {
            if (dsm_expect(p, '[') != 0) return;
            while (**p && **p != ']') {
                dsm_skip_ws(p);
                if (**p == ']') break;
                if (drv->depend_count < DSM_MAX_DEPENDS) {
                    dsm_read_string(p, drv->depends[drv->depend_count], 32);
                    drv->depend_count++;
                } else { dsm_skip_value(p); }
                dsm_skip_ws(p);
                if (**p == ',') { (*p)++; continue; }
                if (**p == ']') break;
            }
            dsm_expect(p, ']');
        } else if (streq_static(key, "provides")) {
            if (dsm_expect(p, '[') != 0) return;
            while (**p && **p != ']') {
                dsm_skip_ws(p);
                if (**p == ']') break;
                if (drv->provide_count < DSM_MAX_PROVIDES) {
                    dsm_read_string(p, drv->provides[drv->provide_count], 32);
                    drv->provide_count++;
                } else { dsm_skip_value(p); }
                dsm_skip_ws(p);
                if (**p == ',') { (*p)++; continue; }
                if (**p == ']') break;
            }
            dsm_expect(p, ']');
        } else {
            dsm_skip_value(p);
        }
        dsm_skip_ws(p);
        if (**p == ',') (*p)++;
    }
    dsm_expect(p, '}');
}

static void dsm_parse_stage(const char **p, dsm_stage_entry *stage) {
    stage->driver_count = 0;
    if (dsm_expect(p, '{') != 0) return;
    while (**p && **p != '}') {
        dsm_skip_ws(p);
        if (**p == '}') break;
        char key[32];
        if (dsm_read_string(p, key, sizeof(key)) != 0) return;
        dsm_expect(p, ':');

        if (streq_static(key, "id")) {
            dsm_skip_ws(p);
            u32 val = 0;
            while (**p >= '0' && **p <= '9') { val = val * 10 + (u32)(**p - '0'); (*p)++; }
            stage->id = val;
        } else if (streq_static(key, "name")) {
            dsm_read_string(p, stage->name, sizeof(stage->name));
        } else if (streq_static(key, "required")) {
            dsm_skip_ws(p);
            if (**p == 't') { stage->required = 1; (*p) += 4; }
            else if (**p == 'f') { stage->required = 0; (*p) += 5; }
        } else if (streq_static(key, "drivers")) {
            if (dsm_expect(p, '[') != 0) return;
            while (**p && **p != ']') {
                dsm_skip_ws(p);
                if (**p == ']') break;
                if (stage->driver_count < DSM_MAX_DRIVERS_PER_STAGE) {
                    dsm_parse_driver(p, &stage->drivers[stage->driver_count]);
                    stage->driver_count++;
                } else {
                    dsm_skip_value(p);
                }
                dsm_skip_ws(p);
                if (**p == ',') { (*p)++; continue; }
                if (**p == ']') break;
            }
            dsm_expect(p, ']');
        } else {
            dsm_skip_value(p);
        }
        dsm_skip_ws(p);
        if (**p == ',') (*p)++;
    }
    dsm_expect(p, '}');
}

static int dsm_parse_manifest(const char *text, u64 size) {
    g_stage_count = 0;
    const char *p = text;
    const char *end = text + size;

    while (p < end && *p) {
        dsm_skip_ws(&p);
        if (p >= end || !*p) break;
        if (*p == '{' || *p == '}') { p++; continue; }
        if (*p == ',' || *p == ':') { p++; continue; }
        if (*p == '[' || *p == ']') { p++; continue; }

        char key[32];
        if (dsm_read_string(&p, key, sizeof(key)) != 0) break;
        dsm_expect(&p, ':');

        if (streq_static(key, "stages")) {
            if (dsm_expect(&p, '[') != 0) return -1;
            while (p < end && *p) {
                dsm_skip_ws(&p);
                if (*p == ']') { p++; break; }
                if (g_stage_count < DSM_MAX_STAGES) {
                    dsm_parse_stage(&p, &g_stages[g_stage_count]);
                    g_stage_count++;
                }
                dsm_skip_ws(&p);
                if (*p == ',') p++;
            }
        } else {
            dsm_skip_value(&p);
        }
        dsm_skip_ws(&p);
        if (*p == ',' || *p == '}' || *p == ']') p++;
    }
    return 0;
}

void dsm_load_by_manifest(void) {
    dsm_load_by_manifest_ex(NULL);
}

static int dsm_ensure_manifest(void) {
    struct limine_module_response *rsp;
    struct limine_file *manifest_file;
    u64 i;

    if (g_stage_count) return 0;
    rsp = g_module_request.response;
    if (!rsp) {
        log_warn("[DSM] no Limine module response");
        return -1;
    }

    manifest_file = 0;
    for (i = 0; i < rsp->module_count; i++) {
        struct limine_file *f = rsp->modules[i];
        if (f && f->path && f->cmdline && streq_static(f->cmdline, "dkm:manifest")) {
            manifest_file = f;
            break;
        }
    }

    if (!manifest_file) {
        log_warn("[DSM] manifest not found in boot modules");
        return -1;
    }

    log_info("[DSM] parsing manifest");
    dsm_parse_manifest((const char *)manifest_file->address, manifest_file->size);
    log_info("[DSM] manifest parsed");
    log_hex64("[DSM] stages=", (u64)g_stage_count);
    return 0;
}

static void dsm_load_stage_range(const ini_config *cfg, int limit_stage, u32 max_stage_id) {
    if (dsm_ensure_manifest() != 0) return;

    for (u32 si = 0; si < g_stage_count; si++) {
        dsm_stage_entry *stage = &g_stages[si];
        if (limit_stage && stage->id > max_stage_id) continue;
        log_info("[DSM] stage begin");
        log_info(stage->name);
        log_hex64("[DSM] driver count=", stage->driver_count);

        for (u32 di = 0; di < stage->driver_count; di++) {
            dsm_driver_entry *drv = &stage->drivers[di];
            struct limine_file *f;
            struct dkm_symbol_scan symbols;
            int result;

            if (drv->loaded) continue;

            /* === FUCK [drivers] 驱动开关检查 === */
            if (cfg) {
                int enabled = ini_get_bool(cfg, "drivers", drv->name, 1);
                if (!enabled) {
                    log_info("[DSM] driver disabled by FUCK: ");
                    log_info(drv->name);
                    continue;
                }
            }

            drv->boot_module = dsm_find_boot_module(drv->path);
            if (!drv->boot_module) {
                log_info("[DSM] module not found");
                log_info(drv->name);
                if (drv->required && stage->required) {
                    /* 严格错误策略插桩：required 驱动缺模块零降级（DKM-E04） */
                    panic_full("DKM-E04 REQUIRED DRIVER MODULE MISSING",
                               "required driver boot module not found in manifest stage", 0);
                    break;
                }
                continue;
            }

            f = drv->boot_module;
            log_info("[DSM] loading driver");
            log_info(drv->name);

            if (dkm_check_elf64(f->address, f->size) != 0) {
                log_error("[DSM] ELF check failed");
                if (drv->required && stage->required) {
                    panic_full("DKM-E05 REQUIRED DRIVER ELF INVALID",
                               "required driver boot module failed ELF64 check", 0);
                }
                continue;
            }

            if (dkm_scan_symbols(f->address, f->size, &symbols) != 0) {
                log_error("[DSM] symbol scan failed");
                if (drv->required && stage->required) {
                    panic_full("DKM-E06 REQUIRED DRIVER SYMBOL SCAN FAILED",
                               "required driver symbol scan failed", 0);
                }
                continue;
            }

            result = dkm_load_elf_rel(f->address, f->size, &symbols);
            if (result != 0) {
                log_error("[DSM] load failed");
                if (drv->required && stage->required) {
                    /* 严格错误策略插桩：required 驱动加载失败零降级（DKM-E07） */
                    panic_full("DKM-E07 REQUIRED DRIVER LOAD FAILED",
                               "required driver relocation/load failed", 0);
                    break;
                }
            } else {
                drv->loaded = 1;
            }
        }
        log_info("[DSM] stage end");
    }
}

void dsm_load_by_manifest_upto(const ini_config *cfg, u32 max_stage_id) {
    dsm_load_stage_range(cfg, 1, max_stage_id);
}

void dsm_load_named(const ini_config *cfg, const char *name) {
    if (!name || dsm_ensure_manifest() != 0) return;

    for (u32 si = 0; si < g_stage_count; si++) {
        dsm_stage_entry *stage = &g_stages[si];
        for (u32 di = 0; di < stage->driver_count; di++) {
            dsm_driver_entry *drv = &stage->drivers[di];
            struct limine_file *f;
            struct dkm_symbol_scan symbols;
            int result;

            if (!streq_static(drv->name, name)) continue;
            if (drv->loaded) return;
            if (cfg && !ini_get_bool(cfg, "drivers", drv->name, 1)) {
                log_info("[DSM] driver disabled by FUCK: ");
                log_info(drv->name);
                return;
            }
            drv->boot_module = dsm_find_boot_module(drv->path);
            if (!drv->boot_module) {
                log_warn("[DSM] named module not found");
                log_info(name);
                return;
            }
            f = drv->boot_module;
            log_info("[DSM] loading named driver");
            log_info(drv->name);
            if (dkm_check_elf64(f->address, f->size) != 0) {
                log_error("[DSM] ELF check failed");
                return;
            }
            if (dkm_scan_symbols(f->address, f->size, &symbols) != 0) {
                log_error("[DSM] symbol scan failed");
                return;
            }
            result = dkm_load_elf_rel(f->address, f->size, &symbols);
            if (result != 0) {
                log_error("[DSM] load failed");
                return;
            }
            drv->loaded = 1;
            return;
        }
    }
    log_warn("[DSM] named driver not in manifest");
    log_info(name);
}

void dsm_load_by_manifest_ex(const ini_config *cfg) {
    dsm_load_stage_range(cfg, 0, 0);
}
