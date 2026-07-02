/* DKM PS/2 Keyboard Driver — IRQ1 scan code → ASCII
 * Stage 3, optional, depends on "irq", provides "keyboard".
 * Registers IRQ1 handler, translates scan codes via lookup table.
 */

#include <stdint.h>

/* DKM ABI types */
#define DKM_DRIVER_MAGIC 0x444B4D31u
#define DKM_ABI_VERSION  1u

typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;
typedef long long          i64;
#define NULL ((void *)0)

struct dkm_log_api {
    void (*info)(const char *msg);
    void (*warn)(const char *msg);
    void (*error)(const char *msg);
    void (*panic)(const char *msg);
};

struct dkm_kernel_api {
    u32 version; u32 size; u64 feature_bits;
    const struct dkm_log_api *log;
    const void *mem,*utsm,*irq_api,*pci_api,*dma,*vfs_api,*net,*timer,*drr;
    const void *rsdp_address,*fb_address;
    u64 fb_width,fb_height,fb_pitch; u16 fb_bpp;
    const void *boot_modules_response;
    int (*irq_register)(u8 irq, void *handler);
};

struct dkm_driver_handle;
struct dkm_driver_desc {
    u32 magic; u16 abi_version; u16 desc_size;
    const char *name,*version,*vendor;
    u32 driver_class,stage,flags,priority;
    const char *const *depends; u32 depends_count;
    const char *const *provides; u32 provides_count;
    u64 min_kernel_abi,feature_bits,reserved0,reserved1;
};

/* Driver descriptor */
static const char *const g_depends[] = {"irq"};
static const char *const g_provides[] = {"keyboard"};

__attribute__((visibility("default"), used))
const struct dkm_driver_desc driver_desc = {
    .magic=DKM_DRIVER_MAGIC, .abi_version=DKM_ABI_VERSION,
    .desc_size=sizeof(struct dkm_driver_desc),
    .name="ps2kbd", .version="0.1.0", .vendor="Deshab",
    .driver_class=10, .stage=3, .flags=0, .priority=0,
    .depends=g_depends, .depends_count=1,
    .provides=g_provides, .provides_count=1,
    .min_kernel_abi=1,
};

/* Scan code set 1 → ASCII (lowercase) */
static const char sc_ascii[] = {
    0,0,'1','2','3','4','5','6','7','8','9','0','-','=',0,
    0,'q','w','e','r','t','y','u','i','o','p','[',']',0,
    0,'a','s','d','f','g','h','j','k','l',';','\'','`',0,
    '\\','z','x','c','v','b','n','m',',','.','/',0,
    '*',0,' ',0
};

static const struct dkm_log_api *g_log;
static volatile int g_has_key;
static volatile u8 g_last_sc;

/* inline I/O */
static u8 inb(u16 p) { u8 v; __asm__("inb %1,%0":"=a"(v):"Nd"(p)); return v; }
static void outb(u16 p, u8 v) { __asm__("outb %0,%1"::"a"(v),"Nd"(p)); }

static void log_hex(const char *prefix, u64 v) {
    static const char h[]="0123456789abcdef";
    char b[19]; u32 i=0; b[i++]='0'; b[i++]='x';
    for (int j=15;j>=0;j--) b[i++]=h[(v>>(j*4))&0xf];
    b[i]=0; g_log->info(prefix); g_log->info(b);
}

static int kbd_handler(u8 irq) {
    (void)irq;
    g_last_sc = inb(0x60);
    g_has_key = 1;
    return 0; /* let kernel send EOI */
}

__attribute__((visibility("default")))
int driver_init(const struct dkm_kernel_api *api,
                struct dkm_driver_handle *handle) {
    (void)handle;
    if (!api||!api->log) return -1;
    g_log=api->log;

    if (!api->irq_register) {
        g_log->warn("[kbd] irq_register not available");
        return 0;
    }

    if (api->irq_register(1, (void*)(uintptr_t)kbd_handler) != 0) {
        g_log->error("[kbd] failed to register IRQ1");
        return -1;
    }

    g_log->info("[kbd] IRQ1 registered, waiting for keypress");
    g_log->info("[kbd] (boot will continue, key echo in ISR)");

    /* spin briefly to show a key echo demo */
    int ticks = 0;
    while (ticks < 200000) {
        if (g_has_key) {
            u8 sc = g_last_sc;
            g_has_key = 0;
            if (sc < 0x80) {
                /* make code */
                log_hex("[kbd] sc=", sc);
                if (sc < sizeof(sc_ascii) && sc_ascii[sc]) {
                    char buf[2] = {sc_ascii[sc], 0};
                    g_log->info("[kbd] key");
                    g_log->info(buf);
                }
            }
        }
        for (volatile int z=0;z<100;z++) {}
        ticks++;
    }

    g_log->info("[kbd] driver ready");
    return 0;
}

__attribute__((visibility("default")))
int driver_exit(struct dkm_driver_handle *handle) {
    (void)handle; return 0;
}
