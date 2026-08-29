#ifndef UTSM_LOG_H
#define UTSM_LOG_H

#include <utsm/types.h>

void serial_init(void);
void serial_putc(char c);
void serial_write(const char *s);

void log_info(const char *msg);
void log_warn(const char *msg);
void log_error(const char *msg);
void log_hex64(const char *prefix, u64 value);
void panic(const char *msg);

/* ---- 内存日志缓冲区（实机启动日志持久化） ----
 * 由 disk_log_flush() 写入 FAT32 文件 SYSTEM/DESHAB64/DEV/BOOTLOG.TXT。 */
void log_capture_enable(int enable);
const char *log_get_buffer(void);
u32 log_get_length(void);

#endif
