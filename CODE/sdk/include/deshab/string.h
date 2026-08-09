/* deshab/string.h - 基础内存操作（无 libc 依赖）
 *
 * 整合 desktop_app.h 的 da_memset/memcpy，统一前缀为 dsb_。
 * SAS-R0 单地址空间模型下应用/驱动无 libc，需自备基础内存操作。
 */
#ifndef DESHAB_STRING_H
#define DESHAB_STRING_H

#include "types.h"

static inline void *dsb_memset(void *d, int c, u64 n) {
    u8 *p = (u8 *)d;
    while (n--) *p++ = (u8)c;
    return d;
}

static inline void *dsb_memcpy(void *d, const void *s, u64 n) {
    u8 *dd = (u8 *)d;
    const u8 *ss = (const u8 *)s;
    while (n--) *dd++ = *ss++;
    return d;
}

static inline u64 dsb_strlen(const char *s) {
    u64 n = 0;
    while (s[n]) n++;
    return n;
}

#endif /* DESHAB_STRING_H */
