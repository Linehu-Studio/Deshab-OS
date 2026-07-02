#ifndef DSK_PORT_H
#define DSK_PORT_H

static inline void outb(unsigned short port, unsigned char value) {
    __asm__ volatile ("outb %0, %1" :: "a"(value), "Nd"(port));
}

static inline unsigned char inb(unsigned short port) {
    unsigned char value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

#endif
