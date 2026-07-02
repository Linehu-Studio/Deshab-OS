# UTSM 设计架构记录

本文记录 Deshab 内核当前 UTSM 相关架构定稿。

---

## 1. 架构目标

UTSM 的目标不是传统强安全隔离，而是在极高权限、极高效率的单地址空间 Ring0 内核中提供一种封缄内存模型。

核心目标：

```text
所有普通内存可读，但默认是密文。
明文读写必须通过 UTRW。
UUID 作为进程和内存段的调谐因子。
数据区保持连续，避免 UUID 孔洞。
DRR 提供 root key、checkpoint、recovery log 和回滚。
调度器保持 O(1)。
```

---

## 2. 总架构模块

```text
Deshab Kernel Core

SAS-R0-PCQ
    单地址空间 Ring0 任务模型
    Per-CPU O(1) 位图调度器

DRR
    Dedicated Recovery Root
    专用恢复根
    watchdog / root key / checkpoint / recovery log / A-B rollback

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

## 3. UTSM 内存模型

每个 UTSM 段由两部分组成：

```text
DMP + Cipher Data
```

布局：

```text
+-------------------------------+
| DMP: Descriptor Meta Page     |
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

规则：

```text
DMP 不属于用户数据。
Cipher Data 只存密文。
UUID 不插入 Cipher Data。
数据区保持连续。
```

---

## 4. 关键设计替代

从原始构想到 UTSM v1 的替代关系：

```text
十进制 UUID
    -> 128-bit 二进制 UUID

UUID 数字间隔插入内存
    -> 独立 DMP 元数据页

UUID 数字位置表
    -> segment_slot + generation + tweak_seed

十进制凯撒
    -> UUID 调谐的 cache-line 流式加密

每次传统 syscall
    -> Ring0 fast accessor + PCKC

写入时避开 UUID
    -> 连续密文数据区直接写
```

---

## 5. 数据结构架构

### 5.1 uuid128_t

```c
typedef struct {
    uint64_t hi;
    uint64_t lo;
} uuid128_t;
```

### 5.2 utsm_process_context

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

### 5.3 utsm_segment_desc

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

### 5.4 utsm_dmp

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

### 5.5 utsm_capability

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

---

## 6. UTRW 读写器架构

UTRW 分三层：

```text
Level 0: Raw Access
    直接访问 cipher_base，读到密文。

Level 1: Fast Path
    capability O(1) 定位段。
    PCKC 命中。
    无全局锁。
    无 trap。

Level 2: Slow Path
    key miss。
    epoch mismatch。
    checkpoint/recovery。
    MAC fail。
    poisoned segment。
```

读写基本流程：

```text
Read:
    cap -> segment desc -> PCKC key -> 64B line 解密 -> 返回明文

Write:
    cap -> segment desc -> PCKC key -> 64B line 加密 -> 写密文 -> dirty bitmap -> recovery log
```

---

## 7. DRR 协作架构

DRR 保存：

```text
root_key 或 root_key handle
global_crypto_epoch
segment table snapshot
DMP snapshot
dirty shard summary
dirty bitmap
MAC root
recovery log
poisoned segment list
Boot Control Block
DRR Emergency Pool
A/B checkpoint metadata slot
```

DRR 负责：

```text
watchdog 检测
freeze-all
checkpoint
page rollback
segment rollback
system rollback
A/B 镜像回滚
```

---

## 8. Checkpoint / Rollback 架构

Checkpoint 类型：

```text
Light Checkpoint
    控制面快照。

Dirty Page Checkpoint
    dirty ciphertext page 快照。

Boot Checkpoint
    启动状态和 A/B slot 信息。
```

### 8.1 Dirty Shard

Dirty Bitmap 采用分片结构。

```text
Dirty Shard
    dirty_count
    shard_id
    page_base
    page_count
    last_checkpoint_epoch
    bitmap
```

checkpoint 时：

```text
dirty_count == 0 的 shard 直接跳过。
dirty_count > 0 的 shard 才扫描 dirty bit。
```

这样 checkpoint 成本为：

```text
O(number_of_dirty_shards + number_of_dirty_pages)
```

### 8.2 A/B Checkpoint Metadata

checkpoint metadata 使用双槽：

```text
Checkpoint Metadata Slot A
Checkpoint Metadata Slot B
```

写入规则：

```text
写 inactive slot
写 metadata / dirty index / MAC root / CRC
flush
原子切换 active slot
旧 slot 作为 fallback
```

恢复规则：

```text
active slot 有效 -> 使用 active
active slot 无效 -> fallback 另一个 slot
双槽都无效 -> system rollback
```

### 8.3 DRR Emergency Pool

DRR 独立预留紧急内存池。

```text
DRR Emergency Pool
    DRR private stack
    recovery log buffer
    checkpoint metadata buffer
    crash buffer
    metadata slab
    emergency pages
```

约束：

```text
普通 allocator 不可使用。
普通 OOM 不影响 DRR recovery。
DRR recovery 路径禁止依赖普通堆分配器。
Emergency Pool 耗尽时进入 system rollback。
```

Rollback 级别：

```text
Page Rollback
    单页恢复。

Segment Rollback
    整段恢复。

System Rollback
    重启并由 bootloader 切换 last_good_slot。
```

---

## 9. 与 SAS-R0-PCQ 的整合

UTSM Task 是：

```text
Ring0 调度实体
+ process_uuid
+ crypto context
+ capability table
+ encrypted stack/heap
+ PCKC hot hint
+ DRR recovery generation
```

调度切换：

```text
schedule()
    -> utsm_switch_out(old)
    -> pick_next_task()
    -> utsm_switch_in(next)
    -> context_switch(old, next)
```

调度路径只允许：

```text
设置 crypto 指针
加载 process_uuid
预热 hot_segment_hint
标记 PWC dirty flush
```

禁止：

```text
扫描段
扫描 capability table
计算 MAC
写 checkpoint
重加密整段
```

---

## 10. 启动顺序

```text
Bootloader
    -> 读取 Boot Control Block
    -> 选择 active_slot / last_good_slot
    -> 加载 SYSTEM/boot/utsm.elf
    -> 传入 memory map 和 DRR region

DRR Init
    -> 初始化 watchdog
    -> 建立 root key handle
    -> 初始化 recovery log
    -> 初始化 checkpoint region
    -> 初始化 heartbeat page
    -> 初始化 DRR Emergency Pool
    -> 初始化 A/B checkpoint metadata slot

UTSM Init
    -> 初始化 global crypto epoch
    -> 初始化 segment table
    -> 初始化 DMP pool
    -> 初始化 PCKC area
    -> 初始化 dirty shard pool
    -> 初始化 dirty bitmap / MAC table pool
    -> 注册 UTRW fast path / slow path

Scheduler Init
    -> 初始化 per-CPU runqueue
    -> 初始化 per-CPU heartbeat
    -> 初始化每 CPU PCKC
    -> 创建 bootstrap task
    -> 创建核心服务 task
```

---

## 11. 当前定稿边界

UTSM v1 确定：

```text
加密粒度：64B cache line
Dirty 粒度：4KB page
Dirty 分片：Dirty Shard
MAC 粒度：4KB page
段定位：O(1)
调度额外成本：O(1)
checkpoint 成本：O(dirty shards + dirty pages)
checkpoint metadata：A/B 双槽，CRC 校验，原子切换
DRR recovery 内存：Emergency Pool 独立预留
```

暂不追求：

```text
完整 RAM 原地回滚
对恶意 Ring0 代码的强安全隔离
设备外部副作用回滚
```

设备恢复策略：

```text
内存恢复后，设备 reset/reinit。
磁盘依赖文件系统 journal。
网卡 TX 已发包不回滚，RX/TX ring 重建。
```
