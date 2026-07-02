/* mouseInit — safe mouse initializer stub
 * DSK-owned system component initializer.
 * Current version must never block or crash first boot.
 */

#include "../UTSM/include/utsm/dsk.h"

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;

#define COM1 0x3F8
#define PS2_STATUS 0x64

static inline void outb(u16 port, u8 value) { __asm__ volatile("outb %0,%1"::"a"(value),"Nd"(port)); }
static inline u8 inb(u16 port) { u8 v; __asm__ volatile("inb %1,%0":"=a"(v):"Nd"(port)); return v; }

static void sputc(char c) {
    for (u32 i=0;i<10000;i++) if (inb(COM1+5)&0x20) break;
    outb(COM1,(u8)c);
}
static void swrite(const char *s) { while(*s){ if(*s=='\n') sputc('\r'); sputc(*s++);} }
static void logl(const char *s) { swrite(s); swrite("\n"); }

__attribute__((visibility("default")))
void dsk_entry(const dsk_boot_context *ctx) {
    (void)ctx;
    logl("[mouseInit] boot");

    /* Safe first version: do not touch PS/2 controller command path yet.
       The previous implementation could block/reboot on some firmware states.
       Full IRQ12 + packet decoding will be implemented as a later input driver. */
    u8 status = inb(PS2_STATUS);
    (void)status;

    logl("[mouseInit] safe init complete");
    return;
}
