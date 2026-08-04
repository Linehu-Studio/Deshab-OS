# 用户态程序功能矩阵

> 对应计划 Phase C4：FirstInit/login/desktop/shell/cmd + 5 个工具应用

## 状态: ⬜ PENDING

## 程序矩阵

| # | 程序 | ELF 路径 | 入口 | 关键命令/功能 | 验证点 | 状态 | Bug# |
|---|---|---|---|---|---|---|---|
| 1 | FirstInit | `system/user/use/FirstInit.elf` | 首启触发 | 计算机名/用户名/密码输入、空字段红字、网络配置 | SHA256 密码、user.conf 写入缓冲 | ⬜ | |
| 2 | login | `system/user/use/login.elf` | 次启触发 | 用户选择、密码输入、错误重试 | USER.CONF 解密、密码匹配 | ⬜ | |
| 3 | desktop | `system/deshab64/desktop.elf` | login 后 | 双击图标、窗口拖动、任务栏、内建 bash | 6 个工具启动、鼠标光标 | ⬜ | |
| 4 | shell | `system/deshab64/shell.elf` | desktop Terminal | ls/cat/cp/mv/rm/echo>/ping/curl/run/linux | dev_mode 自动测试全绿 | ⬜ | |
| 5 | cmd | `system/deshab64/cmd.elf` | desktop CMD | dir/type/copy/del + `pe <NAME.EXE>` | PE 兼容层调用 | ⬜ | |
| 6 | netman | `system/deshab64/network/netman.elf` | DSK 调度 | 读 conf.conf、应用网络配置 | DHCP/静态 IP | ⬜ | |
| 7 | mouseInit | `system/deshab64/mouse/mouseInit.elf` | DSK 调度 | 安全 stub | 不阻塞启动 | ⬜ | |
| 8 | editor | `system/deshab64/tools/editor.elf` | desktop 双击 | 文件打开/编辑/保存 | FAT32 写盘 | ⬜ | |
| 9 | fileman | `system/deshab64/tools/fileman.elf` | desktop 双击 | 浏览/打开/复制 | 目录遍历 | ⬜ | |
| 10 | browser | `system/deshab64/tools/browser.elf` | desktop 双击 | URL 输入/HTTP GET | net_stack 协议栈 | ⬜ | |
| 11 | curl | `system/deshab64/tools/curl.elf` | desktop 双击 | `curl http://...` | HTTP GET | ⬜ | |
| 12 | ping | `system/deshab64/tools/ping.elf` | desktop 双击 | `ping <ip>` | ICMP echo | ⬜ | |

## 启动链路

```
首启: utsm → DSK → mouseInit → netman → FirstInit → desktop
次启: utsm → DSK → login → desktop
dev_mode: utsm → DSK → shell(dev_tests) → Esc → login → desktop
desktop → 双击 Terminal → shell.elf
desktop → 双击 CMD → cmd.elf
shell → run FILEMAN.ELF → fileman.elf → Esc 返回 shell
shell → run BROWSER.ELF → browser.elf
shell → run EDITOR.ELF → editor.elf
shell → run PING.ELF → ping.elf
shell → run CURL.ELF → curl.elf
```

## 详细测试用例

### shell 命令测试

| 命令 | 参数 | 预期 | 状态 |
|---|---|---|---|
| `ls` | (无) | 列出根目录文件（DESHAB.ELF, SHELL.ELF 等） | ⬜ |
| `pwd` | (无) | `/` | ⬜ |
| `cat` | `README.TXT` | 显示文件内容 | ⬜ |
| `cp` | `a b` | 复制成功 | ⬜ |
| `mv` | `a b` | 移动成功 | ⬜ |
| `rm` | `a` | 删除成功 | ⬜ |
| `echo>` | `text > file` | 写入文件 | ⬜ |
| `ping` | `10.0.2.2` | 4/4 reply | ⬜ |
| `curl` | `http://10.0.2.2:8000/` | HTTP 200 + body | ⬜ |
| `run` | `PING.ELF` | 加载并执行 ping | ⬜ |
| `linux` | (无) | 显示兼容层状态 | ⬜ |
| `linux` | `ls` | 输出 initramfs 内容 | ⬜ |
| `help` | (无) | 显示命令列表 | ⬜ |
| `ver`/`version` | (无) | 显示版本 | ⬜ |

### desktop UI 测试

| 操作 | 预期 | 状态 |
|---|---|---|
| 双击 Terminal 图标 | 打开 shell 窗口 | ⬜ |
| 双击 CMD 图标 | 打开 cmd 窗口 | ⬜ |
| 双击 Editor 图标 | 打开 editor | ⬜ |
| 双击 File Manager 图标 | 打开 fileman | ⬜ |
| 双击 Browser 图标 | 打开 browser | ⬜ |
| 鼠标移动 | 光标跟随，无闪烁 | ⬜ |
| 窗口拖动 | 流畅，无撕裂 | ⬜ |
| 任务栏点击 | 窗口切换 | ⬜ |

## 自动化脚本

[tests/qemu/userapps-matrix.ps1](../../../tests/qemu/userapps-matrix.ps1) dev_mode 单会话覆盖
shell 内建命令与工具程序（dev_mode 串口镜像开启，全程可断言）：

```powershell
.\tests\qemu\userapps-matrix.ps1            # 复用现有镜像
.\tests\qemu\userapps-matrix.ps1 -Build     # 先全量构建再测
```

自动断言（两阶段，单会话；2026-08-04 BUG-20260801-001/002/003/008 全部修复后，
原 MANUAL 降级项已回收为自动化）：

1. **dev_tests 硬编码序列**：`[DSK:SCHED] decision: dev_mode -> shell.elf` →
   `[1] ls` → `[2] echo>` → `[3] cat` → `[4] cp` → `[6] mv` → `[8] rm` → `[10] ls`
   → `=== 自动测试完成 ===`（SATA 镜像无 test.bas 时走 fallback 分支）。
2. **交互阶段**（monitor sendkey 驱动）：`help` → `id`（断言 `uid=0(root) gid=0(root)`，
   BUG-001 修复回归守护）→ `pci` 存活验证 →
   `ping`（PATH 查找 `/bin/PING.ELF` → `[ping] boot`，输入 10.0.2.2 发四报文，
   Esc → `[ping] exit` → `[shell] path: tool returned`）→
   `run EDITOR.ELF`（`[editor] boot` → 按键插入 → Esc → `[editor] exit`，
   BUG-002 修复后行为，原"超限优雅拒绝"断言已升级）→
   `run FILEMAN.ELF`（`[fileman] boot` → 方向键选区移动 → Esc → `[fileman] exit`，
   BUG-003/008 修复后键盘路径）→
   `run BROWSER.ELF`（`[browser] boot` → Esc → `[browser] exit`）→
   Esc 回 DSK → cmd(AUTOEXEC=ver/exit) → `desktop ready`。
   锚点：`[shell] path: running /bin/ tool` / `[shell] path: tool returned` /
   `[shell] run: jumping to tool` / `[shell] run: tool returned` /
   `[ping|editor|fileman|browser] boot|exit`。

标 MANUAL 项（帧缓冲限定，无串口锚点）：ping ICMP reply/RTT 与四报文统计、
editor 编辑内容渲染与保存、fileman 列表渲染/进入子目录；
FirstInit/login/desktop UI（双击/拖动/任务栏，boot-regression B1 已覆盖向导按键）。

## 已知问题

- fileman.elf 之前触发过 QEMU 退出（访问未支持 boot context 字段），需观察是否复现
- 真实 TTF 字体未运行时光栅化，使用预渲染位图（已知设计选择，非 bug）
