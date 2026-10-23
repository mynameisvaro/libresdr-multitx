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
CXX     = $(CROSS)g++
OBJCOPY = $(CROSS)objcopy
SIZE    = $(CROSS)size

BUILD = build
TARGET = $(BUILD)/multitx_fw

# Must match sys_mb in hw/multitx/system_bd.tcl (barrel shifter, HW multiply,
# HW divide, pattern compare, reorder instructions, little endian, no FPU).
CPUFLAGS = -mcpu=v11.0 -mlittle-endian -mxl-barrel-shift -mxl-pattern-compare \
           -mno-xl-soft-mul -mno-xl-soft-div -mxl-reorder

CFLAGS  = $(CPUFLAGS) -Os -g -std=gnu11 -Wall -Wextra -Wno-unused-parameter \
          -ffunction-sections -fdata-sections -Isrc -Isrc/mpm -I$(BUILD)

# UHD's AD9361 driver (C++, exceptions on, no RTTI), built against the stand-in
# headers in src/uhd_shim; see third_party/uhd_ad9361/README.md.
UHD_AD9361 = third_party/uhd_ad9361
CXXFLAGS = $(CPUFLAGS) -Os -g -std=gnu++17 -Wall -fno-rtti -fno-threadsafe-statics \
           -ffunction-sections -fdata-sections -DAD9361_NO_FILTER_API \
           -Isrc -Isrc/uhd_shim -I$(UHD_AD9361) -I$(BUILD)

# Multiprotocol's protocol files (git submodule, unmodified), built as one
# translation unit by src/mpm/mpm_core.cpp. -fpermissive as in the Arduino IDE.
MPM = third_party/multiprotocol/Multiprotocol
MPM_CXXFLAGS = $(CXXFLAGS) -fpermissive -w -Isrc/mpm -I$(MPM)

# crti/crtbegin and crtend/crtn bracket the objects: they provide .init (which
# registers the C++ unwind tables) and the constructor lists.
CRT = $(foreach f,$(1),$(shell $(CC) $(CPUFLAGS) -print-file-name=$(f)))
CRT_BEGIN = $(call CRT,crti.o crtbegin.o)
CRT_END   = $(call CRT,crtend.o crtn.o)

# --no-relax: MicroBlaze linker relaxation shortens branches after .eh_frame is
# laid out, which leaves the unwind tables wrong; a C++ throw then hangs.
LDFLAGS = $(CPUFLAGS) -nostartfiles -nostdlib -T linker.ld \
          -Wl,--gc-sections -Wl,--no-relax -Wl,-Map=$(TARGET).map
LDLIBS  = -Wl,--start-group -lstdc++ -lc -lm -lgcc -Wl,--end-group

SRCS = src/crt0.S src/syscalls.c src/main.c src/console.c src/irq.c src/timer.c \
       src/gpio.c src/ad9361_spi.c src/axi_ad9361.c src/gfsk.c src/radio.c \
       src/cc2500.c src/a7105.c src/input.c src/shell.c src/rf.cpp src/mpm/mpm_core.cpp
OBJS = $(patsubst src/%,$(BUILD)/%.o,$(SRCS)) $(BUILD)/uhd_ad9361/ad9361_device.cpp.o

all: $(TARGET).bin

$(BUILD)/version.h: FORCE | $(BUILD)
	@printf '#define FW_VERSION "%s"\n#define FW_BUILD_DATE "%s"\n' \
	  "$$(git describe --always --dirty 2>/dev/null || echo unknown)" \
	  "$$(date -u '+%Y-%m-%d %H:%M UTC')" > $@.tmp
	@cmp -s $@.tmp $@ && rm $@.tmp || mv $@.tmp $@

$(BUILD)/%.c.o: src/%.c $(BUILD)/version.h | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -c $< -o $@

$(BUILD)/%.cpp.o: src/%.cpp | $(BUILD)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -MMD -c $< -o $@

$(BUILD)/mpm/%.cpp.o: src/mpm/%.cpp $(BUILD)/version.h | $(BUILD)
	@mkdir -p $(dir $@)
	$(CXX) $(MPM_CXXFLAGS) -MMD -c $< -o $@

$(BUILD)/uhd_ad9361/%.cpp.o: $(UHD_AD9361)/%.cpp | $(BUILD)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -w -MMD -c $< -o $@

$(BUILD)/%.S.o: src/%.S | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(CPUFLAGS) -c $< -o $@

$(TARGET).elf: $(OBJS) linker.ld
	$(CC) $(LDFLAGS) $(CRT_BEGIN) $(OBJS) $(LDLIBS) $(CRT_END) -o $@
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
