# 17. The X6B that wouldn't bind

## Where we were

The board could receive (chapter 16), the A7105's coding was learned from a real
FS-i6X, and Multiprotocol's AFHDS2A bind ran on the virtual A7105. But the FS-X6B
receiver never answered it. The one lead from the night before: the handset's
bind packets seemed to sit about 1 MHz below where the data sheet puts channel
140, and `gfsk_rx` had seen the handset's packets 500 kHz low. Shifting the
board's channels by −500, −750 and −1000 kHz hadn't helped.

The X6B sits in an Omnibus F4 flight controller running Betaflight, so it can't
be moved to an ESP32 for SBUS monitoring like the XM+ was.

## The problem

Find out why a receiver that binds to its own handset ignores a transmitter that,
as far as we could tell, sends the same thing.

## Options and the decision

There were two ways to go. Keep chasing the frequency lead with finer shifts, or
first make sure the lead was real. The two measurements behind it disagreed with
each other (−970 kHz from captures, −505 kHz from `gfsk_rx`), and neither method
had ever been checked against a signal of known frequency. **We checked the
instruments first.** The board's own transmitter is a known source: the XM+
links to it within 63 kHz, and its frequency is the LO plus an NCO offset we
set.

After that, rather than guessing what the X6B wants, **we recorded what it
actually does with its own handset**, and then removed the differences one at a
time.

## How it was built

**`tools/freq_cal.py`** sends the FS-i6X's own bind packet (coded like the real
chip) at nine known offsets and measures each with both methods. A straight-line
fit of measured against sent gives each method's scale and bias.

**`grx log` (firmware) and `tools/bind_sniff.py`.** To catch a reply that comes
milliseconds after the handset's packet, the receiver has to re-arm right away,
and the PC is 200 ms away over the tunnel. So the firmware does it: a loop that
reads each packet out of `gfsk_rx` and re-arms within microseconds, keeping the
last 96 in a ring. It stops a few packets after a *trigger*, a packet whose bits
at a given position differ from a pattern. Here: bytes 5–8 not `ff ff ff ff`,
which is the first packet that isn't a plain handset bind packet. The PC reads
the ring afterwards (`grx dump I`), because the console drops output it can't
deliver within 50 ms.

**Test switches**, so experiments don't need new firmware each time:
`radio poffset DB` (power offset for every protocol), `a7105 bindseq handset`
(rewrite Multiprotocol's bind packets into the handset's pattern),
`radio quiet 1` (TX attenuation to maximum between packets), and a readout of how
long after the packet start the receiver gets armed.

## How we knew it worked

**The instruments.** The capture method was exact (−0.7 kHz). `gfsk_rx`'s
estimate had the right scale but read 25.6 kHz low every time: a fixed bias,
which comes from its offset tracker averaging the last bits of the ID. Re-reading
the saved handset captures then put the handset at channel 140 **+24 kHz**,
right on the formula. The "−1 MHz" had been the receiver's 1 MHz offset taken
off twice. The "−505 kHz" was channel 139 (in the handset's hop table) heard
while tuned to 140. **The lead had never existed**, and the shifts we tried
moved us away from the right frequency.

**The real exchange.** With the logger on channel 140, then 13, the X6B's reply
showed up: `bc` + the handset's ID + its own ID `5a459d41` + `01`, 1884 µs after
the handset's packet, on the same channel, about 20 times weaker than the
handset. Its sequence turned out to be Multiprotocol's (`bb`/01, `bc`/01, …).

**Ruling things out.** Each check below changes one thing:

| Question | Test | Answer |
|---|---|---|
| Can the board hear a reply that weak, that soon? | arm-delay readout; FS-i6X bind packets as stand-in replies | yes: armed at 1420 µs; after ~100 s, once the two channel cycles drifted into step, 52 packets caught |
| Does a real A7105 decode our signal? | **clone**: the board sends normal packets with the handset's TX ID, the X6B's ID and the hop table | yes: the X6B's LED goes solid and it sends telemetry back |
| Same bytes, same pattern? | the handset's exact bind packet; its pattern via `bindseq`; its TX ID and hops | no answer |
| Power? | −40 to +26 dB, at 20 cm and with the antennas touching | no answer |
| Waveform? | bit rate, deviation on a run of 56 ones, what comes before the ID, settling after a retune | 500.00 kbps both; 186 vs 198 kHz; the handset sends 16 bits of carrier before the preamble (now added: our power ramp had been eating the first preamble bits); flat to 1 kHz |

The clone is the strongest result: the X6B decodes us and answers us, just not
during a bind.

**A bug on the way, in the USB link.** One firmware build wouldn't load: the last
chunk always timed out. That chunk made a command packet of exactly 1024 bytes,
the SuperSpeed packet size. A USB bulk transfer ends at a short packet, so one
made of whole packets needs a zero-length packet after it, and nothing sent one:
the FX3 waited for more data forever. Earlier builds had simply never ended on
that size. `usbdbg.py` now pads such packets with a harmless status read, in
both directions. The timed-out replies had also been arriving later, in the next
program, which explains a strange "sequence 73" error from earlier in the day.

## What it led to

Still open. What's left is the board's behaviour *between* packets, and smaller
waveform details:

- The board's TX stays on in FDD, so LO leakage and noise are always on the air;
  a real A7105 is silent between packets. `radio quiet 1` is built to test that.
- The handset alternates two power levels 12 dB apart.
- Its deviation is 6% higher, and its frequency overshoots on runs of equal bits.

[docs/multiprotocol.md](../multiprotocol.md#results-with-an-fs-i6x-and-an-x6b-2026-10-24)
has the steps, the logs and the commands to pick it up.

## The thinking move

**Before you follow a lead, check the instrument that produced it.** A
measurement that two methods disagree on is a question about the methods, not
yet about the thing measured. Here a ten-minute calibration against a known
source erased the whole "frequency" story. Then **record the system that works**
(the real handset with the real receiver) rather than reasoning about what it
might need. Finally, **split the problem with one test that cuts it in half**:
the clone separated "can it decode us" from "will it bind to us", and every
later test lived on one side of that cut.
