# Prometheus99 Lightweight HMI Runtime Makefile
# C99 Standard + LVGL Presentation Layer

# Any C99 MinGW-w64 GCC on PATH works (MSYS2 ucrt64/mingw64, etc.).
# Override with e.g. `make CC=C:/msys64/mingw64/bin/gcc.exe` if needed.
CC = gcc

# -ffunction-sections/-fdata-sections + --gc-sections drops unreferenced code;
# -s strips the symbol table; -fno-*-unwind-tables removes the C++ EH frames a
# pure-C app never uses. -Os beats -O2 by ~20 KB here and the rasterizer has
# ~30x headroom against the 33 ms frame budget, so size wins. Result: ~51 KB.
# The only runtime deps are Windows system DLLs (kernel32/user32/gdi32/winmm +
# the in-box UCRT api-sets), so no separate -static runtime is needed.
CFLAGS  = -std=c99 -Wall -Wextra -Os -Iinclude \
          -ffunction-sections -fdata-sections \
          -fno-asynchronous-unwind-tables -fno-unwind-tables
LDFLAGS = -s -Wl,--gc-sections -lgdi32 -luser32 -lwinmm

TARGET = prometheus99.exe
SRCS = src/layer0_hardware.c \
       src/layer1_hal.c \
       src/layer2_core.c \
       src/layer3_presentation.c \
       src/main.c

all: $(TARGET)

$(TARGET): $(SRCS) $(wildcard include/*.h)
	$(CC) $(CFLAGS) $(SRCS) -o $(TARGET) $(LDFLAGS)

clean:
	rm -f $(TARGET) src/*.o

.PHONY: all clean
