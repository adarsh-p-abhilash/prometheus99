@echo off
echo =================================================================
echo   Building Prometheus99: Lightweight HMI Runtime (C99 + LVGL)
echo =================================================================

set PATH=C:\msys64\mingw64\bin;%PATH%

echo [BUILD] Compiling C99 sources into standalone static binary...
gcc -std=c99 -Wall -Wextra -O2 -static -Iinclude ^
    src/layer0_hardware.c ^
    src/layer1_hal.c ^
    src/layer2_core.c ^
    src/layer3_presentation.c ^
    src/main.c ^
    -o prometheus99.exe ^
    -lgdi32 -luser32 -lwinmm

if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] Compilation failed with error code %ERRORLEVEL%
    exit /b %ERRORLEVEL%
)

echo [SUCCESS] Build succeeded! Created self-contained standalone prometheus99.exe
echo =================================================================
