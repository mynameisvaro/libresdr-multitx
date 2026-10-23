# 11. Where it goes next

## Where we are

| Works on hardware | Verified how |
|---|---|
| Platform: MicroBlaze, USB debug bridge, capture, AD9361 control | self-test 8/8 |
| AD9361 driver, sample interface 5–61.44 MSPS | digital loopback |
| RF chain, board quirks handled | tone test through cable + attenuator |
| GFSK modulator with channel NCO | simulation + independent demodulator, loopback, over the air |
| Multiprotocol FrSky D8/D16 on a virtual CC2500, hopping over 70 MHz | own CC2500-style decoder |
| FrSky XM+ (D16) bound and linked | receiver LED, failsafe test, SBUS values, 0.74% frames lost |
| Multiprotocol FlySky AFHDS/AFHDS2A on a virtual A7105 | own A7105-style decoder; coding still a guess until learned from a handset |

## Open questions, written down

These are known and recorded, not forgotten:

- **The A7105 coding** (chapter 9): learn it from a FlySky handset with
  `a7105_sniff.py`, then make it the firmware default and test a real FlySky
  receiver.
- **Lost frames in pairs** (chapter 10): an XM+ quirk, or one bad hop channel?
  Logging per frame (not per second) and comparing with the 47-packet hop cycle
  would tell.
- ~~**Long console commands delay the protocol**~~ (chapter 10): solved in
  [chapter 12](12-timer-interrupt.md), the protocol now runs from a timer
  interrupt.
- **Spurs at −5 MHz and ±8 MHz** in the tone test (chapter 6).
- **Output power** isn't calibrated; the AD9361 alone gives about +5 dBm, against
  20–30 dBm for commercial modules. A range test, then an external PA.

## Possible next steps

| Step | What it unlocks | Size |
|---|---|---|
| FlySky with a real handset and receivers | the second chip family on real hardware | small (tools exist) |
| ~~Handset input~~ from the PC over USB: done (Multi serial frames, `multi_link.py`). Left: PPM from a FlySky trainer port on the GPIO pins | flying from the PC or a real radio instead of console commands | small to medium |
| GFSK receive path in the FPGA | FrSky telemetry, AFHDS2A bind replies without the PC, NRF24 auto-ack | large: a demodulator, timing recovery, sync search |
| More virtual chips: NRF24L01, CYRF6936 (DSSS), SX1276 (LoRa) | many more protocols | medium to large each |
| ExpressLRS | a modern long-range protocol | large: LoRa modem, FLRC is undocumented |
| Stand-alone boot | no PC needed: something (e.g. a Pi Zero) loads the FPGA at power-up, since the board has no flash | small to medium |
| Persistent TX ID and settings | survive reboots without a fixed ID | small, needs somewhere to store them |

## How to choose

The same questions that picked every step so far:

1. **What's the riskiest unknown left?** Do that before building on top of it.
   (The coding of the A7105 is one: everything FlySky depends on it.)
2. **What does the next test need?** Handset input is what turns this from a demo
   into something you can fly. The receive path is what most other protocols need.
3. **Can it be tested when it's done?** A step that can't be verified on available
   hardware should wait for the hardware, or come with its own instrument.
4. **What's the smallest version that proves the idea?** The GFSK receiver can
   start as "capture and decode one bind reply in the FPGA", not "full telemetry
   for every protocol".

## The thinking move

**Keep a written list of what's proven, what's open, and what's next.** It makes
choosing the next step a short discussion instead of a guess, and it's what lets
someone else (or you, in six months) pick the project up.
