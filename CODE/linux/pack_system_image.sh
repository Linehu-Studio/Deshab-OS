#!/usr/bin/env bash
#
# Pack SYSTEM/ into a GPT disk:
#   p1  FAT32 ESP  (EF00)  /EFI /limine /boot /driver /system
#       Limine 10 cannot read ext4; every boot(): module must live here.
#   p2  ext4       (8300)  the rest of SYSTEM/ (and a copy of driver/system)
#
# Usage:
#   sudo bash CODE/linux/pack_system_image.sh <SYSTEM_DIR> <OUTPUT_IMG>
#
# FAT32 still cannot hold a single file >= 4GiB. Keep linux-extra-rootfs.img
# under that limit while it lives in SYSTEM/boot.

set -Eeuo pipefail

if [[ $# -ne 2 ]]; then
    echo "Usage: $0 <SYSTEM_DIR> <OUTPUT_IMG>" >&2
    exit 2
fi

SYSTEM="$(cd "$1" && pwd)"
OUT="$2"
OUT_DIR="$(cd "$(dirname "$OUT")" && pwd)"
OUT="$OUT_DIR/$(basename "$OUT")"

# Must match SYSTEM/limine/limine.conf uuid(...) paths.
EXT4_UUID="64657368-6162-4000-8000-000000000002"
DISK_GUID="64657368-6162-4000-8000-000000000001"
ESP_PARTGUID="64657368-6162-4000-8000-0000000000b1"
EXT4_PARTGUID="64657368-6162-4000-8000-0000000000e2"

for tool in sfdisk mkfs.vfat mkfs.ext4 rsync losetup du; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "[pack] ERROR: missing tool: $tool" >&2
        exit 2
    fi
done

if [[ ! -d "$SYSTEM/boot" || ! -d "$SYSTEM/EFI" || ! -f "$SYSTEM/limine/limine.conf" ]]; then
    echo "[pack] ERROR: SYSTEM/ must contain boot/, EFI/, and limine/limine.conf" >&2
    exit 2
fi

bytes_of() {
    local p="$1"
    if [[ -e "$p" ]]; then
        du -sb --apparent-size "$p" | awk '{print $1}'
    else
        echo 0
    fi
}

align_up() {
    local val="$1"
    local align="$2"
    echo $(( (val + align - 1) / align * align ))
}

# Sum each SYSTEM child so 9p/du-on-parent cannot drop boot/.
boot_bytes="$(bytes_of "$SYSTEM/boot")"
efi_bytes="$(bytes_of "$SYSTEM/EFI")"
limine_bytes="$(bytes_of "$SYSTEM/limine")"
driver_bytes="$(bytes_of "$SYSTEM/driver")"
sys_bytes="$(bytes_of "$SYSTEM/system")"
esp_content=$((boot_bytes + efi_bytes + limine_bytes + driver_bytes + sys_bytes))

rest_bytes=0
shopt -s dotglob nullglob
for child in "$SYSTEM"/*; do
    name="$(basename "$child")"
    case "$name" in
        boot|EFI|limine) continue ;;
    esac
    rest_bytes=$((rest_bytes + $(bytes_of "$child")))
done
shopt -u dotglob nullglob

# FAT32/ext4 metadata slack: 15% + 64MiB, 1MiB aligned.
esp_need=$((esp_content + esp_content / 7 + 64 * 1024 * 1024))
ext4_need=$((rest_bytes + rest_bytes / 7 + 64 * 1024 * 1024))
[[ "$esp_need" -lt $((256 * 1024 * 1024)) ]] && esp_need=$((256 * 1024 * 1024))
# User-facing data partition: keep at least 2GiB even when SYSTEM/ is small.
[[ "$ext4_need" -lt $((2048 * 1024 * 1024)) ]] && ext4_need=$((2048 * 1024 * 1024))

MIB=$((1024 * 1024))
SECTOR=512
esp_bytes="$(align_up "$esp_need" "$MIB")"
ext4_bytes="$(align_up "$ext4_need" "$MIB")"
esp_sectors=$((esp_bytes / SECTOR))
ext4_sectors=$((ext4_bytes / SECTOR))
# 1MiB protective gap + backup GPT
gpt_front=2048
gpt_back=2048
total_sectors=$((gpt_front + esp_sectors + ext4_sectors + gpt_back))
total_bytes=$((total_sectors * SECTOR))

extra_bytes="$(bytes_of "$SYSTEM/boot/linux-extra-rootfs.img")"
echo "[pack] GPT dual partition (NOT a single FAT32 disk):"
echo "[pack]   p1 FAT32 ESP = Limine + /boot modules (Limine 10 cannot read ext4)"
echo "[pack]   p2 ext4      = rest of SYSTEM/  uuid=$EXT4_UUID"
echo "[pack] SYSTEM=$SYSTEM"
echo "[pack] ESP content=$((esp_content / 1024 / 1024)) MB  partition=$((esp_bytes / 1024 / 1024)) MB"
echo "[pack]   linux-extra-rootfs.img=$((extra_bytes / 1024 / 1024)) MB (must stay on p1 FAT32)"
echo "[pack] ext4 content=$((rest_bytes / 1024 / 1024)) MB  partition=$((ext4_bytes / 1024 / 1024)) MB"
echo "[pack] disk=$((total_bytes / 1024 / 1024)) MB  -> $OUT"

WORK="$(mktemp -d /var/tmp/deshab_pack.XXXXXX)"
IMG="$WORK/disk.img"
MNT_ESP="$WORK/mnt_esp"
MNT_EXT="$WORK/mnt_ext"
LOOP=""

cleanup() {
    set +e
    if [[ -n "${MNT_ESP:-}" ]]; then
        umount "$MNT_ESP" 2>/dev/null
    fi
    if [[ -n "${MNT_EXT:-}" ]]; then
        umount "$MNT_EXT" 2>/dev/null
    fi
    if [[ -n "${LOOP:-}" ]]; then
        losetup -d "$LOOP" 2>/dev/null
    fi
    rm -rf "$WORK"
}
trap cleanup EXIT

mkdir -p "$MNT_ESP" "$MNT_EXT"
# Prefer a sparse image so the 9p copy back to Windows is smaller when possible.
truncate -s "$total_bytes" "$IMG"

esp_start="$gpt_front"
ext4_start=$((esp_start + esp_sectors))

sfdisk --no-reread "$IMG" <<EOF
label: gpt
label-id: ${DISK_GUID}
first-lba: ${gpt_front}
sector-size: 512

start=${esp_start}, size=${esp_sectors}, type=C12A7328-F81F-11D2-BA4B-00A0C93EC93B, uuid=${ESP_PARTGUID}, name=DESHABBOOT
start=${ext4_start}, size=${ext4_sectors}, type=0FC63DAF-8483-4772-8E79-3D69D8477DE4, uuid=${EXT4_PARTGUID}, name=DESHAB
EOF

LOOP="$(losetup --find --show --partscan "$IMG")"
echo "[pack] loop=$LOOP"
echo "[pack] partition table:"
sfdisk -l "$IMG" || true

p1=""
p2=""
for _ in $(seq 1 25); do
    if [[ -b "${LOOP}p1" && -b "${LOOP}p2" ]]; then
        p1="${LOOP}p1"
        p2="${LOOP}p2"
        break
    fi
    # Some kernels expose /dev/loopXp1 only after partx.
    if command -v partx >/dev/null 2>&1; then
        partx --update "$LOOP" 2>/dev/null || true
    fi
    sleep 0.2
done
if [[ -z "$p1" || -z "$p2" ]]; then
    echo "[pack] ERROR: loop partitions did not appear under $LOOP" >&2
    ls -l "$LOOP"* 2>/dev/null || true
    exit 1
fi

# 4KiB clusters on a ~5GiB ESP; 32KiB if the ESP is 4GiB or larger.
fat_cluster_sectors=8
if [[ "$esp_bytes" -ge $((4096 * 1024 * 1024)) ]]; then
    fat_cluster_sectors=64
fi

mkfs.vfat -F 32 -n DESHABBOOT -s "$fat_cluster_sectors" "$p1" >/dev/null
mkfs.ext4 -F -q -L DESHAB -U "$EXT4_UUID" -m 1 "$p2"
echo "[pack] filesystems:"
blkid "$p1" "$p2" || true

mount "$p1" "$MNT_ESP"
mount "$p2" "$MNT_EXT"

mkdir -p "$MNT_ESP/EFI" "$MNT_ESP/limine" "$MNT_ESP/boot" "$MNT_ESP/driver" "$MNT_ESP/system"
echo "[pack] copying p1 FAT32 ESP (EFI/ limine/ boot/ driver/ system/)"
# vfat has no Unix owners/perms; -a would fail with rsync code 23.
rsync -rltD --info=progress2 "$SYSTEM/EFI/" "$MNT_ESP/EFI/"
rsync -rltD --info=progress2 "$SYSTEM/limine/" "$MNT_ESP/limine/"
cp -f "$SYSTEM/limine/limine.conf" "$MNT_ESP/limine.conf"
rsync -rltD --info=progress2 "$SYSTEM/boot/" "$MNT_ESP/boot/"
rsync -rltD --info=progress2 "$SYSTEM/driver/" "$MNT_ESP/driver/"
rsync -rltD --info=progress2 "$SYSTEM/system/" "$MNT_ESP/system/"
if [[ -d "$SYSTEM/user" ]]; then
    echo "[pack] copying user/ onto FAT32 ESP for native desktop icons"
    rsync -rltD --info=progress2 "$SYSTEM/user/" "$MNT_ESP/user/"
fi

echo "[pack] copying p2 ext4 data partition"
shopt -s dotglob nullglob
for child in "$SYSTEM"/*; do
    name="$(basename "$child")"
    case "$name" in
        boot|EFI|limine) continue ;;
    esac
    rsync -a --info=progress2 "$child" "$MNT_EXT/"
done
shopt -u dotglob nullglob

sync
umount "$MNT_ESP"
umount "$MNT_EXT"
losetup -d "$LOOP"
LOOP=""

echo "[pack] writing $OUT"
mkdir -p "$OUT_DIR"
# Sparse copy keeps the Windows-side file smaller when the host supports it.
cp --sparse=always "$IMG" "$OUT"
sync

echo "[pack] done: $OUT"
echo "[pack]   GPT p1 FAT32 ESP (Limine/boot) + p2 ext4 uuid=$EXT4_UUID"
