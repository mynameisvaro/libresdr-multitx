# 15. ExpressLRS without a receiver

## Where we were

Chapter 14 left a LoRa modulator whose output matches real SX1276 chips symbol for
symbol, at 2.4 GHz and at 868/915 MHz. The PPM input was proven on the board the
same evening (chapter 13). The next step on the plan was ExpressLRS itself at
900 MHz: not a physical layer any more, but a protocol, with packets, CRCs,
frequency hopping and timing.

There was still no ExpressLRS receiver here, and no trainer cable for the FlySky.

## The problem

An ExpressLRS receiver accepts a transmitter only if everything is exact:

- the **UID** derived from the binding phrase,
- the **CRC**, seeded by the UID, the protocol version *and* the packet's slot
  counter (the "nonce"), so a TX one slot off is a TX with every CRC wrong,
- the **hop sequence**, a pseudo-random permutation of the domain's channels,
  also from the UID,
- the **packet layouts**: 10-bit channels packed four into five bytes, a switch
  carried in a slot chosen by the nonce, sync packets with their bit fields,
- the **slot rules**: which slot carries a sync packet, which is left silent for
  the receiver's telemetry, when the next hop starts.

None of it can be checked against a receiver yet. And a TX checked only against a
model we wrote ourselves can be wrong in the same way as the model (chapters 10
and 14).

## Options and the decision

1. **Write the protocol from scratch in C**, from reading the ExpressLRS source.
   Everything would be our reading of it, and the receiver is the only judge.
2. **Port ExpressLRS's transmitter wholesale.** `tx_main.cpp` is 1,600 lines tied
   to ESP32 timers, the radio driver, CRSF, Lua, WiFi and the backpack: most of it
   isn't the protocol.
3. **Build ExpressLRS's own over-the-air code unmodified, and write only the
   scheduler.** The format (packets, CRC, hop sequence) lives in four small files:
   `OTA.cpp`, `FHSS.cpp`, `random.cpp`, `crc.cpp`. The *when* (which packet in
   which slot) is a page of `tx_main.cpp`.

Option 3, the same move as with Multiprotocol (chapter 8): reuse the code that
defines the format, replace only what touches hardware. ExpressLRS became a git
submodule at tag 4.1.0, the four files compile against four stand-in headers
(`fw/src/elrs/shim/`), and `elrs_tx.cpp` is the scheduler.

Two smaller decisions came from reading the source:

- **4.x only.** Comparing 3.6 and 4.1 showed the CRC seed, the arm bit, the rate
  numbers and the EU868 band edges all changed: a 4.x TX can't talk to a 3.x
  receiver. Supporting the current version and saying so beats half-supporting
  both. Receivers can be updated over WiFi.
- **Binding phrase, no bind mode.** Bind mode sends the UID through ExpressLRS's
  MSP data channel with its acknowledgement scheme: real work for a feature the
  binding phrase makes unnecessary.

## How it was built

### Fitting ExpressLRS's code in

ExpressLRS's headers expect an Arduino build: `targets.h` includes `<Arduino.h>`,
`FHSS.cpp` wants the firmware options and the radio driver. Shadowing `targets.h`
with our own didn't work: `crsf_protocol.h` includes `"targets.h"`, and a quoted
include looks in the including file's directory first, so the real one always
won. The fix was to *satisfy* the real one (define `RADIO_SX127X`, `TARGET_TX`, a
regulatory domain, and provide `Arduino.h`) and stand in only for the leaf
headers: `logging.h` (no output), `options.h` (just the domain index),
`SX127xDriver.h` (the frequency step).

One C++ detail cost a few minutes: a `const` array at namespace scope has internal
linkage in C++. `FHSS.cpp`'s domain table can't be reached from another file, so
`elrs_tx.cpp` keeps its own copy of the band edges, used to centre the radio.

### The scheduler

```
every period, from the timer interrupt (the Multiprotocol runner's alarm):
  nonce++
  telemetry slot?                      -> send nothing
  sync channel, sync interval passed,
  free sync slot?                      -> SYNC packet
  otherwise                            -> RC packet from the handset's channels
  CRC, send
  next slot starts a hop?              -> next frequency
```

It runs as protocol 200 in the Multiprotocol runner, so everything chapter 13
built (USB frames, the gamepad, PPM, the link-loss pause) drives it unchanged.

### The radio

At 61.44 MSPS an LO "slice" covers 35 MHz, but the slice grid was anchored for
2.4 GHz, so FCC915's 903.5–926.9 MHz would have straddled two slices, and the
calibrations ran at 2440 MHz. `radio_set_band` now centres a slice on the
protocol's band and calibrates there. A whole ExpressLRS domain fits one slice, so
every hop is just an NCO frequency, with no AD9361 retune at all.

## How we knew it worked

### Two implementations that must agree

`tools/elrs.py` is a second, independent implementation, written from the same
source but in Python and without looking at the firmware: UID, CRCs, hop
sequence, packet layouts, slot rules. `fw/test/test_elrs.py` builds the
firmware's transmitter for the PC (with ExpressLRS's real `OTA.cpp`, `FHSS.cpp`
and friends) and runs both side by side with random sticks: 288 cases (six rates,
four domains, three switch modes, telemetry ratios, model match), every slot, every
byte.

They agreed on the first run, and **that is the moment to be suspicious**. So the
test was checked too: breaking the model on purpose (the CRC seed off by one bit,
the switch index without its rotation) made 197 and 94 slots differ. A test that
can't fail proves nothing.

### On the board

`tools/elrs_check.py` captures packets off the loopback cable and decodes them
with the LoRa decoder from chapter 14 (the one that matches real chips), then
checks them with `elrs.py`: the CRC must match for some nonce, and that nonce's
switch slot must carry the value we set; the frequency must be in the hop table;
a sync packet must carry the hop index of the channel it was caught on.

The first board run found a problem no PC test could: at 200 Hz, **578
packets weren't sent** in the first few seconds, about every other one. The
firmware measured why: a slot's work took
up to 723 µs, and the frame is 4384 µs of the 5000 µs slot. The previous frame
was still on air when the next one wanted to start.

Where do 700 µs go on a 100 MHz CPU? Into floating point. The MicroBlaze here
has no FPU, so every `double` operation is a library call, and `lora_send`
computed `pow(10, dBFS/20)`, a `ceil` and two divisions for every frame,
although they only change with the settings. Caching them brought the slot
work to about 320 µs. The one remaining 700 µs slot was the first after a
start, which also sets the TX gain over SPI, so the second frame now waits
for it instead of being dropped.

Then every rate passed: the slot rate exact against the firmware's clock (200.0
per second at 200 Hz), nothing dropped, no late callbacks, no LO retune (the
whole band in one slice), and 293 µs to spare after the frame at 200 Hz.

Off the air, 20 packets at four rates all passed: CRC with the right nonce,
sticks and switch slot as set, frequency in the hop table, every LoRa symbol, and
a sync packet per rate carrying the hop index of the very channel it was caught
on. **27 of 27 checks.**

## What it led to

The transmitter is as checked as it can be without a receiver. Next: a real
ExpressLRS 4.x receiver with the same binding phrase, the ESP32 on its output,
link quality at 200 Hz; then telemetry (a LoRa receive path), and 2.4 GHz once the
SX1280's coding is learned.

## The thinking move

**Reuse the code that defines the format; write only the part that touches your
hardware.** Then check your part against a second implementation you wrote
separately, and check the check by breaking it on purpose. When two
implementations agree immediately, that's either good news or a test that can't
fail, and a minute of sabotage tells you which.
