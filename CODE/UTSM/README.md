// 编译到 SYSTEM/boot/utsm.elf

# UTSM 内核模块设计

UTSM 是当前内核架构中的封缄内存模块。

```text
UTSM = UUID-Tuned Sealed Memory
中文名：UUID 调谐封缄内存
```

UTSM 的目标是在单地址空间 Ring0 高性能内核中提供一种轻量内存语义：

```text
所有普通内存可以被直接读取，但直接读取到的是密文。
真正有效的明文读写必须经过 UTSM 读写器。
UUID 作为进程和内存段的调谐因子。
DRR 负责 root key、checkpoint、recovery log 和回滚。
```

---

## 1. 所属总架构

```text
SAS-R0-PCQ
    单地址空间 Ring0 任务模型
    Per-CPU O(1) 位图调度器

DRR
    Dedicated Recovery Root
    看门狗 / checkpoint / recovery log / A-B 回滚

UTSM
    UUID-Tuned Sealed Memory
    UUID 调谐封缄内存

UTRW
    UTSM Tagged Reader/Writer
    UTSM 带标记读写器

DMP
    Descriptor Meta Page
    段描述符元页

PCKC
    Per-CPU Key Cache
    每 CPU 密钥缓存
```

---

## 2. UTSM 核心规则

```text
1. 数据区只存连续密文。
2. UUID 不插入数据区。
3. UUID 使用 128-bit 二进制格式。
4. 每个加密段有独立 DMP。
5. 每个任务有 process_uuid 和 capability table。
6. 读写明文必须经过 UTRW。
7. 直接读取 cipher_base 只得到密文。
8. 直接写入 cipher_base 会破坏段一致性，段可被标记为 POISONED。
9. DRR 保存 root key、checkpoint、recovery log 和恢复状态。
10. 调度器只加载 crypto context，不扫描段，不破坏 O(1) 调度。
```

---

## 3. 内存布局

每个 UTSM 段由 DMP 和密文数据区组成。

```text
+-------------------------------+
| DMP: Descriptor Meta Page     |
| 段描述符元页                  |
+-------------------------------+
| Cipher Data Page 0            |
+-------------------------------+
| Cipher Data Page 1            |
+-------------------------------+
| Cipher Data Page 2            |
+-------------------------------+
| ...                           |
+-------------------------------+
```

全局区域：

```text
+--------------------------------------------------+
| Normal Encrypted Segment Region                  |
| 普通加密段数据区                                  |
+--------------------------------------------------+
| DMP Pool                                         |
| 段描述符元页池                                    |
+--------------------------------------------------+
| Segment Table                                    |
| 全局段表                                          |
+--------------------------------------------------+
| PCKC Area                                        |
| Per-CPU Key Cache 区                              |
+--------------------------------------------------+
| DRR Reserved Region                              |
| root key / checkpoint / recovery log             |
+--------------------------------------------------+
```

---

## 4. 基础类型

```c
typedef struct {
    uint64_t hi;
    uint64_t lo;
} uuid128_t;
```

UUID 不转十进制字符串，原因：

```text
16 字节固定长度
比较快
cache 友好
适合 KDF
适合 SIMD
避免十进制解析和孔洞布局
```

---

## 5. 进程加密上下文

```c
struct utsm_process_context {
    uuid128_t process_uuid;

    uint64_t crypto_epoch;
    uint64_t recovery_generation;

    uint32_t capability_table_slot;
    uint32_t capability_count;

    uint32_t flags;
};
```

字段含义：

```text
process_uuid
    创建任务时生成的进程 UUID。

crypto_epoch
    当前密钥版本，用于重加密、回滚和 crypto erase。

recovery_generation
    当前 DRR checkpoint 版本。

capability_table_slot
    该任务 capability table 的位置。

capability_count
    该任务拥有的段能力数量。

flags
    PWC、DMA、强恢复等策略标志。
```

---

## 6. 加密段描述符

```c
struct utsm_segment_desc {
    uuid128_t segment_uuid;
    uuid128_t owner_process_uuid;

    void *cipher_base;
    uint64_t cipher_length;

    void *dmp_base;

    uint64_t key_epoch;
    uint64_t tweak_seed;

    void *dirty_bitmap;
    void *mac_table;

    uint32_t generation;
    uint32_t flags;

    uint32_t state;
    uint32_t last_cpu;

    uint64_t writer_seq;
    uint64_t checkpoint_generation;
};
```

段状态：

```text
FREE
ACTIVE
CHECKPOINTING
SEALED
RECOVERING
POISONED
DESTROYED
```

---

## 7. DMP 段描述符元页

DMP 是热路径加速结构，替代“UUID 数字位置表”。

```c
struct utsm_dmp {
    uint64_t magic;
    uint32_t version;
    uint32_t segment_slot;

    uint64_t fast_base;
    uint64_t fast_limit;

    uint64_t fast_tweak_seed;
    uint64_t fast_key_epoch;

    uint64_t dirty_bitmap_base;
    uint64_t mac_table_base;

    uint64_t last_writer_seq;
    uint32_t last_cpu;
    uint32_t flags;

    uint64_t crc;
};
```

DMP 用于：

```text
O(1) 段定位
快速边界检查
tweak 生成
Dirty Bitmap 定位
MAC Table 定位
checkpoint
recovery
```

---

## 8. Capability

任务访问段时使用 capability，而不是裸 UUID。

```c
struct utsm_capability {
    uint32_t segment_slot;
    uint32_t generation;

    uint32_t rights;
    uint32_t epoch;

    uint64_t uuid_digest;
    uint64_t auth_tag;
};
```

权限：

```text
READ
WRITE
EXEC
SHARE
DMA
```

热路径通过：

```text
segment_slot + generation + epoch
```

直接定位并验证段。

---

## 9. 加密规则

UTSM 不使用十进制凯撒和 UUID 孔洞，改为按 cache line 的 UUID 调谐流式加密。

```text
segment_key = KDF(
    DRR_root_key,
    process_uuid,
    segment_uuid,
    key_epoch
)
```

每 64B cache line 生成 tweak：

```text
line_index = offset / 64
tweak = hash(tweak_seed, line_index, key_epoch)
```

加密和解密：

```text
ciphertext = plaintext  XOR stream(segment_key, tweak)
plaintext  = ciphertext XOR stream(segment_key, tweak)
```

---

## 10. UTRW 读写器

UTRW 分三层：

```text
Level 0: Raw Access
    直接读 cipher_base，得到密文。

Level 1: Fast Path
    capability O(1) 定位段，PCKC 命中，无全局锁，无 trap。

Level 2: Slow Path
    key miss、epoch mismatch、checkpoint、recovery、MAC fail 等异常处理。
```

### 10.1 读流程

```text
UTRW_READ(cap, offset, len, dst)
    -> segment_slot 找 desc
    -> generation 检查
    -> rights 检查
    -> epoch 检查
    -> 边界检查
    -> PCKC 取 key
    -> 按 64B line 解密
    -> 返回明文
```

### 10.2 写流程

```text
UTRW_WRITE(cap, offset, len, src)
    -> segment_slot 找 desc
    -> generation 检查
    -> rights 检查
    -> epoch 检查
    -> 边界检查
    -> PCKC 取 key
    -> 按 64B line 加密
    -> 写入 cipher data
    -> 标记 dirty bitmap
    -> writer_seq++
    -> 按策略写 recovery log
```

部分写必须先解密旧 64B line，合并新明文后重新加密整条 line。

---

## 11. PCKC 每 CPU 密钥缓存

```c
struct utsm_pckc {
    uuid128_t current_process_uuid;

    uint32_t hot_segment_slot[8];
    uint64_t hot_key_epoch[8];

    void *key_schedule[8];

    uint64_t hit_count;
    uint64_t miss_count;
};
```

规则：

```text
每 CPU 一个 PCKC。
调度切换时只加载 next.crypto 指针和 hot_segment_hint。
不扫描 capability table。
不刷新全部 key。
不破坏 PCQ 的 O(1) 调度。
```

---

## 12. 与 SAS-R0-PCQ 的整合

任务结构扩展：

```c
struct task {
    uint64_t tid;
    uint32_t state;
    uint32_t priority;

    uint64_t runtime;
    uint64_t time_slice;

    void *stack_top;
    void *entry;

    struct context cpu_context;

    uint32_t last_cpu;
    uint64_t affinity_mask;

    struct utsm_process_context *crypto;
    struct utsm_capability_table *cap_table;

    uint32_t hot_segment_hint;
    uint32_t fault_segment_slot;

    uint64_t recovery_generation;
    uint64_t heartbeat_seq;

    uint32_t flags;

    struct task *prev;
    struct task *next;
};
```

任务状态：

```text
READY
RUNNING
BLOCKED
SLEEPING
RECOVERING
FAULTED
ZOMBIE
```

---

## 13. 任务创建流程

```text
task_create(entry, priority, flags)
    -> 分配 TCB
    -> 生成 tid
    -> 生成 process_uuid
    -> 创建 utsm_process_context
    -> 创建 stack segment
    -> 创建 heap segment
    -> 创建 ipc segment 可选
    -> 生成 capability table
    -> cap[0] = stack
    -> cap[1] = heap
    -> 初始化 CPU context
    -> 通过 UTRW_WRITE 写入 start packet
    -> hot_segment_hint = stack segment
    -> state = READY
    -> enqueue_task(current_cpu)
```

默认 capability：

```text
cap[0] = stack segment, READ | WRITE
cap[1] = heap segment, READ | WRITE
cap[2] = ipc segment, READ | WRITE | SHARE，可选
cap[3] = code segment, READ | EXEC，可选
```

---

## 14. 调度切换流程

```text
schedule()
    -> old = current
    -> utsm_switch_out(old)
    -> next = pick_next_task()
    -> utsm_switch_in(next)
    -> context_switch(old, next)
```

`utsm_switch_out` 只做：

```text
标记 PWC dirty flush
保存 hot_segment_hint
更新 recovery_generation
更新 heartbeat
```

`utsm_switch_in` 只做：

```text
设置 PCKC.current_process_uuid
加载 next.crypto
检查 recovery_generation
预热 hot_segment_hint
```

禁止在调度路径中：

```text
扫描所有 segment
扫描 capability table
计算 MAC
写 checkpoint
重加密整段
```

---

## 15. Checkpoint / Rollback 协议

### 15.1 Checkpoint 类型

```text
Light Checkpoint
    保存 segment table、DMP、dirty shard summary、writer_seq、key_epoch、MAC root。

Dirty Page Checkpoint
    保存 dirty page 的密文副本、dirty page MAC、dirty shard、segment descriptor。

Boot Checkpoint
    保存内核镜像 slot、UTSM root epoch、DRR boot status、启动阶段状态。
```

### 15.2 Dirty Shard 分片

Dirty Bitmap 不再只是一整块大 bitmap，而是拆成多个 Dirty Shard。

```c
struct utsm_dirty_shard {
    uint32_t dirty_count;
    uint32_t shard_id;

    uint64_t page_base;
    uint64_t page_count;

    uint64_t last_checkpoint_epoch;
    void *bitmap;
};
```

规则：

```text
1. 每个 shard 管理一段连续页范围。
2. UTRW_WRITE 标记 dirty page 时，同时更新 dirty_count。
3. checkpoint 时 dirty_count == 0 的 shard 直接跳过。
4. checkpoint 只扫描 dirty_count > 0 的 shard。
5. 每 CPU 可以维护 dirty shard list，避免全局扫描。
```

复杂度从：

```text
扫描全 dirty bitmap
```

优化为：

```text
O(number_of_dirty_shards + number_of_dirty_pages)
```

### 15.3 A/B Checkpoint Metadata 双槽

每个 checkpoint metadata 使用双槽。

```text
Checkpoint Metadata Slot A
Checkpoint Metadata Slot B
```

提交规则：

```text
当前 active = A
新 checkpoint 写 B
B 写完 metadata、dirty index、MAC root、CRC
flush
最后原子切换 active = B
旧 A 保留为 fallback
```

恢复规则：

```text
优先读取 active slot
如果 active slot CRC 错误或 generation 不完整
    fallback 到另一个 slot
如果两个 slot 都损坏
    进入 system rollback
```

这样可以避免 checkpoint 写一半崩溃导致旧稳定点也损坏。

### 15.4 正常 checkpoint 流程

```text
DRR 发起 checkpoint
    -> checkpoint_generation++
    -> 选择 inactive metadata slot
    -> UTSM 遍历 dirty shard list
    -> 跳过 dirty_count == 0 的 shard
    -> 对 dirty shard 内的 ACTIVE 段执行 checkpoint
    -> desc.state = CHECKPOINTING
    -> 刷出 PWC / per-CPU dirty buffer
    -> 复制 dirty ciphertext page 到 checkpoint region
    -> 计算 page MAC
    -> 写 segment desc snapshot
    -> 写 DMP snapshot
    -> 写 dirty shard summary
    -> 写 checkpoint metadata CRC
    -> flush checkpoint metadata
    -> 原子切换 active metadata slot
    -> CHECKPOINT_COMMIT
    -> 清 dirty bitmap / dirty_count
    -> desc.state = ACTIVE
```

### 15.5 rollback 级别

```text
Page Rollback
    单页损坏，从 checkpoint slot 恢复密文页。

Segment Rollback
    整段损坏，恢复 segment desc、DMP、key_epoch、dirty shard、MAC table。

System Rollback
    控制面严重损坏，写 crash reason，reboot，由 bootloader 切换 last_good_slot。
```

### 15.6 DRR Emergency Pool

DRR 启动时必须预留独立紧急内存池。

```text
DRR Emergency Pool
    emergency pages
    recovery log buffer
    checkpoint metadata buffer
    DRR private stack
    DRR metadata slab
    crash buffer
```

规则：

```text
1. 普通 allocator 永远不能分配 Emergency Pool。
2. Emergency Pool 只供 watchdog、checkpoint、rollback、crash dump 使用。
3. 普通系统 OOM 不等于 DRR OOM。
4. recovery 路径禁止依赖普通堆分配器。
5. Emergency Pool 耗尽时直接进入 system rollback。
```

---

## 16. 启动流程

### 16.1 Bootloader 阶段

```text
1. 读取 Boot Control Block。
2. 校验 BCB CRC。
3. 选择 active_slot。
4. 如果 active_slot 失败次数超限，切换 last_good_slot。
5. 将 SYSTEM 作为打包后 IMG 内的系统根目录。
6. 加载 SYSTEM/boot/utsm.elf。
7. 加载或暴露 SYSTEM/driver/manifest.json。
8. 传入 memory map、BCB 地址、DRR reserved region 信息。
```

### 16.2 DRR 初始化

```text
1. 保留 DRR region。
2. 初始化 watchdog。
3. 建立 root key 或 root key handle。
4. 初始化 recovery log。
5. 初始化 checkpoint region。
6. 初始化 heartbeat page。
7. 初始化 DRR Emergency Pool。
8. 初始化 A/B checkpoint metadata slot。
```

### 16.3 UTSM 初始化

```text
1. 接收 DRR root key handle。
2. 初始化 global crypto epoch。
3. 初始化 segment table。
4. 初始化 DMP pool。
5. 初始化 PCKC area。
6. 初始化 dirty shard pool。
7. 初始化 dirty bitmap pool。
8. 初始化 MAC table pool。
9. 注册 UTRW fast path / slow path。
```

### 16.4 Scheduler 初始化

```text
1. 初始化 per-CPU runqueue。
2. 初始化 per-CPU heartbeat。
3. 初始化每 CPU PCKC。
4. 创建 bootstrap task。
5. 创建核心服务 task。
6. 将 task 加入 PCQ runqueue。
```

### 16.5 Driver Manifest 加载

驱动清单位于系统根目录下：

```text
SYSTEM/driver/manifest.json
```

规则：

```text
1. manifest.json 是启动驱动模块的唯一默认清单。
2. 驱动文件使用 ELF64 .drv 格式。
3. 驱动按 stage 分阶段加载。
4. required=true 的驱动失败时进入 DRR recovery 或 panic。
5. required=false 的驱动失败时标记 FAILED 并继续启动。
6. stage0/stage1 可由 bootloader 预加载为 boot module。
7. stage2/stage3 在文件系统可用后从 SYSTEM/driver 扫描加载。
```

默认阶段：

```text
stage0: platform，timer / apic / acpi / pci
stage1: boot，console / block / bootfs
stage2: filesystem，vfs / fat32 / devfs
stage3: optional，net / input / gpu
```

### 16.6 DKM 驱动 ABI 摘要

驱动模块系统：

```text
DKM = Deshab Kernel Module
DSM = Driver Startup Manager
```

每个 ELF .drv 必须导出：

```text
driver_desc
driver_init
driver_exit
```

`driver_desc` 至少描述：

```text
magic / abi_version / desc_size
name / version / vendor
driver_class / stage / flags / priority
depends / provides
min_kernel_abi / feature_bits
```

驱动入口：

```c
int driver_init(const struct dkm_kernel_api *api, struct dkm_driver_handle *handle);
int driver_exit(struct dkm_driver_handle *handle);
```

驱动默认通过 `dkm_kernel_api` 访问内核服务：

```text
log
mem
utsm
irq
pci
dma
vfs
net
timer
drr
```

ELF loader 第一版支持：

```text
必须支持：
    R_X86_64_64
    R_X86_64_RELATIVE
    R_X86_64_GLOB_DAT
    R_X86_64_JUMP_SLOT

建议支持：
    R_X86_64_PC32
    R_X86_64_PLT32
    R_X86_64_32 / R_X86_64_32S

第一版禁止：
    TLS relocation
    IFUNC
    lazy binding
    外部动态库依赖
    用户态 libc 依赖
```

驱动加载状态机：

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

stage0/stage1 boot module 查找规则：

```text
1. 如果 path 在 boot module table 中，直接从内存加载。
2. 否则如果 VFS 已可用，从 SYSTEM/path 加载。
3. 否则返回 BOOT_MODULE_NOT_FOUND。
```

完整设计见：

```text
RE/驱动模块ABI设计.md
```

---

## 17. 错误处理

UTRW 错误状态：

```text
OK
KEY_MISS_RETRY
STALE_CAPABILITY
EPOCH_EXPIRED
OUT_OF_BOUNDS
ACCESS_DENIED
CHECKPOINT_BUSY
RECOVERING
MAC_FAILED
SEGMENT_POISONED
CORRUPTED_DESCRIPTOR
```

严重错误处理：

```text
1. 记录 task.fault_segment_slot。
2. task.state = FAULTED。
3. 从 runqueue 移除。
4. 通知 DRR。
5. DRR 决定刷新 capability、回滚 segment、kill task 或 system rollback。
```

---

## 18. UTSM v1 定稿

```text
UTSM v1 包含：
    二进制 UUID
    连续密文数据区
    独立 DMP
    全局 Segment Table
    Segment Capability
    Per-CPU Key Cache
    UTRW 统一读写器
    Dirty Shard + Dirty Bitmap
    MAC Table
    DRR Emergency Pool
    A/B Checkpoint Metadata
    DRR checkpoint / rollback 对接
    SAS-R0-PCQ task 整合

加密粒度：
    64B cache line

Dirty 粒度：
    4KB page

Dirty 分片：
    Dirty Shard，checkpoint 只扫描 dirty_count > 0 的 shard

MAC 粒度：
    4KB page

Checkpoint 元数据：
    A/B 双槽，CRC 校验，原子切换 active slot

大页优化：
    2MB hugepage 可选
```
