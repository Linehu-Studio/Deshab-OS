# Deshab 驱动模块 ABI 设计

本文记录 Deshab 内核启动驱动模块系统的 ABI、manifest、ELF loader、加载状态机和 stage0/stage1 boot module 方案。

---

## 1. 总体定义

驱动模块系统命名：

```text
DKM = Deshab Kernel Module
DSM = Driver Startup Manager
```

当前规则：

```text
驱动目录：SYSTEM/driver
驱动清单：SYSTEM/driver/manifest.json
驱动格式：ELF64 .drv
加载模式：module loading
启动顺序：staged loading
内存模型：UTSM
```

---

## 2. driver_desc ABI

每个驱动必须导出三个符号：

```text
driver_desc
driver_init
driver_exit
```

### 2.1 driver_desc

```c
#define DKM_DRIVER_MAGIC 0x444B4D31u /* "DKM1" */
#define DKM_ABI_VERSION  1

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

### 2.2 driver_class

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

### 2.3 flags

```text
DKM_F_REQUIRED
    当前 stage 中失败则进入 DRR recovery 或 panic。

DKM_F_BOOT_MODULE
    可由 bootloader 预加载。

DKM_F_NO_UNLOAD
    不允许卸载。

DKM_F_STRONG_RECOVERY
    驱动 data 段使用 UTSM 强恢复策略。

DKM_F_DMA_REQUIRED
    驱动需要 DMA API。

DKM_F_EARLY_LOG
    驱动 init 期间允许使用 early log。
```

### 2.4 driver_init / driver_exit

```c
typedef int (*dkm_driver_init_fn)(const struct dkm_kernel_api *api,
                                  struct dkm_driver_handle *handle);

typedef int (*dkm_driver_exit_fn)(struct dkm_driver_handle *handle);
```

约定：

```text
driver_init 返回 0 表示成功。
driver_init 返回负数表示失败。
driver_exit 只用于非 NO_UNLOAD 驱动。
required 驱动 init 失败会触发 DRR recovery 或 panic。
optional 驱动 init 失败会被标记为 FAILED。
```

---

## 3. kernel_api ABI

驱动不直接依赖任意内核符号，默认通过 `dkm_kernel_api` 调用内核服务。

### 3.1 API 总结构

```c
#define DKM_KERNEL_API_VERSION 1

struct dkm_kernel_api {
    uint32_t version;
    uint32_t size;
    uint64_t feature_bits;

    const struct dkm_log_api *log;
    const struct dkm_mem_api *mem;
    const struct dkm_utsm_api *utsm;
    const struct dkm_irq_api *irq;
    const struct dkm_pci_api *pci;
    const struct dkm_dma_api *dma;
    const struct dkm_vfs_api *vfs;
    const struct dkm_net_api *net;
    const struct dkm_timer_api *timer;
    const struct dkm_drr_api *drr;
};
```

### 3.2 log API

```c
struct dkm_log_api {
    void (*info)(const char *fmt, ...);
    void (*warn)(const char *fmt, ...);
    void (*error)(const char *fmt, ...);
    void (*panic)(const char *fmt, ...);
};
```

### 3.3 memory API

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

### 3.4 UTSM API

```c
struct dkm_utsm_api {
    int (*create_segment)(uint64_t size, uint32_t flags, struct utsm_capability *out);
    int (*read)(struct utsm_capability cap, uint64_t off, void *dst, uint64_t len);
    int (*write)(struct utsm_capability cap, uint64_t off, const void *src, uint64_t len);
    int (*pin)(struct utsm_capability cap, uint64_t off, uint64_t len, void **window);
    int (*unpin)(void *window, uint32_t flags);
};
```

### 3.5 IRQ API

```c
struct dkm_irq_api {
    int (*register_irq)(uint32_t vector, int (*handler)(void *ctx), void *ctx, uint32_t flags);
    int (*unregister_irq)(uint32_t vector);
    void (*ack)(uint32_t vector);
};
```

### 3.6 PCI API

```c
struct dkm_pci_api {
    int (*scan)(int (*cb)(const struct pci_device *dev, void *ctx), void *ctx);
    int (*enable_device)(const struct pci_device *dev);
    uint64_t (*bar_phys)(const struct pci_device *dev, uint32_t bar);
    uint64_t (*bar_size)(const struct pci_device *dev, uint32_t bar);
};
```

### 3.7 DMA API

```c
struct dkm_dma_api {
    int (*alloc)(uint64_t size, uint32_t flags, struct dkm_dma_buffer *out);
    void (*free)(struct dkm_dma_buffer *buf);
    int (*sync_for_device)(struct dkm_dma_buffer *buf);
    int (*sync_for_cpu)(struct dkm_dma_buffer *buf);
};
```

### 3.8 VFS / NET / TIMER / DRR API

第一版只定义边界：

```text
vfs: register_fs / mount / open / read / write / close
net: register_netdev / submit_tx / poll_rx
timer: sleep_ns / call_later / monotonic_ns
drr: register_recovery_ops / report_fault / mark_critical
```

详细结构后续随对应子系统实现细化。

---

## 4. ELF Loader 支持的 relocation

第一版仅支持 x86_64 ELF64 小型内核模型。

### 4.1 必须支持

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

### 4.2 建议支持

```text
R_X86_64_PC32
    32-bit PC-relative 调用。

R_X86_64_PLT32
    PLT-relative 调用。

R_X86_64_32 / R_X86_64_32S
    仅允许确认地址范围安全时使用。
```

### 4.3 第一版禁止

```text
TLS relocation
IFUNC
lazy binding
外部动态库依赖
用户态 libc 依赖
异常展开表强依赖
```

### 4.4 加载流程

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
    10. 将内核导出符号或 kernel_api 符号填入。
    11. 执行 relocation。
    12. 查找 driver_desc / driver_init / driver_exit。
    13. 设置 code 段 READ|EXEC|SEALED。
    14. 设置 data 段 READ|WRITE|STRONG_RECOVERY。
    15. 返回 module handle。
```

---

## 5. manifest.json 格式

当前使用 JSON，不再使用 ini 格式的 manifest.drv。

路径：

```text
SYSTEM/driver/manifest.json
```

### 5.1 顶层字段

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

### 5.2 abi 字段

```json
{
  "name": "DKM",
  "version": 1,
  "entry": "driver_init",
  "exit": "driver_exit",
  "descriptor": "driver_desc"
}
```

### 5.3 policy 字段

```json
{
  "loadModel": "module",
  "loadOrder": "staged",
  "requiredFailure": "drr_recovery_or_panic",
  "optionalFailure": "mark_failed_and_continue",
  "memoryModel": "UTSM",
  "codeSegment": "READ|EXEC|SEALED",
  "dataSegment": "READ|WRITE|STRONG_RECOVERY",
  "dmaSegment": "DMA|PINNED"
}
```

### 5.4 stage 字段

```json
{
  "id": 0,
  "name": "platform",
  "required": true,
  "description": "平台发现、中断、总线、基础计时。",
  "drivers": []
}
```

### 5.5 driver 字段

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

### 5.6 路径规则

```text
manifest 中的 path 相对 SYSTEM 根目录。
SYSTEM/driver/manifest.json 内的 driver/bus/pci.drv 指向 SYSTEM/driver/bus/pci.drv。
stage0/stage1 驱动允许来自 boot module，不要求文件系统已挂载。
stage2/stage3 驱动默认从 SYSTEM/driver 加载。
```

---

## 6. 驱动加载状态机

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

恢复状态：

```text
ACTIVE
    -> SUSPENDED_BY_DRR
    -> RECOVERING
    -> REINITING
    -> ACTIVE
```

### 6.1 加载策略

```text
1. DSM 读取 manifest.json。
2. 按 stage 顺序加载。
3. 每个 stage 内按 priority 排序。
4. 检查 depends 是否已由 provides 满足。
5. 未满足依赖的驱动进入 DEP_WAIT。
6. 每轮加载成功后更新 provides set。
7. 一轮后仍 DEP_WAIT 的 required 驱动进入 FAILED_REQUIRED。
8. optional 驱动失败只记录状态。
```

### 6.2 required 失败策略

```text
stage0/stage1 required 失败：
    -> DRR report_fault
    -> 尝试 boot fallback
    -> 不可恢复则 panic / system rollback

stage2 required 失败：
    -> DRR recovery
    -> fallback 到 bootfs 或只读模式

stage3 optional 失败：
    -> mark FAILED
    -> continue boot
```

---

## 7. stage0/stage1 boot module 方案

问题：

```text
stage0/stage1 负责 timer、irq、pci、bootfs、block 等早期能力。
此时 VFS 和普通文件系统可能还没准备好。
所以不能完全依赖 SYSTEM/driver 的普通文件遍历。
```

### 7.1 Bootloader 预加载

bootloader 负责把关键文件作为 boot module 交给内核：

```text
SYSTEM/driver/manifest.json
SYSTEM/driver/platform/timer.drv
SYSTEM/driver/platform/apic.drv
SYSTEM/driver/bus/pci.drv
SYSTEM/driver/fs/bootfs.drv
```

可选：

```text
SYSTEM/driver/console/console_fb.drv
SYSTEM/driver/block/ahci.drv
SYSTEM/driver/block/nvme.drv
```

### 7.2 Boot Module 表

内核早期接收 boot module 表：

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

### 7.3 查找规则

```text
DSM_LOAD_DRIVER(path)
    1. 如果 path 在 boot module table 中，直接从内存加载。
    2. 否则如果 VFS 已可用，从 SYSTEM/path 加载。
    3. 否则返回 BOOT_MODULE_NOT_FOUND。
```

### 7.4 stage0/stage1 限制

```text
1. 不依赖普通 VFS。
2. 不依赖普通 heap。
3. 优先使用 early allocator 或 DRR emergency-safe allocator。
4. 日志使用 early log。
5. 不启动复杂后台任务。
6. 只注册最小服务能力。
```

### 7.5 stage 切换点

```text
stage0 完成：
    timer / irq / pci 基础能力可用。

stage1 完成：
    bootfs 或 block + fs 基础能力可用。

stage2 开始：
    VFS 可用，可以从 SYSTEM/driver 正常读取更多驱动。

stage3 开始：
    普通调度、网络、输入、图形等可异步加载。
```

---

## 8. 与 UTSM / DRR 的关系

### 8.1 UTSM 内存策略

```text
驱动 text：READ | EXEC | SEALED
驱动 rodata：READ | SEALED
驱动 data：READ | WRITE | STRONG_RECOVERY
驱动 bss：READ | WRITE | STRONG_RECOVERY
驱动 DMA：DMA | PINNED，必要时 CIPHERTEXT_DMA 或 NO_ENCRYPT
```

### 8.2 DRR 记录项

DRR 为每个驱动记录：

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

### 8.3 驱动 recovery ops

驱动可注册：

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

## 9. 第一版定稿

第一版驱动系统确定：

```text
1. SYSTEM 是 IMG 内系统根目录。
2. 驱动清单为 SYSTEM/driver/manifest.json。
3. 驱动文件为 ELF64 .drv。
4. 驱动必须导出 driver_desc / driver_init / driver_exit。
5. 驱动通过 dkm_kernel_api 调用内核服务。
6. ELF loader 第一版支持常用 x86_64 relocation。
7. 驱动按 stage 分阶段加载。
8. stage0/stage1 支持 bootloader 预加载 boot module。
9. stage2/stage3 从 SYSTEM/driver 加载。
10. 驱动内存纳入 UTSM，驱动恢复纳入 DRR。
```
