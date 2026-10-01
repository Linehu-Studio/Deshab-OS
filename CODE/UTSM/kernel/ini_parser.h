#ifndef UTSM_INI_PARSER_H
#define UTSM_INI_PARSER_H

#include <utsm/types.h>

/* INI 配置条目 */
typedef struct {
    char section[32];
    char key[32];
    char value[64];
} ini_entry;

/* INI 配置集合（栈/静态分配，不依赖堆） */
typedef struct {
    ini_entry entries[128]; /* 最多 128 条配置项（M4: [sched]/[drr] 加入后 64 不够用） */
    u32 count;
} ini_config;

/* 解析 INI 文本到 ini_config。
 * text: INI 文本起始地址
 * size: 文本字节长度
 * cfg:  输出配置集合
 * 返回 0 成功，负数失败 */
int ini_parse(const char *text, u32 size, ini_config *cfg);

/* 在 section 下查找 key，返回 value 字符串。
 * 未找到返回 NULL。 */
const char *ini_get(const ini_config *cfg, const char *section, const char *key);

/* 便捷查询：返回整数值。
 * 未找到或格式错误返回 defval。 */
int ini_get_int(const ini_config *cfg, const char *section, const char *key, int defval);

/* 便捷查询：返回布尔值。
 * "0"/"false" = 0, "1"/"true" = 1，未找到返回 defval。 */
int ini_get_bool(const ini_config *cfg, const char *section, const char *key, int defval);

#endif /* UTSM_INI_PARSER_H */
