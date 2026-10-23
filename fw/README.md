# fw/

MicroBlaze firmware for the MultiTX platform. Plain Makefile, MicroBlaze GCC from
Vitis 2022.2 and newlib, with no Xilinx BSP. The start-up code, linker script and
drivers are all here. The AD9361 is driven by UHD's own AD9361 driver (C++,
[`third_party/uhd_ad9361`](third_party/uhd_ad9361)), set up as a B210. Multiprotocol's
protocol code runs unmodified from the [`third_party/multiprotocol`](third_party/multiprotocol)
submodule against virtual CC2500 and A7105 chips; see [docs/multiprotocol.md](../docs/multiprotocol.md).

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
mtx> mpm start frskyx         # Multiprotocol FrSky X (D16), radio at 61.44 MSPS
mtx> mpm ch 1 1844            # channel 1 to +100%
mtx> mpm status
mtx> mpm stop
mtx> mpm start flsky2a 0 0 0 bind   # AFHDS2A (virtual A7105), sub 0 = PWM,IBUS
mtx> a7105 coding             # CRC/FEC coding in use (a7105 crc / fec to change it)
mtx> mpm stop
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
| `code_ram` | `0x0000_0000` | 508 KB | vectors, `.text`, `.rodata`, `.data` load image |
| mailbox | `0x0007_F000` | 4 KB | console rings, shared with the USB bridge |
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

## Handset input

The rest of the mailbox (from offset `0xC40`) is an 8-slot ring of Multiprotocol
serial frames: what EdgeTX sends a Multi module, written here by the PC over USB
(`tools/multi_link.py`). [`input.c`](src/input.c) takes them from the main loop and
hands them to `mpm_serial_frame()` (in `mpm_core.cpp`, ported from Multiprotocol's
`update_serial_data()`): flags, option and channels apply at once, a new model or
a bind flag restarts the protocol. The frames take control when they arrive;
`mpm start` / `mpm stop` on the console take it back. With no frame for the
timeout (`input timeout MS`, default 70 ms as in Multiprotocol) the protocol is
paused, so the receiver goes to failsafe; the next frame resumes it. Status words
next to the ring (`IN_ST_*` in `console.h`) tell the host what the firmware is
doing.

## Interrupts

Only the timer interrupt is used. The protocol callbacks run from timer 1's alarm,
so the main loop (shell, console) can be as slow as it likes. Shell commands run
with interrupts off, which pauses the protocol, unless their entry in the command
table is marked `SHELL_LIVE`; give a new command that flag only if it doesn't touch
the radio or the protocol's state (or takes `irq_save()` itself where it does).
Code called from a protocol callback must not print.

## Source files

| File | |
|---|---|
| [`crt0.S`](src/crt0.S) | exception vectors, reset entry, stack and small-data pointers |
| [`syscalls.c`](src/syscalls.c) | `.data` copy and `.bss` clear, newlib system calls (`write` goes to the console, `sbrk` to the heap) |
| [`main.c`](src/main.c) | initialisation, main loop (shell and console), heartbeat on the user LED |
| [`irq.c`](src/irq.c) | AXI INTC set-up and the interrupt handler; [`irq.h`](src/irq.h) has `irq_save()`/`irq_restore()` for critical sections |
| [`console.c`](src/console.c) | mailbox and UART console |
| [`shell.c`](src/shell.c) | command shell (`help` lists the commands) |
| [`timer.c`](src/timer.c) | 64-bit time base (timer 0 plus a wrap count in its interrupt), one-shot alarm (timer 1), delays |
| [`gpio.c`](src/gpio.c) | `gpio_ctrl` (AD9361 control lines, LED) and `gpio_fe` |
| [`ad9361_spi.c`](src/ad9361_spi.c) | AD9361 SPI through the AXI Quad SPI, hardware reset |
| [`rf.cpp`](src/rf.cpp) | B220 glue for UHD's AD9361 driver (board parameters: port A only, CMOS, 40 MHz; SPI access; RX DC/I/Q tracking kept on) behind a C API, [`rf.h`](src/rf.h) |
| [`axi_ad9361.c`](src/axi_ad9361.c) | FPGA side of the data port: ADI core set-up (1R1T, inverted FB_CLK), its DDS, TX source select |
| [`lora_code.c`](src/lora_code.c) | LoRa coding: whitening, header, CRC, Hamming, interleaver, Gray (no hardware; `fw/test/test_lora_code.py` compares it with `tools/lora.py`) |
| [`lora.c`](src/lora.c) | `lora_tx` driver: a frame's chirps, chip rate, channel offset, level |
| [`gfsk.c`](src/gfsk.c) | `gfsk_tx` driver: Gaussian pulse table, phase increments, packets; CC2500 rate/deviation maths |
| [`uhd_shim/`](src/uhd_shim) | stand-ins for the UHD/Boost headers the driver includes |
| [`radio.c`](src/radio.c) | radio layer: LO slices with fast retunes, NCO offset, modem cache, power, front end |
| [`cc2500.c`](src/cc2500.c) | virtual CC2500: SPI slave with registers, strobes, FIFO; builds and sends packets |
| [`a7105.c`](src/a7105.c) | virtual A7105: the same for FlySky, plus settable CRC/FEC coding and an RX FIFO filled from the console |
| [`input.c`](src/input.c) | handset input: Multi serial frames from the mailbox, link-loss pause |
| [`mpm/`](src/mpm) | Multiprotocol port: Arduino shim, core (globals, start-up, scheduler on the timer alarm) |
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

Code size today is about 226 KB text plus 12 KB initialised data, of the 508 KB
in `code_ram` (512 KB since phase 3). The biggest parts are the AD9361 driver
(~45 KB), soft-float double maths, printf and the C++ unwinder.
