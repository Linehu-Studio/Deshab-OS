# Deshab-OS cross-platform build (F3: 疯不应绑 Windows)
#
# Linux/macOS (native or WSL):
#   make dev          # build kernels+apps+drivers, inject dev config, pack ISO/deshab-dev.img
#   make release      # same, release config -> ISO/deshab-release.img
#   make realtest     # real-machine test config
#   make both         # dev + release
#   make kernels apps drivers   # compile only
#
# Requirements: clang + ld.lld (LLVM), GNU make, python3 + Pillow (logo),
#   sudo/root + mkfs.vfat + mkfs.ext4 (pack step, via CODE/linux/pack_system_image.sh).
#
# Windows: keep using build.bat / build.ps1 (this Makefile's pack step needs POSIX paths).
#
# Overrides:
#   make CC=clang-15 LD=ld.lld-15 PYTHON=python3
#   make PACK="sudo bash"        # pack command prefix (default: sudo bash)

CC      ?= clang
LD      ?= ld.lld
PYTHON  ?= python3
PACK    ?= sudo bash

ROOT    := $(CURDIR)
SYSTEM  := $(ROOT)/SYSTEM
ISO     := $(ROOT)/ISO
BTMP    := $(ROOT)/.build_tmp

MAKEFLAGS += -j

# ---- sub-builds (each MAKEFILE outputs straight into SYSTEM/) ----
KERNELS := CODE/UTSM CODE/dsk
APPS    := CODE/firstInit CODE/login CODE/mouse CODE/netman CODE/shell CODE/desktop CODE/cmd \
           $(addprefix CODE/tools/,editor fileman browser curl ping settings)

# ---- DKM drivers: src -> SYSTEM/driver/... ----
DRV_FLAGS := -target x86_64-unknown-none -std=c11 -ffreestanding -fno-builtin \
             -fno-stack-protector -fno-pic -fno-pie -mcmodel=kernel -mno-red-zone \
             -mno-sse -mno-sse2 -mno-mmx -msoft-float -m64 -Wall -Wextra -O2

DRV_SRCS := CODE/DKM/console_fb/console_fb.c:SYSTEM/driver/console/console_fb.drv \
            CODE/DKM/apic/apic.c:SYSTEM/driver/platform/apic.drv \
            CODE/DKM/nvme/nvme.c:SYSTEM/driver/block/nvme.drv \
            CODE/DKM/ehci/ehci.c:SYSTEM/driver/block/ehci.drv \
            CODE/DKM/xhci/xhci.c:SYSTEM/driver/block/xhci.drv \
            CODE/DKM/e1000/e1000.c:SYSTEM/driver/net/e1000.drv \
            CODE/DKM/virtio_net/virtio_net.c:SYSTEM/driver/net/virtio_net.drv \
            CODE/DKM/ath9k/ath9k.c:SYSTEM/driver/net/ath9k.drv

DRV_OUTS := $(foreach s,$(DRV_SRCS),$(ROOT)/$(word 2,$(subst :, ,$(s))))

.PHONY: all both dev release realtest kernels apps drivers \
        inject-dev inject-release inject-realtest \
        iso-dev iso-release iso-realtest clean

all: both

both: dev release

kernels:
	@for d in $(KERNELS); do echo "[build] $$d"; $(MAKE) -C $$d -f MAKEFILE CC="$(CC)" LD="$(LD)" || exit 1; done

dsk-logo:
	@if [ -f ohMyLogo.png ]; then \
	  if [ ohMyLogo.png -nt CODE/UTSM/kernel/panic_logo_data.inc ] || [ ! -f CODE/UTSM/kernel/panic_logo_data.inc ]; then \
	    echo "[build] Generating panic_logo_data.inc from ohMyLogo.png..."; \
	    $(PYTHON) CODE/UTSM/kernel/gen_panic_logo.py ohMyLogo.png CODE/UTSM/kernel/panic_logo_data.inc || echo "[build] WARN: panic logo gen failed, using existing"; \
	  fi; \
	fi

apps:
	@for d in $(APPS); do \
	  if [ -f $$d/MAKEFILE ]; then echo "[build] $$d"; $(MAKE) -C $$d -f MAKEFILE CC="$(CC)" LD="$(LD)" || exit 1; \
	  else echo "[build] WARN: no MAKEFILE in $$d, skipped"; fi; done

drivers: $(DRV_OUTS)

$(ROOT)/SYSTEM/driver/%.drv: $(ROOT)/CODE/DKM/%.c
	@mkdir -p $(dir $@) $(BTMP)
	@echo "[build] DKM driver: $< -> $@"
	$(CC) $(DRV_FLAGS) -c $< -o $(BTMP)/$(notdir $<).o
	$(LD) -r -o $@ $(BTMP)/$(notdir $<).o

utsm:
	$(MAKE) -C CODE/UTSM -f MAKEFILE CC="$(CC)" LD="$(LD)"

dsk: dsk-logo
	$(MAKE) -C CODE/dsk -f MAKEFILE CC="$(CC)" LD="$(LD)"

# ---- config injection (mirrors build.ps1 Build-ImageVariant) ----
inject-%:
	@if [ -d build/configs/$* ]; then \
	  if [ ! -f build/configs/$*/FUCK ] || [ ! -f build/configs/$*/firstInit.txt ]; then \
	    echo "Missing FUCK/firstInit.txt template in build/configs/$*"; exit 1; fi; \
	  cp build/configs/$*/FUCK SYSTEM/system/deshab64/FUCK; \
	  cp build/configs/$*/firstInit.txt SYSTEM/system/user/use/firstInit.txt; \
	  echo "[build] Injected config variant=$* (FUCK + firstInit.txt)"; \
	else \
	  echo "[build] WARN: configs/$* absent, packaging SYSTEM as-is"; \
	fi

# ---- pack: GPT p1 FAT32 ESP + p2 ext4 (CODE/linux/pack_system_image.sh) ----
iso-%: inject-%
	@mkdir -p $(ISO)
	$(PACK) $(ROOT)/CODE/linux/pack_system_image.sh $(SYSTEM) $(ISO)/deshab-$*.img
	@echo "[build] $* image done: $(ISO)/deshab-$*.img"

dev: kernels apps drivers iso-dev
release: kernels apps drivers iso-release
realtest: kernels apps drivers iso-realtest

clean:
	@for d in $(KERNELS) $(APPS); do $(MAKE) -C $$d -f MAKEFILE clean >/dev/null 2>&1 || true; done
	@rm -rf $(BTMP)/*.o
	@echo "[build] clean done"
