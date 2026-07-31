#ifndef UTSM_DKM_H
#define UTSM_DKM_H

#include <utsm/types.h>
#include <utsm/dma.h>
#include <utsm/block.h>
#include <utsm/net.h>
#include <utsm/paging.h>

#define DKM_DRIVER_MAGIC 0x444B4D31u
#define DKM_ABI_VERSION  1u
#define DKM_KERNEL_API_VERSION 1u

#define DKM_CLASS_PLATFORM  1u
#define DKM_CLASS_BUS       2u
#define DKM_CLASS_INTERRUPT 3u
#define DKM_CLASS_TIMER     4u
#define DKM_CLASS_CONSOLE   5u
#define DKM_CLASS_STORAGE   6u
#define DKM_CLASS_FS        7u
#define DKM_CLASS_NET       8u
#define DKM_CLASS_INPUT     9u
#define DKM_CLASS_GPU       10u
#define DKM_CLASS_MISC      11u

#define DKM_F_REQUIRED        (1u << 0)
#define DKM_F_BOOT_MODULE     (1u << 1)
#define DKM_F_NO_UNLOAD       (1u << 2)
#define DKM_F_STRONG_RECOVERY (1u << 3)
#define DKM_F_DMA_REQUIRED    (1u << 4)
#define DKM_F_EARLY_LOG       (1u << 5)

struct dkm_kernel_api;
struct dkm_driver_handle;

typedef struct dkm_driver_desc {
    u32 magic;
    u16 abi_version;
    u16 desc_size;

    const char *name;
    const char *version;
    const char *vendor;

    u32 driver_class;
    u32 stage;
    u32 flags;
    u32 priority;

    const char *const *depends;
    u32 depends_count;

    const char *const *provides;
    u32 provides_count;

    u64 min_kernel_abi;
    u64 feature_bits;

    u64 reserved0;
    u64 reserved1;
} dkm_driver_desc;

typedef int (*dkm_driver_init_fn)(const struct dkm_kernel_api *api, struct dkm_driver_handle *handle);
typedef int (*dkm_driver_exit_fn)(struct dkm_driver_handle *handle);

typedef struct dkm_log_api {
    void (*info)(const char *msg);
    void (*warn)(const char *msg);
    void (*error)(const char *msg);
    void (*panic)(const char *msg);
} dkm_log_api;

typedef struct dkm_kernel_api {
    u32 version;
    u32 size;
    u64 feature_bits;

    const dkm_log_api *log;
    const void *mem;
    const void *utsm;
    const void *irq;
    const void *pci;
    const dkm_dma_api *dma;
    const void *vfs;
    const dkm_net_api *net;
    const void *timer;
    const void *drr;

    const void *rsdp_address;   /* ACPI RSDP physical address */
    const void *fb_address;     /* framebuffer base */
    u64 fb_width;
    u64 fb_height;
    u64 fb_pitch;
    u16 fb_bpp;
    const void *boot_modules_response;  /* limine_module_response* */
    int (*irq_register)(u8 irq, void *handler);  /* kernel irq_register */
    u64 hhdm_offset;                               /* physical → virtual */
    const dkm_block_api *block;                    /* block provider registry */
    const dkm_mmio_api *mmio;                      /* 页表/高位 MMIO 映射服务（尾部追加，ABI 兼容） */
    /* —— 新增字段只允许追加在末尾（DSK 以 api+0xA8 硬偏移读取 block） —— */
    void *(*mm_map_mmio)(u64 phys, u64 size);      /* MMIO 独立窗口映射（PCD|PWT），返回虚拟地址，失败返回 0 */
    void (*mm_unmap_mmio)(void *virt, u64 size);   /* 解除 MMIO 独立窗口映射 */
} dkm_kernel_api;

typedef enum dkm_driver_state {
    DKM_STATE_DISCOVERED = 0,
    DKM_STATE_ABI_CHECKED,
    DKM_STATE_INITING,
    DKM_STATE_ACTIVE,
    DKM_STATE_FAILED
} dkm_driver_state;

typedef struct dkm_driver_handle {
    const dkm_driver_desc *desc;
    dkm_driver_state state;
    i32 init_status;
    u32 load_stage;
    u32 flags;
} dkm_driver_handle;

typedef struct dkm_builtin_driver {
    const dkm_driver_desc *desc;
    dkm_driver_init_fn init;
    dkm_driver_exit_fn exit;
} dkm_builtin_driver;

void dkm_init(void);
int dkm_load_builtin(const dkm_builtin_driver *driver);
void dkm_scan_boot_modules(void);
void dsm_load_by_manifest(void);
void dkm_fill_platform_info(void);
const dkm_kernel_api *dkm_get_kernel_api(void);

/* exposed for manifest parser */
struct dkm_symbol_scan {
    const void *driver_desc;
    const void *driver_init;
    const void *driver_exit;
};
int dkm_check_elf64(const void *address, u64 size);
int dkm_scan_symbols(const void *address, u64 size, struct dkm_symbol_scan *out);
int dkm_load_elf_rel(const void *address, u64 size, const struct dkm_symbol_scan *symbols);

extern const dkm_builtin_driver dkm_builtin_console_early;

#endif
