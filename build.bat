@echo off
echo =================================================================
echo   Building Prometheus99: Lightweight HMI Runtime (C99 + LVGL)
echo =================================================================

set PATH=C:\msys64\mingw64\bin;%PATH%

echo [BUILD] Compiling C99 sources with size minimization (-Os -s -fno-ident -fno-asynchronous-unwind-tables)...
gcc -std=c99 -Wall -Wextra -Os -s -fno-ident -fno-asynchronous-unwind-tables -Iinclude ^
    src/lvgl.c ^
    src/layer0_hardware.c ^
    src/layer1_hal.c ^
    src/layer2_core.c ^
    src/layer3_presentation.c ^
    src/main.c ^
    -o prometheus99.exe ^
    -Wl,--stack,16384 ^
    -lgdi32 -luser32 -lwinmm

if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] Compilation failed with error code %ERRORLEVEL%
    exit /b %ERRORLEVEL%
)

echo [SUCCESS] Build succeeded! Created RAM-optimized prometheus99.exe
echo =================================================================
