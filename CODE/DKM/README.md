# DKM 驱动模块系统

```text
DKM = Deshab Kernel Module
DSM = Driver Startup Manager
```

DKM 是 Deshab 内核的驱动模块系统，负责在内核启动时加载 `SYSTEM/driver` 下的 ELF64 `.drv` 驱动模块，并把驱动注册到内核服务体系中。

当前实现状态：CODE/UTSM 内核已接入完整 DKM/DSM 驱动加载系统。内置 `console_early` + manifest.json 解析 + Limine boot module 预加载 + 4 stage 分阶段加载。全部 14 个外部 `.drv` 已完成 ET_REL 装载全链路：ELF64 校验、符号扫描、section 拷贝到 arena、SHT_NOBITS 清零、R_X86_64_64/32/32S/PC32 重定位、driver_desc magic 校验、外部 driver_init 调用进入 ACTIVE。当前镜像还预加载 `driver/test.fat32` 作为 FAT32 驱动测试镜像。

Linux/Windows 已编译驱动不能直接作为 DKM 驱动使用。真实驱动必须按本文件定义的 DKM ABI 编译，导出 `driver_desc`、`driver_init`、`driver_exit`，并只通过 `dkm_kernel_api` 调用内核服务。

当前真实驱动进度：14/14。

```text
stage0: timer, apic, acpi, pci
stage1: console_fb, ahci, nvme, bootfs
stage2: vfs, devfs, fat32
stage3: ps2kbd, e1000, virtio_net
```

manifest 中 14 个外部 `.drv` 均已替换为真实 DKM 驱动并可加载执行。当前 IDT/PIC/IRQ 基础设施已在内核侧实现，`apic` 已完成 APIC discovery 并保留 PIC IRQ 路由，`ahci` 已完成 PCI/ABAR/HBA/port discovery，并通过 `kernel_api.dma.alloc_pages` 接入早期低位 DMA buffer，支持 SATA disk IDENTIFY 与 LBA0 READ 最小路径；识别 SATA disk 后可向 `kernel_api.block` 注册 `ahci0` block provider。`fat32` 已优先通过 block provider 读取 FAT32 镜像，无设备或 BPB 无效时回退 boot module。`nvme` 已完成 PCI discovery 并能识别 QEMU NVMe 控制器；由于当前 QEMU BAR0 位于 4G 以上，NVMe MMIO register 读取等待高位 PCI MMIO 映射能力。`ps2kbd` 和 `e1000` 已通过 `kernel_api.irq_register` 验证 IRQ handler 注册路径。`virtio_net` 已完成 PCI discovery 和 modern virtio capability 枚举，尚未进行 feature negotiation / virtqueue / RX-TX。

下一阶段 DKM/驱动路线：

```text
P0: 物理页分配器 + 页表/MMIO 映射接口
P1: DMA buffer/屏障约定 + AHCI/NVMe block 数据路径
P2: IDT 256 vectors + vector allocator + APIC/IOAPIC/MSI IRQ backend
P3: e1000 与 virtio_net RX/TX 数据路径
P4: FAT32 接入真实 block provider，替代 test.fat32 启动镜像
P5: 驱动 ABI 公共头、warning 清理、QEMU 场景自动化
```

---

## 1. 模块目标

DKM 的目标：

```text
1. 从 SYSTEM/driver/manifest.json 读取驱动清单。
2. 按 stage 分阶段加载驱动。
3. 支持 ELF64 .drv 模块加载和重定位。
4. 提供稳定的 driver_desc ABI。
5. 通过 dkm_kernel_api 向驱动暴露内核服务。
6. 支持 stage0/stage1 boot module 早期加载。
7. 将驱动内存纳入 UTSM。
8. 将驱动状态和恢复操作纳入 DRR。
```

---

## 2. 系统目录约定

打包为 IMG 后，`SYSTEM` 是系统根目录。

```text
SYSTEM/
    boot/
        utsm.elf
        limine.conf

    driver/
        manifest.json

        platform/
            timer.drv
            apic.drv
            acpi.drv

        bus/
            pci.drv

        block/
            ahci.drv
            nvme.drv

        fs/
            bootfs.drv
            vfs.drv
            fat32.drv
            devfs.drv

        net/
            e1000.drv
            virtio_net.drv

        input/
            ps2kbd.drv

        console/
            console_fb.drv
```

当前驱动清单：

```text
SYSTEM/driver/manifest.json
```

---

## 3. 启动阶段

DKM 使用分阶段加载。

```text
stage0: platform
    timer / apic / acpi / pci
    建立平台发现、中断、总线和基础计时能力。

stage1: boot
    console / block / bootfs
    建立启动日志、块设备和启动文件访问能力。

stage2: filesystem
    vfs / fat32 / devfs
    建立完整 VFS 和可挂载文件系统。

stage3: optional
    net / input / gpu
    加载网络、输入、图形等非启动关键驱动。
```

规则：

```text
1. stage0/stage1 可由 bootloader 预加载为 boot module。
2. stage2/stage3 默认从 SYSTEM/driver 加载。
3. required=true 的驱动失败时进入 DRR recovery 或 panic。
4. required=false 的驱动失败时标记 FAILED 并继续启动。
```

---

## 4. driver_desc ABI

每个 `.drv` 必须导出三个符号：

```text
driver_desc
driver_init
driver_exit
```

### 4.1 常量

```c
#define DKM_DRIVER_MAGIC 0x444B4D31u
#define DKM_ABI_VERSION  1
```

`0x444B4D31` 表示：

```text
DKM1
```

### 4.2 driver_desc 结构

```c
struct dkm_driver_desc {
    uint32_t magic;
    uint16_t abi_version;
    uint16_t desc_size;

    const char *name;
    const char *version;
    const char *vendor;

    uint32_t driver_class;
    uint32_t stage;
    uint32_t flags;
    uint32_t priority;

    const char *const *depends;
    uint32_t depends_count;

    const char *const *provides;
    uint32_t provides_count;

    uint64_t min_kernel_abi;
    uint64_t feature_bits;

    uint64_t reserved0;
    uint64_t reserved1;
};
```

### 4.3 driver_class

```text
DKM_CLASS_PLATFORM
DKM_CLASS_BUS
DKM_CLASS_INTERRUPT
DKM_CLASS_TIMER
DKM_CLASS_CONSOLE
DKM_CLASS_STORAGE
DKM_CLASS_FS
DKM_CLASS_NET
DKM_CLASS_INPUT
DKM_CLASS_GPU
DKM_CLASS_MISC
```

### 4.4 flags

```text
DKM_F_REQUIRED
    当前 stage 中失败则进入 DRR recovery 或 panic。

DKM_F_BOOT_MODULE
    可由 bootloader 预加载。

DKM_F_NO_UNLOAD
    不允许卸载。

DKM_F_STRONG_RECOVERY
    驱动 data/bss 段使用 UTSM 强恢复策略。

DKM_F_DMA_REQUIRED
    驱动需要 DMA API。

DKM_F_EARLY_LOG
    驱动 init 期间允许使用 early log。
```

---

## 5. 驱动入口

```c
int driver_init(const struct dkm_kernel_api *api,
                struct dkm_driver_handle *handle);

int driver_exit(struct dkm_driver_handle *handle);
```

约定：

```text
1. driver_init 返回 0 表示成功。
2. driver_init 返回负数表示失败。
3. driver_exit 仅用于非 DKM_F_NO_UNLOAD 驱动。
4. required 驱动失败会触发 DRR recovery 或 panic。
5. optional 驱动失败会被标记为 FAILED。
```

---

## 6. kernel_api ABI

驱动默认不直接依赖任意内核符号，而是通过 `dkm_kernel_api` 调用内核服务。

```c
#define DKM_KERNEL_API_VERSION 1

struct dkm_kernel_api {
    uint32_t version;
    uint32_t size;
    uint64_t feature_bits;

    const struct dkm_log_api *log;
    const void *mem;
    const void *utsm;
    const void *irq;
    const void *pci;
    const void *dma;
    const void *vfs;
    const void *net;
    const void *timer;
    const void *drr;

    const void *rsdp_address;
    const void *fb_address;
    uint64_t fb_width;
    uint64_t fb_height;
    uint64_t fb_pitch;
    uint16_t fb_bpp;
    const void *boot_modules_response;
    int (*irq_register)(uint8_t irq, void *handler);
    uint64_t hhdm_offset;
};
```

### 6.1 log API

```c
struct dkm_log_api {
    void (*info)(const char *msg);
    void (*warn)(const char *msg);
    void (*error)(const char *msg);
    void (*panic)(const char *msg);
};
```

### 6.2 memory API

```c
struct dkm_mem_api {
    void *(*alloc)(uint64_t size, uint32_t flags);
    void (*free)(void *ptr);

    void *(*alloc_pages)(uint32_t order, uint32_t flags);
    void (*free_pages)(void *ptr, uint32_t order);

    void *(*mmio_map)(uint64_t phys, uint64_t size, uint32_t flags);
    void (*mmio_unmap)(void *virt, uint64_t size);
};
```

### 6.3 UTSM API

```c
struct dkm_utsm_api {
    int (*create_segment)(uint64_t size, uint32_t flags, struct utsm_capability *out);
    int (*read)(struct utsm_capability cap, uint64_t off, void *dst, uint64_t len);
    int (*write)(struct utsm_capability cap, uint64_t off, const void *src, uint64_t len);
    int (*pin)(struct utsm_capability cap, uint64_t off, uint64_t len, void **window);
    int (*unpin)(void *window, uint32_t flags);
};
```

### 6.4 当前平台直通能力

当前内核已通过 `dkm_kernel_api` 直接暴露早期平台信息和 IRQ/HHDM 能力：

```text
rsdp_address              Limine RSDP 地址，用于 ACPI 驱动
fb_address/width/height   Limine framebuffer 信息，用于 console_fb
fb_pitch/fb_bpp           framebuffer 行跨度和像素格式
boot_modules_response     Limine module response，用于 bootfs/fat32 测试镜像
irq_register              注册 PIC IRQ0–15 handler，用于 ps2kbd/e1000
hhdm_offset               physical → virtual direct map，用于 PCI BAR MMIO
```

### 6.5 IRQ / PCI / DMA API

```c
struct dkm_irq_api {
    int (*register_irq)(uint32_t vector, int (*handler)(void *ctx), void *ctx, uint32_t flags);
    int (*unregister_irq)(uint32_t vector);
    void (*ack)(uint32_t vector);
};

struct dkm_pci_api {
    int (*scan)(int (*cb)(const struct pci_device *dev, void *ctx), void *ctx);
    int (*enable_device)(const struct pci_device *dev);
    uint64_t (*bar_phys)(const struct pci_device *dev, uint32_t bar);
    uint64_t (*bar_size)(const struct pci_device *dev, uint32_t bar);
};

struct dkm_dma_api {
    int (*alloc)(uint64_t size, uint32_t flags, struct dkm_dma_buffer *out);
    void (*free)(struct dkm_dma_buffer *buf);
    int (*sync_for_device)(struct dkm_dma_buffer *buf);
    int (*sync_for_cpu)(struct dkm_dma_buffer *buf);
};
```

---

## 7. ELF64 .drv Loader

第一版仅支持 x86_64 ELF64 小型内核模型。

### 7.1 必须支持的 relocation

```text
R_X86_64_64
    绝对 64-bit 地址重定位。

R_X86_64_RELATIVE
    base + addend。

R_X86_64_GLOB_DAT
    GOT 符号地址。

R_X86_64_JUMP_SLOT
    PLT 函数地址。
```

### 7.2 建议支持的 relocation

```text
R_X86_64_PC32
    32-bit PC-relative 调用。

R_X86_64_PLT32
    PLT-relative 调用。

R_X86_64_32 / R_X86_64_32S
    仅允许确认地址范围安全时使用。
```

### 7.3 第一版禁止

```text
TLS relocation
IFUNC
lazy binding
外部动态库依赖
用户态 libc 依赖
异常展开表强依赖
```

### 7.4 加载流程

```text
DKM_ELF_LOAD(path_or_boot_module)
    1. 读取 ELF header。
    2. 校验 ELFCLASS64 / EM_X86_64 / endian / ABI。
    3. 读取 program headers 或 section headers。
    4. 计算模块内存大小和对齐。
    5. 通过 UTSM 创建 code/data 段。
    6. 加载 PT_LOAD 或 SHF_ALLOC section。
    7. 清零 .bss。
    8. 解析 symbol table。
    9. 解析 relocation section。
    10. 解析内核导出符号和 dkm_kernel_api。
    11. 执行 relocation。
    12. 查找 driver_desc / driver_init / driver_exit。
    13. 设置 code 段 READ|EXEC|SEALED。
    14. 设置 data 段 READ|WRITE|STRONG_RECOVERY。
    15. 返回 module handle。
```

---

## 8. manifest.json 格式

路径：

```text
SYSTEM/driver/manifest.json
```

顶层结构：

```json
{
  "schema": "deshab.driver.manifest.v1",
  "root": "SYSTEM",
  "driverRoot": "SYSTEM/driver",
  "format": "elf64-drv",
  "abi": {},
  "policy": {},
  "stages": []
}
```

驱动项示例：

```json
{
  "name": "pci",
  "path": "driver/bus/pci.drv",
  "class": "bus",
  "required": true,
  "depends": ["irq"],
  "provides": ["pci"]
}
```

路径规则：

```text
manifest 中的 path 相对 SYSTEM 根目录。
SYSTEM/driver/manifest.json 内的 driver/bus/pci.drv 指向 SYSTEM/driver/bus/pci.drv。
stage0/stage1 驱动允许来自 boot module。
stage2/stage3 驱动默认从 SYSTEM/driver 加载。
```

---

## 9. 驱动加载状态机

正常加载：

```text
DISCOVERED
    -> QUEUED
    -> DEP_WAIT
    -> LOADING
    -> ELF_CHECKED
    -> MEMORY_ALLOCATED
    -> RELOCATED
    -> ABI_CHECKED
    -> INITING
    -> ACTIVE
```

失败状态：

```text
FAILED_ELF
FAILED_RELOCATION
FAILED_DEPENDENCY
FAILED_ABI
FAILED_INIT
FAILED_REQUIRED
```

卸载状态：

```text
ACTIVE
    -> QUIESCING
    -> EXITING
    -> UNLOADING
    -> UNLOADED
```

DRR 恢复状态：

```text
ACTIVE
    -> SUSPENDED_BY_DRR
    -> RECOVERING
    -> REINITING
    -> ACTIVE
```

---

## 10. DSM 加载策略

```text
DSM_LOAD_ALL()
    1. 读取 SYSTEM/driver/manifest.json。
    2. 校验 schema / abi / policy。
    3. 按 stage 顺序加载。
    4. 每个 stage 内按 priority 排序。
    5. 检查 depends 是否已由 provides 满足。
    6. 未满足依赖的驱动进入 DEP_WAIT。
    7. 每轮加载成功后更新 provides set。
    8. 一轮后仍 DEP_WAIT 的 required 驱动进入 FAILED_REQUIRED。
    9. optional 驱动失败只记录状态。
```

required 失败策略：

```text
stage0/stage1 required 失败：
    DRR report_fault -> boot fallback -> panic / system rollback

stage2 required 失败：
    DRR recovery -> fallback 到 bootfs 或只读模式

stage3 optional 失败：
    mark FAILED -> continue boot
```

---

## 11. stage0/stage1 boot module 方案

stage0/stage1 不应依赖普通 VFS。

bootloader 应预加载：

```text
SYSTEM/driver/manifest.json
SYSTEM/driver/platform/timer.drv
SYSTEM/driver/platform/acpi.drv
SYSTEM/driver/platform/apic.drv
SYSTEM/driver/bus/pci.drv
SYSTEM/driver/console/console_fb.drv
SYSTEM/driver/fs/bootfs.drv
SYSTEM/driver/test.fat32
```

可选预加载：

```text
SYSTEM/driver/block/ahci.drv
SYSTEM/driver/block/nvme.drv
SYSTEM/driver/fs/vfs.drv
SYSTEM/driver/fs/devfs.drv
SYSTEM/driver/fs/fat32.drv
SYSTEM/driver/net/e1000.drv
SYSTEM/driver/net/virtio_net.drv
SYSTEM/driver/input/ps2kbd.drv
```

boot module 表：

```c
struct dkm_boot_module {
    const char *path;
    void *base;
    uint64_t size;
    uint32_t type;
    uint32_t flags;
    uint64_t checksum;
};

struct dkm_boot_module_table {
    uint32_t count;
    struct dkm_boot_module modules[];
};
```

查找规则：

```text
DSM_LOAD_DRIVER(path)
    1. 如果 path 在 boot module table 中，直接从内存加载。
    2. 否则如果 VFS 已可用，从 SYSTEM/path 加载。
    3. 否则返回 BOOT_MODULE_NOT_FOUND。
```

---

## 12. 与 UTSM / DRR 的关系

### 12.1 UTSM 内存策略

```text
驱动 text：READ | EXEC | SEALED
驱动 rodata：READ | SEALED
驱动 data：READ | WRITE | STRONG_RECOVERY
驱动 bss：READ | WRITE | STRONG_RECOVERY
驱动 DMA：DMA | PINNED，必要时 CIPHERTEXT_DMA 或 NO_ENCRYPT
```

### 12.2 DRR 记录项

```text
driver name
stage
state
module memory range
provided services
recovery ops
init result
last fault
```

### 12.3 recovery ops

```c
struct dkm_driver_recovery_ops {
    int (*quiesce)(void *ctx);
    int (*reset)(void *ctx);
    int (*reinit)(void *ctx);
    int (*dump_state)(void *ctx, void *buf, uint64_t len);
};
```

DRR recovery 顺序：

```text
quiesce -> UTSM rollback -> device reset -> driver reinit -> resume
```

---

## 13. 第一版实现清单

```text
1. manifest.json parser
2. boot module table lookup
3. ELF64 header 校验
4. ELF64 section/program loading
5. relocation 处理
6. driver_desc 校验
7. kernel_api table 初始化
8. depends/provides resolver
9. staged loader
10. driver state table
11. DRR driver fault reporting
12. UTSM module memory allocation
```

---

## 14. 相关文件

```text
SYSTEM/driver/manifest.json
    驱动启动清单。

RE/驱动模块ABI设计.md
    DKM ABI 详细设计。

CODE/UTSM/README.md
    UTSM 与驱动加载的整合说明。
```
