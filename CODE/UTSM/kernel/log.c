#include <utsm/log.h>

/* ---- 内存日志缓冲区（实机启动日志持久化） ----
 * 默认启用捕获：所有日志同时写串口和内存缓冲。
 * 块设备就绪后由 disk_log_flush() 把缓冲区写入磁盘原始扇区。 */
#define LOG_BUF_SIZE 524288u  /* 512KB, 覆盖 LBA 2-1000 (999 扇区 ~500KB) */
static char g_log_buf[LOG_BUF_SIZE];
static u32  g_log_len = 0;
static int  g_log_capture_on = 1;

void log_capture_enable(int enable) {
    g_log_capture_on = enable;
}

const char *log_get_buffer(void) {
    return g_log_buf;
}

u32 log_get_length(void) {
    return g_log_len;
}

static void log_emit(const char *s) {
    serial_write(s);
    if (g_log_capture_on && s) {
        while (*s) {
            if (g_log_len < LOG_BUF_SIZE) {
                g_log_buf[g_log_len++] = *s;
            }
            s++;
        }
    }
}

static void log_line(const char *level, const char *msg) {
    log_emit(level);
    log_emit(" ");
    log_emit(msg);
    log_emit("\n");
}

void log_info(const char *msg) {
    log_line("[INFO]", msg);
}

void log_warn(const char *msg) {
    log_line("[WARN]", msg);
}

void log_error(const char *msg) {
    log_line("[ERR ]", msg);
}

void log_hex64(const char *prefix, u64 value) {
    static const char hex[] = "0123456789abcdef";
    char buf[19];
    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 0; i < 16; i++) {
        buf[2 + i] = hex[(value >> ((15 - i) * 4)) & 0xf];
    }
    buf[18] = 0;
    log_emit(prefix);
    log_emit(buf);
    log_emit("\n");
}
