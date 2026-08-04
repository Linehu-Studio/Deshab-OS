# 性能基线测量

> 对应计划 Phase C5：基于 TSC 的关键性能指标测量

## 状态: ⬜ PENDING

## 测量方法

所有指标基于 TSC（Time Stamp Counter）：

```c
static inline u64 rdtsc(void) {
    u32 lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((u64)hi << 32) | lo;
}
```

TSC 频率需先校准（PIT 100ms busy-wait 测量）：

```c
u64 tsc_freq;  // Hz
u64 calibrate_tsc(void) {
    u64 start = rdtsc();
    pit_busy_wait(100);  // 100ms
    u64 end = rdtsc();
    return (end - start) * 10;  // ×10 得到 Hz
}
```

时间换算：`ms = (tsc_diff * 1000) / tsc_freq`

## 基线指标

| # | 指标 | 测量位置 | 基线目标 | 实测 | 状态 |
|---|---|---|---|---|---|
| P1 | UTSM 启动到 DSK SELFTEST | UTSM entry → DSK selftest pass | < 500ms (QEMU) | | ⬜ |
| P2 | DSK 加载 shell.elf 时间 | FAT32 read + PIE load 包围 | < 200ms | | ⬜ |
| P3 | DSK 加载 desktop.elf 时间 | 同上 | < 200ms | | ⬜ |
| P4 | shell `ping 10.0.2.2` RTT | echo request → reply | < 10ms (user-net) | | ⬜ |
| P5 | shell `linux ls` 完整 RTT | IPC exec round-trip | < 100ms (park-and-resume) | | ⬜ |
| P6 | shell `run PING.ELF` 加载时间 | FAT32 read + PIE load | < 150ms | | ⬜ |
| P7 | desktop 窗口拖动帧率 | 帧计数/TSC | 30+ FPS | | ⬜ |
| P8 | `pe HELLO64.EXE` 加载+执行 | pe_service_run 包围 | < 50ms | | ⬜ |
| P9 | `pe HELLO32.EXE` 加载+执行 | x86emu32 解释 | < 200ms | | ⬜ |
| P10 | UTSM VMM self-test | vmm_init → vmm_self_test | < 50ms | | ⬜ |
| P11 | Linux guest 启动到 park | vmlaunch → daemon HLT | < 3s (QEMU WHPX) | | ⬜ |
| P12 | e1000 DHCP 完整耗时 | netman DHCP start → final rc | < 5s | | ⬜ |

## 自动化测量

在 dev_mode run_dev_tests 中追加性能测量代码：

```c
/* 在 run_dev_tests 开头校准 TSC */
u64 tsc_freq = calibrate_tsc();
logl("[shell] TSC freq: <Hz>");

/* 各项测试包围 TSC */
u64 t0 = rdtsc();
/* ... 操作 ... */
u64 t1 = rdtsc();
u64 ms = (t1 - t0) * 1000 / tsc_freq;
logl("[shell] perf: <name> = <ms>ms");
```

输出到串口日志，由 [ISO/run_tests.bat](../../ISO/run_tests.bat) 收集汇总。

## 性能回归检测

```powershell
# 比较两次测试结果
$prev = Get-Content ISO/logs/perf-baseline-prev.json | ConvertFrom-Json
$curr = Get-Content ISO/logs/perf-baseline-curr.json | ConvertFrom-Json

foreach ($m in $curr.metrics) {
    $p = $prev.metrics | Where-Object name -eq $m.name
    if ($p -and $m.ms -gt $p.ms * 1.5) {
        Write-Host "[REGRESSION] $($m.name): $($p.ms)ms -> $($m.ms)ms (>50%)"
    }
}
```

## 自动化脚本

[tests/qemu/performance-baseline.ps1](../../../tests/qemu/performance-baseline.ps1) 以主机墙上时钟
轮询串口锚点，测量启动阶段耗时（正常启动 firstInit=`1\n0`，AUTOEXEC=ver/exit）：

```powershell
.\tests\qemu\performance-baseline.ps1            # 默认 3 轮取中位数
.\tests\qemu\performance-baseline.ps1 -Runs 5    # 自定义轮数
```

测量锚点与阶段换算：

| 阶段 | 起 → 止 | 对应指标 |
|---|---|---|
| ovmf_bootloader_ms | QEMU 进程启动 → `[UTSM] boot` | OVMF + Limine |
| utsm_drivers_ms | `[UTSM] boot` → `[UTSM] SELFTEST PASS` | P1（UTSM+16 驱动装载） |
| selftest_to_dsk_ms | SELFTEST → `[DSK] boot` | DSK 加载（FAT32 read + PIE load） |
| dsk_to_desktop_ms | `[DSK] boot` → `desktop ready` | DSK → 桌面就绪 |
| total_boot_ms | QEMU 启动 → `desktop ready` | 端到端总启动 |

结果写入 `.build_tmp\perf_baseline.json`（锚点绝对时刻 + 各轮数据 + 中位数 + 环境信息）。
本脚本只测量展示、不做阈值断言；轮询精度 ±200ms，QEMU 数值仅作回归参考。

未自动化项（需内核 TSC 打点日志，当前串口无锚点，保持 PENDING）：P2/P3/P6（单 ELF 加载计时）、
P4 ping RTT、P5 linux RTT、P7 桌面帧率、P8~P11（PE/VMM/Linux）、P12 DHCP 耗时。

## 已知限制

- QEMU 性能数据不等于真机，仅作回归参考
- WHPX vs TCG 性能差异巨大，必须固定加速方案
- 多次测量取中位数，避免单次抖动
