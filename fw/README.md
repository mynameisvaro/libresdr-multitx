# fw/

MicroBlaze firmware for the MultiTX platform. Plain Makefile, MicroBlaze GCC from
Vitis 2022.2 and newlib, with no Xilinx BSP. The start-up code, linker script and
drivers are all here. The AD9361 is driven by UHD's own AD9361 driver (C++,
[`third_party/uhd_ad9361`](third_party/uhd_ad9361)), set up as a B210.

```bash
make            # build/multitx_fw.elf, .bin, .map
make load       # halt the CPU, load code_ram over the USB bridge, verify, run
make console    # interactive console (Ctrl-C to leave)
python3 ../tools/usbdbg.py console info "ad 0x37"   # run commands from a script
```

A typical session:

```
mtx> rf init                  # AD9361 reset, calibrations, 30.72 MSPS, RX1 + TX1 on
mtx> rf freq tx 2440e6
mtx> rf gain tx 40
mtx> fe 1 fdd                 # front end: TX on TX/RX, RX on RX2 (PA needs ~0.5 s)
mtx> dds 1e6 -12              # 1 MHz complex tone from the FPGA DDS into TX1
mtx> rf temp
mtx> dds off
mtx> gfsk cc2500 d16          # FrSky D16 modulation from the CC2500 register values
mtx> gfsk chan 300e3          # NCO offset from the TX LO
mtx> gfsk level -6 20         # -6 dBFS, 20 us ramp
mtx> gfsk send AAAAAAAAD391D391 1D0102  # bytes as hex, sent MSB first
mtx> fe 1 off
```

`python3 ../tools/rf_loopback.py` checks the FPGA ↔ AD9361 sample interface at
several rates using the AD9361's digital loopback (no RF). `python3 ../tools/rf_tone.py`
is the over-the-air version (TX/RX → cable + attenuator → RX2).

The FPGA must be running a MultiTX bitstream (see [docs/development.md](../docs/development.md)).
Loading takes well under a second on a direct USB connection, so there's no need to
rebuild the bitstream to change the firmware.

## Memory layout ([`linker.ld`](linker.ld))

| Region | Address | Size | Contents |
|---|---|---|---|
| `code_ram` | `0x0000_0000` | 252 KB | vectors, `.text`, `.rodata`, `.data` load image |
| mailbox | `0x0003_F000` | 4 KB | console rings, shared with the USB bridge |
| `data_ram` | `0x0010_0000` | 64 KB | `.data`, `.bss`, heap, 16 KB stack |

The CPU fetches code over the instruction LMB and reads constants from the same
BRAM through its AXI port. `data_ram` is on the data LMB only, so the host can't
see it. Anything the host needs to read goes in the mailbox.

## Console

[`console.h`](src/console.h) defines the mailbox: a 2 KB output ring (firmware →
host) and a 256-character input ring (host → firmware), with head/tail counters
that are each written by one side only. `tools/usbdbg.py console` polls it over the
bridge. Output is also mirrored to the UART on CN2 (115200 8N1), and characters
typed there are echoed. If the host stops reading, output stalls for up to 50 ms
and is then dropped (counted in `info`), so the firmware never hangs on the console.

The session counter changes at every boot, which lets the host notice a reload.

## Source files

| File | |
|---|---|
| [`crt0.S`](src/crt0.S) | exception vectors, reset entry, stack and small-data pointers |
| [`syscalls.c`](src/syscalls.c) | `.data` copy and `.bss` clear, newlib system calls (`write` goes to the console, `sbrk` to the heap) |
| [`main.c`](src/main.c) | initialisation, main loop, heartbeat on the user LED |
| [`console.c`](src/console.c) | mailbox and UART console |
| [`shell.c`](src/shell.c) | command shell (`help` lists the commands) |
| [`timer.c`](src/timer.c) | 64-bit time base from `sys_timer` (cascade mode), delays |
| [`gpio.c`](src/gpio.c) | `gpio_ctrl` (AD9361 control lines, LED) and `gpio_fe` |
| [`ad9361_spi.c`](src/ad9361_spi.c) | AD9361 SPI through the AXI Quad SPI, hardware reset |
| [`rf.cpp`](src/rf.cpp) | B220 glue for UHD's AD9361 driver (board parameters: port A only, CMOS, 40 MHz; SPI access; RX DC/I/Q tracking kept on) behind a C API, [`rf.h`](src/rf.h) |
| [`axi_ad9361.c`](src/axi_ad9361.c) | FPGA side of the data port: ADI core set-up (1R1T, inverted FB_CLK), its DDS, TX source select |
| [`gfsk.c`](src/gfsk.c) | `gfsk_tx` driver: Gaussian pulse table, phase increments, packets; CC2500 rate/deviation maths |
| [`uhd_shim/`](src/uhd_shim) | stand-ins for the UHD/Boost headers the driver includes |
| [`hw.h`](src/hw.h) | address map and register access helpers |

The compiler flags in the [`Makefile`](Makefile) must match the MicroBlaze
configuration in `hw/multitx/system_bd.tcl` (barrel shifter, hardware multiply and
divide, pattern compare, little endian).

## C++

Only the AD9361 driver is C++. It keeps its exceptions; `rf.cpp` catches them and
returns -1 after printing the message. That needs `crti.o`/`crtbegin.o` and
`crtend.o`/`crtn.o` around the objects (they register the unwind tables from
`.init`), and `_cstart` calls `__libc_init_array()`. The link uses `--no-relax`:
MicroBlaze linker relaxation shortens branches without fixing `.eh_frame`, and a
throw then hangs in the unwinder. RTTI is off. Two things keep
libstdc++ small: the exception shim stores its message in a fixed buffer, so no
`std::string`, and `rf.cpp` replaces the verbose terminate handler, so the
demangler is never linked.

Code size today is about 189 KB text plus 11 KB initialised data, of the 252 KB
in `code_ram`. Most of it is the driver (~45 KB), soft-float double maths, printf
and the C++ unwinder. If protocol code later needs more room, `code_ram` can grow
in the block design: the XC7A200T has plenty of block RAM left.
