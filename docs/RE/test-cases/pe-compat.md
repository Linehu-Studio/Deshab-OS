# PE/EXE 兼容层测试矩阵

> 对应计划 Phase B：PE32+ (64位 native) 与 PE32 (32位 x86emu32) 兼容层验证
> 入口：cmd.elf 通过 `ctx->reserved[4]` 取 pe_service

## 状态: ⬜ PENDING

## 测试程序

| 文件 | 类型 | 来源 | FAT32 8.3 名 | 路径 |
|---|---|---|---|---|
| hello64.exe | PE32+ (64位) | MinGW/MSVC 编译或 Python 生成 | `HELLO64 EXE` | SYSTEM/system/deshab64/tools/ |
| hello32.exe | PE32 (32位) | MinGW 交叉编译或 Python 生成 | `HELLO32 EXE` | SYSTEM/system/deshab64/tools/ |

## 测试矩阵

| # | 测试项 | 命令 | 预期输出 | 状态 | Bug# |
|---|---|---|---|---|---|
| B2.1 | PE32+ 加载执行 | `pe HELLO64.EXE` | `Hello from PE32+!` | ⬜ | |
| B2.2 | PE32 加载执行（解释器） | `pe HELLO32.EXE` | `Hello from PE32!` | ⬜ | |
| B2.3 | PE 头解析 | `peinfo HELLO64.EXE` | `machine=0x8664 entry=0x... image_base=0x...` | ⬜ | |
| B2.4 | 文件不存在错误路径 | `pe NONEXIST.EXE` | `Error: file not found` | ⬜ | |
| B2.5 | 无参用法提示 | `pe` | 显示用法 | ⬜ | |
| B2.6 | ExitProcess 返回 | (执行后) | cmd 接管，longjmp 跳回 pe_service_run | ⬜ | |
| B2.7 | PE32 头解析 | `peinfo HELLO32.EXE` | `machine=0x14c entry=0x... image_base=0x00400000` | ⬜ | |

## 关键 ABI 验证

| ABI | 验证方法 | 状态 |
|---|---|---|
| `ctx->reserved[4]` 持 pe_service* | [cmd/main.c:825](../../CODE/cmd/main.c#L825) | ⬜ |
| `PE_SERVICE_MAGIC` 校验 | [cmd/main.c:830](../../CODE/cmd/main.c#L830) | ⬜ |
| `g_pe_svc` 缓存 | [cmd/main.c:108](../../CODE/cmd/main.c#L108) | ⬜ |
| PE32+ IAT 填 ms_abi shim | [pe_loader.c](../../CODE/UTSM/pe/pe_loader.c) | ⬜ |
| PE32 IAT 填合成地址 0x00010000\|idx | [pe_loader.c](../../CODE/UTSM/pe/pe_loader.c) | ⬜ |
| x86emu32 拦截 CALL 合成地址 | [x86emu32.c](../../CODE/UTSM/pe/x86emu32.c) | ⬜ |

## 测试程序源码

### hello64.c (PE32+)

```c
#include <windows.h>
int main(void) {
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), "Hello from PE32+!\r\n", 19, NULL, NULL);
    return 0;
}
```

编译：`x86_64-w64-mingw32-gcc -o hello64.exe hello64.c` 或 `cl /Fe:hello64.exe hello64.c`

### hello32.c (PE32)

```c
#include <windows.h>
int main(void) {
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), "Hello from PE32!\r\n", 18, NULL, NULL);
    return 0;
}
```

编译：`i686-w64-mingw32-gcc -o hello32.exe hello32.c -Wl,--image-base=0x00400000`

### Fallback: Python 生成器

若无 MinGW/MSVC，使用 [CODE/tools/pe-samples/gen_pe.py](../../CODE/tools/pe-samples/gen_pe.py) 生成最小 PE 程序。

## 自动化脚本

[tests/qemu/pe-compat.ps1](../../../tests/qemu/pe-compat.ps1) 正常启动到 cmd.elf，
AUTOEXEC.BAT 自动执行 PE 命令序列：

```powershell
.\tests\qemu\pe-compat.ps1            # 复用现有镜像
.\tests\qemu\pe-compat.ps1 -Build     # 先全量构建再测
```

因两个已登记系统 bug（[Bug记录.txt](../Bug记录.txt) BUG-20260801-006/007），脚本拆三个独立会话：

- **会话 A (pe64)**：B2.1 `pe HELLO64.EXE` → `[PE] PE32+ (64-bit)` →
  `[PE] running PE32+ natively` → `Hello from PE32+!`（自动断言 PASS）。
  随后的 ExitProcess 触发 BUG-006（setjmp/longjmp UB → 三重故障），会话终止；
  `[cmd] PE run end` 返回链标 MANUAL，日志出现即自动升级 PASS。
- **会话 B (pe32)**：B2.2 `pe HELLO32.EXE` → `[PE] running PE32 via x86emu32`
  解释器接管（自动断言 PASS，且无 emulator error / unimplemented）。
  首个 shim 调用 printf 触发 BUG-007（fn64 回退 guest 指针当 host 指针），
  会话终止；`Hello from PE32!` 标 MANUAL，出现即自动升级 PASS。
- **会话 C (peerr)**：B2.4 `pe NONEXIST.EXE` 错误路径（不产生 `[cmd] PE run begin`）
  + `exit` 正常返回 DSK（`[DSK] cmd.elf returned`，自动断言 PASS）。

WHPX 下任一会话未命中锚点自动以 `-Accel tcg` 重跑（可 `-Accel tcg` 直接指定）。
禁止锚点：`[PANIC]` / `[PE32] emulator error` / `[PE] unimplemented import`。

B2.3/B2.7（peinfo）与 B2.5（无参用法提示）仅写帧缓冲（cmd 非 dev_mode 无串口镜像），标 MANUAL。
PE32 解释器指令级覆盖率另见 [x86emu32-coverage.ps1](../../../tests/qemu/x86emu32-coverage.ps1)。

### GUI 应用端到端：deaicup-e2e.ps1

[tests/qemu/deaicup-e2e.ps1](../../../tests/qemu/deaicup-e2e.ps1) 验证 Deaicup
（Rust/egui 移植的原始 Win32 API PE64 GUI 应用）端到端：AUTOEXEC `pe DEAICUP.EXE` 启动后
经 monitor 驱动键鼠并断言串口锚点 + screendump 像素：

- **9 锚点顺序链**：`[cmd] cmdline: DEAICUP.EXE` → `[PE] running PE32+ natively` →
  `[deaicup] mainCRTStartup` → `[shim] RegisterClassExW ok` → `[shim] CreateWindowExW ok` →
  `[shim] CreateDIBSection ok` → `[dc] Context::default ok` →
  `[shim] BitBlt first frame on screen` → `[dc] bitblt ok`
- **首帧像素分析**：非全黑（nonBlack≥100000）、颜色种数 ≥32、GUI 窗口矩形检测
  （窗口主体色 #1B1B1B 行扫描实心段 → 800x508 包围盒、填充率 ≥75%）
- **鼠标**：AUX 初始化 + mouse_move → `[shim] mpkt`（WM_MOUSEMOVE）、
  mouse_button → `[shim] LDOWN/LUP` 边沿（WM_LBUTTONDOWN/UP）、
  两次 screendump 光标区像素差异（8..60000 采样点）
- **键盘**：sendkey → `[shim] KEYDOWN/CHAR/KEYUP`（WM_KEYDOWN/WM_CHAR/WM_KEYUP）
- **退出链**：Esc → WM_CLOSE → `[deaicup] quit, ExitProcess(0)` →
  `[PE] ExitProcess code=` → `[cmd] PE run end`（回归 BUG-006 长运行返回链、BUG-005 异常噪声）
- 禁止锚点：`[PANIC]` / `[IDT] exception` / `Unimplemented handler`

WHPX 未命中首帧锚点自动以 `-Accel tcg` 重跑（可 `-Accel tcg` 直接指定）；monitor 固定 45454。
证据帧输出 `.build_tmp/tests/deaicup-frame.png`。

## 已知限制

- pe_shim 实现的 Win32 API 子集有限，测试程序仅使用 WriteFile + ExitProcess
- x86emu32 指令覆盖率参见 [x86emu32-coverage.md](x86emu32-coverage.md)
- TLS / IFUNC / lazy binding 不支持（DKM ABI 约束）
