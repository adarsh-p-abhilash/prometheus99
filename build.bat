@echo off
echo =================================================================
echo   Building Prometheus99: Lightweight HMI Runtime (C99 + LVGL)
echo =================================================================

REM Prefer an MSYS2 MinGW-w64 GCC if present; otherwise rely on gcc already on PATH.
if exist C:\msys64\ucrt64\bin\gcc.exe set PATH=C:\msys64\ucrt64\bin;%PATH%
if exist C:\msys64\mingw64\bin\gcc.exe set PATH=C:\msys64\mingw64\bin;%PATH%

echo [BUILD] Compiling C99 sources into a compact standalone binary...
gcc -std=c99 -Wall -Wextra -O2 -Iinclude ^
    -ffunction-sections -fdata-sections ^
    -fno-asynchronous-unwind-tables -fno-unwind-tables ^
    src/layer0_hardware.c ^
    src/layer1_hal.c ^
    src/layer2_core.c ^
    src/layer3_presentation.c ^
    src/main.c ^
    -o prometheus99.exe ^
    -s -Wl,--gc-sections -lgdi32 -luser32 -lwinmm

if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] Compilation failed with error code %ERRORLEVEL%
    exit /b %ERRORLEVEL%
)

echo [SUCCESS] Build succeeded! prometheus99.exe depends only on Windows system DLLs.
echo =================================================================
