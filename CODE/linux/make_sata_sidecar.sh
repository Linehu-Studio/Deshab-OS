#!/usr/bin/env bash
# make_sata_sidecar.sh — 重建 .build_tmp/sata_fat32_dsk.img（AHCI 边车测试盘）
# 用法: sudo bash make_sata_sidecar.sh <SYSTEM_DIR> <OUT_IMG>
# 说明: 纯 FAT32（无 GPT），DSK 经 ahci0 provider 读 BPB。
#   只拷贝 DSK 运行期需要的子树（排除 boot/ EFI/ limine/ 等仅 Limine
#   ESP 需要的大文件），大小按内容自适应。需要 mkfs.vfat + loop mount。
set -euo pipefail

SRC="${1:?usage: $0 <SYSTEM_DIR> <OUT_IMG>}"
IMG="${2:?usage: $0 <SYSTEM_DIR> <OUT_IMG>}"

command -v mkfs.vfat >/dev/null || { echo "mkfs.vfat missing" >&2; exit 1; }

STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT

# 仅运行期子树；boot/EFI/limine 是 Limine ESP 专属（rootfs 镜像 GB 级）
for d in SYSTEM EFI limine; do
    [ -e "$SRC/$d" ] && cp -r "$SRC/$d" "$STAGE"/
done

need_kb=$(du -sk "$STAGE" | cut -f1)
size_mb=$(( need_kb / 1024 * 2 + 64 ))
[ "$size_mb" -lt 64 ] && size_mb=64

rm -f "$IMG"
truncate -s "${size_mb}M" "$IMG"
mkfs.vfat -F 32 -n DESHAB "$IMG" >/dev/null

MNT=$(mktemp -d)
mount -o loop "$IMG" "$MNT"
cp -r "$STAGE"/. "$MNT"/
sync
umount "$MNT"
rmdir "$MNT"
echo "[sidecar] rebuilt: $IMG (${size_mb}MB FAT32)"
