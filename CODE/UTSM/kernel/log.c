#include <utsm/log.h>

static void log_line(const char *level, const char *msg) {
    serial_write(level);
    serial_write(" ");
    serial_write(msg);
    serial_write("\n");
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
    serial_write(prefix);
    serial_write(buf);
    serial_write("\n");
}
