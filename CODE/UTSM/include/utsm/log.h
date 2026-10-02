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

/* D4: DKM 彩色日志——驱动 init 期间的颜色上下文。
 * log_set_color 传 ANSI CSI（如 "\x1b[36m"），0/"" 恢复默认；
 * 颜色只走串口，BOOTLOG 缓冲保持纯文本。log_color_enable 总开关
 * （FUCK [debug] color_log）。 */
void log_set_color(const char *ansi);
void log_color_enable(int enable);

/* ---- 内存日志缓冲区（实机启动日志持久化） ----
 * 由 disk_log_flush() 写入 FAT32 文件 SYSTEM/DESHAB64/DEV/BOOTLOG.TXT。 */
void log_capture_enable(int enable);
const char *log_get_buffer(void);
u32 log_get_length(void);

#endif
