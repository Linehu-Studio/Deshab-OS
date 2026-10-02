@echo off
setlocal enabledelayedexpansion

set "SCRIPT_DIR=%~dp0"

REM ===== Variant selection: dev / release / realtest =====
REM Priority: argument > QEMU_VARIANT env > interactive menu > default dev
REM Usage: run_qemu.bat [dev|release|realtest]
set "VARIANT="
if not "%~1"=="" set "VARIANT=%~1"
if not defined VARIANT if defined QEMU_VARIANT set "VARIANT=%QEMU_VARIANT%"
if not defined VARIANT (
    echo [qemu] Select image variant to boot:
    echo   1. dev      ^(deshab-dev.img, debug all on, auto test^)
    echo   2. release  ^(deshab-release.img, desktop + login^)
    echo   3. realtest ^(deshab-realtest.img, real-machine safe options^)
    set /p CHOICE="Choice [1-3, Enter=dev]: "
    if "!CHOICE!"=="2" set "VARIANT=release"
    if "!CHOICE!"=="3" set "VARIANT=realtest"
    if not defined VARIANT set "VARIANT=dev"
)
REM Validate variant name (also accepts passing 1/2/3 as argument)
if "%VARIANT%"=="1" set "VARIANT=dev"
if "%VARIANT%"=="2" set "VARIANT=release"
if "%VARIANT%"=="3" set "VARIANT=realtest"
if not "%VARIANT%"=="dev" if not "%VARIANT%"=="release" if not "%VARIANT%"=="realtest" (
    echo [qemu] Unknown variant: %VARIANT% ^(expected dev / release / realtest^)
    exit /b 1
)

set "IMG=%SCRIPT_DIR%deshab-%VARIANT%.img"
set "SATA_IMG=%SCRIPT_DIR%..\.build_tmp\sata_fat32_dsk.img"
set "NVME_IMG=%SCRIPT_DIR%..\.build_tmp\nvme_test.img"

if not exist "%IMG%" (
    echo [qemu] Image not found: %IMG%
    echo [qemu] Run ..\build.bat first -- default builds both dev and release.
    echo [qemu] Or build selected only: build.bat -Variant %VARIANT%
    exit /b 1
)
echo [qemu] Variant: %VARIANT%

REM QEMU_SERIAL_LOG=path  : redirect serial to file instead of stdio.
REM Windows console QuickEdit selection can block stdio serial output and
REM freeze the whole VM mid-boot (appears as AHCI reads "stuck" at random LBA).
set "SERIAL_ARGS=-serial stdio"
if defined QEMU_SERIAL_LOG (
    if exist "%QEMU_SERIAL_LOG%" del "%QEMU_SERIAL_LOG%"
    set "SERIAL_ARGS=-serial file:%QEMU_SERIAL_LOG%"
    echo [qemu] Serial log: %QEMU_SERIAL_LOG%
)


set "QEMU="
for %%Q in (qemu-system-x86_64.exe qemu-system-x86_64) do (
    for /f "delims=" %%P in ('where %%Q 2^>nul') do (
        if not defined QEMU set "QEMU=%%P"
    )
)

if not defined QEMU if exist "C:\Program Files\qemu\qemu-system-x86_64.exe" set "QEMU=C:\Program Files\qemu\qemu-system-x86_64.exe"
if not defined QEMU if exist "C:\Program Files (x86)\qemu\qemu-system-x86_64.exe" set "QEMU=C:\Program Files (x86)\qemu\qemu-system-x86_64.exe"
if not defined QEMU if exist "C:\msys64\mingw64\bin\qemu-system-x86_64.exe" set "QEMU=C:\msys64\mingw64\bin\qemu-system-x86_64.exe"
if not defined QEMU if exist "C:\msys64\ucrt64\bin\qemu-system-x86_64.exe" set "QEMU=C:\msys64\ucrt64\bin\qemu-system-x86_64.exe"

if not defined QEMU (
    echo [qemu] qemu-system-x86_64 was not found.
    echo [qemu] Install QEMU or add it to PATH.
    exit /b 1
)

set "OVMF="
if exist "C:\Program Files\qemu\share\edk2-x86_64-code.fd" set "OVMF=C:\Program Files\qemu\share\edk2-x86_64-code.fd"
if not defined OVMF if exist "C:\Program Files\qemu\share\OVMF_CODE.fd" set "OVMF=C:\Program Files\qemu\share\OVMF_CODE.fd"
if not defined OVMF if exist "C:\msys64\mingw64\share\edk2\ovmf\OVMF_CODE.fd" set "OVMF=C:\msys64\mingw64\share\edk2\ovmf\OVMF_CODE.fd"
if not defined OVMF if exist "C:\msys64\ucrt64\share\edk2\ovmf\OVMF_CODE.fd" set "OVMF=C:\msys64\ucrt64\share\edk2\ovmf\OVMF_CODE.fd"

if not defined OVMF (
    echo [qemu] UEFI firmware was not found.
    echo [qemu] Expected edk2-x86_64-code.fd or OVMF_CODE.fd under QEMU install directory.
    exit /b 1
)

REM ===== Accelerator selection =====
REM QEMU_ACCEL env var can override: "whpx" / "tcg"
REM Default: WHPX for performance; TCG as fallback.
REM NOTE: WHPX does NOT support nested VMX. CPUID reports vmx but VMXON
REM       causes a fatal hypervisor exit. When QEMU_ACCEL=whpx, we disable
REM       the VMX CPUID flag so UTSM gracefully skips VMX init.
REM       Set QEMU_ACCEL=tcg for full software emulation (also no VMX).
if not defined QEMU_ACCEL set "QEMU_ACCEL=whpx"

REM Detect whether Linux compat layer is enabled (bzImage present in IMG)
set "LINUX_ENABLED=0"
findstr /C:"module_path: boot():/boot/linux-bzImage" "%SCRIPT_DIR%..\SYSTEM\limine\limine.conf" >nul 2>&1
if not errorlevel 1 (
    set "LINUX_ENABLED=1"
)

REM Build CPU model string: disable VMX under WHPX (no nested virtualization)
set "QEMU_CPU=qemu64"
if "%QEMU_ACCEL%"=="whpx" (
    set "QEMU_CPU=qemu64,-vmx"
)

echo [qemu] Using: %QEMU%
echo [qemu] UEFI:  %OVMF%
echo [qemu] Image: %IMG%
echo [qemu] Accel: %QEMU_ACCEL%
echo [qemu] CPU:   %QEMU_CPU%
if "%LINUX_ENABLED%"=="1" (
    echo [qemu] Linux compat: ENABLED (modules present)
    if "%QEMU_ACCEL%"=="whpx" (
        echo [qemu]   WARNING: WHPX does not support nested VMX
        echo [qemu]   Linux guest will NOT run under WHPX; VMX CPUID flag disabled
        echo [qemu]   For VSCode integration path ^(Linux guest + virtio-gpu + VSCode^):
        echo [qemu]     Use WSL2 + KVM instead — run ISO\run_qemu_kvm.sh from WSL2 shell
        echo [qemu]     Requires Windows 11 Pro/Enterprise ^(nested VMX^) or native Linux host
        echo [qemu]   Or set QEMU_ACCEL=tcg for full software emulation ^(also no VMX^)
    ) else if "%QEMU_ACCEL%"=="tcg" (
        echo [qemu]   WARNING: TCG does not support VMX
        echo [qemu]   Linux guest will NOT run under TCG
        echo [qemu]   For VSCode integration path, use WSL2 + KVM ^(run ISO\run_qemu_kvm.sh^)
    ) else (
        echo [qemu]   VMX may be available for Linux guest
    )
) else (
    echo [qemu] Linux compat: disabled (build via CODE/linux/build.sh to enable)
)
REM NVMe test disk (generated by build.ps1, 64MB raw with DESHABNVME0 signature at LBA0)
set "NVME_OPTS="
if exist "%NVME_IMG%" (
    echo [qemu] NVMe:  %NVME_IMG%
    set "NVME_OPTS=-drive if=none,id=nvme0,file=%NVME_IMG%,format=raw -device nvme,drive=nvme0,serial=deadbeef"
)

REM USB xHCI test disk (64MB raw image for USB Mass Storage testing)
set "USB_IMG=%SCRIPT_DIR%..\.build_tmp\usb_test.img"
set "USB_OPTS="
if exist "%USB_IMG%" (
    echo [qemu] USB:   %USB_IMG%
    set "USB_OPTS=-device qemu-xhci,id=xhci0 -drive id=usb0,if=none,file=%USB_IMG%,format=raw -device usb-storage,bus=xhci0.0,drive=usb0"
)

REM ===== Boot disk attach =====
REM Main disk must use explicit ide-hd + bootindex=1: the DSK/AHCI block
REM provider only registers the first ready SATA port, so the GPT image
REM (p1 FAT32 ESP + p2 ext4) has to live on SATA for DSK to see ext4 at all.
REM bootindex=1 injects the fw_cfg bootorder entry so OVMF always picks it.
set "OSDISK_OPTS=-drive if=none,id=osdisk,format=raw,file="%IMG%" -device ide-hd,drive=osdisk,bus=ide.0,bootindex=1"

if exist "%SATA_IMG%" (
    echo [qemu] SATA:  %SATA_IMG%
    "%QEMU%" -accel %QEMU_ACCEL% -machine q35 -m 12G -cpu %QEMU_CPU% %SERIAL_ARGS% -drive if=pflash,format=raw,readonly=on,file="%OVMF%" %OSDISK_OPTS% -drive id=sata0,format=raw,file="%SATA_IMG%",if=none -device ide-hd,drive=sata0,bus=ide.1 %NVME_OPTS% %USB_OPTS% -netdev user,id=net0 -device e1000,netdev=net0,mac=52:54:00:12:34:56 -boot menu=on
) else (
    echo [qemu] SATA image not found, booting without block device
    "%QEMU%" -accel %QEMU_ACCEL% -machine q35 -m 12G -cpu %QEMU_CPU% %SERIAL_ARGS% -drive if=pflash,format=raw,readonly=on,file="%OVMF%" %OSDISK_OPTS% %NVME_OPTS% %USB_OPTS% -netdev user,id=net0 -device e1000,netdev=net0 -boot menu=on
)

endlocal
