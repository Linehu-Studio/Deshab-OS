#include <utsm/log.h>
#include <utsm/arena.h>
#include <utsm/utsm.h>
#include <utsm/drr.h>
#include <utsm/dkm.h>
#include <utsm/idt.h>
#include <utsm/dsk.h>
#include <utsm/block.h>
#include <utsm/net.h>
#include <utsm/ipc_shm.h>
#include <utsm/linux_compat.h>
#include <utsm/instr.h>
#include "ini_parser.h"
#include "../arch/x86_64/limine.h"

void arch_halt_forever(void);

extern volatile struct limine_module_request g_module_request;

/* dkm/manifest.c: 带 FUCK [drivers] 过滤的 manifest 加载入口 */
void dsm_load_by_manifest_ex(const ini_config *cfg);

/* ---- 运行期配置全局变量（从 FUCK 文件初始化） ---- */
u32 g_utsm_max_segments     = UTSM_MAX_SEGMENTS_DEFAULT;
u32 g_utsm_max_capabilities = UTSM_MAX_CAPABILITIES_DEFAULT;
u32 g_utsm_max_pckc_keys    = UTSM_MAX_PCKC_KEYS_DEFAULT;
u64 g_utsm_arena_size       = UTSM_ARENA_SIZE_DEFAULT;
u64 g_utsm_dirty_shard_pages = UTSM_DIRTY_SHARD_PAGES_DEFAULT;

/* 将 FUCK [utsm] 区的配置应用到运行期全局变量 */
static void utsm_apply_config(const ini_config *cfg) {
    if (!cfg) return;
    g_utsm_max_segments     = (u32)ini_get_int(cfg, "utsm", "max_segments", UTSM_MAX_SEGMENTS_DEFAULT);
    g_utsm_max_capabilities = (u32)ini_get_int(cfg, "utsm", "max_capabilities", UTSM_MAX_CAPABILITIES_DEFAULT);
    g_utsm_max_pckc_keys    = (u32)ini_get_int(cfg, "utsm", "max_pckc_keys", UTSM_MAX_PCKC_KEYS_DEFAULT);
    g_utsm_arena_size       = (u64)ini_get_int(cfg, "utsm", "arena_size_mb", 16) * 1024 * 1024;
    g_utsm_dirty_shard_pages = (u64)ini_get_int(cfg, "utsm", "dirty_shard_pages", UTSM_DIRTY_SHARD_PAGES_DEFAULT);

    log_info("[UTSM] FUCK config applied");
    log_hex64("[UTSM] max_segments=", g_utsm_max_segments);
    log_hex64("[UTSM] max_pckc_keys=", g_utsm_max_pckc_keys);
    log_hex64("[UTSM] dirty_shard_pages=", g_utsm_dirty_shard_pages);
}

/* ---- 全局启动配置（从 FUCK 文件读取） ---- */
static ini_config g_boot_cfg;
static int g_boot_cfg_loaded = 0;

/* 从 Limine boot module 读取 FUCK 配置。
 * cmdline="fuck:config" 的模块即为 FUCK 配置文件。
 * 返回 0 成功，负数失败。 */
static int utsm_load_fuck_config(ini_config *cfg) {
    struct limine_module_response *rsp = g_module_request.response;
    if (!rsp) return -1;
    for (u64 i = 0; i < rsp->module_count; i++) {
        struct limine_file *f = rsp->modules[i];
        if (f && f->cmdline) {
            /* 比较 cmdline 是否为 "fuck:config" */
            const char *c = f->cmdline;
            const char *tag = "fuck:config";
            int match = 1;
            for (int j = 0; tag[j]; j++) {
                if (c[j] != tag[j]) { match = 0; break; }
            }
            if (match && c[11] == 0) {
                int rc = ini_parse((const char *)f->address, (u32)f->size, cfg);
                if (rc == 0) {
                    log_info("[UTSM] FUCK config loaded from boot module");
                    return 0;
                }
            }
        }
    }
    log_warn("[UTSM] FUCK boot module not found");
    return -2;
}

void kernel_main(void) {
    /* ---- 插桩: 启动总时间戳 ---- */
    INSTR_TS_DECL(boot_total);
    INSTR_TS_BEGIN(boot_total);

    serial_init();
    log_info("[UTSM] boot");

    /* ==== 真机硬件特征早期检测 (在任何初始化之前) ====
     * 检测项目:
     *   1. CPU vendor (GenuineIntel / AuthenticAMD / etc.)
     *   2. 最大物理地址宽度 (CPUID.80000008H:EAX[7:0])
     *   3. LA57 (5-level paging) 是否激活 → 拒绝启动
     *   4. CPU 核心数 (CPUID.0BH 或 CPUID.01H)
     *   5. PCIe MCFG 可用性 (后续由 ACPI 驱动检测)
     *
     * **QEMU 无法测试但真机必需的代码路径**:
     *   - LA57 拒绝: QEMU 默认不启用 5 级分页
     *   - 物理地址宽度 > 48: 只有大内存真机才有
     *   - AMD CPU vendor: QEMU 默认 Intel, 实机可能是 AMD
     *   - 多核: QEMU -smp 可以模拟, 但真机拓扑更复杂
     */
    {
        /* CPU vendor */
        u32 a, b, c, d;
        __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0), "c"(0));
        char vendor[13];
        vendor[0]  = (char)(b & 0xff);
        vendor[1]  = (char)((b >> 8) & 0xff);
        vendor[2]  = (char)((b >> 16) & 0xff);
        vendor[3]  = (char)((b >> 24) & 0xff);
        vendor[4]  = (char)(d & 0xff);
        vendor[5]  = (char)((d >> 8) & 0xff);
        vendor[6]  = (char)((d >> 16) & 0xff);
        vendor[7]  = (char)((d >> 24) & 0xff);
        vendor[8]  = (char)(c & 0xff);
        vendor[9]  = (char)((c >> 8) & 0xff);
        vendor[10] = (char)((c >> 16) & 0xff);
        vendor[11] = (char)((c >> 24) & 0xff);
        vendor[12] = 0;
        log_info("[UTSM] CPU vendor: ");
        log_info(vendor);

        /* Max physical address width */
        __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0x80000000), "c"(0));
        if (a >= 0x80000008) {
            __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0x80000008), "c"(0));
            u32 phys_bits = a & 0xff;
            u32 virt_bits = (a >> 8) & 0xff;
            log_hex64("[UTSM] MaxPhysAddr bits=", phys_bits);
            log_hex64("[UTSM] MaxVirtAddr bits=", virt_bits);
        }

        /* LA57 early detection — 在 CR4 读之前先通过 CPUID 检查是否支持 */
        __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(7), "c"(0));
        if (c & (1u << 16)) {
            log_warn("[UTSM] CPU supports LA57 (5-level paging)");
            /* 检查 CR4 是否实际启用了 LA57 */
            u64 cr4;
            __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
            if (cr4 & (1ULL << 12)) {
                log_error("[UTSM] LA57 is ACTIVE — this kernel requires 4-level paging");
                log_error("[UTSM] Refusing to boot. Disable LA57 in firmware settings.");
                arch_halt_forever();
            }
        }

        /* CPU core count — CPUID leaf 0BH (Intel Extended Topology) */
        __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0), "c"(0));
        if (a >= 0x0B) {
            /* 尝试 level 0 获取 SMT 核心数 */
            u32 level_type = 0, cores = 0;
            __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0x0B), "c"(0));
            level_type = (c >> 8) & 0xff;
            cores = b & 0xffff;
            if (level_type == 1 && cores > 0) {
                log_hex64("[UTSM] SMT threads=", cores);
            }
            __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0x0B), "c"(1));
            level_type = (c >> 8) & 0xff;
            cores = b & 0xffff;
            if (level_type == 2 && cores > 0) {
                log_hex64("[UTSM] logical processors=", cores);
            }
        } else {
            /* fallback: CPUID.01H EBX[23:16] */
            __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
            u32 lc = (b >> 16) & 0xff;
            log_hex64("[UTSM] logical processors (legacy)=", lc);
        }

        /* ---- 插桩: CPU 检测完成，记录 vendor ---- */
        INSTR_PROBE(BOOT, (u64)vendor[0] | ((u64)vendor[1] << 8), 0, 0, 0);
    }

    /* ---- 插桩: IDT 初始化耗时 ---- */
    INSTR_TS_DECL(ts_idt);
    INSTR_TS_BEGIN(ts_idt);
    idt_init();
    INSTR_TS_END(ts_idt, "idt_init");

    /* === 读取 FUCK 配置（通过 Limine boot module 预加载） === */
    g_boot_cfg_loaded = (utsm_load_fuck_config(&g_boot_cfg) == 0);
    ini_config *cfg = g_boot_cfg_loaded ? &g_boot_cfg : NULL;

    /* ---- 插桩: FUCK 配置加载完成 ---- */
    INSTR_PROBE(BOOT, (u64)g_boot_cfg_loaded, 0, 0, 0);

    /* === 初始化插桩系统（在 arena_init 之前，但需要 FUCK 配置） === */
    /* 注意: probe_init 需要 arena，所以 instr_init 必须在 arena_init 之后。
     *       此处先设置 g_instr_enabled 等全局变量，probe_init 延后到 arena 后。
     *       实际上 instr_init 内部会调用 probe_init，而 probe_init 需要 kmem_alloc。
     *       所以我们将 instr_init 移到 arena_init 之后。*/

    /* === 应用 UTSM 算法参数到运行期全局变量 === */
    utsm_apply_config(cfg);

    log_info("[UTSM] arena init begin");
    INSTR_TS_DECL(ts_arena);
    INSTR_TS_BEGIN(ts_arena);
    arena_init();
    INSTR_TS_END(ts_arena, "arena_init");
    log_info("[UTSM] arena init ok");

    /* ---- 插桩: arena 就绪后初始化插桩系统（含探测缓冲分配） ---- */
    instr_init(cfg);

    log_info("[UTSM] dma init begin");
    INSTR_TS_DECL(ts_dma);
    INSTR_TS_BEGIN(ts_dma);
    dma_init();
    INSTR_TS_END(ts_dma, "dma_init");
    log_info("[UTSM] dma init ok");

    log_info("[UTSM] net registry init begin");
    INSTR_TS_DECL(ts_net);
    INSTR_TS_BEGIN(ts_net);
    net_init();
    INSTR_TS_END(ts_net, "net_init");
    log_info("[UTSM] net registry init ok");

    log_info("[UTSM] drr init begin");
    INSTR_TS_DECL(ts_drr);
    INSTR_TS_BEGIN(ts_drr);
    drr_init();
    INSTR_TS_END(ts_drr, "drr_init");
    log_info("[UTSM] drr init ok");

    log_info("[UTSM] core init begin");
    INSTR_TS_DECL(ts_utsm);
    INSTR_TS_BEGIN(ts_utsm);
    utsm_init();
    INSTR_TS_END(ts_utsm, "utsm_init");
    log_info("[UTSM] init ok");

    INSTR_TS_DECL(ts_dkm);
    INSTR_TS_BEGIN(ts_dkm);
    dkm_init();
    dkm_fill_platform_info();
    INSTR_TS_END(ts_dkm, "dkm_init");

    /* === 驱动加载 — 按 [drivers] 区过滤 === */
    INSTR_TS_DECL(ts_dsm);
    INSTR_TS_BEGIN(ts_dsm);
    dsm_load_by_manifest_ex(cfg);
    INSTR_TS_END(ts_dsm, "dsm_load_manifest");

    /* === selftest — 可通过 FUCK [boot] selftest=0 跳过 === */
    int run_selftest = ini_get_bool(cfg, "boot", "selftest", 1);
    if (run_selftest) {
        INSTR_TS_DECL(ts_self);
        INSTR_TS_BEGIN(ts_self);
        int result = utsm_selftest_run();
        INSTR_TS_END(ts_self, "selftest");
        if (result == 0) {
            log_info("[UTSM] SELFTEST PASS");
        } else {
            log_error("[UTSM] SELFTEST FAIL");
            arch_halt_forever();
        }
    } else {
        log_info("[UTSM] selftest skipped by FUCK config");
    }

    /* === VMM — 可通过 FUCK [boot] vmm=0 跳过 === */
    int run_vmm = ini_get_bool(cfg, "boot", "vmm", 1);
    if (run_vmm && vmm_init() == 0) {
        INSTR_TS_DECL(ts_vmm);
        INSTR_TS_BEGIN(ts_vmm);
        log_info("[UTSM] VMM init ok");
        int vmm_st = vmm_self_test();
        if (vmm_st == 0) {
            INSTR_TS_END(ts_vmm, "vmm_init+selftest");
            log_info("[UTSM] VMM self-test PASS");

            /* === Linux guest — 可通过 FUCK [boot] linux_guest=0 跳过 === */
            int run_linux = ini_get_bool(cfg, "boot", "linux_guest", 1);
            if (run_linux && linux_loader_init() == 0) {
                INSTR_TS_DECL(ts_linux);
                INSTR_TS_BEGIN(ts_linux);
                log_info("[UTSM] Linux loader init ok");

                if (ipc_shm_init() == 0) {
                    log_info("[UTSM] IPC shm init ok");
                } else {
                    log_error("[UTSM] IPC shm init failed (non-fatal)");
                }

                int lin_st = linux_launch();
                INSTR_TS_END(ts_linux, "linux_launch");
                if (lin_st == 0) {
                    log_info("[UTSM] Linux guest parked (daemon ready)");
                    linux_compat_init();
                } else {
                    log_error("[UTSM] Linux launch failed");
                    log_hex64("[UTSM] Linux st=", (u64)(i64)lin_st);
                }
            } else {
                log_warn("[UTSM] Linux loader unavailable (no bzImage or disabled)");
            }
        } else {
            INSTR_TS_END(ts_vmm, "vmm_init+selftest");
            log_error("[UTSM] VMM self-test FAIL");
            log_hex64("[UTSM] VMM st=", (u64)(i64)vmm_st);
        }
    } else {
        log_warn("[UTSM] VMM unavailable (VMX not supported or disabled by FUCK)");
    }

    /* ---- 插桩: DSK 加载跳转前记录 ---- */
    INSTR_PROBE(DSK0, 0, 0, 0, 0);

    if (dsk_load_and_jump() != 0) {
        log_error("[UTSM] DSK jump failed");
    }

    /* ---- 插桩: 启动总耗时（仅在 DSK 跳转失败时可见） ---- */
    INSTR_TS_END(boot_total, "boot_total");

    arch_halt_forever();
}
