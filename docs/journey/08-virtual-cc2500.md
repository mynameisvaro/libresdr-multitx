# 8. Virtual chips and Multiprotocol's own code

## Where we were

A GFSK modulator that sends any packet at any channel within ±17.6 MHz of the LO
(chapter 7). Multiprotocol's code not built yet.

## The problem

Three problems, really:

1. **Build Multiprotocol's protocol files**, unmodified, for a CPU they were never
   written for.
2. **Be a CC2500**: answer their SPI bytes exactly as the chip would, and turn
   `STX` into a packet on the modulator.
3. **Cover the whole band** with correct timing: FrSky hops over 70 MHz every 9 ms.

## Part 1: building Multiprotocol for the MicroBlaze

Multiprotocol is an Arduino project. Its main file, `Multiprotocol.ino`, is full
of AVR/STM32 specifics (hardware timers, USARTs, PPM input pins), but the protocol
files (`FrSkyD_cc2500.ino`, …) only need a small set of things from it.

The approach, in [`fw/src/mpm/`](../../fw/src/mpm):

- **Multiprotocol as a git submodule** ([`fw/third_party/multiprotocol`](../../fw/third_party/multiprotocol)),
  pinned to a commit and never edited. Upstream fixes can be pulled in later.
- **An Arduino shim**, [`mpm_arduino.h`](../../fw/src/mpm/mpm_arduino.h): the
  handful of Arduino functions the protocol code calls. `millis()` and `micros()`
  from our 64-bit timer, `delayMicroseconds()`, `PROGMEM` (meaningless here),
  EEPROM in RAM, `random()`, and the bridge to the virtual chips:

  ```c
  uint8_t SPI_Read(void);            // → whichever virtual chip is selected
  void    SPI_Write(uint8_t b);
  #define CC25_CSN_on   cc2500_csn(1)
  #define CC25_CSN_off  cc2500_csn(0)
  ```

- **A core**, [`mpm_core.cpp`](../../fw/src/mpm/mpm_core.cpp), replacing
  `Multiprotocol.ino`: one C++ file that `#include`s the protocol `.ino` files
  (exactly as the Arduino IDE would glue them together), declares the global
  variables they share, starts a protocol, and runs its callbacks.
- **More memory.** Code size was heading past the 252 KB available, so `code_ram`
  was doubled to 512 KB in the block design. That's a one-line change in
  `system_bd.tcl` plus moving the console mailbox to the new top of memory. Timing
  still met (WNS +0.83 ns), BRAM use 52%.

### A scheduler that doesn't drift

Each protocol callback returns how many microseconds until it wants to run again
(chapter 1). On an AVR, Multiprotocol does `OCR1A += next_callback`: the next
deadline is counted from the *previous deadline*, not from "now". The core does
the same in software:

```c
if (now < next_cb)                    // not yet
	return;
late_us = now - next_cb;              // how late are we? (kept for statistics)
uint16_t next = remote_callback();    // run the protocol
next_cb += TIMER_US(next);            // next deadline from the last *deadline*
```

If one callback runs 30 µs late, the next one is still on time. Counting from
"now" would let every small delay accumulate into drift, and the receiver's hop
timing would slowly slide away. `mpm status` reports the number of late callbacks
and the worst one.

## Part 2: being a CC2500

[`fw/src/cc2500.c`](../../fw/src/cc2500.c) is a **byte-level SPI slave**: it sees
the same bytes the real chip would see, in the same order, and answers the same.

### The SPI protocol of the chip

The first byte after chip select goes low is a header:

```
bit 7: 1 = read, 0 = write
bit 6: 1 = burst (keep going to the next address)
bits 5..0: address (0x00–0x2E registers, 0x30–0x3D strobes, 0x3E PATABLE, 0x3F FIFO)
```

So the virtual chip is a small state machine:

```c
if (!header_seen) {                       // first byte: a header
	rw_read = mosi >> 7;
	burst   = (mosi >> 6) & 1;
	addr    = mosi & 0x3F;
	if (addr >= 0x30 && addr <= 0x3D)     // a strobe: a one-byte command
		strobe(addr);
	return status_byte();                 // the real chip answers every byte with status
}
/* data bytes: */
if (addr < 0x2F)       regs[addr] = mosi;          // a register
else if (addr == FIFO) txfifo[txlen++] = mosi;     // the TX FIFO
...
if (burst) addr++;
```

Details that matter because the protocol code checks them: the status byte
(state + FIFO space) returned on every byte, the `MARCSTATE` register, `RXBYTES`
(FrSky D8 reads it to look for telemetry, so it must say "nothing received"), the
8-entry power table, and the state after transmitting (set by `MCSM1`).

### What happens on `STX`

`transmit()` does what the datasheet says the chip does with its FIFO:

```c
preamble  (0xAA × n, n from MDMCFG1)
sync word (SYNC1, SYNC0; twice for the 30/32 modes)
payload   (from the FIFO; length byte or fixed length per PKTCTRL0)
CRC-16    (poly 0x8005, init 0xFFFF, if enabled)
whitening (PN9, if enabled)
```

and works out the radio settings from the registers, with the datasheet formulas:

```c
freq = FXOSC / 2^16 * FREQ  +  CHANNR * channel_spacing  +  FXOSC / 2^14 * FSCTRL0
baud = (256 + DRATE_M) * 2^DRATE_E * FXOSC / 2^28
dev  = FXOSC / 2^17 * (8 + DEV_M) * 2^DEV_E
```

Then it calls `radio_tx(freq, modem, power, bits)`. Note `FSCTRL0`: that's
Multiprotocol's per-receiver frequency fine tune, and it becomes important in
chapter 10.

What's **not** emulated (yet): the receiver (RX FIFO stays empty, RSSI reads
−100 dBm), OOK, Manchester and FEC (FrSky doesn't use them), and calibration
(`SCAL` is accepted and does nothing, since the AD9361 calibrates the real
synthesizer).

## Part 3: covering 70 MHz on time

[`fw/src/radio.c`](../../fw/src/radio.c) sits between the virtual chips and the
hardware. For each packet:

```c
double want = slice_lo(freq_hz);        // which slice holds this channel?
if (want != lo_hz)                      // a different one: move the LO
	rf_fast_tune(RF_TX, want, ...);
gfsk_channel(freq_hz - lo_hz);          // the NCO offset inside the slice
gfsk_send(bits);
```

with slices on a fixed grid:

```c
#define SLICE_BASE_HZ   2403.9e6        /* two 35.2 MHz slices cover 2404 - 2474.2 MHz */
static double slice_lo(double f)
{
	return SLICE_BASE_HZ + (floor((f - SLICE_BASE_HZ) / slice) + 0.5) * slice;
}
```

That gives two LO positions, 2421.5 and 2456.7 MHz (you'll see exactly those in
`mpm status`).

### The plan said fastlock. A measurement said no.

The original plan (chapter 1) was to store AD9361 fastlock profiles for the slice
LOs. UHD's driver has no fastlock API, and its normal `tune()` takes **2.4 ms**
(it re-runs parts of the synthesizer set-up and changes the chip's state machine).
Too slow? Before building fastlock, measure what's actually needed.

`rf_fast_tune()` in [`rf.cpp`](../../fw/src/rf.cpp) does only what a frequency
change really requires: compute the synthesizer's integer and fractional divider,
write the VCO settings from the driver's own calibration table, and poll the lock
bit. The chip stays in FDD the whole time. Measured: the PLL relocks in **about
110 µs**, the whole retune takes **about 0.3 ms**.

A real CC2500 spends about 0.8 ms calibrating after `STX`. Our retune is faster
than the chip we're imitating, so fastlock wasn't needed, and wasn't built.

## Bugs found on the way

| Symptom | Cause | Fix |
|---|---|---|
| FrSky X never hopped: every packet on the same channel | FrSky X picks its hop step with `FrSkyX_chanskip = random(0xfefefefe) % 47`. Arduino's `random(n)` takes a `long`, so `0xfefefefe` is *negative*, and Arduino still returns `random() % n`. Our first version returned 0 for a negative limit, so the step was 0 | match Arduino's behaviour exactly |
| Restarting a protocol came up at the wrong sample rate | UHD's `set_clock_rate()` returns the *bandwidth* in one case, not the rate, and the code used the return value | don't trust the return value; use the requested rate |
| Callbacks up to tens of ms late | `printf` to the UART waited for each character at 115200 baud | buffer UART output, drain it in the main loop |
| The first packet 0.3 s late | the Gaussian pulse table (1,500 soft-float `erf()` calls) was computed on the first packet | compute it when the radio starts |

All four were found because the tools reported numbers: channel per packet, the
sample rate in `mpm status`, late-callback counts, the time of the first packet.

## How we knew it worked

[`tools/frsky_check.py`](../../tools/frsky_check.py) runs a protocol on the board,
captures single packets on RX2, and decodes them **like a CC2500 would**: find the
carrier offset, demodulate, find the sync word, de-whiten, check the CRC (the
CC2500's for D8, FrSky's own for D16), map the frequency back to a channel number,
and check it's in the protocol's hop table.

| Mode | Result |
|---|---|
| FrSky D16 | 15/15: CRC ok, channel in hop table, frequency exact, across 2405–2471 MHz |
| FrSky D8 | 15/15 |
| Bind, D8 and D16 | 4/4 each |
| Timing, 10 s at 61.44 MSPS | no callback more than 100 µs late (worst 12 µs) |

Captures run at only 2–3 MSPS (so one capture holds a whole packet), which makes
the slices tiny and forces a retune on nearly every hop. That's a harsher test of
the retune path than normal operation.

## What it led to

Multiprotocol's own FrSky code transmits correct, hopping packets. The real test
(a FrSky receiver) had to wait until the receivers were within reach. In the
meantime: the second chip, the A7105, for FlySky.

## The thinking move

**Emulate at the lowest level the code depends on.** Emulating the chip's SPI
bytes (instead of rewriting `CC2500_WriteReg` or the protocol files) means
Multiprotocol's code runs untouched, including its quirks, and every protocol on
that chip comes almost for free. And **measure before building the "required"
feature**: fastlock was in the plan for a day and turned out to be unnecessary.
