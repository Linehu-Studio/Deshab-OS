#include <utsm/log.h>

/* ---- 内存日志缓冲区（实机启动日志持久化） ----
 * 默认启用捕获：所有日志同时写串口和内存缓冲。
 * 块设备就绪后由 disk_log_flush() 把缓冲区写入 FAT32 文件
 * SYSTEM/DESHAB64/DEV/BOOTLOG.TXT（DEV 缺失时回退根目录 BOOTLOG.TXT）。 */
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
    /* Phase7: 日志行临界区——抢占可能把一行日志切成两半（串口与
     * BOOTLOG 缓冲都会花），pushfq/popfq 保存并恢复本上下文的 IF。 */
    u64 flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    serial_write(s);
    if (g_log_capture_on && s) {
        while (*s) {
            if (g_log_len < LOG_BUF_SIZE) {
                g_log_buf[g_log_len++] = *s;
            }
            s++;
        }
    }
    /* BUG-GP-IRET: 只恢复 IF，不做全量 popfq。全量 popfq 会把进入本
     * 函数前 flags 里的历史污染位（如 NT=1）原样写回 RFLAGS 并在上下文
     * 间传播（NT=1 时下一次 iretq 直接 #GP(0)）。本临界区的语义仅仅是
     * "保存并恢复 IF"。 */
    if (flags & (1ULL << 9)) {        /* 恢复进入前的 IF */
        __asm__ volatile("sti" ::: "memory");
    }
}

/* ---- D4: DKM 彩色日志 ----
 * 驱动 init 期间由 dkm_log_set_driver() 设置 ANSI 颜色上下文；
 * 颜色只走串口（QEMU stdio 可见），BOOTLOG 缓冲保持纯文本。 */
static const char *g_log_color = 0;
static int g_log_color_on = 1;

void log_set_color(const char *ansi) {
    g_log_color = ansi;
}

void log_color_enable(int enable) {
    g_log_color_on = enable;
    if (!enable) g_log_color = 0;
}

/* 串口带色输出 + 缓冲纯文本（临界区内） */
static void log_emit_colored(const char *color, const char *a,
                             const char *b, const char *c) {
    u64 flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    serial_write(color);
    serial_write(a);
    if (b) serial_write(b);
    if (c) serial_write(c);
    serial_write("\x1b[0m\n");
    if (g_log_capture_on) {
        const char *parts[3] = { a, b, c };
        for (int i = 0; i < 3; i++) {
            const char *s = parts[i];
            if (!s) continue;
            while (*s) {
                if (g_log_len < LOG_BUF_SIZE) {
                    g_log_buf[g_log_len++] = *s;
                }
                s++;
            }
        }
        if (g_log_len < LOG_BUF_SIZE) {
            g_log_buf[g_log_len++] = '\n';
        }
    }
    if (flags & (1ULL << 9)) {
        __asm__ volatile("sti" ::: "memory");
    }
}

static void log_line(const char *level, const char *msg) {
    if (g_log_color && g_log_color_on) {
        log_emit_colored(g_log_color, level, " ", msg);
        return;
    }
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
    if (g_log_color && g_log_color_on) {
        log_emit_colored(g_log_color, prefix, buf, 0);
        return;
    }
    log_emit(prefix);
    log_emit(buf);
    log_emit("\n");
}
