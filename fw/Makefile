# MultiTX MicroBlaze firmware.
#
#   make            build build/multitx_fw.elf and .bin
#   make load       load it over the USB debug bridge and start it
#   make console    attach to the firmware console
#
# Uses the MicroBlaze GCC from Vitis 2022.2 (no Xilinx BSP).

VITIS ?= /opt/AMD/2022.2/Vitis/2022.2
CROSS ?= $(VITIS)/gnu/microblaze/lin/bin/microblaze-xilinx-elf-
CC      = $(CROSS)gcc
OBJCOPY = $(CROSS)objcopy
SIZE    = $(CROSS)size

BUILD = build
TARGET = $(BUILD)/multitx_fw

# Must match sys_mb in hw/multitx/system_bd.tcl (barrel shifter, HW multiply,
# HW divide, pattern compare, reorder instructions, little endian, no FPU).
CPUFLAGS = -mcpu=v11.0 -mlittle-endian -mxl-barrel-shift -mxl-pattern-compare \
           -mno-xl-soft-mul -mno-xl-soft-div -mxl-reorder

CFLAGS  = $(CPUFLAGS) -Os -g -std=gnu11 -Wall -Wextra -Wno-unused-parameter \
          -ffunction-sections -fdata-sections -Isrc -I$(BUILD)
LDFLAGS = $(CPUFLAGS) -nostartfiles -nostdlib -T linker.ld \
          -Wl,--gc-sections -Wl,-Map=$(TARGET).map
LDLIBS  = -Wl,--start-group -lc -lm -lgcc -Wl,--end-group

SRCS = src/crt0.S src/syscalls.c src/main.c src/console.c src/timer.c \
       src/gpio.c src/ad9361_spi.c src/shell.c
OBJS = $(patsubst src/%,$(BUILD)/%.o,$(SRCS))

all: $(TARGET).bin

$(BUILD)/version.h: FORCE | $(BUILD)
	@printf '#define FW_VERSION "%s"\n#define FW_BUILD_DATE "%s"\n' \
	  "$$(git describe --always --dirty 2>/dev/null || echo unknown)" \
	  "$$(date -u '+%Y-%m-%d %H:%M UTC')" > $@.tmp
	@cmp -s $@.tmp $@ && rm $@.tmp || mv $@.tmp $@

$(BUILD)/%.c.o: src/%.c $(BUILD)/version.h | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -c $< -o $@

$(BUILD)/%.S.o: src/%.S | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(CPUFLAGS) -c $< -o $@

$(TARGET).elf: $(OBJS) linker.ld
	$(CC) $(LDFLAGS) $(OBJS) $(LDLIBS) -o $@
	$(SIZE) $@

$(TARGET).bin: $(TARGET).elf
	$(OBJCOPY) -O binary -R .mailbox -R .bss -R .heap -R .stack $< $@

$(BUILD):
	mkdir -p $@

load: $(TARGET).bin
	python3 ../tools/usbdbg.py load $<

console:
	python3 ../tools/usbdbg.py console

clean:
	rm -rf $(BUILD)

.PHONY: all load console clean FORCE
-include $(OBJS:.o=.d)
