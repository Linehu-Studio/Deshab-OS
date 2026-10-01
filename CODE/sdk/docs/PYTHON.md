# CPython 应用（Linux guest）

## 当前架构

Deshab 的 CPython 路径只运行在 Arch Linux guest 中。原生 DSK 应用是
freestanding Ring 0 ELF，没有可供 CPython 使用的完整 libc、动态链接器、
解释器和标准库，因此不能把 CPython 当作原生 DSK 运行时。

`SYSTEM/lib` 中的 `libpython` 和扩展模块只是兼容层库集合的一部分，不构成
完整 Python 安装，也不应作为此路径的主运行时。解释器、标准库和扩展模块
必须来自同一套 Arch 软件包，使 `/usr/bin/python3` 与其 distro 库版本保持
一致。

运行前需要：

- Linux guest 内核、initrd 和 Arch `linux-rootfs.img` 已构建并在 Limine
  配置中启用；
- guest 已启动并且 `linux_compat` daemon 处于 ready/parked 状态；
- Arch rootfs 中已安装 distro Python；
- 示例源码已复制到 guest 的固定路径
  `/usr/local/share/deshab/python/hello.py`。

## 在 Arch rootfs 安装 Python

Arch 的安装命令是：

```sh
pacman -S python
```

当前 guest 把基础 rootfs 以只读方式挂载，所以不要在已启动 guest 中直接向
`/usr` 安装软件包。应在 WSL/Linux 的可写 rootfs staging tree 中安装，然后
重新打包。`build_arch_rootfs.sh` 默认 `ARCH_INSTALL_PYTHON=1`，会在 staging
里执行 `pacman -S python` 并写入 `hello.py`。也可单独跑：

```sh
sudo bash CODE/linux/install_guest_python.sh
# 或指向 extra-rootfs 的 KDE 树：
sudo ROOTFS=/root/extra_rootfs_work/mnt bash CODE/linux/install_guest_python.sh
```

使用 `build_arch_rootfs.sh` 的默认工作目录时，也可以手动执行：

```sh
cd /mnt/d/Code/DEAICUP/Deshab

# 首次运行会创建 /root/arch_rootfs_work/rootfs staging tree。
sudo bash CODE/linux/build_arch_rootfs.sh

ROOTFS=/root/arch_rootfs_work/rootfs
sudo mkdir -p "$ROOTFS/tmp/pacman/lib/sync" "$ROOTFS/tmp/pacman/cache/pkg"
printf 'nameserver 1.1.1.1\n' | sudo tee "$ROOTFS/tmp/resolv.conf" >/dev/null
sudo chroot "$ROOTFS" /usr/bin/pacman -Sy --noconfirm
sudo chroot "$ROOTFS" /usr/bin/pacman -S --noconfirm python

sudo install -D -m 0644 \
  SYSTEM/user/python/hello.py \
  "$ROOTFS/usr/local/share/deshab/python/hello.py"

# staging tree 已存在，第二次运行会保留以上内容并重新生成 rootfs image。
sudo bash CODE/linux/build_arch_rootfs.sh
```

若设置了 `ARCH_ROOTFS_WORK`，相应调整 `ROOTFS`。更新 Arch Python 时应通过
pacman 一起更新解释器和标准库；不要从 `SYSTEM/lib` 拼装或覆盖 distro
`libpython`。

## 脚本如何进入 guest

`SYSTEM/user/python/hello.py` 是系统盘中的示例源码。当前构建不会把
`SYSTEM/user/python/` 任意目录自动同步到 Linux rootfs；`linux_compat`
启动时的自动同步只覆盖专用内容（例如兼容库和 VSCode 模块）。因此，上节的
`install` 命令是当前确定性的发布步骤：在打包前把脚本写入 Arch staging
tree。

开发时也可以使用 Deshab shell 的 `dsl push`，但它的来源只支持 FAT32
根目录中的 8.3 文件名。先把脚本临时放到系统盘根目录并重建镜像：

```powershell
Copy-Item .\SYSTEM\user\python\hello.py .\SYSTEM\HELLO.PY
.\build.bat -Variant dev
```

启动后，在 Deshab shell 中复制到 guest 的可写目录：

```text
dsl /bin/mkdir -p /root/deshab/python
dsl push HELLO.PY /root/deshab/python/hello.py
dsl /usr/bin/env -u LD_LIBRARY_PATH /usr/bin/python3 /root/deshab/python/hello.py
```

此开发路径不是任意目录自动推送，且 `/root` 需要可写的 extra-rootfs。正式
注册项仍使用打包进基础 rootfs 的 `/usr/local/share/...` 路径。

## 桌面注册格式

当前 `SYSTEM/LINUXAPP.CNF` 由 `desktop.elf` 按以下格式解析：

```text
NAME|DISPLAY|/guest/executable|arg1 arg2
```

- `NAME` 是内部名，最多 11 个字符；只有精确的 `VSCODE` 有共享 IDE 特殊语义。
- `DISPLAY` 是图标名，最多 15 个字符。
- executable 必须是 guest 绝对路径；path 和 args 各最多 95 个字符。
- args 只按 ASCII 空格拆分，最多形成 14 个附加参数；不支持引号、转义或含
  空格的单个参数。
- `#` 必须位于行首才会被识别为注释。
- 当前 desktop 最多注册 4 个 Linux 应用。

Python 示例注册项为：

```text
PYTHON|Python|/usr/bin/env|-u LD_LIBRARY_PATH /usr/bin/python3 /usr/local/share/deshab/python/hello.py
```

desktop 的 `exec_async` 会注入
`LD_LIBRARY_PATH=/usr/lib/deshab`。注册项先运行 distro
`/usr/bin/env` 删除该变量，再 exec `/usr/bin/python3`，避免同步到
`SYSTEM/lib` 的局部 Python/glibc 文件优先于 Arch distro 库。`DISPLAY=:0`
仍会保留。

## 验证与限制

桌面图标走面向 GUI 的异步启动路径：它会准备 X server、切换到 IDE attach
页，并把子进程 stdout/stderr 重定向到 `/dev/null`。因此点击 Python 图标
不会显示 `hello.py` 的文本输出，也不代表脚本最终成功退出。

请用同步 `dsl` 路径验证，这会捕获 stdout/stderr 和退出码：

```text
dsl /usr/bin/env -u LD_LIBRARY_PATH /usr/bin/python3 /usr/local/share/deshab/python/hello.py
```

成功输出至少包含：

```text
DESHAB_PYTHON_READY
version=...
platform=...
```

其他当前限制：

- 只有 guest CPython；没有原生 DSK CPython ABI。
- `LINUXAPP.CNF` 没有 shell quoting，复杂命令应放进 guest 侧脚本。
- desktop 异步启动不返回应用输出和最终退出状态。
- 尚无 Python 专用 Deshab GUI、窗口、输入、通知或文件选择 API。
- CPython 版本由 Arch 仓库决定，应用不应假设 `SYSTEM/lib` 中的版本。

## 未来 IPC GUI 方向

后续可在 guest 提供版本化的 `deshab` Python 包，通过现有共享内存/ring
IPC 与 host desktop 服务通信。建议逐步提供窗口/Surface 生命周期、批量绘制
缓冲、键鼠事件队列、剪贴板、文件选择和通知 API。CPython 继续由 distro
维护；Python 包只封装稳定 IPC 消息，不直接链接 DSK，也不依赖
`SYSTEM/lib` 的局部 `libpython`。
