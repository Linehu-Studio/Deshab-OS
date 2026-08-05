#!/bin/bash
# run_desktop_host.sh — 在 Linux 下直接启动 Deshab 桌面进行测试
#
# 用法：
#   ./ISO/run_desktop_host.sh                # 编译并运行，默认 ISO/deshab.img
#   ./ISO/run_desktop_host.sh /path/to.img   # 指定镜像
#
# 依赖：libsdl2-dev（Debian/Ubuntu: sudo apt install libsdl2-dev）

set -e

# 定位项目根（本脚本位于 ISO/ 下）
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
IMG="${1:-$ROOT/ISO/deshab.img}"

# 检查 SDL2
if ! command -v sdl2-config >/dev/null 2>&1; then
    echo "[run] 错误：未找到 sdl2-config，请先安装 libsdl2-dev" >&2
    echo "      Debian/Ubuntu: sudo apt install libsdl2-dev" >&2
    echo "      Fedora:        sudo dnf install SDL2-devel" >&2
    exit 1
fi

# 检查镜像
if [ ! -f "$IMG" ]; then
    echo "[run] 错误：镜像不存在: $IMG" >&2
    echo "      请先在 Windows 下运行 build.bat 生成 ISO/deshab.img" >&2
    exit 1
fi

# 编译
echo "[run] 编译 desktop_host（$(basename "$IMG")）..."
make -C "$ROOT/CODE/desktop" -f Makefile.linux

# 运行
echo "[run] 启动桌面..."
exec "$ROOT/CODE/desktop/desktop_host" --img "$IMG"
