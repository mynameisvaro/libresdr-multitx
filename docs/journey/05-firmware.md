# 5. Firmware from nothing

## Where we were

A platform that passes its self-test (chapter 4): a MicroBlaze with empty memory,
an AD9361 held in reset, and a USB bridge that can load anything into `code_ram`.

## The problem

Write a program for a CPU with no operating system and no debugger, that can:

1. start up correctly (stack, variables, C runtime);
2. talk to us (there's no screen and no JTAG console);
3. bring up the AD9361, a chip whose initialisation is thousands of register writes
   and a dozen calibrations.

## Step 1: start-up without a vendor BSP

Xilinx's usual flow (Vitis) generates a "BSP" (board support package) for you.
It's heavy and hides what's going on, and it ties the build to their IDE. The
firmware here is a plain `Makefile` with the MicroBlaze GCC and newlib (the C
library), and three small files that a BSP would otherwise provide:

**[`crt0.S`](../../fw/src/crt0.S)**: what runs first. The MicroBlaze jumps to
address 0 at reset, so the vector table lives there:

```asm
	.section .vectors.reset, "ax"
_vector_reset:
	brai	_start                      /* reset: jump to _start */
	...
_start:
	addik	r1, r0, _stack_top - 16     /* stack pointer (in data_ram) */
	addik	r2, r0, _SDA2_BASE_         /* small-data anchors the compiler uses */
	addik	r13, r0, _SDA_BASE_
	brlid	r15, _cstart                /* C start-up, then main() */
```

**[`linker.ld`](../../fw/linker.ld)**: where everything goes. This is where the
hardware design shows up in software:

```
MEMORY
{
	code    (rx)  : ORIGIN = 0x00000000, LENGTH = 0x7F000   /* code_ram */
	mailbox (rw)  : ORIGIN = 0x0007F000, LENGTH = 0x1000    /* top 4 KB: the console */
	data    (rw)  : ORIGIN = 0x00100000, LENGTH = 64K       /* data_ram */
}
```

Code and constants go in `code_ram`. Variables go in `data_ram`, but their
*initial values* must be stored in `code_ram` (that's what gets loaded) and copied
across at start-up.

**[`syscalls.c`](../../fw/src/syscalls.c)**: `_cstart()` does that copy, zeroes
`.bss`, runs C++ constructors and calls `main()`. It also implements the few
system calls newlib needs: `write()` goes to the console, `sbrk()` hands out heap.

The compiler flags in the Makefile must match the CPU as built in the block design
(barrel shifter, hardware multiply and divide, little-endian). If they don't, the
compiler emits instructions the CPU doesn't have.

## Step 2: a console with no serial cable

The board has a UART on connector CN2, but nothing was connected to it, and the
board was remote. The USB bridge can read and write `code_ram`, so the console
lives **in memory both sides can see**: a 4 KB "mailbox" at the top of
`code_ram` ([`console.h`](../../fw/src/console.h), [`console.c`](../../fw/src/console.c)).

```
mailbox: magic | session | tx_head | tx_tail | rx_head | rx_tail | ... | tx ring (2 KB) | rx ring (256 B)
```

- Firmware → PC: `printf` writes into the TX ring and advances `tx_head`. The PC
  polls over USB, reads new bytes, advances `tx_tail`.
- PC → firmware: the PC writes typed characters into the RX ring and advances
  `rx_head`; the firmware consumes them.

The rule that makes this safe without locks: **each counter is written by only one
side**. The firmware owns `tx_head` and `rx_tail`; the PC owns `tx_tail` and
`rx_head`. Each side only reads the other's counters. A `session` counter changes
at every boot, so the PC notices when the firmware was reloaded.

Two later refinements came from real problems:

- If the PC stops reading and the ring fills, the firmware waits at most 50 ms,
  then drops output (and counts what it dropped). The firmware must never hang
  because nobody is listening.
- Output is also copied to the CN2 UART, but *buffered* and drained from the main
  loop (chapter 8 found that writing the UART directly stalled the protocol for
  tens of milliseconds).

On top of the console sits a tiny shell ([`shell.c`](../../fw/src/shell.c)):
`info`, `md`/`mw` (memory dump/write), `ad` (AD9361 register), and later `rf`,
`fe`, `dds`, `gfsk`, `mpm`. Commands can be run from scripts:

```bash
python3 tools/usbdbg.py console info "ad 0x37"
```

**A first small bug:** the banner and `help` worked, but the first `ad` command
(read an AD9361 register) hung. Looking at the Quad SPI block's own status
registers *through the USB bridge* (the CPU was stuck, but the bus wasn't) showed
the cause: after a FIFO reset, the SPI block needs a moment before it accepts
data, and the driver didn't wait. One loop fixed it. This is the debug bridge
paying for itself: you can inspect a peripheral while the CPU is hung.

**Checked:** the firmware loads in under 0.1 s over direct USB, prints its banner,
reads the AD9361 product ID itself, and the PC detects a reboot.

## Step 3: the AD9361 driver, and a licence decision

Bringing up an AD9361 by hand is not realistic: reset, clock set-up, filter
coefficients, synthesizer tuning, and calibrations for RF DC offset, baseband DC,
TX quadrature, RX quadrature and more. Use an existing driver. Two candidates:

| Driver | For | Against |
|---|---|---|
| ADI **no-OS** | made for bare-metal CPUs like this one; has fastlock functions | its licence adds a condition ("must run on or be connected to an ADI part") that GPL-3.0 doesn't allow in a combined binary, and this project must be GPL-3.0 because of Multiprotocol |
| **UHD's `ad9361_device`** | GPL-3.0; it's what this board's own images were tested with, so the B210 settings apply directly | written for a PC: C++, Boost, exceptions, logging |

UHD's driver won on the licence. Its PC-isms were handled with a **shim**
([`fw/src/uhd_shim/`](../../fw/src/uhd_shim)): stand-in headers that give it the
few Boost and UHD pieces it includes (exceptions without `std::string`, no-op
logging and mutexes, a tiny `boost::format`). The driver source itself is
unchanged except one `#ifndef` around the filter API.

[`rf.cpp`](../../fw/src/rf.cpp) is the glue: it tells the driver how to reach the
chip (SPI through the `axi_quad_spi`), describes the board (clock, interface,
which ports are wired), and turns everything into a C API ([`rf.h`](../../fw/src/rf.h))
that the rest of the firmware calls: `rf_init()`, `rf_tune()`, `rf_set_gain()`.

### Bug 1: errors hung the CPU

When the driver reported an error (it throws a C++ exception), the firmware froze
instead of printing it.

C++ exceptions find their way up the stack using unwind tables (`.eh_frame`). The
MicroBlaze linker has a size optimisation, *relaxation*, that shortens branch
instructions after the tables were written. The tables then point at the wrong
places and the unwinder loops forever. The fix: link with `--no-relax` (costs
about 4 KB). Plus the C runtime pieces exceptions need (`crti/crtbegin/crtend/crtn`
around the objects, `__libc_init_array()` at start-up).

Once fixed, it was checked on purpose: make the driver fail, and see the message
on the console. Error paths need testing as much as the normal path, and they're
easy to forget.

## Step 4: test the FPGA ↔ AD9361 data path, with no RF at all

The driver initialised the chip (391 ms with all calibrations) and the
synthesizers locked at 2.44 GHz. But is the sample bus between the FPGA and the
AD9361 right? Testing that over the air would mix in a dozen other unknowns
(amplifiers, switches, antennas, gains).

The AD9361 has a **digital loopback**: it can send the TX samples it receives from
the FPGA straight back to the FPGA as RX samples, without going through any analog
circuit. So: generate a tone with `axi_ad9361`'s DDS, turn on the loopback,
capture with `adc_capture`, check the tone. Only the digital interface is being
tested. That's [`tools/rf_loopback.py`](../../tools/rf_loopback.py).

### Bug 2: the AD9361 received nothing

The capture came back as zeros. Is it the capture side or the transmit side? Split
the path in two:

- Plain RX with the loopback off (just noise from the receiver) wasn't zero, so
  `adc_capture` and the RX half of the interface worked. It did show a smaller
  bug on the way: the samples jumped between 0 and 4095, which is what small
  negative 12-bit numbers look like when nobody sign-extends them to 16 bits.
  `axi_ad9361` has a per-channel format setting for that; the firmware now sets it.
- The AD9361's own built-in test tone, injected on its RX side, came through
  clean. So the problem was on the FPGA → AD9361 (TX) half.

The CMOS interface sends I and Q on alternating edges of a clock the FPGA provides
(`FB_CLK`), with a `TX_FRAME` signal saying which is which. Reading ADI's core and
Ettus' FPGA side by side showed the difference: ADI's core puts **Q** on the
rising edge and **I** on the falling one; the AD9361, set up the way UHD sets it up
for a B210, expects **I** on the rising edge, as Ettus' FPGA sends it. The two
halves of every sample were swapped relative to the frame signal, so the AD9361
rejected them.

The fix is one register write in [`axi_ad9361.c`](../../fw/src/axi_ad9361.c):
invert `FB_CLK` (DAC common register `0x60` = 1). It's documented in
[hardware.md](../hardware.md#ad9361-interface) so nobody has to find it again.

**Checked:** the loopback passes at every rate from 5 to 61.44 MSPS: right
frequency, right sign (a +1 MHz tone comes back at +1 MHz, not −1 MHz), −12 dBFS,
spurs at least 62 dB down.

## The main loop

[`main.c`](../../fw/src/main.c) ended up as a classic cooperative loop: no RTOS,
no interrupts yet:

```c
for (;;) {
	...blink the user LED every 500 ms (a heartbeat you can see)...
	shell_poll();      /* one console command, if any */
	mpm_poll();        /* run the protocol callback if its deadline came (ch. 8) */
	console_poll();    /* drain buffered UART output */
}
```

Simple to reason about: each function must return quickly. That constraint comes
back in chapters 8 and 10.

## What it led to

The CPU controls the AD9361 and the sample interface is proven. The next unknown
is the analog side: does a signal actually come out of the antenna port?

## The thinking move

**Test each interface in isolation, using the hardware's own loopback modes.** The
FB_CLK bug was found quickly because the digital loopback removed every other
suspect. And when choosing a dependency, the licence is a technical constraint,
not paperwork: it decided the driver here.
