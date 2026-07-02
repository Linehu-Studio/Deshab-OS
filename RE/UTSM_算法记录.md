# UTSM 算法记录

```text
UTSM = UUID-Tuned Sealed Memory
中文名：UUID 调谐封缄内存
```

本文记录 UTSM v1 的核心算法。

---

## 1. 段创建算法

目标：为任务创建一个只存密文的连续内存段，并返回 capability。

```text
utsm_create_segment(process, size, flags)
    1. size 按 64B / 4KB 对齐。
    2. 从 buddy/slab 分配 cipher data pages。
    3. 从 DMP pool 分配 DMP。
    4. 生成 segment_uuid。
    5. key_epoch = process.crypto_epoch。
    6. tweak_seed = hash(process_uuid, segment_uuid, key_epoch)。
    7. 初始化 dirty_bitmap。
    8. 初始化 mac_table。
    9. 填写 utsm_segment_desc。
    10. 填写 utsm_dmp。
    11. generation++。
    12. 生成 utsm_capability。
    13. 将 capability 写入任务 capability table。
    14. 返回 capability。
```

复杂度：

```text
段元数据初始化：O(1)
数据页分配：O(page_count)
热路径访问定位：O(1)
```

---

## 2. 密钥派生算法

UUID 不作为明文密钥，而是作为调谐因子。

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

加密和解密对称：

```text
ciphertext = plaintext  XOR stream(segment_key, tweak)
plaintext  = ciphertext XOR stream(segment_key, tweak)
```

设计理由：

```text
1. 避免十进制 UUID 解析。
2. 避免 UUID 孔洞破坏连续内存。
3. 以 64B cache line 为单位，适合 CPU cache 和 SIMD。
4. key_epoch 可用于快速作废旧段。
```

---

## 3. UTRW Fast Read 算法

```text
UTRW_READ_FAST(cap, offset, len, dst)
    1. desc = SEGMENT_TABLE[cap.segment_slot]。
    2. 检查 desc.generation == cap.generation。
    3. 检查 cap.rights 包含 READ。
    4. 检查 cap.epoch == desc.key_epoch。
    5. 检查 offset + len <= desc.cipher_length。
    6. 检查 desc.state == ACTIVE。
    7. key = PCKC_LOOKUP(desc.segment_slot, desc.key_epoch)。
    8. 如果 key miss，进入 slow path。
    9. 读取 writer_seq_before。
    10. 如果 writer_seq_before 为奇数，重试或 slow path。
    11. 对访问范围涉及的每个 64B line：
        a. 计算 line_index。
        b. 生成 tweak。
        c. 读取密文 line。
        d. 解密成明文 line。
        e. 拷贝目标字节到 dst。
    12. 读取 writer_seq_after。
    13. 如果 before != after，重试。
    14. 返回 OK。
```

复杂度：

```text
段定位：O(1)
读写长度 n：O(n / 64)
```

---

## 4. UTRW Fast Write 算法

```text
UTRW_WRITE_FAST(cap, offset, len, src)
    1. desc = SEGMENT_TABLE[cap.segment_slot]。
    2. 检查 desc.generation == cap.generation。
    3. 检查 cap.rights 包含 WRITE。
    4. 检查 cap.epoch == desc.key_epoch。
    5. 检查 offset + len <= desc.cipher_length。
    6. 检查 desc.state == ACTIVE。
    7. key = PCKC_LOOKUP(desc.segment_slot, desc.key_epoch)。
    8. 如果 key miss，进入 slow path。
    9. 如果是强恢复段，写 WRITE_INTENT log。
    10. writer_seq++，进入奇数写入状态。
    11. 对访问范围涉及的每个 64B line：
        a. 计算 line_index。
        b. 生成 tweak。
        c. 如果覆盖完整 line，直接使用 src 明文。
        d. 如果部分覆盖 line，读取旧密文，解密旧明文，合并新明文。
        e. 加密成新密文。
        f. 写回 cipher data。
    12. 标记 dirty_bitmap。
    13. writer_seq++，回到偶数稳定状态。
    14. 如果是强恢复段，写 WRITE_COMMIT log。
    15. 返回 OK。
```

部分写规则：

```text
UTSM 的加密单位是 64B line。
如果只写 line 的一部分，必须读旧密文、解密、合并、重新加密整条 line。
```

---

## 5. UTRW Slow Path 算法

Slow Path 触发条件：

```text
PCKC key miss
generation mismatch
epoch mismatch
state != ACTIVE
checkpoint busy
recovery busy
MAC failed
segment poisoned
out of bounds
access denied
```

处理策略：

```text
KEY_MISS
    -> 派生 segment_key
    -> 生成 key_schedule
    -> 写入 PCKC
    -> 回到 fast path

STALE_CAPABILITY
    -> 尝试从 capability table 刷新
    -> 失败则返回错误

EPOCH_EXPIRED
    -> 刷新 cap.epoch 和 auth_tag
    -> 清除旧 PCKC key
    -> 回到 fast path

CHECKPOINT_BUSY
    -> 短暂等待或写 side log

RECOVERING
    -> 阻塞任务，等待 DRR 完成恢复

MAC_FAILED
    -> 标记 suspicious page
    -> 通知 DRR
    -> 尝试 page rollback

SEGMENT_POISONED
    -> 拒绝访问
    -> 交给 DRR 决定回滚、重建或 kill task
```

---

## 6. PCKC 查找算法

每 CPU 保存少量热段 key。

```text
PCKC_LOOKUP(slot, epoch)
    1. 检查 current hot slot。
    2. 检查 hot_segment_slot[0..N]。
    3. 如果 slot 和 epoch 均匹配，返回 key_schedule。
    4. 否则返回 MISS。
```

替换策略：

```text
第一版使用小型 LRU 或替换最冷 slot。
```

建议槽数：

```text
8 个 key slot 起步。
高并发或服务型任务可扩展到 16 个。
```

---

## 7. Checkpoint 算法

### 7.1 Dirty Shard 标记算法

Dirty Bitmap 分片为 Dirty Shard，避免 checkpoint 扫描整块大 bitmap。

```text
UTSM_MARK_DIRTY(desc, page_index)
    1. shard = desc.dirty_shards[page_index / PAGES_PER_SHARD]。
    2. bit = page_index % PAGES_PER_SHARD。
    3. 如果 bit 原来是 0：
        a. 设置 bitmap bit。
        b. shard.dirty_count++。
        c. 如果 shard.dirty_count 从 0 变为 1，加入 per-cpu dirty_shard_list。
    4. 返回。
```

复杂度：

```text
Dirty 标记：O(1)
Checkpoint 扫描：O(number_of_dirty_shards + number_of_dirty_pages)
```

### 7.2 A/B Checkpoint Metadata 提交算法

```text
CHECKPOINT_METADATA_COMMIT(gen)
    1. active = checkpoint.active_slot。
    2. inactive = opposite(active)。
    3. 清理 inactive slot 的临时状态。
    4. 写 inactive.metadata_header。
    5. 写 segment desc snapshot。
    6. 写 DMP snapshot。
    7. 写 dirty shard summary。
    8. 写 MAC root。
    9. 写 generation。
    10. 计算并写 CRC。
    11. flush inactive slot。
    12. 原子切换 checkpoint.active_slot = inactive。
    13. 写 CHECKPOINT_COMMIT log。
```

恢复时：

```text
1. 读取 active slot。
2. 校验 generation 和 CRC。
3. 如果 active 有效，使用 active。
4. 如果 active 无效，校验 fallback slot。
5. 如果 fallback 有效，使用 fallback。
6. 如果双槽都无效，进入 system rollback。
```

### 7.3 Dirty Page Checkpoint 算法

```text
DRR_CHECKPOINT()
    1. checkpoint_generation++。
    2. 写 CHECKPOINT_BEGIN log。
    3. inactive_slot = opposite(active_slot)。
    4. 遍历 per-cpu dirty_shard_list。
    5. 跳过 dirty_count == 0 的 shard。
    6. 对每个 dirty shard：
        a. 找到关联 segment。
        b. desc.state = CHECKPOINTING。
        c. 冻结 writer。
        d. flush PWC / per-CPU dirty buffer。
        e. 只扫描 shard 内 dirty bit。
        f. 复制 dirty ciphertext page 到 checkpoint region。
        g. 计算 page MAC。
        h. 写 dirty shard summary。
        i. 清 shard bitmap。
        j. shard.dirty_count = 0。
        k. desc.state = ACTIVE。
    7. 写 segment desc snapshot。
    8. 写 DMP snapshot。
    9. CHECKPOINT_METADATA_COMMIT(checkpoint_generation)。
```

提交规则：

```text
只有存在 CHECKPOINT_COMMIT 且 CRC 正确的 checkpoint 才是稳定点。
中途崩溃的 checkpoint 直接丢弃。
A/B metadata 至少保留一个旧稳定点。
```

### 7.4 DRR Emergency Pool 分配算法

DRR Recovery 路径不能依赖普通 allocator。

```text
DRR_EMERGENCY_ALLOC(size, class)
    1. 从 DRR emergency pool 的 class freelist 取对象。
    2. 如果 freelist 非空，返回对象。
    3. 如果 freelist 为空，从 emergency pages 切分。
    4. 如果 emergency pages 不足：
        a. 写 EMERGENCY_POOL_EXHAUSTED。
        b. 进入 system rollback。
```

Emergency Pool 包含：

```text
DRR private stack
recovery log buffer
checkpoint metadata buffer
crash buffer
metadata slab
emergency pages
```

规则：

```text
普通 allocator 不可使用 Emergency Pool。
Emergency Pool 不参与普通内存回收。
DRR OOM 直接视为不可恢复故障。
```

---

## 8. Rollback 算法

分三级。

### 8.1 Page Rollback

```text
1. 定位最近稳定 checkpoint。
2. 校验 checkpoint page MAC。
3. 将旧密文页复制回 cipher data。
4. 恢复 page MAC。
5. 清 dirty bit。
6. 段保持 ACTIVE。
```

### 8.2 Segment Rollback

```text
1. desc.state = RECOVERING。
2. 恢复 segment desc snapshot。
3. 恢复 DMP snapshot。
4. 恢复 key_epoch。
5. 恢复 dirty bitmap。
6. 恢复 mac_table。
7. 清除对应 PCKC key。
8. desc.state = ACTIVE。
```

### 8.3 System Rollback

```text
1. DRR 写 crash reason。
2. 标记 boot attempt failed。
3. 触发 reboot。
4. bootloader 读取 BCB。
5. 如果失败次数超限，切换 last_good_slot。
```

---

## 9. 任务创建算法

```text
task_create(entry, priority, flags)
    1. 分配 TCB。
    2. 生成 tid。
    3. 生成 process_uuid。
    4. 创建 utsm_process_context。
    5. 创建 stack segment。
    6. 创建 heap segment。
    7. 可选创建 ipc segment。
    8. 初始化 capability table。
    9. cap[0] = stack。
    10. cap[1] = heap。
    11. 初始化 CPU context。
    12. 通过 UTRW_WRITE 写入 task_start_packet。
    13. hot_segment_hint = stack.segment_slot。
    14. state = READY。
    15. enqueue_task(current_cpu)。
```

---

## 10. 调度切换算法

```text
schedule()
    1. old = current。
    2. utsm_switch_out(old)。
    3. next = pick_next_task()。
    4. utsm_switch_in(next)。
    5. context_switch(old, next)。
```

`utsm_switch_out`：

```text
1. 如果 old 有 PWC dirty，登记 flush pending。
2. 保存 old.hot_segment_hint。
3. 更新 old.recovery_generation。
4. 更新 heartbeat。
```

`utsm_switch_in`：

```text
1. 如果 next.state == RECOVERING，跳过。
2. PCKC.current_process_uuid = next.crypto.process_uuid。
3. PCKC.current_crypto_epoch = next.crypto.crypto_epoch。
4. 预热 next.hot_segment_hint。
```

调度路径禁止：

```text
扫描段
扫描 capability table
计算 MAC
写 checkpoint
重加密整段
```

---

## 11. 任务异常处理算法

```text
UTRW_DETECT_FAULT(task, segment, error)
    1. task.fault_segment_slot = segment.slot。
    2. task.state = FAULTED。
    3. 从 runqueue 移除。
    4. 写 DRR fault event。
    5. DRR 判断：
        a. 刷新 capability。
        b. page rollback。
        c. segment rollback。
        d. kill task。
        e. system rollback。
```

任务 kill：

```text
task_kill(task)
    1. task.state = ZOMBIE。
    2. 从 runqueue 移除。
    3. revoke capability table。
    4. owned segment key_epoch++。
    5. desc.state = DESTROYED。
    6. 加入 deferred free。
    7. 写 TASK_KILL recovery log。
```

---

## 12. 复杂度总结

```text
段定位：O(1)
UTRW 读写：O(n / 64)
PCKC 命中：O(1)
PCKC miss：O(KDF + key schedule)
Dirty 标记：O(1)
Checkpoint：O(number_of_dirty_shards + number_of_dirty_pages)
A/B metadata 切换：O(1)
DRR emergency alloc：O(1)
Page rollback：O(1 page)
Segment rollback：O(segment_dirty_pages + metadata)
调度切换额外成本：O(1)
```
