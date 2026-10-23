# 14. LoRa, checked against real chips

## Where we were

FrSky and FlySky are FSK protocols, and the GFSK modulator (chapter 7) covers
them. ExpressLRS, the protocol most FPV pilots fly today, is different: it sends
**LoRa**, Semtech's chirp spread spectrum, from SX127x, SX1280 and LR1121 radios.
No receiver or ELRS transmitter was available yet (one is on order), so this step
had to be built and proven without one.

## The problem

Two of them:

1. **What exactly does ELRS send?** Not "LoRa" in general: which spreading
   factors, bandwidths, coding rates, header mode, CRC, sync word.
2. **How do you check a LoRa transmitter without a LoRa receiver?** A modulator
   checked only by a decoder we wrote ourselves can be wrong in exactly the same
   way as the decoder (the chapter 10 lesson).

## Step 1: read the source, not the marketing

The ELRS source answers question 1 (`common.cpp`, `SX1280Driver`): at 2.4 GHz,
SF5–SF8 at 812.5 kHz bandwidth; at 900 MHz, SF6–SF9 at 500 kHz; implicit header,
CRC off, IQ inversion per link.

And one surprise: every 2.4 GHz LoRa rate uses the SX1280's **"LI" coding rates**,
*long interleaving*. Semtech doesn't document that interleaver, and a search found
no public decoder for it. The 900 MHz rates use the standard coding, which
researchers reverse engineered years ago. That changed the plan (and the shopping
list): **900 MHz ELRS first**, 2.4 GHz once the LI interleaver can be learned from
captures, just as the A7105's coding was (chapter 9). A quick tone test showed the
B220 transmits cleanly at 868 and 915 MHz too.

## Step 2: an oracle made of silicon

The fix for question 2 is to check against something we didn't write. The gr-lora
project published recordings of a real LoRa chip (an RN2483, which has an SX1276
inside) sending known payloads: different coding rates, CRC on and off, 1 to 255
bytes.

So the model, [`tools/lora.py`](../../tools/lora.py), got a decoder that reads only
the data bits of each codeword (no error correction), and a check that goes one
step further than "did the payload come out right":

> decode the real frame → re-encode its payload with our encoder → compare **every
> symbol** with what the chip actually sent.

If our encoder disagrees with the chip about anything (the parity bits, the
interleaver, the padding, the CRC), some symbol differs. A payload match alone
wouldn't show parity errors, because the decoder doesn't use the parity bits.

## What the recordings taught us

The first runs found **no frames at all**: the RN2483 uses sync word 0x34 (the
"public" LoRaWAN one), not 0x12. Printing the raw peak bins showed it directly:
the preamble at bin 29, then 53 and 61, which is +24 and +32.

Then the header decoded correctly (length, coding rate, checksum all right), but
the payload was `feadbeef` instead of `deadbeef` and many symbols were one bin off.
Three receiver problems, each found by looking at the numbers:

- **Peaks between bins.** The chip's timing doesn't line up with our samples, so
  some peaks fall halfway between two bins. Fixed by keeping 8 samples per chip
  and choosing the sample phase nearest the chip boundary.
- **The wrapped part of a chirp.** At one sample per chip the part of a symbol
  after its frequency wrap lands on the same bin with a phase that depends on
  sub-sample timing. Dechirping at 8 samples per chip keeps the two parts in
  different bins, which are then added.
- **The reference chirp itself was half a bin off.** It was built as a running sum
  of per-sample frequencies; a real chirp is the exact integral. The RTL testbench
  exposed this one (SF5 failed), not the recordings.

What was left was real knowledge about the chip: its CRC (CCITT over all but the
last two bytes, XORed with them, which looks odd but is what the chip does), and
**padding**: the chip fills the unused rows of the last block with values that
aren't zeros. Receivers ignore them, so our encoder pads with zeros, and the check
takes the chip's padding into account.

Finally, the 255-byte frames failed after symbol 240: the chip's crystal is 20 ppm
off, so over a 0.6 s frame its timing slips by 1.5 chips. One crystal sets both
the carrier and the chip clock, so the drift follows from the measured frequency
offset.

Result: **68 of 68 real frames reproduced symbol for symbol.**

## Step 3: the modulator

[`lora_tx.v`](../../hw/ip/lora_tx/lora_tx.v) plays back a list of chirps the
firmware writes (preamble, sync word, down-chirps, data). Its one idea worth
copying: **compute, don't accumulate**. Instead of ramping a frequency register
up a little every sample (which drifts with rounding), the frequency is calculated
fresh every sample from where we are in the symbol:

```
f = BW * (((chip + s) mod N + fraction) / N) - BW / 2
```

`chip` and `fraction` come from a chip-rate NCO, so the chirp is locked to the
chip clock by construction. Everything after that (channel offset, sin/cos table,
amplitude ramp) is the same as the GFSK modulator, and a small `tx_select` block
passes whichever modulator is active to the AD9361.

The firmware's encoder ([`lora_code.c`](../../fw/src/lora_code.c)) is the model in
C, built for the PC by a test that compares the two on 1024 random cases.

## How we knew it worked

The testbench ([`check.py`](../../hw/ip/lora_tx/tb/check.py)) simulates the Verilog
and decodes its output with the silicon-checked model: four cases, including the
two ELRS-like ones at the board's real 61.44 MSPS (a non-integer 75.6 samples per
chip), with IQ inversion and channel offsets of −7 and +10 MHz. Every data symbol
equal.

The chain of trust, from the bottom: real SX1276 recordings → the Python model →
the C encoder (1024 cases) and the Verilog modulator (simulation) → the board
(`lora_check.py`, through the loopback cable).

## What it led to

The physical layer is ready for ELRS at 900 MHz. Next: the ELRS protocol itself
(packet format, its CRC, frequency hopping and timing from the bind phrase) on top
of `lora_tx`, tested against a real ELRS receiver when it arrives; and learning the
SX1280's LI coding from a 2.4 GHz ELRS transmitter.

## The thinking move

**Find an oracle you didn't build.** Public recordings of real hardware turned "our
encoder and our decoder agree" into "our encoder agrees with the chip". And make
the check strict: comparing every symbol, not just the payload, is what caught the
CRC rule and the padding. When there's no oracle (the LI coding), say so, and
order the work so the known parts come first.
