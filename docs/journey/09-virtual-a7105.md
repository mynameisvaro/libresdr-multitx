# 9. FlySky and the coding nobody documented

## Where we were

Multiprotocol's FrSky code runs on a virtual CC2500 and its packets decode off the
air (chapter 8). The FlySky receivers use a different chip, the AMICCOM **A7105**.

## The problem

Do for the A7105 what was done for the CC2500. Most of it is the same recipe
(byte-level SPI slave, registers, strobes, FIFO, build the packet on the TX
strobe), but two things are new:

1. The A7105 adds **forward error correction**: each 4-bit nibble is sent as a
   7-bit Hamming code word, so the receiver can fix single-bit errors. AFHDS2A uses
   it.
2. The datasheet describes the packet layout, but **not the exact coding**: it
   doesn't give the CRC-16's initial value or bit order, nor the bit layout of the
   Hamming code words.

A virtual chip that gets the CRC or the FEC layout slightly wrong produces packets
that look perfect on our own decoder and are silently thrown away by every real
receiver.

## What was built first: the chip

[`fw/src/a7105.c`](../../fw/src/a7105.c) follows the same pattern as the CC2500:

- SPI framing (strobe commands have bit 7 set; register access is
  `{0, R/W, address}`), multi-byte access to the FIFO and ID registers.
- On the TX strobe: preamble, **ID code** (the A7105's equivalent of a sync word),
  payload, CRC-16, FEC and whitening as enabled, frequency from the LO base and
  channel registers, data rate, deviation, Gaussian filter and power from the
  registers.
- Both chips share the SPI bus with their own chip selects, as on a real
  Multiprotocol module, so FlySky (AFHDS) and AFHDS2A were enabled in the core with
  no changes to their files.

[`tools/flysky_check.py`](../../tools/flysky_check.py) decodes the packets like an
A7105 (ID code, FEC, CRC), checks the channel against the hop table and the
payload against what the protocol should send. AFHDS 8/8, AFHDS2A 8/8, bind 4/4.

But that only proves our packets match *our* coding. The question in "the
problem" remained.

## Options for the unknown coding

| Option | Problem |
|---|---|
| Guess, and try receivers until one binds | many combinations, and "doesn't bind" gives no information about *which* part is wrong |
| Take it from other projects' code | possible, but a value copied from somewhere else is still unverified until a real chip confirms it |
| **Learn it from a real A7105**: capture a FlySky handset's packets and solve for the coding | needs a handset, but gives a definite answer |

The decision had two halves:

1. **Make the coding a run-time setting**, not a constant. `struct a7105_coding`
   holds the CRC polynomial, initial value, reflection, output XOR and the 16 FEC
   code words; console commands `a7105 crc ...` and `a7105 fec ...` set them. The
   default is a reasonable guess (CRC-16/CCITT-FALSE, textbook Hamming(7,4)).
2. **Write a tool that solves for it** from captured packets:
   [`tools/a7105_sniff.py`](../../tools/a7105_sniff.py).

## How the sniffer solves for the coding

This is the most "mathematical" part of the project, and a good example of
turning an unknown into a solvable problem.

**FEC.** A Hamming(7,4) code is *linear*: the code word of `a XOR b` is the XOR of
their code words. So among all packets, the 7-bit words seen must form a set of
exactly 16 words closed under XOR. Find that set, and every way of mapping it to
nibbles that keeps it linear is a candidate (there aren't many). Then keep only
the candidates for which byte 0 of every packet is a known AFHDS2A packet type
(`0xBB`/`0xBC` bind, `0x58` sticks, `0xAA` settings, `0x56` failsafe). Packets of
more than one type make the answer unique.

**CRC.** For a CRC with initial value `init` and output XOR `x`, and two messages
`a` and `b` of the same length:

```
CRC(a) XOR CRC(b) = CRC_0(a XOR b)       (CRC_0 = same polynomial, init 0, no XOR)
```

The unknown initial value and output XOR cancel out. So try each polynomial (and
bit order) on *pairs* of packets: only the right one makes this equation hold for
every pair. With the polynomial known, one packet gives the initial value. That's
also why the sticks must move while capturing: identical packets have a zero
difference and say nothing.

**Testing the tool without a handset.** The firmware's coding is settable, so the
board itself can play "a FlySky handset with an unknown coding": set a *different*
coding (another Hamming layout, low nibble first, CRC poly 0x8005, init 0x1D0F,
reflected) without telling the sniffer, transmit, and see if the sniffer recovers
it. It did, exactly: FEC and CRC from 12 AFHDS2A bind packets, and the AFHDS CRC
from 9 packets at different stick positions. `--apply` then sets the result on the
board.

## The other AFHDS2A problem: binding is a conversation

AFHDS2A binding goes both ways: the transmitter sends bind packets, the receiver
**answers with its own ID**, and from then on the transmitter puts that ID in
every packet. Our virtual A7105 has no receiver.

Instead of building a full GFSK receiver for one reply,
[`tools/afhds2a_bind.py`](../../tools/afhds2a_bind.py) does it from the PC:

1. Run the protocol in bind mode at 4 MSPS, with the RX LO following the TX
   (`radio rxfollow 1`), so one capture covers a whole packet period.
2. After each of our bind packets, capture the air on RX2 and look for a weaker
   burst in the ~2.6 ms window when Multiprotocol's code would be listening.
3. Decode it like an A7105 (ID code, FEC, CRC) and check it is a bind reply
   (`0xBC`) carrying our TX ID.
4. Queue it in the virtual chip with `a7105 rx HEX`: the next time Multiprotocol's
   code puts the chip in RX, the packet is in the FIFO, with a good CRC.
   Multiprotocol's own AFHDS2A code then finishes the bind by itself.

Tested without a receiver: a made-up reply queued by hand completes the bind and
the data packets then carry that ID; the reply decoder works on a synthetic
capture; with nothing answering it times out cleanly. A receiver with no telemetry
might never answer; for that case there's a manual fallback (`mpm binddone`).

## Where it stands

All of this is verified against the board itself, not yet against a real FlySky
handset or receiver. The step-by-step procedure for that is in
[multiprotocol.md](../multiprotocol.md#testing-with-flysky-at-home): sniff, apply,
check, bind.

## What it led to

Both chips ready for real receivers. A FrSky receiver was tested first, because
its coding was fully documented and needed no handset (chapter 10).

## The thinking move

**When a specification has a hole, don't guess: make the unknown a parameter, and
build an instrument that measures it.** Then test the instrument on a case where
you know the answer (here: the board pretending to be a handset with a hidden
coding). A little algebra (linearity of the FEC, the CRC difference trick) turned
"try combinations until something works" into "solve for it".
