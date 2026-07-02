#include <utsm/types.h>

void *memset(void *dst, int value, usize len) {
    u8 *d = (u8 *)dst;
    for (usize i = 0; i < len; i++) {
        d[i] = (u8)value;
    }
    return dst;
}

void *memcpy(void *dst, const void *src, usize len) {
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    for (usize i = 0; i < len; i++) {
        d[i] = s[i];
    }
    return dst;
}

int memcmp(const void *a, const void *b, usize len) {
    const u8 *x = (const u8 *)a;
    const u8 *y = (const u8 *)b;
    for (usize i = 0; i < len; i++) {
        if (x[i] != y[i]) {
            return (int)x[i] - (int)y[i];
        }
    }
    return 0;
}

usize strlen(const char *s) {
    usize len = 0;
    while (s[len]) {
        len++;
    }
    return len;
}
