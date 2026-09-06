# KDE Plasma guest rootfs

`build_extra_rootfs.sh` has two modes:

- Default (`KDE_FULL=0`): build the existing 1 GiB persistence volume.
- Opt-in (`KDE_FULL=1`): use Arch `pacstrap` to build a complete KDE Plasma
  environment inside `linux-extra-rootfs.img`.

No KDE packages are downloaded unless `KDE_FULL=1` is explicitly set.

## Build

Run the full build from an Arch Linux environment with
`arch-install-scripts`, working pacman mirrors, and a valid keyring:

```bash
sudo pacman -S --needed arch-install-scripts
sudo KDE_FULL=1 IMG_SIZE_MB=8192 \
  bash CODE/linux/build_extra_rootfs.sh
```

The default size for `KDE_FULL=1` is 8192 MiB. The complete
`kde-applications-meta` suite can grow over time, so a larger value such as
12288 MiB may be needed. Values below 8192 MiB are allowed but produce a
warning.

Useful build settings:

```text
IMG_SIZE_MB=12288             image capacity in MiB
EXTRA_ROOTFS_WORK=/native/fs  native-Linux work directory
KDE_PACSTRAP=/usr/bin/pacstrap
KDE_PACMAN_CONF=/path/to/pacman.conf
KDE_USER=deshab
KDE_UID=1000
KDE_EXTRA_PACKAGES="pkg1 pkg2"
```

`KDE_PACMAN_CONF` can point at a project-controlled mirror/snapshot
configuration when package-version reproducibility is required. Without it,
the build intentionally follows the Arch host's current repositories and
keyring.

The package set includes `plasma-meta`, the full
`kde-applications-meta`, `plasma-workspace`, SDDM, Wayland, XWayland,
Mesa/LLVM llvmpipe, Vulkan lavapipe (`vulkan-swrast`), D-Bus, systemd user
units, input/seat support, portals, and Noto CJK/emoji fonts. Current Arch
ships the Wayland session in `plasma-workspace`; the obsolete
`plasma-wayland-session` package is intentionally not requested.

## Storage and attachment

The output remains:

```text
SYSTEM/boot/linux-extra-rootfs.img
```

That path is the existing build staging convention, not a claim that the full
image fits in the boot partition. A full KDE image must **not** be copied into
the 2 GiB ESP or enabled as a large Limine boot module. It must later be backed
by external storage and attached to the Linux guest through the existing
virtio-blk extra-rootfs path (normally `/dev/vdc`). Transporting that large
backing image is separate from this preparation script.

Allocate at least 4 GiB of guest RAM for Plasma; 6-8 GiB is preferable when
running the complete application suite.

## Starting the session

The current guest mounts the extra volume at `/mnt/persist` while retaining
its minimal read-only root. Start the prepared chroot from the guest console:

```bash
sudo /mnt/persist/usr/local/bin/deshab-kde-session
```

The launcher:

- bind-mounts `/dev`, `/proc`, `/sys`, and `/run` into the prepared root;
- creates the user's `XDG_RUNTIME_DIR` with safe ownership and permissions;
- starts a private D-Bus session using Plasma's current Arch wrapper;
- starts `startplasma-wayland` as the unprivileged `deshab` user;
- defaults to Mesa llvmpipe/lavapipe software rendering; and
- returns to the existing guest console on failure or logout.

Logs are written to:

```text
/mnt/persist/var/log/deshab-kde-session.log
```

Software rendering can be disabled for a working accelerated virtual GPU:

```bash
sudo DESHAB_KDE_SOFTWARE_RENDERING=0 \
  /mnt/persist/usr/local/bin/deshab-kde-session
```

The image contains SDDM and systemd user-session support, but neither replaces
the current custom guest PID 1. The prepared user selects Plasma's script-based
startup fallback for that environment. SDDM is deliberately not enabled, so a
broken graphical session cannot remove the guest console or Deshab native
desktop fallback.
