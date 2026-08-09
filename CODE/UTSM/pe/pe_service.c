/* pe_service.c — PE 兼容层服务装配.
 *
 * 将 pe_loader + pe_shim + x86emu32 整合为统一的 pe_service,
 * 通过 pe_get_service() 暴露给 DSK/cmd.elf(经 dsk_boot_context.reserved[4]).
 *
 * 执行策略:
 *   PE32+ (64位):pe_load_image 加载(actual_base 重定位),
 *                 IAT 填 ms_abi shim 函数地址,Ring0 原生执行入口.
 *                 ExitProcess 经 __builtin_longjmp 跳回 pe_service_run.
 *   PE32  (32位):直接以 preferred_base 加载到一块含栈/堆的大缓冲区,
 *                 无需重定位.IAT 填合成地址(0x00010000 | shim_idx),
 *                 x86emu32 解释执行,CALL 合成地址时拦截分发到 shim32.
 */

#include "pe_loader.h"
#include "pe_shim.h"
#include "pe_dll_manager.h"
#include "x86emu32.h"
#include "../include/utsm/pe.h"
#include <utsm/arena.h>
#include <utsm/idt.h>
#include <utsm/log.h>
#include <utsm/types.h>

/* 32位 PE 用的栈与堆 */
#define PE32_STACK_SIZE  (64 * 1024)   /* 64KB 栈 */
#define PE32_HEAP_SIZE   (256 * 1024)  /* 256KB 堆 */
#define PE32_MAX_IAT     256

/* 合成 shim 地址范围(与 pe_loader.c 一致) */
#define PE32_SHIM_ADDR_BASE 0x00010000u
#define PE32_SHIM_UNIMPL    0x0001FFFFu

/* 前向声明(服务表引用) */
int  pe_service_load(const void *pe_data, u64 size, pe_image_info *out);
int  pe_service_run(const void *pe_data, u64 size, const char *cmdline, u64 *exit_code);
void pe_service_unload(pe_image_info *info);
int  pe_service_run_windowed(const void *pe_data, u64 size, const char *cmdline,
                             u64 *exit_code, const pe_window_host *host);

/* 全局服务表实例（P5：ABI 尾部追加 run_windowed/inject_*，同镜像一致性
 * 假设见 pe.h；旧调用方按前 4 字段使用不受影响） */
static const pe_service g_pe_service = {
    PE_SERVICE_MAGIC,
    pe_service_load,
    pe_service_run,
    pe_service_unload,
    pe_service_run_windowed,
    pe_shim_inject_scancode,
    pe_shim_inject_pointer,
    pe_shim_inject_input,
};

const pe_service *pe_get_service(void) {
    return &g_pe_service;
}

/* ===== 加载(不执行)===== */
int pe_service_load(const void *pe_data, u64 size, pe_image_info *out) {
    if (!pe_data || !out) return -1;
    return pe_load_image(pe_data, size, out);
}

void pe_service_unload(pe_image_info *info) {
    pe_unload_image(info);
}

/* ===== PE32 专用加载器(加载到指定缓冲区,base=preferred_base,无重定位)===== */
typedef struct {
    u8 *mem;                /* 大缓冲区起始 */
    u32 mem_size;           /* 缓冲区总大小 */
    u32 image_base;         /* = preferred_base */
    u32 image_size;
    u32 entry_point;        /* 绝对入口 = image_base + entry_rva */
    pe_iat_entry iat[PE32_MAX_IAT];
    int iat_count;
} pe32_interp_image;

static int pe32_load_into(const void *pe_data, u64 size, pe32_interp_image *img) {
    const u8 *base = (const u8 *)pe_data;
    if (size < sizeof(image_dos_header)) return -1;
    const image_dos_header *dos = (const image_dos_header *)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return -2;
    if ((u64)dos->e_lfanew + 4 + sizeof(image_file_header) > size) return -3;

    const u8 *nt = base + dos->e_lfanew;
    if (*(const u32 *)nt != IMAGE_NT_SIGNATURE) return -4;
    const image_file_header *fh = (const image_file_header *)(nt + 4);
    const void *opt = (const void *)(nt + 4 + sizeof(image_file_header));
    u16 magic = *(const u16 *)opt;
    if (magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) return -5;  /* 仅 PE32 */

    const image_optional_header32 *oh = (const image_optional_header32 *)opt;
    u32 preferred_base = oh->image_base;
    u32 image_size = oh->size_of_image;
    u32 entry_rva = oh->address_of_entry_point;
    if (image_size == 0 || image_size > 64 * 1024 * 1024) return -6;
    if (preferred_base > 0x10000000u) return -7;  /* 解释器地址空间限制 */

    int nsec = fh->number_of_sections;
    if (nsec <= 0 || nsec > 96) return -8;
    const image_section_header *sections = (const image_section_header *)
        ((const u8 *)opt + fh->size_of_optional_header);

    /* 分配大缓冲区:[0 .. preferred_base + image_size + stack + heap] */
    u32 total = preferred_base + image_size + PE32_STACK_SIZE + PE32_HEAP_SIZE;
    u8 *mem = (u8 *)kmem_alloc_aligned((u64)total, 0x1000);
    if (!mem) return -9;
    for (u32 i = 0; i < total; i++) mem[i] = 0;

    /* 复制 headers */
    u32 hdr_copy = oh->size_of_headers;
    if (hdr_copy > size) hdr_copy = (u32)size;
    if (hdr_copy > image_size) hdr_copy = image_size;
    u8 *img_start = mem + preferred_base;
    for (u32 i = 0; i < hdr_copy; i++) img_start[i] = base[i];

    /* 复制各节区 */
    for (int i = 0; i < nsec; i++) {
        u32 va = sections[i].virtual_address;
        u32 raw_size = sections[i].size_of_raw_data;
        u32 raw_off = sections[i].pointer_to_raw_data;
        if (va >= image_size) continue;
        u32 copy_size = raw_size;
        if (va + copy_size > image_size) copy_size = image_size - va;
        if ((u64)raw_off + copy_size > size) copy_size = (u32)(size - raw_off);
        for (u32 j = 0; j < copy_size; j++) img_start[va + j] = base[raw_off + j];
    }

    /* 解析 imports,填充 IAT(合成地址) */
    img->iat_count = 0;
    const image_data_directory *import_dir = &oh->data_directory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (import_dir->virtual_address != 0 && import_dir->size != 0) {
        /* RVA->offset 辅助 */
        for (int dll_idx = 0; ; dll_idx++) {
            u32 imp_rva = import_dir->virtual_address + (u32)dll_idx * sizeof(image_import_descriptor);
            /* imp_rva 在 image 内 */
            if (imp_rva + sizeof(image_import_descriptor) > image_size) break;
            const image_import_descriptor *desc = (const image_import_descriptor *)(img_start + imp_rva);
            if (desc->name == 0 && desc->first_thunk == 0) break;

            /* DLL 名 */
            u32 name_rva = desc->name;
            const char *dll_name = 0;
            if (name_rva < image_size) dll_name = (const char *)(img_start + name_rva);

            u32 iat_rva = desc->first_thunk;
            u32 int_rva = desc->original_first_thunk;
            if (int_rva == 0) int_rva = iat_rva;

            for (int fn_idx = 0; ; fn_idx++) {
                u32 int_entry_rva = int_rva + (u32)fn_idx * 4;
                if (int_entry_rva + 4 > image_size) break;
                u32 thunk = *(const u32 *)(img_start + int_entry_rva);
                if (thunk == 0) break;

                const char *func_name = 0;
                if (thunk & 0x80000000u) {
                    func_name = "#ordinal";
                } else {
                    u32 hint_rva = thunk;
                    if (hint_rva + 2 < image_size) {
                        func_name = (const char *)(img_start + hint_rva + 2);
                    }
                }

                int shim_idx = -1;
                if (func_name && func_name[0] != '#') {
                    shim_idx = pe_shim_lookup(dll_name, func_name);
                }

                u32 synth_addr;
                if (shim_idx >= 0) {
                    synth_addr = PE32_SHIM_ADDR_BASE | (u32)(shim_idx & 0xFFFF);
                } else {
                    synth_addr = PE32_SHIM_UNIMPL;
                }

                u32 iat_entry_rva = iat_rva + (u32)fn_idx * 4;
                if (iat_entry_rva + 4 <= image_size) {
                    *(u32 *)(img_start + iat_entry_rva) = synth_addr;
                }

                if (img->iat_count < PE32_MAX_IAT) {
                    img->iat[img->iat_count].iat_rva = iat_entry_rva;
                    img->iat[img->iat_count].shim_index = shim_idx;
                    img->iat[img->iat_count].target_addr = synth_addr;
                    img->iat_count++;
                }

                if (shim_idx < 0 && func_name) {
                    log_info("[PE32] unimplemented import");
                    if (dll_name) serial_write(dll_name);
                    serial_write("!");
                    serial_write(func_name);
                    serial_write("\n");
                }
            }
        }
    }

    img->mem = mem;
    img->mem_size = total;
    img->image_base = preferred_base;
    img->image_size = image_size;
    img->entry_point = preferred_base + entry_rva;
    return 0;
}

/* ===== SSE/SSE2 启用 =====
 * UTSM 自身用 -mno-sse -msoft-float 编译(启动早期安全),但 PE32+ 原生
 * 代码(MSVC/LLVM x86_64 ABI)假定 SSE2 可用——浮点参数走 XMM0-3,
 * egui 的 f32/f64 运算也全部编译为 SSE 指令.跳转 PE 入口前必须:
 *   CR0.EM=0, CR0.TS=0, CR0.MP=1, CR0.NE=1
 *   CR4.OSFXSR=1, CR4.OSXMMEXCPT=1
 *   MXCSR=0x1F80(屏蔽全部异常、round-to-nearest,Windows 默认)
 * 内核 C 代码不带 SSE 指令,开启后对内核自身无副作用. */
static void pe_enable_sse(void) {
    u64 cr0, cr4;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~((1ULL << 2) | (1ULL << 3));  /* EM=0, TS=0 */
    cr0 |=  (1ULL << 1) | (1ULL << 5);    /* MP=1, NE=1 */
    __asm__ volatile("mov %0, %%cr0" :: "r"(cr0));
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1ULL << 9) | (1ULL << 10);    /* OSFXSR=1, OSXMMEXCPT=1 */
    __asm__ volatile("mov %0, %%cr4" :: "r"(cr4));
    __asm__ volatile("fninit");
    {
        u32 mxcsr = 0x1F80;
        __asm__ volatile("ldmxcsr %0" :: "m"(mxcsr));
    }
    log_info("[PE] SSE/SSE2 enabled for native PE32+ execution");
}

/* ===== PE 专用大栈 =====
 * 调用链 UTSM->DSK->cmd->pe_service 全部运行在 16KB 内核栈上,
 * Rust/egui 渲染(tessellate + 软光栅)栈需求远超 16KB,
 * 因此跳转 PE 入口前切换到 arena 分配的 1MB 独立栈.
 * 正常 ret 时由 trampoline 恢复旧 RSP;ExitProcess longjmp 路径
 * 由 setjmp 时保存的内核 RSP 恢复,两条路径都安全. */
#define PE_STACK_BYTES (1024ULL * 1024ULL)
static u64 g_pe_saved_rsp;

typedef void (*pe_entry_fn)(void);

static void pe_call_on_stack(pe_entry_fn fn, u64 stack_top) {
    __asm__ volatile(
        "mov %%rsp, %0\n\t"
        "mov %1, %%rsp\n\t"
        "call *%2\n\t"
        "mov %0, %%rsp\n\t"
        : "=m"(g_pe_saved_rsp)
        : "r"(stack_top), "r"(fn)
        : "memory", "cc",
          "rax", "rcx", "rdx", "rsi", "rdi",
          "r8", "r9", "r10", "r11");
}

/* PE32+ 大栈引导入口：先执行加载期延迟登记的真实 DLL DllMain(ATTACH)，
 * 再跳 PE 入口。此时 SSE 已启用且运行在 1MB PE 大栈上。 */
static pe_entry_fn g_pe_boot_entry;
static void pe_boot_with_dllmains(void) {
    pe_dll_run_pending_dllmains();
    g_pe_boot_entry();
}

/* ===== 运行 =====
 * 加载 + 执行 + 卸载.返回 0 成功,*exit_code 为进程退出码. */
int pe_service_run(const void *pe_data, u64 size, const char *cmdline, u64 *exit_code) {
    if (!pe_data || size == 0) return -1;

    /* 初始化 shim 状态(cmdline for GetCommandLineA) */
    pe_shim_init(cmdline ? cmdline : "");

    /* 先解析 PE 判断 32/64 位 */
    if (size < sizeof(image_dos_header)) return -2;
    const image_dos_header *dos = (const image_dos_header *)pe_data;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return -3;
    const u8 *nt_base = (const u8 *)pe_data + dos->e_lfanew;
    if (*(const u32 *)nt_base != IMAGE_NT_SIGNATURE) return -4;
    const void *opt = (const void *)(nt_base + 4 + sizeof(image_file_header));
    u16 opt_magic = *(const u16 *)opt;

    if (opt_magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        /* ===== PE32+ : Ring0 原生执行 ===== */
        pe_image_info info;
        /* SSE 必须在任何 PE/DLL 代码执行前启用：load 期会递归加载真实 DLL，
         * MSVC CRT 代码含 SSE 指令，后置启用曾在 DllMain 内触发 #UD。 */
        pe_enable_sse();
        /* DllMain 延迟到 1MB PE 大栈上执行（16KB 内核栈跑不动 CRT init） */
        pe_dll_defer_dllmain(1);
        int rc = pe_load_image(pe_data, size, &info);
        if (rc != 0) {
            pe_dll_defer_dllmain(0);
            log_error("[PE] PE32+ load failed");
            return -5;
        }
        log_info("[PE] running PE32+ natively");

        /* 恢复异常可见性:UTSM 交权 DSK 时 idt_halt_all() 将 IDT 全部
         * 替换为静默 halt stub,PE 原生执行期间的 CPU 异常(PF/GP/UD)
         * 会被无声吞掉。安装 PE 诊断 IDT 后异常走 idt_handler 打完整
         * vector/err/rip/CR2/寄存器日志再安全停机。 */
        idt_install_pe_diag();

        /* 独立 1MB 栈(内核栈仅 16KB,egui 渲染会溢出) */
        u8 *pe_stack = (u8 *)kmem_alloc_aligned(PE_STACK_BYTES, 4096);
        if (!pe_stack) {
            log_error("[PE] stack alloc failed");
            return -9;
        }
        u64 stack_top = (u64)pe_stack + PE_STACK_BYTES;  /* 4096 对齐 -> 16 对齐 */

        /* BUG-20260801-006: __builtin_setjmp 必须直接出现在
         * pe_service_run 本体内——该帧在 PE 执行全程存活,longjmp
         * 恢复合法.原 pe_shim_setup_exit() 包装函数 setjmp 后即返回,
         * longjmp 跨已销毁帧 = UB(实测从脏栈 pop 出 log_hex64 缓冲
         * 残片充当 R12-R15/RBP → 野指针 call → 三重故障). */
        u64 code = 0;
        if (__builtin_setjmp(pe_shim_exit_jmpbuf()) == 0) {
            log_hex64("[PE] entry=", (u64)info.entry_point);
            log_hex64("[PE] stack_top=", stack_top);
            g_pe_boot_entry = (pe_entry_fn)info.entry_point;
            pe_call_on_stack(pe_boot_with_dllmains, stack_top);
            code = 0;
        } else {
            code = pe_shim_get_exit_code();
            log_hex64("[PE] ExitProcess code=", code);
        }

        pe_unload_image(&info);
        if (exit_code) *exit_code = code;
        return 0;

    } else if (opt_magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
        /* ===== PE32 : x86-32 解释器执行 ===== */
        pe32_interp_image img;
        int rc = pe32_load_into(pe_data, size, &img);
        if (rc != 0) {
            log_error("[PE] PE32 interp load failed");
            log_hex64("[PE] rc=", (u64)(i64)rc);
            return -6;
        }
        log_info("[PE] running PE32 via x86emu32");

        u32 stack_top = img.image_base + img.image_size + PE32_STACK_SIZE - 16;

        x86emu_state emu;
        int irc = x86emu32_init(&emu, img.mem, img.mem_size,
                                img.image_base, img.image_size,
                                img.iat, img.iat_count, stack_top);
        if (irc != 0) {
            log_error("[PE32] emu init failed");
            return -7;
        }
        emu.eip = img.entry_point;

        /* BUG-20260801-007: fn64 回退 shim 收到的指针参数是 32 位
         * guest VA,设置解释器内存基址使 shim 内 gp() 完成 guest→host
         * 转换;运行结束立即复位(原生 PE32+ 路径恒为 0,gp 恒等映射). */
        pe_shim_set_emu_base((u64)img.mem);

        /* BUG-20260801-006: 同 PE32+ 路径,setjmp 直接出现在本体内.
         * PE32 解释器路径同一 longjmp 机制,同样暴露在 UB 下. */
        u64 code = 0;
        if (__builtin_setjmp(pe_shim_exit_jmpbuf()) == 0) {
            int rrc = x86emu32_run(&emu, &code);
            if (rrc != 0) {
                log_error("[PE32] emulator error");
                code = 0xFFFFFFFFULL;
            }
            /* emu 字段只在未发生 longjmp 的正常返回路径读取
             * (setjmp 语义:longjmp 后被修改的自动变量值不确定) */
            if (emu.error && !emu.exited) {
                log_warn("[PE32] emulator stopped with error");
            }
        } else {
            code = pe_shim_get_exit_code();
            log_hex64("[PE32] ExitProcess code=", code);
        }
        pe_shim_set_emu_base(0);

        if (exit_code) *exit_code = code;
        return 0;
    }

    return -8;  /* 未知 PE magic */
}

/* ===== P5 窗口模式运行 =====
 * 与 pe_service_run 相同的加载/执行路径，区别仅在 shim 行为：
 * 渲染目标重定向到 host->surface，PS/2 轮询关闭，消息空转回调
 * host->pump()（desktop 帧）。host==NULL/surface==NULL 时等价 run。 */
int pe_service_run_windowed(const void *pe_data, u64 size, const char *cmdline,
                            u64 *exit_code, const pe_window_host *host) {
    if (host && host->surface) {
        pe_shim_set_window_host(host);
        log_info("[PE] run_windowed: surface mode");
    } else {
        pe_shim_set_window_host(0);
    }
    int rc = pe_service_run(pe_data, size, cmdline, exit_code);
    pe_shim_set_window_host(0);  /* 防止残留配置串到下一次 run */
    return rc;
}
