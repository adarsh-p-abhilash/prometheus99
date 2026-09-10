# Prometheus99 Lightweight HMI Runtime Makefile
# C99 Standard + LVGL Presentation Layer

CC ?= C:/msys64/mingw64/bin/gcc.exe
CFLAGS = -std=c99 -Wall -Wextra -O2 -Iinclude
LDFLAGS = -lgdi32 -luser32 -lwinmm

TARGET = prometheus99.exe
SRCS = src/layer0_hardware.c \
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
