# The MultiTX journey

How this project went from "could an SDR pretend to be a Multiprotocol module?"
to a FrSky receiver following the sticks, told step by step, with the reasoning
behind each step.

This isn't the reference documentation (that's the rest of [docs/](..)). It's the
story of *how* the project was approached: what was known at each point, what
problem stood in the way, which options there were, what was chosen and why, how
the result was checked, and why that pointed at the next step. The aim is that
you could start a project like this yourself, even if you've never touched an
FPGA before.

## How to read it

The chapters are in project order. Each one has the same parts:

- **Where we were**: what worked and what we knew at that point.
- **The problem**: what stood in the way.
- **Options and the decision**: what we could have done, and why we picked one.
- **How it was built**: the actual blocks, Verilog, Tcl or C, explained.
- **How we knew it worked**: the test, and why that test was the right one.
- **What it led to**: why the next chapter is the next chapter.
- **The thinking move**: the habit from that step that you can reuse.

If FPGAs are new to you, read chapter 2 first: it explains the vocabulary (LUT,
clock, register, AXI, block design, timing) with examples from this repository.

## Chapters

| # | Chapter | In one line |
|---|---|---|
| 0 | [How to think about a project like this](00-how-to-think.md) | The habits behind every other chapter, in one place |
| 1 | [The idea and the plan](01-idea-and-plan.md) | Finding the seam in Multiprotocol, and letting the hardware decide the architecture |
| 2 | [FPGA primer, using this project](02-fpga-primer.md) | Just enough FPGA to read the rest: Verilog, clocks, AXI, block designs, timing |
| 3 | [Reaching the board and a known-good baseline](03-board-and-baseline.md) | USB/IP, loading bitstreams, and rebuilding the community design first |
| 4 | [Our own platform: CPU, buses and a way to see inside](04-platform.md) | MicroBlaze, the USB debug bridge, sample capture, the address map |
| 5 | [Firmware from nothing](05-firmware.md) | Start-up code, a console, the AD9361 driver, and the first two real bugs |
| 6 | [First RF](06-first-rf.md) | A tone out, a tone back, and what the board was hiding |
| 7 | [A GFSK modulator in Verilog](07-gfsk-modulator.md) | Phase accumulators, Gaussian pulses, and a testbench that demodulates |
| 8 | [Virtual chips and Multiprotocol's own code](08-virtual-cc2500.md) | Pretending to be a CC2500, hopping across 70 MHz, scheduling to the microsecond |
| 9 | [FlySky and the coding nobody documented](09-virtual-a7105.md) | When the datasheet is missing a piece: make it settable, then learn it |
| 10 | [A real receiver](10-real-receiver.md) | Binding an XM+, proving the link is ours, and measuring it with an ESP32 |
| 11 | [Where it goes next](11-next.md) | Open questions and how to choose the next step |
| 12 | [The protocol on a timer interrupt](12-timer-interrupt.md) | Measuring a 59 ms stall, then making the console unable to delay a packet |
| 13 | [The PC as the handset](13-pc-as-handset.md) | Multiprotocol's own serial frames over USB, and a test that was one frame early |
| 14 | [LoRa, checked against real chips](14-lora.md) | ExpressLRS's physical layer: a model proven on SX1276 recordings, a chirp modulator in Verilog |
| 15 | [ExpressLRS without a receiver](15-elrs.md) | ExpressLRS's own packet code, our scheduler, and two implementations that must agree |
| 16 | [Receiving, and real chips as judges](16-receiving.md) | A GFSK receiver in the FPGA, three bugs simulation hid, an SX1278 and a handset as referees |
| 17 | [The X6B that wouldn't bind](17-x6b-bind.md) | Checking the instruments first, recording the real exchange, and ruling causes out one by one (still open) |

## Timeline

| Date | Milestone | Commit |
|---|---|---|
| 2026-10-21 | Idea, plan, remote USB access, reference design rebuilt | — |
| 2026-10-22 00:02 | Phase 1 platform on the board: bridge, memory, AD9361 ID | `fb3873b` |
| 2026-10-22 09:34 | Firmware runs, console over USB | `b67646f` |
| 2026-10-22 09:56 | AD9361 driver, sample interface verified to 61.44 MSPS | `374ddf7` |
| 2026-10-22 10:06 | First RF: tone out of TX/RX, received on RX2 | `0072b96` |
| 2026-10-22 10:27 | GFSK modulator, FrSky-style packets decode over the air | `40ef2ce` |
| 2026-10-22 11:49 | Multiprotocol's FrSky D8/D16 running on a virtual CC2500 | `3ea02a4` |
| 2026-10-22 12:15 | Virtual A7105: FlySky AFHDS / AFHDS2A | `bf0999c` |
| 2026-10-22 23:52 | A real FrSky XM+ binds and holds the link | `3739e55` |
| 2026-10-23 00:25 | Link measured: exact channels, 0.74% frames lost | `bfa6b2d` |
| 2026-10-23 | Protocol callbacks on a timer interrupt: worst lateness 58.8 ms → 2 µs | `bc1bebd` |
| 2026-10-23 | No slow work inside callbacks: longest 21 ms → 1.5 ms | `61d3f92` |
| 2026-10-23 | Handset input from the PC: Multi serial frames over USB | `5f8649d` |
| 2026-10-23 | LoRa: model reproduces 68/68 real SX1276 frames; lora_tx 30/31 frames on the board | `a2e94e5`, `99ce770` |
| 2026-10-23 | PPM input from a radio's trainer port: 6/6 on the board with the built-in generator | `d940810` |
| 2026-10-23 | ExpressLRS 4.x at 900 MHz: 288 PC cases, 27/27 on the board | `ad3b496` |
| 2026-10-24 | GFSK receive path 14/14 over the air; SX1278 receives ExpressLRS 306/307; A7105 coding learned from an FS-i6X | `b207096`…`16d5a50` |
| 2026-10-24 | Frequency measurements calibrated; real FS-i6X ↔ X6B bind recorded; the board clones the FS-i6X (X6B locks, telemetry back); a 1024-byte USB bug fixed; X6B bind still open | `f5bff4a`, `0104e5b`, this chapter's commit |
