# LoRa and ExpressLRS

ExpressLRS (ELRS) sends its control packets as LoRa (or FLRC) frames from Semtech
radios: SX127x / LR1121 at 900 MHz, SX1280 / LR1121 at 2.4 GHz. This page covers
the LoRa physical layer on MultiTX: the coding, the modulator, how both were
checked, and what is still unknown for ELRS.

## What ELRS sends

From `src/src/common.cpp` and `src/lib/SX1280Driver` in the ExpressLRS source:

| Band | Rates | Spreading factor | Bandwidth | Coding rate | Preamble |
|---|---|---|---|---|---|
| 2.4 GHz LoRa (SX1280) | 500, 333 (8 ch), 250, 150, 100 (8 ch), 50 Hz | SF5–SF8 | 812.5 kHz ("800") | **4/6 LI, 4/8 LI** | 12–14 |
| 2.4 GHz FLRC (SX1280) | 1000, 500 Hz (+ DVDA) | – | 650 kbit/s | 1/2 | 32 bits |
| 900 MHz LoRa (SX127x) | 200, 100, 50, 25 Hz | SF6–SF9 | 500 kHz | 4/7, 4/8 | 8–10 |

All LoRa rates use an **implicit header** (fixed 8-byte "OTA4" or 13-byte "OTA8"
packets), the LoRa CRC **off** (ELRS has its own CRC inside the packet), and the
IQ inversion as a per-link parameter. The LoRa sync word isn't set, so it is the
chip's default (on the SX1280 that is the "private" 0x12; that it maps to the
same two chirps as on an SX127x is an assumption until a real capture shows it).

**The open question is the 2.4 GHz coding.** Every 2.4 GHz LoRa rate uses the
SX1280's *long interleaving* ("LI") coding rates. Semtech's data sheet names them
and gives their time on air, but not the interleaver; no public decoder handles
them (as of October 2026). The 900 MHz rates use the standard SX127x coding,
which is publicly reverse engineered (Knight 2016, Robyns et al. gr-lora, Tapparel
et al. gr-lora_sdr) and is what this page's model implements and checks. FLRC is
undocumented as well. So the plan is:

1. **900 MHz ELRS first** (standard coding, all known). The B220 transmits there
   too: a tone through the TX/RX → 30 dB → RX2 cable is clean at 868 and 915 MHz.
2. **2.4 GHz LoRa after that**, once the LI interleaver is learned from captures
   of a real ELRS 2.4 GHz transmitter (the same approach as the A7105's coding,
   [multiprotocol.md](multiprotocol.md)).

Hardware that fits this: a dual-band LR1121 receiver (900 MHz with standard
coding, 2.4 GHz with LI), and an ELRS transmitter module to capture.

## The coding (tools/lora.py, fw/src/lora_code.c)

```
payload ─ whitening ─ nibbles (low first) ─ [header] ─ [CRC] ─ Hamming 4/5..4/8
        ─ diagonal interleaver ─ inverse Gray, +1 ─ chirp symbols
```

- **Whitening:** XOR with an LFSR sequence (next bit = b7 ^ b5 ^ b4 ^ b3, from
  0xFF), payload only.
- **Explicit header:** length, coding rate and CRC flag, then a 5-bit checksum
  (5 nibbles). Implicit mode leaves it out.
- **CRC-16:** CCITT (poly 0x1021, init 0) over all but the last two payload bytes,
  then XOR with those two bytes; appended low byte first, not whitened.
- **Hamming:** each nibble becomes d0 d1 d2 d3 + 1..4 parity bits
  (4/5: overall parity; 4/6..4/8: d0^d1^d2, d1^d2^d3, d0^d1^d3, d0^d2^d3).
- **Interleaver:** blocks of SF codewords make 4+CR symbols; symbol i takes bit i
  of codeword (i − j − 1) mod SF for its bit j. The first block always uses
  SF − 2 codewords at 4/8 plus a parity bit and a zero ("reduced rate"), as do all
  blocks with the low data rate optimisation.
- **Mapping:** inverse Gray code, then +1 (mod 2^SF).
- **Frame:** preamble up-chirps, two sync-word chirps (8 × each nibble), 2.25
  down-chirps, data chirps. Inverted IQ runs the chirps the other way.

The unused rows of the last block are padding. A real SX127x fills them with
values we haven't found a rule for; receivers ignore them, so the encoder pads
with zeros.

### How it was checked against real chips

The decoder in `tools/lora.py` uses only the data bits of each codeword (no error
correction), so it doesn't depend on the parity rules. It was run on public
recordings of a Microchip RN2483 (SX1276 inside) from the gr-lora project
(HackRF at 10 MSPS and a USRP at 1 MSPS, 868.1 MHz, SF7, BW 125 kHz, sync word
0x34), each with a known payload:

```bash
python3 tools/lora.py decode usrp-868.1-sf7-cr4-bw125-crc-0.sigmf-data --fs 1e6 \
    --offset 100e3 --sf 7 --sync 0x34 --check --freq 868.1e6
```

`--check` re-encodes every decoded payload (with the chip's padding) and compares
**every symbol** with what the chip sent. Result: **68 of 68 frames identical**,
covering CR 4/5, 4/6, 4/7 and 4/8, CRC on and off, and payloads of 1 to 255 bytes
(600-symbol frames). That confirms the whitening, header checksum, CRC, all four
Hamming codes, both interleaver modes, the Gray mapping and the chirp convention
against silicon. SF5/SF6 and the implicit header follow the same rules (checked
by the model's own encode-decode test, `lora.py selftest`, for SF5–SF12); the
first real SF5/SF6 check will be an ELRS receiver.

Three receiver details the recordings forced:

- **Continuous-phase chirps.** Building the reference chirp from a running sum of
  per-sample frequencies leaves it half a bin off a real chirp; the model uses the
  exact phase integral.
- **Oversampled demodulation.** At one sample per chip, the part of a symbol after
  its frequency wrap aliases onto the same bin with a phase step that depends on
  sub-sample timing, so symbols near a bin edge flip. The decoder keeps 8 samples
  per chip, where the two parts land in different bins (s and s − N), and adds
  their magnitudes.
- **Timing drift.** A 0.6 s frame from a crystal 20 ppm off slips by 1.5 chips;
  `--freq` derives the drift from the carrier offset (one crystal sets both).

`fw/test/test_lora_code.py` builds the firmware's encoder for the PC and compares
it with the model: 1024 random cases (SF5–12, every CR, header mode, CRC, LDRO,
0–255 bytes), all identical.

## The modulator (hw/ip/lora_tx)

The firmware writes the frame as a list of chirps (type and start value); the
modulator plays them back. A chip NCO (increment BW / fs) clocks the chips, and
the frequency is computed from the chip position every sample:

```
up:    f = BW * (((c + s) mod N + frac) / N) - BW/2
down:  f = BW/2 - BW * (c + frac) / N
```

so nothing accumulates and nothing drifts. A phase accumulator adds the channel
offset (the radio layer's LO slices and NCO, as for GFSK), then the same sin/cos
table and amplitude ramp as `gfsk_tx`. `tx_select` passes whichever modulator is
active to the AD9361.

`hw/ip/lora_tx/tb/check.py` simulates it in xsim and decodes the output with the
model's receiver, comparing every data symbol:

| Case | Result |
|---|---|
| SF7, BW 500 kHz, 4/8, explicit + CRC, 4 MSPS | 64 symbols equal |
| SF8, BW 125 kHz, 4/5, explicit, +300 kHz offset | 18 symbols equal |
| SF5, BW 812.5 kHz, 4/6, implicit 8 bytes, IQ inverted, 61.44 MSPS, −7 MHz | 26 symbols equal |
| SF6, BW 812.5 kHz, 4/8, implicit 8 bytes, 61.44 MSPS, +10 MHz | 24 symbols equal |

## On the board

```
mtx> radio rate 4e6
mtx> lora cfg 5 812.5e3 2 nocrc implicit pre 12 sync 12 iqinv
mtx> lora send 2440e6 0102030405060708 -10
```

`tools/lora_check.py` does this for several configurations and frequencies with
RX2 capturing each frame through the loopback cable (30 dB attenuator, about
−35 dBFS received), and decodes it, comparing every data symbol:

| Case | Frequencies | Frames |
|---|---|---|
| ELRS-like 2.4 GHz: SF5, 812.5 kHz, 4/6, implicit 8 bytes, IQ inverted, preamble 12 | 2405, 2440, 2478 MHz | 17/18 (then 12/12 on a repeat) |
| ELRS-like 2.4 GHz: SF6, 812.5 kHz, 4/8, implicit, preamble 14 | 2410, 2465 MHz | 4/4 |
| ELRS-like 900 MHz: SF6, 500 kHz, 4/7, implicit, preamble 8 | 868.4, 915 MHz | 4/4 |
| SF5, 812.5 kHz, 4/8, explicit header + CRC, sync 0x34 | 2440 MHz | 2/2 |

30 of 31 frames exact. The one miss (the first frame of a run, at 2405 MHz) did
not come back in 12 repeats; the decoder uses no error correction, which a real
receiver would, so a single bad symbol at this level is plausible but not
confirmed. Bitstream `multitx_p4_lora.bin` (WNS +0.67 ns; `lora_tx` uses 6 DSPs,
80 LUT RAMs, 4 block RAMs).
