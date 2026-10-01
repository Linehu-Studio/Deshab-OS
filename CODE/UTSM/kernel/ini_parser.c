/* INI 分区格式解析器 — freestanding, 无堆分配, 无 libc 依赖 */

#include "ini_parser.h"

/* ---- 内部辅助 ---- */

static int ini_streq(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == *b;
}

static int ini_isspace(char c) {
    return c == ' ' || c == '\t' || c == '\r';
}

static int ini_isdigit(char c) {
    return c >= '0' && c <= '9';
}

static int ini_strncpy(char *dst, const char *src, u32 max) {
    u32 i = 0;
    while (i + 1 < max && src[i]) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
    return (int)i;
}

static int ini_atoi(const char *s) {
    int neg = 0, v = 0;
    if (!s) return 0;
    while (ini_isspace(*s)) s++;
    if (*s == '-') { neg = 1; s++; }
    while (ini_isdigit(*s)) {
        v = v * 10 + (*s - '0');
        s++;
    }
    return neg ? -v : v;
}

static int ini_parse_bool(const char *s) {
    if (!s) return -1;
    if (s[0] == '1') return 1;
    if (s[0] == '0') return 0;
    if (ini_streq(s, "true"))  return 1;
    if (ini_streq(s, "false")) return 0;
    if (ini_streq(s, "yes"))   return 1;
    if (ini_streq(s, "no"))    return 0;
    return -1;
}

/* ---- 公共接口 ---- */

int ini_parse(const char *text, u32 size, ini_config *cfg) {
    if (!text || !cfg) return -1;

    cfg->count = 0;
    char cur_section[32] = {0};  /* 当前分区名，默认空 */

    u32 pos = 0;
    while (pos < size) {
        /* 跳过行首空白 */
        while (pos < size && (text[pos] == ' ' || text[pos] == '\t'))
            pos++;

        if (pos >= size) break;

        char c = text[pos];

        /* 空行或换行 */
        if (c == '\n') { pos++; continue; }

        /* 注释行: # 或 ; 开头 */
        if (c == '#' || c == ';') {
            while (pos < size && text[pos] != '\n') pos++;
            continue;
        }

        /* 分区头: [section] */
        if (c == '[') {
            pos++;  /* 跳过 '[' */
            u32 si = 0;
            while (pos < size && text[pos] != ']' && text[pos] != '\n' && si + 1 < 32) {
                cur_section[si++] = text[pos++];
            }
            cur_section[si] = 0;
            /* 跳过到行尾 */
            while (pos < size && text[pos] != '\n') pos++;
            continue;
        }

        /* key=value 行 */
        if (cfg->count >= 128) break;  /* 达到上限 */

        ini_entry *e = &cfg->entries[cfg->count];

        /* 读取 key */
        u32 ki = 0;
        while (pos < size && text[pos] != '=' && text[pos] != '\n' &&
               !ini_isspace(text[pos]) && ki + 1 < 32) {
            e->key[ki++] = text[pos++];
        }
        e->key[ki] = 0;

        /* 跳过 key 和 '=' 之间的空白 */
        while (pos < size && ini_isspace(text[pos]) && text[pos] != '\n')
            pos++;

        /* 期望 '=' */
        if (pos >= size || text[pos] != '=') {
            /* 无效行，跳到行尾 */
            while (pos < size && text[pos] != '\n') pos++;
            continue;
        }
        pos++;  /* 跳过 '=' */

        /* 跳过 '=' 后的空白 */
        while (pos < size && ini_isspace(text[pos]) && text[pos] != '\n')
            pos++;

        /* 读取 value（到行尾，去除尾部空白） */
        u32 vi = 0;
        while (pos < size && text[pos] != '\n' && vi + 1 < 64) {
            e->value[vi++] = text[pos++];
        }
        /* 去除尾部空白 */
        while (vi > 0 && ini_isspace(e->value[vi - 1])) vi--;
        e->value[vi] = 0;

        /* 复制 section 名 */
        ini_strncpy(e->section, cur_section, 32);

        cfg->count++;
    }

    return 0;
}

const char *ini_get(const ini_config *cfg, const char *section, const char *key) {
    if (!cfg || !section || !key) return NULL;
    for (u32 i = 0; i < cfg->count; i++) {
        if (ini_streq(cfg->entries[i].section, section) &&
            ini_streq(cfg->entries[i].key, key)) {
            return cfg->entries[i].value;
        }
    }
    return NULL;
}

int ini_get_int(const ini_config *cfg, const char *section, const char *key, int defval) {
    const char *v = ini_get(cfg, section, key);
    if (!v || v[0] == 0) return defval;
    return ini_atoi(v);
}

int ini_get_bool(const ini_config *cfg, const char *section, const char *key, int defval) {
    const char *v = ini_get(cfg, section, key);
    if (!v || v[0] == 0) return defval;
    int b = ini_parse_bool(v);
    return (b >= 0) ? b : defval;
}
