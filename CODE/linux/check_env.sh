#!/bin/bash
#
# check_env.sh — Linux 内核构建环境依赖检查脚本
#
# 用途: 在 WSL/Linux 中运行，验证构建 Linux 6.6 LTS bzImage 所需依赖
#
# 使用:
#   ! wsl bash CODE/linux/check_env.sh
#
# 退出码:
#   0 = 所有依赖已满足
#   1 = 缺少依赖（脚本会打印安装命令）

set -e

echo "[check] Linux 内核构建环境检查"
echo "[check] ==========================="

# 仓库根目录：按脚本自身位置推导，不依赖当前工作目录，也不认死任何盘符路径。
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
echo "[check] 仓库根: $REPO_ROOT"

# 1. 检查操作系统
if [ -f /etc/os-release ]; then
    . /etc/os-release
    echo "[check] OS: $NAME $VERSION"
    if [ "$ID" != "ubuntu" ] && [ "$ID_LIKE" != "debian" ]; then
        echo "[check] WARNING: 非 Ubuntu/Debian，依赖名可能不同"
    fi
else
    echo "[check] WARNING: 无法识别操作系统"
fi

# 2. 检查必要包
echo "[check]"
echo "[check] 检查构建依赖..."
MISSING=()
for pkg in build-essential bc flex bison libelf-dev libssl-dev cpio gzip git; do
    if dpkg -s "$pkg" >/dev/null 2>&1; then
        echo "[check]   ✓ $pkg"
    else
        echo "[check]   ✗ $pkg (缺失)"
        MISSING+=("$pkg")
    fi
done

# 2b. 检查镜像打包工具（pack_system_image.sh 硬依赖，不在上面那组内核依赖里）
echo "[check]"
echo "[check] 检查镜像打包工具..."
for pkg in dosfstools e2fsprogs fdisk rsync; do
    if dpkg -s "$pkg" >/dev/null 2>&1; then
        echo "[check]   ✓ $pkg"
    else
        echo "[check]   ✗ $pkg (缺失)"
        MISSING+=("$pkg")
    fi
done

# 3. 检查 gcc 版本
echo "[check]"
echo "[check] 工具链版本:"
if command -v gcc >/dev/null 2>&1; then
    GCC_VER=$(gcc -dumpversion)
    echo "[check]   gcc: $GCC_VER"
    if [ "$(printf '%s\n' "11" "$GCC_VER" | sort -V | head -n1)" != "11" ]; then
        echo "[check]   WARNING: gcc 版本 < 11，Linux 6.6 可能需要 gcc 11+"
    fi
else
    echo "[check]   ✗ gcc 未安装"
    MISSING+=("gcc")
fi

if command -v make >/dev/null 2>&1; then
    echo "[check]   make: $(make --version | head -1 | awk '{print $3}')"
fi

# 4. 检查磁盘空间（需要 ~3GB）
echo "[check]"
PROJECT_ROOT="$REPO_ROOT"
if [ -d "$PROJECT_ROOT" ]; then
    AVAILABLE_KB=$(df "$PROJECT_ROOT" | awk 'NR==2 {print $4}')
    AVAILABLE_GB=$((AVAILABLE_KB / 1024 / 1024))
    echo "[check] 磁盘空间: ${AVAILABLE_GB}GB 可用 ($PROJECT_ROOT)"
    if [ "$AVAILABLE_GB" -lt 3 ]; then
        echo "[check]   ✗ 磁盘空间不足 3GB"
        MISSING+=("disk_space")
    else
        echo "[check]   ✓ 磁盘空间充足"
    fi
fi

# 5. 检查 WSL 能否访问当前仓库（不认死盘符路径）
echo "[check]"
if [ -d "$REPO_ROOT/CODE/linux" ] && [ -d "$REPO_ROOT/SYSTEM" ]; then
    echo "[check] ✓ WSL 可访问当前仓库 ($REPO_ROOT)"
else
    echo "[check] ✗ WSL 无法访问当前仓库 ($REPO_ROOT)"
    echo "[check]   请从仓库内运行本脚本，且 Windows 盘已挂载到 /mnt/<盘符>"
    MISSING+=("wsl_mount")
fi

# 6. 检查 git 网络（克隆 Linux 6.6 需要）
echo "[check]"
echo "[check] 测试 Git 网络连通..."
if timeout 10 git ls-remote https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git v6.6 >/dev/null 2>&1; then
    echo "[check]   ✓ kernel.org 可达"
else
    echo "[check]   ✗ kernel.org 不可达，尝试清华镜像..."
    if timeout 10 git ls-remote https://mirrors.tuna.tsinghua.edu.cn/git/linux.git v6.6 >/dev/null 2>&1; then
        echo "[check]   ✓ 清华镜像可达（build.sh 会自动使用）"
    else
        echo "[check]   ✗ 两个源都不可达，请检查网络/代理"
        MISSING+=("network")
    fi
fi

# 7. 总结
echo "[check]"
echo "[check] ==========================="
if [ ${#MISSING[@]} -gt 0 ]; then
    echo "[check] 缺失: ${MISSING[*]}"
    echo "[check]"
    echo "[check] 请运行以下命令安装依赖:"
    echo "[check]   sudo apt update"
    echo "[check]   sudo apt install -y build-essential bc flex bison libelf-dev libssl-dev cpio gzip git"
    echo "[check]   sudo apt install -y dosfstools e2fsprogs fdisk rsync"
    exit 1
fi

echo "[check] ✓ 所有依赖已满足，可以构建 Linux 内核"
echo "[check]"
echo "[check] 下一步执行:"
echo "[check]   cd $REPO_ROOT/CODE/linux && chmod +x build.sh && ./build.sh"
exit 0
