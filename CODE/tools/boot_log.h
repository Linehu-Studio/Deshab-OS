/* boot_log.h — 跨 UTSM/DSK/应用 ELF 的启动日志环形缓冲（freestanding，无 libc）
 *
 * boot_log_info 由 UTSM 在静态内存中创建，经 dsk_boot_context.utsm_state 传递。
 * DSK 注册 flush 回调，将缓冲写入 FAT32: SYSTEM/DESHAB64/DEV/BOOTLOG.TXT
 * （DEV 目录不存在时回退 ESP 根目录 BOOTLOG.TXT）。
 */
#ifndef BOOT_LOG_H
#define BOOT_LOG_H

typedef unsigned int       u32;
typedef unsigned long long u64;

#define BOOT_LOG_MAGIC   0x424C4746u  /* "BLGF" */
#define BOOT_LOG_VERSION 1u

typedef struct boot_log_info boot_log_info;

typedef int (*boot_log_flush_fn)(void);

struct boot_log_info {
    u32 magic;
    u32 version;
    char *buf;
    u32 capacity;
    u32 length;
    u32 dropped;
    boot_log_flush_fn flush;
};

static inline void boot_log_append(boot_log_info *bi, const char *s) {
    if (!bi || bi->magic != BOOT_LOG_MAGIC || !s || !bi->buf)
        return;
    while (*s) {
        if (bi->length < bi->capacity)
            bi->buf[bi->length++] = *s;
        else
            bi->dropped++;
        s++;
    }
}

static inline void boot_log_bind(boot_log_info **slot, u64 utsm_state) {
    if (!slot)
        return;
    *slot = 0;
    if (!utsm_state)
        return;
    boot_log_info *bi = (boot_log_info *)(u64)utsm_state;
    if (bi && bi->magic == BOOT_LOG_MAGIC && bi->buf && bi->capacity)
        *slot = bi;
}

static inline int boot_log_try_flush(boot_log_info *bi) {
    if (bi && bi->flush)
        return bi->flush();
    return -1;
}

#endif /* BOOT_LOG_H */
