# 6. First RF

## Where we were

The firmware drives the AD9361 and the digital sample path is proven by the
chip's loopback (chapter 5). Nothing has gone through the analog side yet.

## The problem

Get a known signal out of the TX/RX antenna port and back in on RX2, and measure
it well enough to trust the analog chain: right frequency, right level, and how
big the unwanted products are.

## Setting up a test you can repeat

Over-the-air tests vary with where things sit on the desk. A **cable from TX/RX to
RX2 with a 30 dB attenuator** gives the same result every time and keeps the
signal off the air. The attenuator matters: the receiver input mustn't see the
full transmit power.

The signal: the DDS inside `axi_ad9361` makes a +1 MHz tone (a complex tone, so it
appears on one side of the LO only).

### Placing the LOs so every artefact has its own frequency

A real transmitter/receiver pair produces several unwanted things besides the
tone: LO leakage (a spike at the TX LO), the TX image (a mirror of the tone on the
other side of the TX LO), the receiver's DC offset and RX image. If TX and RX use
the same LO, these all land on top of each other and you can't tell them apart.

[`tools/rf_tone.py`](../../tools/rf_tone.py) offsets the two LOs (TX at 2440 MHz,
RX at 2438 MHz), so in the received spectrum:

| Thing | Appears at (RX baseband) |
|---|---|
| the tone (2441 MHz) | +3 MHz |
| TX LO leakage (2440 MHz) | +2 MHz |
| TX image (2439 MHz) | +1 MHz |
| RX DC offset | 0 Hz |
| RX image of the tone | −3 MHz |

Each artefact is then measured separately against the tone. That's a cheap trick
worth remembering: **choose test frequencies so that the things you want to
distinguish land in different places.**

## What the board was hiding

Three facts about the B220 mini surfaced here, none of them in its documentation.
Each would have broken the protocol work later in a confusing way.

### Only the A ports are wired

The AD9361 has three RF ports per direction (A, B, C) for different frequency
ranges. A real B210 uses B or C at 2.4 GHz, and UHD's B210 settings select ports
by frequency. On the B220 mini, the schematic shows baluns (the transformers the
ports need) only on the **A** ports; B and C go nowhere. With UHD's B210 band
edges, the signal went into an unconnected port. The firmware now uses port A at
every frequency, as UHD does for the B205mini (which has the same arrangement).

### Which front-end word controls which chain

Each RF channel has an 8-bit switch/LED word (`gpio_fe`). A real B210 swaps them
(UHD's `swap_atr`). Measured here: AD9361 chain 1 is controlled by front-end
word 1, and word 2 has no effect on it. Testing both was the only way to be sure.

### The transmit amplifier takes 450 ms to wake up

Sweeping the timing between setting TX_ENABLE and measuring the output showed:
about +6 dB at once (the switch), then nothing for ~270 ms, then another ~18 dB,
settled by ~450 ms. Switching off is instant.

An RC protocol sends a packet every few milliseconds. If the amplifier were
switched per packet, nothing useful would come out. So **TX_ENABLE stays on while
a protocol runs, and packets are turned on and off digitally**, by sending zero
samples between them. The plan from chapter 1 (FDD with digital TX gating) had
assumed this for turnaround speed; now it was a hard requirement.

### RX image correction kept turning itself off

The RX image was only −28 dBc, much worse than an AD9361 should do. The chip has
a background **RX quadrature tracking** loop that cancels the image, and it was
off. The driver's calibrations switch it off, and UHD switches it back on from a
layer above the driver (its `ad936x_manager`), which we don't use. The firmware
now re-enables it (register `0x169` = `0xCF`) after every init, rate change and
retune. With it on: −65 dBc.

## How we knew it worked

At 2410, 2440 and 2475 MHz (60 dB TX gain, 40 dB RX gain, 30.72 MSPS):

| | 2410 MHz | 2440 MHz | 2475 MHz |
|---|---|---|---|
| tone | −28.5 dBFS | −29.0 dBFS | −29.6 dBFS |
| TX LO leakage | −35 dBc | −35 dBc | −36 dBc |
| TX image | −48 dBc | −47 dBc | −48 dBc |
| RX DC | −53 dBc | −54 dBc | −50 dBc |
| RX image | −65 dBc | −67 dBc | −66 dBc |

![spectrum](../img/rf_tone_2441MHz.png)

The tone is where it should be across the band, and every artefact is at least
35 dB below it. Some spurs at −5 MHz and ±8 MHz remain unexplained and are noted
as open in [hardware.md](../hardware.md#first-rf-test): an honest "we don't know
yet" is better than pretending.

## What it led to

A trustworthy transmit chain with known quirks, and a capture-and-analyse tool
that can check any signal we send. Next: real modulation, packets made of bits.

## The thinking move

**Expect the clone to differ from the original, and measure each assumption once.**
Port wiring, channel mapping and amplifier timing were all "same as the B210"
assumptions, and three of them were wrong. Each took minutes to measure and would
have taken days to find later, buried under a protocol that "just doesn't bind".
