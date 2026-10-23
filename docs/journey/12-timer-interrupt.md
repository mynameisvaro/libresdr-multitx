# 12. The protocol on a timer interrupt

## Where we were

The XM+ held the link with 0.74% of frames lost (chapter 10), but one open
question from that chapter kept coming back: **can the console delay a packet?**
The firmware was one loop:

```c
for (;;) {
	shell_poll();      // read a command line, run it
	mpm_poll();        // run the protocol callback if its deadline has passed
	console_poll();
}
```

`mpm_poll()` only runs between commands. A command that takes 2 ms delays the
callback by 2 ms. Every next step makes this worse: handset input means a steady
stream of serial frames to parse, and over the USB/IP tunnel the host reads the
console slowly, so the firmware's 2 KB output ring fills and printing waits for
the host (up to 50 ms, then output is dropped).

## The problem, measured first

Before changing anything, a test that puts a number on it:
[`tools/sched_check.py`](../../tools/sched_check.py) starts FrSky X and loads the
console in three ways, reading the scheduler's counters (`mpm status`) after each:

- **idle**: just the status reads;
- **busy**: back-to-back commands with long output;
- **stall**: commands sent without reading the output, so the output ring fills.

On the old firmware, over the tunnel:

| Phase | Late callbacks (>100 µs) | Worst |
|---|---|---|
| idle | 0 | 12 µs |
| busy | 7 | 2.3 ms |
| stall | 4 | **58.8 ms** |

A FrSky X packet goes out every 9 ms, so a 59 ms stall is about six missing
packets in a row. That is a failsafe waiting to happen, and now it's a number,
not a suspicion.

## Options and the decision

1. **Make every command short**: split long prints, never wait for the host.
   This would have to hold for every future command too, and it does nothing for
   work that is long by nature (an `rf init` takes seconds).
2. **Call `mpm_poll()` from inside the slow code** (the print loop, the console
   wait). This spreads scheduling into unrelated code, and it's still late by
   whatever happens between two calls.
3. **Run the protocol from a timer interrupt.** The hardware timer fires at the
   deadline and the CPU drops whatever it's doing to run the callback. This is
   what Multiprotocol does on its AVR and STM32 modules (`OCR1A`).

Option 3, because it fixes the cause: the protocol's timing no longer depends on
what the main loop is doing.

## How it was built

### Two timers out of one

`sys_timer` is an AXI Timer with two 32-bit counters. Until now they were
**cascaded** into one 64-bit counter for `timer_ticks()`, which left nothing free
for an alarm. Adding a second timer to the block design would have meant a new
bitstream; instead, [`timer.c`](../../fw/src/timer.c) splits them:

- **Timer 0** counts up and wraps every 43 s at 100 MHz. Its interrupt adds one to
  a software counter, `ticks_hi`, so `timer_ticks()` is still 64 bits.
- **Timer 1** is a one-shot alarm: load the number of ticks to wait, count down,
  interrupt at zero, stop.

The tricky part is reading a counter that's half hardware, half software. If
timer 0 wraps just as you read it, you could pair the new low half with the old
high half and jump back 43 s. The read handles both cases:

```c
do {
	hi  = ticks_hi;
	lo  = TCR0;                    // hardware low half
	csr = TCSR0;                   // has it wrapped without being counted yet?
} while (hi != ticks_hi);          // the interrupt ran meanwhile: try again
if ((csr & TINT) && lo < 0x80000000)
	hi++;                          // wrapped, interrupt still pending
```

The last line matters inside the interrupt itself, where interrupts are off and
`ticks_hi` can't update until the handler returns.

### The interrupt path

The MicroBlaze has one interrupt input. The AXI interrupt controller (`sys_intc`,
already in the block design with the timer on input 1) collects the sources and
says which one fired. [`irq.c`](../../fw/src/irq.c) enables only the timer, and
the handler is short:

```c
void __attribute__((interrupt_handler)) irq_handler(void)
{
	uint32_t pending = INTC_IPR;
	if (pending & 1u << IRQ_TIMER)
		timer_irq();               // wrap count, and the alarm function
	INTC_IAR = pending;            // acknowledge
}
```

`interrupt_handler` makes GCC save every register the handler might touch,
including the status register with the carry flag, and return with `rtid`, which
turns interrupts back on. The start-up code now turns interrupts off first, since
`reboot` jumps to address 0 with them on.

### The scheduler

[`mpm_core.cpp`](../../fw/src/mpm/mpm_core.cpp) keeps the same no-drift rule as
chapter 8 (the next deadline counts from the last deadline), but the alarm calls
it instead of the main loop:

```c
static void mpm_alarm(void)                // runs in the interrupt
{
	uint16_t next = remote_callback();     // the protocol: SPI to the virtual chip, packet out
	next_cb += TIMER_US(next);
	timer_alarm_at(next_cb);               // wake me at the next deadline
}
```

### Sharing the hardware

Now two pieces of code can run "at the same time": the shell and the protocol,
which interrupts it. If the shell is halfway through an AD9361 SPI transfer
(`ad 0x37`) when the protocol retunes the synthesizer over the same SPI bus, both
transfers get corrupted.

The fix is a rule in the shell's command table: **a command pauses the protocol
unless it's marked live.** The shell turns interrupts off around it
(`irq_save()` / `irq_restore()`). The live ones are those that only read, or that
write one value the protocol picks up atomically: `help`, `info`, `md`, the
displays, `mpm status`, `mpm ch`, `mpm option`. A channel value is one 16-bit
store, so the protocol sees either the old value or the new one, never half of
each. Commands like `mpm start`, `rf ...` and `a7105 rx` still pause the
protocol, which is fine: they're bench commands, and correct beats on-time for
them.

One more rule: **nothing in the protocol path prints.** `printf` isn't safe to
call from an interrupt that may have interrupted another `printf`. The
Multiprotocol debug output is compiled out, and the radio code only prints on
driver errors.

## How we knew it worked

The same test, on the new firmware, over the same tunnel:

| Phase | Late callbacks (>100 µs) | Worst |
|---|---|---|
| idle | 0 | 2 µs |
| busy | 0 | 2 µs |
| stall | 0 | 2 µs |

The worst lateness dropped from 58.8 ms to 2 µs, which is the interrupt latency
plus reading the clock. The packets themselves have to be unchanged too:
`frsky_check.py` decoded D16 packets off RX2 (4/4, CRC, hop table and frequency
all correct), and `flysky_check.py` AFHDS2A (3/3).

Those longer runs also showed what the new scheduler doesn't fix: **one** late
callback right after `mpm start` (19–22 ms with AFHDS2A at 4 MSPS), before any
other command. The likely cause is the first packet's LO tune, which goes through
UHD's full tune path rather than the fast retune; a callback that itself takes
that long delays the next one, interrupt or not. It's written down as open
(chapter 11).

## What it led to

The main loop is now free to be slow. That is what handset input needs: parsing
the Multiprotocol serial stream from an EdgeTX radio can now take as long as it
takes, without touching packet timing.

## The thinking move

**Measure the problem before fixing it, with the test you'll use to prove the
fix.** `sched_check.py` was written against the old firmware first. It showed the
problem was real (59 ms, not a guess), and the same script, unchanged, showed it
was gone. A fix checked only on the new code can't tell you whether the test would
have caught the bug at all.
