# PBL build: local STM32CubeCLT GCC, no PlatformIO, no downloads.
TOOLS ?= /opt/st/stm32cubeclt_1.21.0/GNU-tools-for-STM32/bin
CC      = $(TOOLS)/arm-none-eabi-gcc
OBJCOPY = $(TOOLS)/arm-none-eabi-objcopy
OBJDUMP = $(TOOLS)/arm-none-eabi-objdump
SIZE    = $(TOOLS)/arm-none-eabi-size

CFLAGS = -mcpu=cortex-m0 -mthumb -Os -std=c11 -Wall -Wextra -Werror \
         -ffreestanding -fno-builtin -ffunction-sections -fdata-sections \
         -fno-unwind-tables -fno-asynchronous-unwind-tables -fno-jump-tables
LDFLAGS = -nostdlib -nostartfiles -Wl,--gc-sections -Wl,-T,pbl.ld -Wl,-Map,build/pbl.map

SRC = pbl_entry_f0.c pbl_core.c

all: build/pbl.bin

build/pbl.elf: $(SRC) pbl.h hal_f0.h pbl.ld | build
	$(CC) $(CFLAGS) $(LDFLAGS) -Wl,--entry=pbl_entry_app -o $@ $(SRC)

build/pbl.bin: build/pbl.elf
	$(OBJCOPY) -O binary $< $@
	$(OBJDUMP) -d $< > build/pbl.lst
	$(SIZE) -A $<
	@sz=$$(stat -c %s $@); echo "PBL size: $$sz / 1024 B"; test $$sz -le 1024

build:
	mkdir -p build

clean:
	rm -rf build

.PHONY: all clean
