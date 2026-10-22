# 10. A real receiver

## Where we were

Multiprotocol's FrSky D16 packets decode perfectly on our own decoder (chapter 8).
But our decoder was written from the same datasheet as our virtual chip. If both
share a misunderstanding, both agree and both are wrong. Only a real receiver can
settle it.

The receiver: a FrSky **XM+**, a tiny D16 receiver with no servo outputs, only
SBUS (a serial bus that flight controllers read). The board was at home, on the
USB/IP tunnel again (chapter 3), driven from Vega-1.

## Step 1: bind, and read the LED

```
mpm start frskyx 0 0 0 bind      # FrSky X, sub 0 = D16 FCC, RX 0, option 0, bind
```

Power the XM+ with its bind button held. Its LED went from red + green to solid
green: bound. Then `mpm binddone`, restart without `bind`, power-cycle the
receiver without the button: red, then solid green within a moment.

## Step 2: prove the link is ours

Solid green is promising, but is it *our* signal? A test with a clear yes/no:

> Stop transmitting for 15 seconds, then start again, while watching the LED.

It went red when we stopped and green when we restarted. Nothing else in the room
could have made that happen at those moments. **Change the cause, watch the
effect** is the strongest evidence you can get without instruments.

A practical detail: over the tunnel, a console command takes a few seconds, so
"15 s off" was really about 25 s. The script printed timestamps for each step, so
the observation could be lined up with what actually happened, not with what was
planned.

## Step 3: measure, not just look

An LED says "linked or not". It doesn't say whether the channel values arrive
correctly, or how many packets are lost. The XM+'s SBUS output carries both: 16
channel values and a "frame lost" flag, about 111 times per second.

### An ESP32 as an instrument

[`tools/sbus_monitor/sbus_monitor.ino`](../../tools/sbus_monitor/sbus_monitor.ino)
turns an ESP32 into an SBUS reader:

- SBUS is 100,000 baud, 8 data bits, even parity, 2 stop bits, **inverted**. The
  ESP32's UART can invert in hardware:
  `Serial2.begin(100000, SERIAL_8E2, RX_PIN, TX_PIN, true)`.
- A frame is 25 bytes: `0x0F`, 22 bytes holding 16 × 11-bit channels (least
  significant bit first), a flags byte (bit 2 = frame lost, bit 3 = failsafe),
  `0x00`. Frames are separated by a gap of a few ms; the sketch uses that gap to
  find the start.
- Once a second it prints: frames received, frames flagged lost, failsafe, and
  channels 1–8.

The ESP32 also powers the XM+ from its 5 V pin. Then, instead of copying its output
by hand, the ESP32 was shared over the same USB/IP tunnel as the SDR. Vega-1 could
read the receiver's output directly ([`tools/sbus_tune.py`](../../tools/sbus_tune.py)
logs it with timestamps). That closed the loop: **send from the board, read what
the receiver decoded, on the same machine, automatically.**

### Channel values: exact

Set each channel to a different value from the console and compare with SBUS:

| Ch | Sent (Multiprotocol 0–2047) | Expected SBUS | Received |
|---|---|---|---|
| 1 | 1844 (+100%) | 1811 | 1811 |
| 2 | 204 (−100%) | 172 | 171 |
| 3 | 1024 (centre) | 992 | 992 |
| 4 | 1500 | 1468 | 1467 |
| 5 | 0 (−125%) | below 0 → clipped | 0 |
| 6 | 2047 (+125%) | 2015 | 2014 |
| 7 | 500 | 467 | 467 |
| 8 | 1700 | 1667 | 1667 |

"Expected" comes from Multiprotocol's FrSky scaling (`value × 15/16 + 1290` on air)
and FrSky's SBUS scaling (±100% = 172/1811, centre 992), computed *before* reading
the result. Using values all over the range (not just centre and ends) checks the
whole mapping, not just two points.

### Lost frames: a frequency question

At first, 2–5% of frames were lost, at under a metre. That's too many. Two
candidates:

- **Timing**: a late packet, if the CPU was busy (for example printing a long
  status over the console, since the shell and the protocol share one loop).
- **Frequency**: the receiver's crystal and ours don't agree exactly, so our
  packets land slightly off-centre in its filter.

Multiprotocol has a per-receiver frequency fine tune for exactly this (the
`option` byte, written to the CC2500's `FSCTRL0`, about 1.59 kHz per step). The
firmware got a new command, `mpm option N`, that changes it while the protocol
runs, and `sbus_tune.py --sweep` steps it and measures the loss at each value:

| Option | −120 | −100 | −80 | −60 | −50 | −40 | −30 | −20 | 0 | +10 | +20 | +40 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| Lost | 100% | 47% | 4.3% | 1.3% | 1.4% | 1.1% | 1.1% | 1.7% | 1.9–2.6% | 7.7% | 39% | 100% |

The receiver's window is about −100 to +20 at the 50% points, so its centre is
near **−40 (−63 kHz)**. We had been transmitting near the window's upper edge.
At −40 over 87 s: **0.74% lost, no failsafe**, 56 of 87 seconds with no loss at
all, and no late callbacks (worst 18 µs).

Whose crystal is off? Our 40 MHz reference's tuning DAC was at mid-scale (nominal),
so most of the 26 ppm is probably the XM+'s. That's normal for these receivers,
which is why Multiprotocol makes the fine tune a per-model setting. Without an
independent frequency reference we can't be sure, and the docs say so.

### The open question

Lost frames come in pairs (2, 4, 6 per second). That could be how the XM+ reports
a missed packet, or a real effect on our side (for example one hop channel that's
worse than the rest). It's written down as open, with the data, rather than
explained away.

## What it led to

The first end-to-end proof: Multiprotocol's unmodified FrSky code, on a soft CPU,
through a virtual CC2500, a Verilog modulator and the AD9361, controls a real
receiver, with exact channel values and under 1% frame loss. Next steps are in
chapter 11.

## The thinking move

**Escalate the evidence step by step, and turn observations into numbers.** LED on
→ LED follows our transmitter on/off → the receiver's own output matches
predicted values → a lost-frame rate measured against a swept parameter. And when
a human is relaying observations, give the machine a way to observe directly: the
ESP32 on USB/IP turned a ten-minute back-and-forth into a script.
