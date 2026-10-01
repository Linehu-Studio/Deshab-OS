#include <utsm/idt.h>
#include <utsm/log.h>
#include <utsm/types.h>
#include <utsm/instr.h>
#include <utsm/panic.h>

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

/* assembly stub declarations — vectors 0-255 全覆盖 */
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
extern void isr_stub_48(void);
extern void isr_stub_49(void);
extern void isr_stub_50(void);
extern void isr_stub_51(void);
extern void isr_stub_52(void);
extern void isr_stub_53(void);
extern void isr_stub_54(void);
extern void isr_stub_55(void);
extern void isr_stub_56(void);
extern void isr_stub_57(void);
extern void isr_stub_58(void);
extern void isr_stub_59(void);
extern void isr_stub_60(void);
extern void isr_stub_61(void);
extern void isr_stub_62(void);
extern void isr_stub_63(void);
extern void isr_stub_64(void);
extern void isr_stub_65(void);
extern void isr_stub_66(void);
extern void isr_stub_67(void);
extern void isr_stub_68(void);
extern void isr_stub_69(void);
extern void isr_stub_70(void);
extern void isr_stub_71(void);
extern void isr_stub_72(void);
extern void isr_stub_73(void);
extern void isr_stub_74(void);
extern void isr_stub_75(void);
extern void isr_stub_76(void);
extern void isr_stub_77(void);
extern void isr_stub_78(void);
extern void isr_stub_79(void);
extern void isr_stub_80(void);
extern void isr_stub_81(void);
extern void isr_stub_82(void);
extern void isr_stub_83(void);
extern void isr_stub_84(void);
extern void isr_stub_85(void);
extern void isr_stub_86(void);
extern void isr_stub_87(void);
extern void isr_stub_88(void);
extern void isr_stub_89(void);
extern void isr_stub_90(void);
extern void isr_stub_91(void);
extern void isr_stub_92(void);
extern void isr_stub_93(void);
extern void isr_stub_94(void);
extern void isr_stub_95(void);
extern void isr_stub_96(void);
extern void isr_stub_97(void);
extern void isr_stub_98(void);
extern void isr_stub_99(void);
extern void isr_stub_100(void);
extern void isr_stub_101(void);
extern void isr_stub_102(void);
extern void isr_stub_103(void);
extern void isr_stub_104(void);
extern void isr_stub_105(void);
extern void isr_stub_106(void);
extern void isr_stub_107(void);
extern void isr_stub_108(void);
extern void isr_stub_109(void);
extern void isr_stub_110(void);
extern void isr_stub_111(void);
extern void isr_stub_112(void);
extern void isr_stub_113(void);
extern void isr_stub_114(void);
extern void isr_stub_115(void);
extern void isr_stub_116(void);
extern void isr_stub_117(void);
extern void isr_stub_118(void);
extern void isr_stub_119(void);
extern void isr_stub_120(void);
extern void isr_stub_121(void);
extern void isr_stub_122(void);
extern void isr_stub_123(void);
extern void isr_stub_124(void);
extern void isr_stub_125(void);
extern void isr_stub_126(void);
extern void isr_stub_127(void);
extern void isr_stub_128(void);
extern void isr_stub_129(void);
extern void isr_stub_130(void);
extern void isr_stub_131(void);
extern void isr_stub_132(void);
extern void isr_stub_133(void);
extern void isr_stub_134(void);
extern void isr_stub_135(void);
extern void isr_stub_136(void);
extern void isr_stub_137(void);
extern void isr_stub_138(void);
extern void isr_stub_139(void);
extern void isr_stub_140(void);
extern void isr_stub_141(void);
extern void isr_stub_142(void);
extern void isr_stub_143(void);
extern void isr_stub_144(void);
extern void isr_stub_145(void);
extern void isr_stub_146(void);
extern void isr_stub_147(void);
extern void isr_stub_148(void);
extern void isr_stub_149(void);
extern void isr_stub_150(void);
extern void isr_stub_151(void);
extern void isr_stub_152(void);
extern void isr_stub_153(void);
extern void isr_stub_154(void);
extern void isr_stub_155(void);
extern void isr_stub_156(void);
extern void isr_stub_157(void);
extern void isr_stub_158(void);
extern void isr_stub_159(void);
extern void isr_stub_160(void);
extern void isr_stub_161(void);
extern void isr_stub_162(void);
extern void isr_stub_163(void);
extern void isr_stub_164(void);
extern void isr_stub_165(void);
extern void isr_stub_166(void);
extern void isr_stub_167(void);
extern void isr_stub_168(void);
extern void isr_stub_169(void);
extern void isr_stub_170(void);
extern void isr_stub_171(void);
extern void isr_stub_172(void);
extern void isr_stub_173(void);
extern void isr_stub_174(void);
extern void isr_stub_175(void);
extern void isr_stub_176(void);
extern void isr_stub_177(void);
extern void isr_stub_178(void);
extern void isr_stub_179(void);
extern void isr_stub_180(void);
extern void isr_stub_181(void);
extern void isr_stub_182(void);
extern void isr_stub_183(void);
extern void isr_stub_184(void);
extern void isr_stub_185(void);
extern void isr_stub_186(void);
extern void isr_stub_187(void);
extern void isr_stub_188(void);
extern void isr_stub_189(void);
extern void isr_stub_190(void);
extern void isr_stub_191(void);
extern void isr_stub_192(void);
extern void isr_stub_193(void);
extern void isr_stub_194(void);
extern void isr_stub_195(void);
extern void isr_stub_196(void);
extern void isr_stub_197(void);
extern void isr_stub_198(void);
extern void isr_stub_199(void);
extern void isr_stub_200(void);
extern void isr_stub_201(void);
extern void isr_stub_202(void);
extern void isr_stub_203(void);
extern void isr_stub_204(void);
extern void isr_stub_205(void);
extern void isr_stub_206(void);
extern void isr_stub_207(void);
extern void isr_stub_208(void);
extern void isr_stub_209(void);
extern void isr_stub_210(void);
extern void isr_stub_211(void);
extern void isr_stub_212(void);
extern void isr_stub_213(void);
extern void isr_stub_214(void);
extern void isr_stub_215(void);
extern void isr_stub_216(void);
extern void isr_stub_217(void);
extern void isr_stub_218(void);
extern void isr_stub_219(void);
extern void isr_stub_220(void);
extern void isr_stub_221(void);
extern void isr_stub_222(void);
extern void isr_stub_223(void);
extern void isr_stub_224(void);
extern void isr_stub_225(void);
extern void isr_stub_226(void);
extern void isr_stub_227(void);
extern void isr_stub_228(void);
extern void isr_stub_229(void);
extern void isr_stub_230(void);
extern void isr_stub_231(void);
extern void isr_stub_232(void);
extern void isr_stub_233(void);
extern void isr_stub_234(void);
extern void isr_stub_235(void);
extern void isr_stub_236(void);
extern void isr_stub_237(void);
extern void isr_stub_238(void);
extern void isr_stub_239(void);
extern void isr_stub_240(void);
extern void isr_stub_241(void);
extern void isr_stub_242(void);
extern void isr_stub_243(void);
extern void isr_stub_244(void);
extern void isr_stub_245(void);
extern void isr_stub_246(void);
extern void isr_stub_247(void);
extern void isr_stub_248(void);
extern void isr_stub_249(void);
extern void isr_stub_250(void);
extern void isr_stub_251(void);
extern void isr_stub_252(void);
extern void isr_stub_253(void);
extern void isr_stub_254(void);
extern void isr_stub_255(void);

static void *g_isr_table[256] = {
    isr_stub_0,   isr_stub_1,   isr_stub_2,   isr_stub_3,
    isr_stub_4,   isr_stub_5,   isr_stub_6,   isr_stub_7,
    isr_stub_8,   isr_stub_9,   isr_stub_10,  isr_stub_11,
    isr_stub_12,  isr_stub_13,  isr_stub_14,  isr_stub_15,
    isr_stub_16,  isr_stub_17,  isr_stub_18,  isr_stub_19,
    isr_stub_20,  isr_stub_21,  isr_stub_22,  isr_stub_23,
    isr_stub_24,  isr_stub_25,  isr_stub_26,  isr_stub_27,
    isr_stub_28,  isr_stub_29,  isr_stub_30,  isr_stub_31,
    isr_stub_32,  isr_stub_33,  isr_stub_34,  isr_stub_35,
    isr_stub_36,  isr_stub_37,  isr_stub_38,  isr_stub_39,
    isr_stub_40,  isr_stub_41,  isr_stub_42,  isr_stub_43,
    isr_stub_44,  isr_stub_45,  isr_stub_46,  isr_stub_47,
    isr_stub_48,  isr_stub_49,  isr_stub_50,  isr_stub_51,
    isr_stub_52,  isr_stub_53,  isr_stub_54,  isr_stub_55,
    isr_stub_56,  isr_stub_57,  isr_stub_58,  isr_stub_59,
    isr_stub_60,  isr_stub_61,  isr_stub_62,  isr_stub_63,
    isr_stub_64,  isr_stub_65,  isr_stub_66,  isr_stub_67,
    isr_stub_68,  isr_stub_69,  isr_stub_70,  isr_stub_71,
    isr_stub_72,  isr_stub_73,  isr_stub_74,  isr_stub_75,
    isr_stub_76,  isr_stub_77,  isr_stub_78,  isr_stub_79,
    isr_stub_80,  isr_stub_81,  isr_stub_82,  isr_stub_83,
    isr_stub_84,  isr_stub_85,  isr_stub_86,  isr_stub_87,
    isr_stub_88,  isr_stub_89,  isr_stub_90,  isr_stub_91,
    isr_stub_92,  isr_stub_93,  isr_stub_94,  isr_stub_95,
    isr_stub_96,  isr_stub_97,  isr_stub_98,  isr_stub_99,
    isr_stub_100, isr_stub_101, isr_stub_102, isr_stub_103,
    isr_stub_104, isr_stub_105, isr_stub_106, isr_stub_107,
    isr_stub_108, isr_stub_109, isr_stub_110, isr_stub_111,
    isr_stub_112, isr_stub_113, isr_stub_114, isr_stub_115,
    isr_stub_116, isr_stub_117, isr_stub_118, isr_stub_119,
    isr_stub_120, isr_stub_121, isr_stub_122, isr_stub_123,
    isr_stub_124, isr_stub_125, isr_stub_126, isr_stub_127,
    isr_stub_128, isr_stub_129, isr_stub_130, isr_stub_131,
    isr_stub_132, isr_stub_133, isr_stub_134, isr_stub_135,
    isr_stub_136, isr_stub_137, isr_stub_138, isr_stub_139,
    isr_stub_140, isr_stub_141, isr_stub_142, isr_stub_143,
    isr_stub_144, isr_stub_145, isr_stub_146, isr_stub_147,
    isr_stub_148, isr_stub_149, isr_stub_150, isr_stub_151,
    isr_stub_152, isr_stub_153, isr_stub_154, isr_stub_155,
    isr_stub_156, isr_stub_157, isr_stub_158, isr_stub_159,
    isr_stub_160, isr_stub_161, isr_stub_162, isr_stub_163,
    isr_stub_164, isr_stub_165, isr_stub_166, isr_stub_167,
    isr_stub_168, isr_stub_169, isr_stub_170, isr_stub_171,
    isr_stub_172, isr_stub_173, isr_stub_174, isr_stub_175,
    isr_stub_176, isr_stub_177, isr_stub_178, isr_stub_179,
    isr_stub_180, isr_stub_181, isr_stub_182, isr_stub_183,
    isr_stub_184, isr_stub_185, isr_stub_186, isr_stub_187,
    isr_stub_188, isr_stub_189, isr_stub_190, isr_stub_191,
    isr_stub_192, isr_stub_193, isr_stub_194, isr_stub_195,
    isr_stub_196, isr_stub_197, isr_stub_198, isr_stub_199,
    isr_stub_200, isr_stub_201, isr_stub_202, isr_stub_203,
    isr_stub_204, isr_stub_205, isr_stub_206, isr_stub_207,
    isr_stub_208, isr_stub_209, isr_stub_210, isr_stub_211,
    isr_stub_212, isr_stub_213, isr_stub_214, isr_stub_215,
    isr_stub_216, isr_stub_217, isr_stub_218, isr_stub_219,
    isr_stub_220, isr_stub_221, isr_stub_222, isr_stub_223,
    isr_stub_224, isr_stub_225, isr_stub_226, isr_stub_227,
    isr_stub_228, isr_stub_229, isr_stub_230, isr_stub_231,
    isr_stub_232, isr_stub_233, isr_stub_234, isr_stub_235,
    isr_stub_236, isr_stub_237, isr_stub_238, isr_stub_239,
    isr_stub_240, isr_stub_241, isr_stub_242, isr_stub_243,
    isr_stub_244, isr_stub_245, isr_stub_246, isr_stub_247,
    isr_stub_248, isr_stub_249, isr_stub_250, isr_stub_251,
    isr_stub_252, isr_stub_253, isr_stub_254, isr_stub_255,
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
    /* save masks (kept for reference; fixed masks are applied below) */
    u8 m1 = inb(PIC1_DATA);
    u8 m2 = inb(PIC2_DATA);
    (void)m1;
    (void)m2;

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

/* 运行时读取当前内核 CS — 必须与实际生效的 GDT 一致。
 * 注意:UTSM 运行在 Limine 提供的 GDT 下(CS=0x28),不能假设
 * OSDev 教程布局(0x08=code)。若 IDT gate selector 指向无效/非64位
 * 代码段,异常派发时加载 CS 会 #GP(selector error),异常链升级为
 * #DF -> triple fault,且全程无任何日志输出。 */
static u16 idt_kernel_cs(void) {
    u16 cs;
    __asm__ volatile("mov %%cs, %0" : "=r"(cs));
    return cs;
}

static void idt_set_entry(u8 vector, void *handler, u8 ist, u8 type) {
    u64 addr = (u64)handler;
    g_idt[vector].offset_low  = (u16)(addr & 0xffff);
    g_idt[vector].selector     = idt_kernel_cs();
    g_idt[vector].ist          = ist;
    g_idt[vector].type_attr    = type;
    g_idt[vector].offset_mid   = (u16)((addr >> 16) & 0xffff);
    g_idt[vector].offset_high  = (u32)(addr >> 32);
    g_idt[vector].reserved     = 0;
}

void idt_init(void) {
    log_info("[IDT] init begin");

    for (u16 i = 0; i < IDT_ENTRIES; i++) {
        idt_set_entry((u8)i, g_isr_table[i], 0, 0x8E);  /* present, ring0, 64-bit interrupt gate */
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

/* 安全 halt stub: cli; hlt 循环。用于 idt_halt_all()。
 * 任何异常/IRQ 触发此 stub 将安全停机，不会三重故障。 */
static void isr_halt_stub(void) {
    for (;;) { __asm__ volatile("cli; hlt"); }
}

void idt_halt_all(void) {
    /* 将 IDT 所有 256 个条目替换为安全 halt stub。
     * 调用前应已 cli + mask PIC，此函数是额外防御层:
     * - 实机 spurious IRQ7/15 即使 PIC 已 mask 也可能触发
     * - NMI / SMI 可能导致异常
     * - DSK 运行期间 CPU 异常 (PF/GP) 应安全停机而非执行过期 handler */
    for (u16 i = 0; i < IDT_ENTRIES; i++) {
        idt_set_entry((u8)i, (void *)isr_halt_stub, 0, 0x8E);
    }
    /* 重新加载 IDTR 以确保更改生效 */
    idt_ptr idtr;
    idtr.limit = (u16)(sizeof(g_idt) - 1);
    idtr.base  = (u64)&g_idt;
    __asm__ volatile("lidt %0" :: "m"(idtr));
}

void idt_install_dsk_diag(void) {
    /* BUG-20260801-005: DSK 交接诊断 IDT（替代原 idt_halt_all 静默化）。
     * 原 halt_all 将 256 项全部换成静默 halt stub——防御正确，但 DSK
     * 阶段任何 CPU 异常（PF/GP/UD）被无声吞掉，零日志无法诊断
     * （实测 pe HELLO64.EXE 早期 #PF 直接表现为"卡死/三重故障"）。
     * 本函数在保留 IRQ 防御的前提下恢复异常可见性:
     *   vector 0-31   -> 原 isr_stub（走 idt_handler 异常路径，打完整
     *                    vector/err/rip/CR2/寄存器日志后安全停机）
     *   vector 32-255 -> 保持 halt stub（PIC 已全 mask 且 IF=0，IRQ 本
     *                    不该到达；spurious IRQ 维持原防御行为）
     * 不做 pic_remap / sti——DSK 上下文 IF=0 不变式保持不变。
     * isr_stub / idt_handler 是 UTSM .text 代码，SAS-R0 下常驻内存
     * 有效（BUG-20260729-013 登记行为:halt_all 引入前 DSK 阶段异常
     * 本就能送达 idt_handler）；异常路径只用 log/serial，不依赖已过期
     * 的 IRQ handler 状态。 */
    for (u16 i = 0; i < 32; i++) {
        idt_set_entry((u8)i, g_isr_table[i], 0, 0x8E);
    }
    for (u16 i = 32; i < IDT_ENTRIES; i++) {
        idt_set_entry((u8)i, (void *)isr_halt_stub, 0, 0x8E);
    }
    idt_ptr idtr;
    idtr.limit = (u16)(sizeof(g_idt) - 1);
    idtr.base  = (u64)&g_idt;
    __asm__ volatile("lidt %0" :: "m"(idtr));
    log_info("[IDT] DSK diag IDT installed (exc 0-31 logged, IRQ halted)");
}

void idt_install_pe_diag(void) {
    /* 语义与 idt_install_dsk_diag 相同：BUG-20260801-005 起 DSK 交接
     * 默认装备诊断 IDT，PE 原生执行前重装一次保持幂等（防御 PE 运行
     * 期间 IDT 被第三方改写的场景）。 */
    idt_install_dsk_diag();
}

/* IRQ handler table — 扩展到 256 个 vector (真机 APIC/IOAPIC/MSI 需要) */
static irq_handler_t g_irq_handlers[256];

/* B7 阶段2: LAPIC EOI 钩子 (由 apic.drv 经 kernel_api.register_apic_eoi 注册)。
 * 指向写 LAPIC EOI 寄存器 (0xFEE000B0) 的函数; NULL = PIC 路由模式, 不调用。
 * 单核 BSP 场景: 注册发生在 apic.drv init (普通上下文), 调用发生在 IRQ
 * 上下文; 函数指针赋值对齐原子, 无需锁。 */
static void (*g_apic_eoi_hook)(void) = 0;

void idt_register_apic_eoi(void (*eoi_fn)(void)) {
    g_apic_eoi_hook = eoi_fn;
    if (eoi_fn) {
        log_info("[IDT] APIC EOI hook registered (LAPIC EOI on IRQ dispatch)");
    } else {
        log_info("[IDT] APIC EOI hook cleared");
    }
}

/* ---- 插桩: IRQ 统计计数器 ---- */
INSTR_STAT_DECL(pic_irq_total);
INSTR_STAT_DECL(apic_irq_total);

/* ---- B7 阶段3: 动态 IDT 向量分配器 (MSI/MSI-X 用) ----
 * 分配范围 0x40-0xDF (160 个向量):
 *   0x00-0x1F  CPU 异常
 *   0x20-0x2F  PIC legacy IRQ (保留不动)
 *   0x30-0x3F  保留给 IOAPIC GSI 16-31 未来扩展 (本阶段不动态分配)
 *   0x40-0xDF  动态分配池 (MSI/MSI-X)
 *   0xE0       APIC tick 自测预留 (阶段1 惯例)
 *   0xFF       LAPIC spurious vector
 * 数据结构: 3 x u64 位图 (192 bit, 用后 160), __builtin_ctzll 找首个
 * 零位, 分配/释放均 O(1)。热路径无锁: SAS-R0 单核 BSP, alloc/free 只在
 * driver_init 主上下文调用 (MSI 编程期间对应设备尚未解除中断屏蔽,
 * IRQ 上下文不可能并发分配), 位图字为对齐 u64 单写原子。
 * 动态入口无需新增 stub: idt_stubs.S 已为 0-255 全部向量预生成入口,
 * 分配后 irq_register(vector, handler) 直接挂上既有分发表。 */
#define IRQ_VEC_POOL_BASE  0x40
#define IRQ_VEC_POOL_COUNT 160
static u64 g_vec_bitmap[3];

int irq_vector_alloc(void) {
    for (u32 w = 0; w < 3; w++) {
        u64 free_bits = ~g_vec_bitmap[w];
        if (!free_bits) continue;
        u32 bit = (u32)__builtin_ctzll(free_bits);
        u32 idx = w * 64u + bit;
        if (idx >= IRQ_VEC_POOL_COUNT) break;
        g_vec_bitmap[w] |= (1ULL << bit);
        int vec = IRQ_VEC_POOL_BASE + (int)idx;
        log_info("[IDT] vector alloc");
        log_hex64("[IDT]   vector=", (u64)vec);
        return vec;
    }
    log_warn("[IDT] vector alloc failed: pool exhausted");
    return -1;
}

void irq_vector_free(int vector) {
    if (vector < IRQ_VEC_POOL_BASE ||
        vector >= IRQ_VEC_POOL_BASE + (int)IRQ_VEC_POOL_COUNT) {
        return;   /* 越界输入幂等忽略 (静态向量不在池内) */
    }
    u32 idx = (u32)(vector - IRQ_VEC_POOL_BASE);
    g_vec_bitmap[idx / 64u] &= ~(1ULL << (idx % 64u));
    log_info("[IDT] vector free");
    log_hex64("[IDT]   vector=", (u64)vector);
}

int irq_register(u8 vector, irq_handler_t handler) {
    /* IRQ号/向量兼容 shim: 0-15 视为 ISA IRQ 号, 换算 vector=0x20+IRQ。
     * 依据: vector 0-15 是 CPU 异常向量 (#DE/#DB/...), idt_handler 的 IRQ
     * 分发只覆盖 32-255, 故 0-15 作为注册目标永不可达、无歧义。
     * 现状调用方两命名空间并存: ps2kbd(1)/e1000(11)/mouseInit(12) 传 IRQ 号,
     * apic(0xE0)/virtio_net(0x20+n) 传向量 — 统一在此归一到向量空间。 */
    if (vector < 16) {
        u8 v = (u8)(0x20u + vector);
        log_info("[IDT] irq_register: ISA IRQ number translated to vector");
        log_hex64("[IDT]   irq=", vector);
        log_hex64("[IDT]   vector=", v);
        vector = v;
    }
    g_irq_handlers[vector] = handler;
    INSTR_LOG(INSTR_LOG_DEBUG, INSTR_F_UTSM_TRACE, "[IDT] IRQ handler registered");
    return 0;
}

/* Phase7: 显式 EOI（调度器 tick 等"handler 内可能切换任务"的路径用）。
 * 序列复刻 idt_handler 默认 EOI: 先 LAPIC 钩子再 8259（按向量范围）。
 * spurious 0xFF 特判与 idt_handler 一致（空 ISR 不该有 EOI）。 */
void idt_irq_eoi(u8 vector) {
    if (vector < 16) vector = (u8)(0x20u + vector);
    if (vector >= 32 && vector <= 47) {
        if (g_apic_eoi_hook) g_apic_eoi_hook();
        if (vector >= 0x28) outb(PIC2_CMD, 0x20);
        outb(PIC1_CMD, 0x20);
    } else if (vector >= 48 && vector != 0xFF) {
        if (g_apic_eoi_hook) g_apic_eoi_hook();
    }
}

/* Phase8: CPU 异常钩子（DRR 任务级 fault 接管）。 */
static int (*g_exc_hook)(u64 vector, u64 err, u64 rip) = 0;
void idt_set_exception_hook(int (*hook)(u64 vector, u64 err, u64 rip)) {
    g_exc_hook = hook;
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
        /* Phase8: DRR 异常钩子最前——返回非 0 = 异常已被消费
         * （fault 任务被杀/切换，控制权不回到日志+停机路径） */
        if (g_exc_hook &&
            g_exc_hook(frame->vector, frame->error_code, frame->rip)) {
            return;
        }
        /* CR2: #PF 的 fault 线性地址；CR3: 当前页表基址。
         * 寄存器 dump 用于定位 fault 指令的操作数来源。
         * F1: 异常统一走莲花崩溃屏（panic_full 内部保留串口 dump +
         * DRR 归档 + 帧缓冲直绘，最后 halt——不返回）。 */
        u64 cr2, cr3;
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
        __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
        (void)cr3;
        INSTR_PROBE(EXCP, frame->vector, frame->error_code, frame->rip, 0);
        panic_regs pr;
        pr.rip = frame->rip;
        pr.rsp = 0;             /* isr_frame 不含 rsp（stub 内使用），置 0 */
        pr.rflags = frame->rflags;
        pr.cs = frame->cs;
        pr.err = frame->error_code;
        pr.vector = frame->vector;
        pr.cr2 = cr2;
        pr.rax = frame->rax;
        pr.rcx = frame->rcx;
        pr.rdx = frame->rdx;
        pr.rsi = frame->rsi;
        pr.rdi = frame->rdi;
        pr.r8 = frame->r8;
        pr.r9 = frame->r9;
        pr.r10 = frame->r10;
        pr.r11 = frame->r11;
        char sym[16];
        sym[0] = 'U'; sym[1] = 'T'; sym[2] = 'S'; sym[3] = 'M'; sym[4] = '-';
        sym[5] = 'E'; sym[6] = 'X'; sym[7] = 'C';
        u32 v = frame->vector;
        u32 hi = (v >> 4) & 0xF, lo = v & 0xF;
        sym[8] = (char)(hi < 10 ? '0' + hi : 'A' + hi - 10);
        sym[9] = (char)(lo < 10 ? '0' + lo : 'A' + lo - 10);
        sym[10] = 0;
        panic_full(sym, "cpu exception", &pr);
        for (;;) { __asm__ volatile ("cli; hlt"); }  /* 兜底（panic_full 不返回） */
    }

    /* PIC IRQ: vectors 0x20-0x2F (32-47)
     * B7 阶段2: 本分支同时服务两种路由来源:
     *   - 8259 PIC 路由 (默认): handler 后只需 8259 EOI, 钩子为 NULL 跳过;
     *   - IOAPIC->LAPIC 路由 (apic_route=1, legacy 线重定向到 0x20-0x2F):
     *     中断由 LAPIC 递交, ISR 位置位, 必须写 LAPIC EOI (钩子) 清除,
     *     否则同级 (vector[7:4] 相同) 中断被永久阻塞 — 每条线只投递一次。
     * LAPIC EOI 先于 8259 EOI; PIC 全屏蔽时 8259 EOI 为无害 no-op
     * (8259 无 in-service 位, 非特异性 EOI 不写任何状态)。 */
    if (frame->vector >= 32 && frame->vector <= 47) {
        INSTR_STAT_INC(pic_irq_total);
        u8 irq = (u8)(frame->vector - 0x20);

        /* call registered handler; 返回 1 = 自负 EOI 全责 (两者皆抑制) */
        if (g_irq_handlers[frame->vector]) {
            int suppress_eoi = g_irq_handlers[frame->vector](irq);
            if (suppress_eoi) return;
        }

        /* LAPIC EOI (IOAPIC 路由时必需; PIC 路由时钩子为 NULL) */
        if (g_apic_eoi_hook) {
            g_apic_eoi_hook();
        }

        /* send PIC EOI */
        if (frame->vector >= 0x28) {
            outb(PIC2_CMD, 0x20);
        }
        outb(PIC1_CMD, 0x20);
        return;
    }

    /* APIC/IOAPIC/MSI IRQ: vectors 0x30-0xFF (48-255)
     * handler 约定: 返回 0 -> 由钩子补 LAPIC EOI (安全网);
     *               返回 1 -> handler 已自行 LAPIC EOI, 跳过钩子。
     * 钩子未注册 (纯 PIC 路由) 时本分支不做任何 EOI — 与旧版行为一致,
     * 此时 handler 必须自行 LAPIC EOI (参见 apic.drv tick handler)。 */
    if (frame->vector >= 48 && frame->vector <= 255) {
        /* LAPIC spurious (SVR 低 8 位 = 0xFF): 规范上不置 ISR 位, 永不需 EOI。
         * 若在此调钩子写 EOI, 会误清嵌套场景下真正 in-service 的最高位,
         * 故特判直接返回 (EOI 空 ISR 虽无害, 但语义上 spurious 不该有 EOI)。 */
        if (frame->vector == 0xFF) return;
        INSTR_STAT_INC(apic_irq_total);
        int suppress_eoi = 0;
        if (g_irq_handlers[frame->vector]) {
            suppress_eoi = g_irq_handlers[frame->vector]((u8)frame->vector);
        }
        if (!suppress_eoi && g_apic_eoi_hook) {
            g_apic_eoi_hook();
        }
        return;
    }
}
