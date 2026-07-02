@echo off
setlocal

set "SCRIPT_DIR=%~dp0"
set "IMG=%SCRIPT_DIR%deshab.img"
set "SATA_IMG=%SCRIPT_DIR%..\.build_tmp\sata_fat32_dsk.img"

if not exist "%IMG%" (
    echo [qemu] Image not found: %IMG%
    echo [qemu] Run ..\build.bat first.
    exit /b 1
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

echo [qemu] Using: %QEMU%
echo [qemu] UEFI:  %OVMF%
echo [qemu] Image: %IMG%
if exist "%SATA_IMG%" (
    echo [qemu] SATA:  %SATA_IMG%
    "%QEMU%" ^
        -machine q35 ^
        -m 512M ^
        -cpu max ^
        -serial stdio ^
        -drive if=pflash,format=raw,readonly=on,file="%OVMF%" ^
        -drive format=raw,file="%IMG%",if=virtio ^
        -drive id=sata0,format=raw,file="%SATA_IMG%",if=none ^
        -device ide-hd,drive=sata0,bus=ide.0 ^
        -netdev user,id=net0 ^
        -device e1000,netdev=net0,mac=52:54:00:12:34:56 ^
        -boot menu=on
) else (
    echo [qemu] SATA image not found, booting without block device
    "%QEMU%" ^
        -machine q35 ^
        -m 512M ^
        -cpu max ^
        -serial stdio ^
        -drive if=pflash,format=raw,readonly=on,file="%OVMF%" ^
        -drive format=raw,file="%IMG%",if=virtio ^
        -netdev user,id=net0 ^
        -device e1000,netdev=net0 ^
        -boot menu=on
)

endlocal
