#!/usr/bin/env bash
# Restore the PROVEN soname families into xorg-libs from plasma-libs.
# The original 137-file set (proven in the 12:18 Xorg run) had exactly
# these soname prefixes; everything else stays in plasma-libs.
set -Eeuo pipefail
PROVEN_SONAMES="libXau libXdmcp libXfont2 libbrotlicommon libbrotlidec libbrotlienc libbz2 libcap-ng libcom_err libdbus-1 libdrm libdrm_amdgpu libdrm_intel libdrm_nouveau libdrm_radeon libevdev libffi libfontenc libfreetype libgmp libgssapi_krb5 libhogweed libinput libk5crypto libkadm5clnt libkadm5srv libkeyutils libkrb5 libkrb5support liblzma libmtdev libnettle libpciaccess libpixman-1 libpng libpng16 libsystemd libtirpc libudev libunwind libxcvt libxshmfence libz"
mkdir -p /home/deshab/xorg-libs
cd /home/deshab/plasma-libs
back=0
for f in *; do
    base="${f%%.so*}"
    keep=0
    for s in $PROVEN_SONAMES; do
        if [ "$base" = "$s" ]; then keep=1; break; fi
    done
    if [ "$keep" = 1 ]; then
        mv -f "$f" /home/deshab/xorg-libs/
        back=$((back + 1))
    fi
done
echo "restored_to_xorg=$back"
echo "xorg-libs: $(ls /home/deshab/xorg-libs | wc -l) files"
echo "plasma-libs: $(ls /home/deshab/plasma-libs | wc -l) files"
echo '--- xorg-libs sonames ---'
ls /home/deshab/xorg-libs | sed 's/\.so.*//' | sort -u | tr '\n' ' '; echo