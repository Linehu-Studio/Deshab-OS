#include <utsm/types.h>

void outb(u16 port, u8 value);
u8 inb(u16 port);

#define COM1 0x3F8

static int serial_ready(void) {
    return (inb(COM1 + 5) & 0x20) != 0;
}

static int serial_wait_ready(void) {
    for (u64 spin = 0; spin < 1000000ULL; spin++) {
        if (serial_ready()) {
            return 1;
        }
    }
    return 0;
}

void serial_init(void) {
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x80);
    outb(COM1 + 0, 0x03);
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);
    outb(COM1 + 2, 0xC7);
    outb(COM1 + 4, 0x0B);
}

void serial_putc(char c) {
    if (c == '\n') {
        serial_putc('\r');
    }
    if (!serial_wait_ready()) {
        return;
    }
    outb(COM1, (u8)c);
}

void serial_write(const char *s) {
    while (*s) {
        serial_putc(*s++);
    }
}
