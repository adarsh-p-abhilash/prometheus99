# Prometheus99 Lightweight HMI Runtime Makefile
# C99 Standard + Native LVGL Presentation Engine

CC ?= C:/msys64/mingw64/bin/gcc.exe
CFLAGS = -std=c99 -Wall -Wextra -Os -s -Iinclude
LDFLAGS = -Wl,--stack,131072 -lgdi32 -luser32 -lwinmm

TARGET = prometheus99.exe
SRCS = src/lvgl.c \
       src/layer0_hardware.c \
       src/layer1_hal.c \
       src/layer2_core.c \
       src/layer3_presentation.c \
       src/main.c

OBJS = $(SRCS:.c=.o)

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) $(SRCS) -o $(TARGET) $(LDFLAGS)

clean:
	rm -f $(TARGET) src/*.o

.PHONY: all clean
