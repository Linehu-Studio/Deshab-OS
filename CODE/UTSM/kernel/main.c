#include <utsm/log.h>
#include <utsm/arena.h>
#include <utsm/utsm.h>
#include <utsm/drr.h>
#include <utsm/sched_ext.h>
#include <utsm/dkm.h>
#include <utsm/idt.h>
#include <utsm/dsk.h>
#include <utsm/block.h>
#include <utsm/net.h>
#include <utsm/ipc_shm.h>
#include <utsm/linux_compat.h>
#include <utsm/xj380_loader.h>
#include <utsm/instr.h>
#include "ini_parser.h"
#include "../arch/x86_64/limine.h"
#include "../pe/pe_dll_manager.h"

void arch_halt_forever(void);

extern volatile struct limine_module_request g_module_request;

/* dkm/manifest.c: 带 FUCK [drivers] 过滤的 manifest 加载入口 */
void dsm_load_by_manifest_ex(const ini_config *cfg);
void dsm_load_named(const ini_config *cfg, const char *name);

/* dsk_loader.c: 按路径读取 FAT32 文件到 g_dsk_fat32_filedata 静态缓冲。
 * Phase 1 已支持 LFN 长文件名 + 子目录遍历。 */
extern int fat32_read_path(const char *path, u8 **out_data, u32 *out_size);

/* dsk_loader.c: 实机启动日志落盘。把日志缓冲写入 FAT32 文件：
 * SYSTEM/DESHAB64/DEV/BOOTLOG.TXT（DEV 缺失时回退根目录 BOOTLOG.TXT）。 */
extern int utsm_bootlog_write(const u8 *data, u32 size);

/* ---- 运行期配置全局变量（从 FUCK 文件初始化） ---- */
u32 g_utsm_max_segments     = UTSM_MAX_SEGMENTS_DEFAULT;
u32 g_utsm_max_capabilities = UTSM_MAX_CAPABILITIES_DEFAULT;
u32 g_utsm_max_pckc_keys    = UTSM_MAX_PCKC_KEYS_DEFAULT;
u64 g_utsm_arena_size       = UTSM_ARENA_SIZE_DEFAULT;
u64 g_utsm_dirty_shard_pages = UTSM_DIRTY_SHARD_PAGES_DEFAULT;

/* ---- 兼容层配置全局变量（[compat] 段，在所有使用它们的函数前定义） ---- */
char g_compat_lib_path[32]          = "lib";              /* FAT32 根目录下的库子目录名 */
char g_compat_linux_guest_path[64]  = "/usr/lib/deshab";  /* Linux guest 内库目录路径 */
int  g_compat_linux_lib_sync        = 1;                  /* 启动时推送 .so 到 Linux guest */
int  g_compat_pe_dll_search         = 1;                  /* PE 加载时从 FAT32 lib 搜索真实 DLL */
int  g_compat_vscode_install        = 1;                  /* 启动时安装 VSCode tarball 到 guest /opt */
int  g_sched_enabled_cfg            = 1;                  /* FUCK [sched] enable */

/* 将 FUCK [compat] 区的配置应用到兼容层全局变量。
 * 必须在 utsm_apply_config 之后调用。 */
static void utsm_apply_compat_config(const ini_config *cfg) {
    if (!cfg) return;
    const char *lib_path = ini_get(cfg, "compat", "lib_path");
    if (lib_path && lib_path[0]) {
        int i = 0;
        while (lib_path[i] && i + 1 < (int)sizeof(g_compat_lib_path)) {
            g_compat_lib_path[i] = lib_path[i];
            i++;
        }
        g_compat_lib_path[i] = 0;
    }
    const char *guest_path = ini_get(cfg, "compat", "linux_lib_guest_path");
    if (guest_path && guest_path[0]) {
        int i = 0;
        while (guest_path[i] && i + 1 < (int)sizeof(g_compat_linux_guest_path)) {
            g_compat_linux_guest_path[i] = guest_path[i];
            i++;
        }
        g_compat_linux_guest_path[i] = 0;
    }
    g_compat_linux_lib_sync = ini_get_bool(cfg, "compat", "linux_lib_sync", 1);
    g_compat_pe_dll_search  = ini_get_bool(cfg, "compat", "pe_dll_search", 1);
    g_compat_vscode_install = ini_get_bool(cfg, "compat", "vscode_install", 1);

    log_info("[UTSM] compat config applied");
    log_info("[UTSM] lib_path=");
    log_info(g_compat_lib_path);
    log_info("[UTSM] linux_guest_path=");
    log_info(g_compat_linux_guest_path);
    log_hex64("[UTSM] linux_lib_sync=", (u64)g_compat_linux_lib_sync);
    log_hex64("[UTSM] pe_dll_search=", (u64)g_compat_pe_dll_search);
    log_hex64("[UTSM] vscode_install=", (u64)g_compat_vscode_install);
}

/* DLL 文件读取回调：从 FAT32 根目录下 lib/ 子目录读取 DLL。
 * 路径构造成 "<g_compat_lib_path>/<name>"（lib 在 FAT32 根，不是 SYSTEM/lib！）。 */
static int dll_reader_from_fat32(const char *name, u8 **out_data, u32 *out_size) {
    if (!name || !out_data || !out_size) return -1;
    char path[128];
    int pl = 0;
    /* 拼接 g_compat_lib_path */
    int i = 0;
    while (g_compat_lib_path[i] && pl + 1 < (int)sizeof(path)) {
        path[pl++] = g_compat_lib_path[i++];
    }
    if (pl + 1 >= (int)sizeof(path)) return -1;
    path[pl++] = '/';
    /* 拼接 name */
    i = 0;
    while (name[i] && pl + 1 < (int)sizeof(path)) {
        path[pl++] = name[i++];
    }
    path[pl] = 0;
    return fat32_read_path(path, out_data, out_size);
}

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

    /* 兼容层配置（[compat] 段）— 复用同一 cfg 对象 */
    utsm_apply_compat_config(cfg);
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

/* === 实机日志持久化：把内存日志缓冲区写入 FAT32 文件 ===
 * 目标: SYSTEM/DESHAB64/DEV/BOOTLOG.TXT（DEV 目录不存在时回退根目录 BOOTLOG.TXT）。
 * 由 FUCK [boot] disk_log=1 启用。
 * 相比旧原始扇区方案（LBA 2-1000）：
 *   - 不再覆盖 GPT header/entries，不破坏 Limine 引导链
 *   - 日志以普通 FAT32 文件呈现，主机直接读取即可，无需 WinHex/dd
 * 日志超出 BOOTLOG_MAX_BYTES 时截断（128KB 足够覆盖完整启动链）。 */
#define BOOTLOG_MAX_BYTES (128u * 1024u)

static void disk_log_flush(void) {
    const dkm_kernel_api *api = dkm_get_kernel_api();
    if (!api || !api->block || !api->block->write || !api->block->device_count) {
        log_warn("[UTSM] disk_log: block device unavailable, skip");
        return;
    }
    if (api->block->device_count() == 0) {
        log_warn("[UTSM] disk_log: no block device, skip");
        return;
    }

    const char *buf = log_get_buffer();
    u32 len = log_get_length();
    if (len == 0) {
        log_warn("[UTSM] disk_log: log buffer empty, skip");
        return;
    }
    if (len > BOOTLOG_MAX_BYTES) len = BOOTLOG_MAX_BYTES;

    int rc = utsm_bootlog_write((const u8 *)buf, len);
    if (rc == 0) {
        log_info("[UTSM] disk_log: BOOTLOG.TXT written");
        log_hex64("[UTSM] disk_log: bytes=", len);
    } else {
        log_error("[UTSM] disk_log: FAT32 write failed");
        log_hex64("[UTSM] disk_log: rc=", (u64)(i64)rc);
    }
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

    /* D4: DKM 彩色日志总开关（FUCK [debug] color_log，默认开；
     * 颜色只走串口，BOOTLOG.TXT 保持纯文本） */
    log_color_enable(ini_get_bool(cfg, "debug", "color_log", 1));

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
    /* R3b: FUCK [utsm] drr_emergency_pool_kb / drr_recovery_log_size 接线 */
    drr_apply_config((u64)ini_get_int(cfg, "utsm", "drr_emergency_pool_kb", 0),
                     (u64)ini_get_int(cfg, "utsm", "drr_recovery_log_size", 0));
    drr_init();
    INSTR_TS_END(ts_drr, "drr_init");
    log_info("[UTSM] drr init ok");

    log_info("[UTSM] core init begin");
    INSTR_TS_DECL(ts_utsm);
    INSTR_TS_BEGIN(ts_utsm);
    utsm_init();
    INSTR_TS_END(ts_utsm, "utsm_init");
    log_info("[UTSM] init ok");

    /* === SAS-R0-PCQ 调度器：bootstrap 任务绑定（Phase 7） ===
     * 只做 sched_init + cpu_init + bootstrap TCB（绑定当前 rsp），
     * 不开中断、不碰定时器，对既有启动行为零影响。
     * 抢占在 selftest 之后、demo 窗口内才启用。 */
    {
        u32 s_tick_hz  = (u32)ini_get_int(cfg, "sched", "tick_hz", 100);
        u32 s_slice_ms = (u32)ini_get_int(cfg, "sched", "timeslice_ms", 10);
        u32 s_demo_ms  = (u32)ini_get_int(cfg, "sched", "demo_ms", 1000);
        int s_demo_fault = ini_get_bool(cfg, "sched", "demo_fault", 0);
        g_sched_enabled_cfg = ini_get_bool(cfg, "sched", "enable", 1);
        utsm_sched_config(s_tick_hz, s_slice_ms, s_demo_ms, s_demo_fault);
        log_hex64("[SCHED] cfg enable=", (u64)g_sched_enabled_cfg);
        log_hex64("[SCHED] cfg demo_ms=", s_demo_ms);
        if (utsm_sched_bootstrap_init() < 0) {
            log_warn("[UTSM] scheduler bootstrap failed (non-fatal)");
        }
    }

    INSTR_TS_DECL(ts_dkm);
    INSTR_TS_BEGIN(ts_dkm);
    dkm_init();
    dkm_fill_platform_info();
    INSTR_TS_END(ts_dkm, "dkm_init");

    /* === VMM — 可通过 FUCK [boot] vmm=0 跳过 ===
     * P8.4: Moved BEFORE DKM driver loading so VMM self-test runs even if
     * a stage2 driver (e.g. fat32) crashes under KVM nested VMX. */
    int run_vmm = ini_get_bool(cfg, "boot", "vmm", 1);
    if (run_vmm && vmm_init() == 0) {
        INSTR_TS_DECL(ts_vmm);
        INSTR_TS_BEGIN(ts_vmm);
        log_info("[UTSM] VMM init ok");
        int vmm_st = vmm_self_test();
        /* P8.4: On KVM nested VMX, self-test VMCS may fail but Linux guest VMCS
         * (configured separately) may still work. Force-continue regardless. */
        if (vmm_st != 0) {
            log_warn("[UTSM] VMM self-test FAIL, but continuing to Linux guest");
            vmm_st = 0;  /* force pass to continue to Linux guest */
        }
        if (vmm_st == 0) {
            INSTR_TS_END(ts_vmm, "vmm_init+selftest");
            log_info("[UTSM] VMM self-test PASS");

            /* Only pci+ahci before linux_launch. Loading apic/stage0 first
             * left the guest in an IDTR=0 VM-exit storm. Remaining DKM
             * still loads after the guest parks. */
            dsm_load_named(cfg, "pci");
            dsm_load_named(cfg, "ahci");

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
                    /* Phase 2: 启动时把 FAT32 lib 目录下 .so 全量推送到 Linux guest */
                    if (g_compat_linux_lib_sync) {
                        int lsrc = lxc_sync_lib_dir(g_compat_linux_guest_path);
                        if (lsrc == 0) log_info("[UTSM] lib sync ok");
                        else log_warn("[UTSM] lib sync skipped (non-fatal)");
                    }
                    /* VSCode Phase 5: tarball module 存在时装到 guest /opt/vscode
                     * （已安装则秒跳过；失败不阻断启动，桌面降级显示未安装） */
                    if (g_compat_vscode_install) {
                        int vsrc = lxc_vscode_install();
                        if (vsrc == 0) log_info("[UTSM] vscode ready");
                        else log_warn("[UTSM] vscode install skipped (non-fatal)");
                    }
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

        /* === OpenXJ380 guest — 可通过 FUCK [boot] xj380_guest=0 跳过 ===
         * 三内核架构路线 B：XJ380 作为第二 guest（与 Linux 并列）。
         * 需要 Limine boot module xj380.krl（OpenXJ380 构建产物 kernel.krl）。
         * guest 终止前 host 挂起；无模块/被禁用时优雅跳过。 */
        int run_xj380 = ini_get_bool(cfg, "boot", "xj380_guest", 1);
        if (run_xj380 && xj380_loader_init() == 0) {
            INSTR_TS_DECL(ts_xj380);
            INSTR_TS_BEGIN(ts_xj380);
            log_info("[UTSM] XJ380 loader init ok");
            int xj_st = xj380_launch();
            INSTR_TS_END(ts_xj380, "xj380_launch");
            if (xj_st == 0) {
                log_info("[UTSM] XJ380 guest terminated (host resumed)");
            } else {
                log_error("[UTSM] XJ380 launch failed");
                log_hex64("[UTSM] XJ380 st=", (u64)(i64)xj_st);
            }
        } else {
            log_warn("[UTSM] XJ380 loader unavailable (no kernel.krl or disabled)");
        }
    } else {
        log_warn("[UTSM] VMM unavailable (VMX not supported or disabled by FUCK)");
    }

    /* === 驱动加载 — 按 [drivers] 区过滤 === */
    INSTR_TS_DECL(ts_dsm);
    INSTR_TS_BEGIN(ts_dsm);
    dsm_load_by_manifest_ex(cfg);
    INSTR_TS_END(ts_dsm, "dsm_load_manifest");

    /* === F4: 分级启动自检 — FUCK [boot] selftest=0|1|2 ===
     * 0=关 / 1=快速（结构健全性） / 2=全量（+DMA 探针 + UTSM selftest） */
    {
        int self_level = ini_get_int(cfg, "boot", "selftest", 1);
        extern int boot_selftest_run(int level);
        extern void boot_panic_test(const ini_config *cfg);
        boot_selftest_run(self_level);
        boot_panic_test(cfg);
        if (self_level >= 2) {
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
        }
    }

    /* === SAS-R0-PCQ 调度器：demo 任务 + LAPIC tick 抢占启用（Phase 7） ===
     * 此刻 DKM 全部驱动加载完毕（timer.drv 的 PIT 校准已完成且此后无人
     * 再用 PIT）、VMM/Linux/XJ380 guest 已 park/terminate —— LAPIC timer
     * 接管 tick 源。demo 窗口内 bootstrap 被周期抢占，demo 任务交错运行。 */
    if (g_sched_enabled_cfg) {
        utsm_sched_demo_window();
    }

    /* ---- 插桩: DSK 加载跳转前记录 ---- */
    INSTR_PROBE(DSK0, 0, 0, 0, 0);

    /* Phase 3: 初始化 PE DLL 管理器（注册 FAT32 读取回调）。
     * DSK 跳转前 block provider 已就绪，fat32_read_path 可用。 */
    if (g_compat_pe_dll_search) {
        pe_dll_manager_init(dll_reader_from_fat32);
        log_info("[UTSM] PE DLL manager initialized (SYSTEM/lib)");
    }

    /* === 实机日志持久化：把启动日志写入 FAT32 文件 BOOTLOG.TXT === */
    {
        int disk_log = ini_get_bool(cfg, "boot", "disk_log", 0);
        if (disk_log) {
        disk_log_flush();
        }
    }

    /* === 调度器静默（Phase 7/8 收尾） ===
     * 停 LAPIC timer、注销 tick/异常 handler、杀 demo 任务——保证
     * dsk_load_and_jump 的 DSK 交接路径与无调度器时逐字节一致。 */
    utsm_sched_quiesce();

    if (dsk_load_and_jump() != 0) {
        log_error("[UTSM] DSK jump failed");
    }

    /* ---- 插桩: 启动总耗时（仅在 DSK 跳转失败时可见） ---- */
    INSTR_TS_END(boot_total, "boot_total");

    arch_halt_forever();/*I LOVE YOU*/
}
