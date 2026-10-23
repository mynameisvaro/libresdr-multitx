# 16. Receiving, and real chips as judges

## Where we were

Everything so far only transmitted. The receive side existed only as the
sample capture (`adc_capture`) and Python on the PC: good for tests, useless
inside a protocol, which has a few milliseconds to hear a reply. FlySky's
AFHDS2A needs one to bind (the receiver answers with its ID) and sends
telemetry back. And the A7105's coding (its CRC and error correction) was still
a guess, waiting for the user's handset.

That evening the hardware was all at hand: the FS-i6X handset, its X6B
receiver, an ESP32 with an SX1278 LoRa chip, and the XM+.

## The problem

1. A receiver in the FPGA: find a packet in the samples, recover its bits, fast
   enough for the protocol to use them.
2. Make the virtual A7105 receive the way the real chip does, so Multiprotocol's
   own bind and telemetry code works unchanged.
3. Check all of it, and the ExpressLRS work, against chips we didn't write.

## Options and the decision

**Where to decode.** The FPGA could decode everything (CRC, FEC), or only find
the packet and hand over raw bits. The A7105's coding wasn't known yet, so
**the FPGA finds the sync word and keeps N raw bits; the CPU decodes them**
exactly as the virtual chip's transmit side encodes them, with the same
settable coding. Learning the coding then changes no hardware.

**How to demodulate.** For FSK the frequency is the information. A CORDIC turns
each sample into a phase; the phase step from one sample to the next is the
frequency. That works at any signal level (no gain control needed) and for both
FlySky (500 kbit/s) and FrSky (70 kbit/s) with one block.

## How it was built

`hw/ip/gfsk_rx/gfsk_rx.v`, one stage at a time:

```
samples ─ mixer ─ CIC /R ─ CORDIC ─ phase step ─ minus offset ─ bit clock ─ sync ─ bits
```

- **Mixer**: a phase accumulator (the same idea as the modulators') and a
  sin/cos table move the wanted channel to 0 Hz.
- **CIC decimator**: three integrators at the input rate, three combs at the
  output rate, no multipliers. It cuts the 61.44 MSPS down to about 8 samples
  per bit and filters what's outside the channel.
- **CORDIC**: 14 add-and-shift stages rotate the sample onto the x axis; the
  total rotation is its phase.
- **Offset tracker**: a slow average of the frequency, subtracted, so a
  transmitter 50 kHz off still slices at zero. It freezes once the sync word is
  found, so the packet's own bits don't move it.
- **Bit clock**: a counter that wraps once per bit. At every zero crossing it is
  nudged toward the crossing (a digital PLL); between wraps the frequency is
  summed, and the sign at the wrap is the bit.
- **Sync correlator**: the last 32 bits against the sync word, counting
  mismatches. Below the limit, the next N bits go into a buffer.

The firmware side: `radio_rx()` tunes the receive LO if needed and arms the
block; the virtual A7105 arms it when Multiprotocol strobes RX, with the ID code
as the sync word, and decodes the bits when they arrive.

## How we knew it worked

**Simulation first**, with packets made in Python: FlySky and FrSky settings,
channel offsets of ±15 MHz, carrier errors of ±60 kHz, weak and strong, noise
alone. 6 of 6.

**On the board: 0 of 14.** Three separate causes, each found by looking at one
stage at a time:

1. **The level was noise.** A raw capture showed the packet arriving fine, so
   the block itself was suspect. The integrators took the mixer's output one
   pipeline stage late. In simulation a sample came on every clock, so it didn't
   matter; the AD9361 core delivers one every *other* clock, and the integrators
   mostly added idle cycles. The fix was one signal; the lesson was the
   testbench. It now delivers samples on every other clock with garbage in
   between, and the old code fails it exactly as the board did.
2. **Nothing was sent.** A FlySky test packet is 156 hex digits; the console
   takes 128 characters. The shell said "bad hex data" and the script didn't
   look. The test now writes packets straight into the modulator.
3. **Noise matched the sync word.** The offset tracker high-pass filters noise
   into alternating bits, and the FlySky ID is mostly alternating. A level gate
   (a match counts only with a signal there) removed the false matches.

Then **14 of 14 over the air.** And the same real-capture trick confirmed it:
a packet recorded off the board, replayed through the RTL in simulation,
decoded identically.

**A real chip judged ExpressLRS.** The ESP32 + SX1278 (its MOSI pin turned out to
be mislabelled on the board) received the board's 433 MHz ExpressLRS: 306 of 307
packets with a valid CRC, every field right, and arrival times matching the
slot counter to 39 µs. That is the first time silicon, not our own decoder,
accepted MultiTX's LoRa frames.

**The handset taught the A7105 coding.** The capture-based sniffer got only 4
packets (each capture crosses the network), almost identical bind packets: the
error correction came out (a systematic (7,4) Hamming code), the CRC didn't. With
`gfsk_rx` the sniffer got 55 normal packets in 3 minutes on one of the hop
channels the handset itself announced in its bind packet, and exactly one CRC
fitted: CRC-16 poly 0x1021, start 0x1D0F. It's now the firmware default.

**The X6B didn't bind.** That one is still open. The board's bind packets are
right in content, but the receiver never answered. Measuring the handset showed
its channels well below where the data sheet's formula puts them (about 1 MHz at
the bind channel), a real lead, though shifting by a fixed amount didn't fix it
yet.

## What it led to

The receive path exists and works; the next step is the A7105's real channel
frequencies, measured precisely from the handset, then the X6B bind and its
telemetry through `gfsk_rx`. On the ExpressLRS side, the same ESP32 + SX1278 can
run ExpressLRS's own receiver firmware, which turns "a chip decodes our
packets" into "a receiver follows our hops".

## The thinking move

**When the test passes in simulation and fails on hardware, find the one
assumption the simulation made that the hardware doesn't.** Here it was "a
sample every clock". Then put that difference into the testbench, so it fails
the same way, and only then fix it. And whenever you can, let a real chip be
the judge: a capture replayed through the RTL, an SX1278 receiving your frames,
a receiver deciding whether to bind. Each of those found something a
self-consistent test couldn't.
