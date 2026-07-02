#include <utsm/idt.h>
#include <utsm/log.h>
#include <utsm/types.h>

/* IDT gate descriptor (64-bit) */
typedef struct __attribute__((packed)) {
    u16 offset_low;
    u16 selector;
    u8  ist;
    u8  type_attr;
    u16 offset_mid;
    u32 offset_high;
    u32 reserved;
} idt_entry;

/* IDT register */
typedef struct __attribute__((packed)) {
    u16 limit;
    u64 base;
} idt_ptr;

#define IDT_ENTRIES 256
static idt_entry g_idt[IDT_ENTRIES] __attribute__((aligned(16)));

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1
#define ICW1_ICW4 0x01
#define ICW1_INIT 0x10

/* assembly stub declarations */
extern void isr_stub_0(void);
extern void isr_stub_1(void);
extern void isr_stub_2(void);
extern void isr_stub_3(void);
extern void isr_stub_4(void);
extern void isr_stub_5(void);
extern void isr_stub_6(void);
extern void isr_stub_7(void);
extern void isr_stub_8(void);
extern void isr_stub_9(void);
extern void isr_stub_10(void);
extern void isr_stub_11(void);
extern void isr_stub_12(void);
extern void isr_stub_13(void);
extern void isr_stub_14(void);
extern void isr_stub_15(void);
extern void isr_stub_16(void);
extern void isr_stub_17(void);
extern void isr_stub_18(void);
extern void isr_stub_19(void);
extern void isr_stub_20(void);
extern void isr_stub_21(void);
extern void isr_stub_22(void);
extern void isr_stub_23(void);
extern void isr_stub_24(void);
extern void isr_stub_25(void);
extern void isr_stub_26(void);
extern void isr_stub_27(void);
extern void isr_stub_28(void);
extern void isr_stub_29(void);
extern void isr_stub_30(void);
extern void isr_stub_31(void);
extern void isr_stub_32(void);
extern void isr_stub_33(void);
extern void isr_stub_34(void);
extern void isr_stub_35(void);
extern void isr_stub_36(void);
extern void isr_stub_37(void);
extern void isr_stub_38(void);
extern void isr_stub_39(void);
extern void isr_stub_40(void);
extern void isr_stub_41(void);
extern void isr_stub_42(void);
extern void isr_stub_43(void);
extern void isr_stub_44(void);
extern void isr_stub_45(void);
extern void isr_stub_46(void);
extern void isr_stub_47(void);

static void *g_isr_table[] = {
    isr_stub_0,  isr_stub_1,  isr_stub_2,  isr_stub_3,
    isr_stub_4,  isr_stub_5,  isr_stub_6,  isr_stub_7,
    isr_stub_8,  isr_stub_9,  isr_stub_10, isr_stub_11,
    isr_stub_12, isr_stub_13, isr_stub_14, isr_stub_15,
    isr_stub_16, isr_stub_17, isr_stub_18, isr_stub_19,
    isr_stub_20, isr_stub_21, isr_stub_22, isr_stub_23,
    isr_stub_24, isr_stub_25, isr_stub_26, isr_stub_27,
    isr_stub_28, isr_stub_29, isr_stub_30, isr_stub_31,
    isr_stub_32, isr_stub_33, isr_stub_34, isr_stub_35,
    isr_stub_36, isr_stub_37, isr_stub_38, isr_stub_39,
    isr_stub_40, isr_stub_41, isr_stub_42, isr_stub_43,
    isr_stub_44, isr_stub_45, isr_stub_46, isr_stub_47,
};

static void outb(u16 port, u8 value) {
    __asm__ volatile ("outb %0, %1" :: "a"(value), "Nd"(port));
}

static u8 inb(u16 port) {
    u8 value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static void io_wait(void) {
    outb(0x80, 0);
}

static void pic_remap(void) {
    /* save masks */
    u8 m1 = inb(PIC1_DATA);
    u8 m2 = inb(PIC2_DATA);

    /* ICW1: start init + ICW4 */
    outb(PIC1_CMD, ICW1_INIT | ICW1_ICW4);
    io_wait();
    outb(PIC2_CMD, ICW1_INIT | ICW1_ICW4);
    io_wait();

    /* ICW2: vector offsets */
    outb(PIC1_DATA, 0x20);   /* master: vectors 0x20-0x27 */
    io_wait();
    outb(PIC2_DATA, 0x28);   /* slave:  vectors 0x28-0x2F */
    io_wait();

    /* ICW3: master/slave wiring */
    outb(PIC1_DATA, 0x04);   /* master: IRQ2 → slave */
    io_wait();
    outb(PIC2_DATA, 0x02);   /* slave:  cascade identity */
    io_wait();

    /* ICW4: 8086 mode */
    outb(PIC1_DATA, 0x01);
    io_wait();
    outb(PIC2_DATA, 0x01);
    io_wait();

    /* restore masks: mask all except IRQ1 (keyboard) */
    outb(PIC1_DATA, 0xFD);   /* 1111_1101: enable IRQ0 (timer) and IRQ1 (kb) */
    outb(PIC2_DATA, 0xFF);   /* mask all slave */
}

static void idt_set_entry(u8 vector, void *handler, u8 ist, u8 type) {
    u64 addr = (u64)handler;
    g_idt[vector].offset_low  = (u16)(addr & 0xffff);
    g_idt[vector].selector     = 0x08;  /* kernel code segment (from GDT) */
    g_idt[vector].ist          = ist;
    g_idt[vector].type_attr    = type;
    g_idt[vector].offset_mid   = (u16)((addr >> 16) & 0xffff);
    g_idt[vector].offset_high  = (u32)(addr >> 32);
    g_idt[vector].reserved     = 0;
}

void idt_init(void) {
    log_info("[IDT] init begin");

    for (u16 i = 0; i < 48; i++) {
        idt_set_entry((u8)i, g_isr_table[i], 0, 0x8E);  /* present, ring0, 64-bit interrupt gate */
    }
    /* fill unused entries with null */
    for (u16 i = 48; i < IDT_ENTRIES; i++) {
        idt_set_entry((u8)i, 0, 0, 0);
    }

    idt_ptr idtr;
    idtr.limit = (u16)(sizeof(g_idt) - 1);
    idtr.base  = (u64)&g_idt;

    __asm__ volatile ("lidt %0" :: "m"(idtr));

    pic_remap();
    log_info("[IDT] PIC remapped (master=0x20 slave=0x28)");

    __asm__ volatile ("sti");
    log_info("[IDT] interrupts enabled");
    log_info("[IDT] init ok");
}

/* IRQ handler table */
static irq_handler_t g_irq_handlers[16];

int irq_register(u8 irq, irq_handler_t handler) {
    if (irq >= 16) return -1;
    g_irq_handlers[irq] = handler;
    return 0;
}

/* Stack frame pushed by isr_common in idt.S */
typedef struct {
    u64 r11, r10, r9, r8, rax, rcx, rdx, rsi, rdi;
    u64 vector;
    u64 error_code;
    u64 rip, cs, rflags;
} isr_frame;

void idt_handler(isr_frame *frame) {
    if (frame->vector < 32) {
        log_error("[IDT] exception");
        log_hex64("[IDT] vector=", frame->vector);
        log_hex64("[IDT] err=", frame->error_code);
        log_hex64("[IDT] rip=", frame->rip);
        for (;;) { __asm__ volatile ("cli; hlt"); }
    }

    if (frame->vector >= 32 && frame->vector <= 47) {
        u8 irq = (u8)(frame->vector - 0x20);

        /* call registered handler */
        if (irq < 16 && g_irq_handlers[irq]) {
            int suppress_eoi = g_irq_handlers[irq](irq);
            if (suppress_eoi) return;
        }

        /* send EOI */
        if (frame->vector >= 0x28) {
            outb(PIC2_CMD, 0x20);
        }
        outb(PIC1_CMD, 0x20);
    }
}
