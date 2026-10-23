# ExpressLRS

MultiTX runs an **ExpressLRS 4.x transmitter** at 900 MHz: the protocol's packets,
CRCs and frequency hopping on top of the LoRa modulator (`lora_tx`,
[lora.md](lora.md)). It is a protocol like the Multiprotocol ones, so every
handset input works with it: the PC over USB (`multi_link.py`, also with a
gamepad), PPM, or the console.

Status: checked against ExpressLRS's own code, off the air through the loopback
cable, and received by a real SX1278 (306 of 307 packets, [below](#a-real-semtech-chip-2026-10-24));
**not yet tried with an ExpressLRS receiver.**

## Using it

```
mtx> elrs phrase my binding phrase     # the same phrase as on the receiver
mtx> elrs domain EU868                 # or FCC915 (default), AU915, IN866, ...
mtx> mpm start elrs 0                  # rate 0 = 200 Hz (mpm list shows them all)
mtx> elrs                              # UID, rate, nonce, hop index, packet counts
```

| Sub-protocol | Rate | LoRa | Packet | Hop every | Telemetry slot |
|---|---|---|---|---|---|
| 0 | 200 Hz | SF6, 4/7, preamble 8 | 8 bytes (4 ch + switches) | 4 packets | 1:64 |
| 1 | 100 Hz Full | SF6, 4/8, preamble 8 | 13 bytes (8–16 ch at 10 bits) | 4 | 1:32 |
| 2 | 100 Hz | SF7, 4/7, preamble 8 | 8 bytes | 4 | 1:32 |
| 3 | 50 Hz | SF8, 4/7, preamble 10 | 8 bytes | 4 | 1:16 |
| 4 | 25 Hz | SF9, 4/7, preamble 10 | 8 bytes | 2 | 1:8 |
| 5 | D50 | SF6, 4/7, preamble 8 | 8 bytes, each sent 4 times | 2 | 1:64 |

All at 500 kHz bandwidth, implicit header, LoRa CRC off (ExpressLRS has its own),
sync word 0x12, IQ not inverted (900 MHz never inverts).

From the PC, ELRS is protocol **200** in the Multi serial frames (above
Multiprotocol's own numbers), the sub-protocol is the rate, and the RX number is
the model ID for model match:

```bash
python3 tools/multi_link.py elrs --sub 0 --gamepad      # elrs = 200
```

Other settings, applied at the next `mpm start`:

| Command | |
|---|---|
| `elrs uid 1,2,3,4,5,6` | the UID directly (as the ELRS web page shows it), instead of a phrase |
| `elrs switch 0\|1\|2` | switch mode: 0 Wide (or 8 ch at Full res), 1 Hybrid (16 ch), 2 12 ch (Full res only) |
| `elrs tlm std\|off\|N` | telemetry ratio announced to the receiver: the rate's default, none, or 1:N |
| `elrs modelmatch on\|off` | model match, with the Multi RX number as the model ID |
| `elrs power DBM` | output level (also while running) |

The channels map as ExpressLRS's own PPM input maps them: Multiprotocol's ±100%
(988–2012 µs) is CRSF's 172–1811. The arm flag is CH5 (AUX1), as with ExpressLRS's
"CH5" arming mode.

### The receiver

- **ExpressLRS 4.x.** 3.x and 4.x don't understand each other over the air (the
  CRC seed includes the protocol version, and the channel bits and rate numbers
  changed). ELRS receivers can be updated over WiFi.
- **The same binding phrase**, set when flashing the receiver or on its WiFi page.
  There is no bind mode yet.
- **The same regulatory domain** (FCC915, EU868, ...): it sets the channels.
- A 900 MHz receiver: SX127x or LR1121 (the LR1121 runs these rates compatibly,
  ExpressLRS sets its SF6 register for that).

## How it works

### What comes from ExpressLRS, unmodified

`fw/third_party/expresslrs` is the ExpressLRS repository (tag 4.1.0, git
submodule). The firmware builds four of its files as they are:

| File | What it gives |
|---|---|
| `src/lib/OTA/OTA.cpp` | the packet layouts, channel packing (10-bit channels, Wide and Hybrid switches, full-res 8/12/16 ch), the CRC with the nonce |
| `src/lib/FHSS/FHSS.cpp`, `random.cpp` | the hop sequence from the UID, channel frequencies per domain |
| `src/lib/CRC/crc.cpp` | the 14- and 16-bit CRCs |

`fw/src/elrs/shim/` has the few headers they need from the Arduino and ExpressLRS
build (`Arduino.h`, `logging.h`, `options.h`, `SX127xDriver.h`).

### What is ours

`fw/src/elrs/elrs_tx.cpp` replaces ExpressLRS's `tx_main.cpp`, which is tied to
its hardware and timers. It keeps the TX's slot rules, adapted from
`timerCallback()`, `SendRCdataToRF()`, `TXdoneISR()` and `GenerateSyncPacketData()`:

```
every period (5 ms at 200 Hz), from the timer interrupt:
  nonce++
  telemetry slot?          -> send nothing (the receiver answers in it)
  on the sync channel, after the sync interval, in a free sync slot?
                           -> SYNC packet: hop index, nonce, rate, telemetry ratio, UID
  otherwise                -> RC packet: channels from the handset input
  CRC (seeded by the UID, the protocol version and the nonce), send
  next packet starts a new hop?  -> next frequency of the sequence
```

The receiver locks onto the sync packet (it waits on the sync channel), then
follows the hop sequence by itself, counting the nonce. A nonce off by one makes
every CRC fail; a hop off by one puts the receiver on the wrong channel.

What a real ExpressLRS TX does that this one doesn't (yet): receive telemetry (so
it keeps sending a sync packet every 600 ms, as a real TX does while it hears no
receiver), bind mode, MSP/data packets, sync spam on rate changes, Gemini, LBT.

### The radio

At 61.44 MSPS one LO "slice" is 35 MHz wide, and the radio layer now centres one
on the protocol's band (`radio_set_band`): 915.2 MHz for FCC915, 866.4 MHz for
EU868. The whole domain then fits one slice, so a hop is just a new NCO
frequency, without retuning the AD9361; and the start-up calibrations run in the
band. The 2.4 GHz protocols keep their grid.

## How it was checked

There's no ExpressLRS receiver here yet, so the checks lean on two things we
didn't write: ExpressLRS's own code, and the LoRa decoder that reproduces real
SX127x recordings ([lora.md](lora.md)).

**On the PC** (`fw/test/test_elrs.py`): the firmware's transmitter, built with
ExpressLRS's OTA/FHSS/CRC code, against `tools/elrs.py`, an independent model
written from the ExpressLRS source. Every slot of 288 cases (all six rates; FCC915,
EU868, AU915, IN866; the three switch modes; telemetry std / off / 1:8; model
match on and off; random channels) must match: silent slots, frequencies and
every packet byte. Breaking the model on purpose (the CRC seed, the Wide switch
index) makes hundreds of slots differ, so the test does catch mistakes. The
firmware's MD5 (binding phrase → UID) is checked against Python's.

**On the board** (`tools/elrs_check.py`), first the timing, at 61.44 MSPS as in use,
three seconds per rate, counted against the firmware's own clock:

| Rate | Slots/s | Not sent | Late callbacks | Build + start | Left after the frame | LO retunes |
|---|---|---|---|---|---|---|
| 200 Hz | 200.0 | 0 | 0 | 323 µs | 293 µs | 0 |
| 100 Hz Full | 99.8 | 0 | 0 | 459 µs | 2853 µs | 0 |
| 100 Hz | 100.1 | 0 | 0 | 350 µs | 882 µs | 0 |
| 50 Hz | 49.9 | 0 | 0 | 380 µs | 1060 µs | 0 |
| 25 Hz | 25.0 | 0 | 0 | 328 µs | 9720 µs | 0 |
| D50 | 200.0 | 0 | 0 | 328 µs | 288 µs | 0 |

"Build + start" is the longest time from the slot's start to the frame on air
(packet, CRC, LoRa coding, modulator set-up); with the frame's time on air it has
to fit in the slot. At 200 Hz the frame alone is 4384 µs of the 5000.

Then off the air: packets captured on RX2 through the loopback cable (30 dB
attenuator), at a low sample rate so that 16K samples hold a whole frame. Each is
demodulated by `tools/lora.py` and checked by `tools/elrs.py`:

- the CRC must match for some nonce, and that nonce's switch slot must carry the
  value set on that channel (so the nonce, the CRC seed and the Wide switch
  rotation are all right);
- the four stick channels and the arm flag (or all 8 channels at Full res) as set;
- the frequency must be one of the domain's channels;
- every LoRa symbol must equal a fresh encoding of the decoded bytes;
- one sync packet per rate (`elrs capsync` arms the capture for the next one): on
  the sync channel, with the FHSS index of the channel it was caught on, the
  rate, the telemetry ratio and the UID.

| Rate | RC packets | Sync packet |
|---|---|---|
| 200 Hz | 4/4 (nonces 235, 159, 99, 220) | fhss 0, nonce 130, rate enum 5, 1:64 |
| 100 Hz Full | 4/4 | fhss 40, nonce 99, rate enum 3, 1:32 |
| 100 Hz | 4/4 | fhss 40, nonce 99, rate enum 2, 1:32 |
| D50 | 4/4 | fhss 80, nonce 32, rate enum 10, 1:64 |

**27/27 checks passed** (UID, six timing runs, 20 packets). 50 Hz and 25 Hz
frames are too long for one capture at a rate the AD9361 runs; they share the
coding and the slot logic with the others (checked on the PC). FrSky D16 still
decodes 10/10 afterwards, and a protocol switch from ELRS (LO at 915.2 MHz) to
FrSky X while running moves the LO back to 2.4 GHz.

## A real Semtech chip (2026-10-24)

An ESP32 with an SX1278 (`tools/sx127x_listen/`, register-level, ExpressLRS's
LoRa settings) listened on EU433's sync channel (433.775 MHz) while the board
sent 200 Hz for 20 s (`tools/sx127x_listen.py`):

| | |
|---|---|
| packets received | 307 |
| valid ExpressLRS CRC | 306 (295 RC, 11 sync), fields and channels as sent |
| CRC failures | 1 (RSSI −89 dBm, SNR 5.5 dB: the board was cabled, so this was leakage) |
| arrival time vs nonce × 5000 µs | mean +1.2 µs, spread 39 µs over 305 pairs |
| carrier error (the chip's FEI) | −4.5 ppm |

So the framing (SF6, implicit header, sync word 0x12) and the packets are right
on silicon, and the slots keep time. An ExpressLRS receiver itself is the next
check; the same ESP32 + SX1278 can run ExpressLRS's `Unified_ESP32_900_RX`
firmware (pins: SCK 18, MISO 19, MOSI 25, NSS 5, RST 14, DIO0 26).

## What's next

- A real receiver: bind with the phrase, watch the channels (the ESP32 SBUS
  monitor, [multiprotocol.md](multiprotocol.md#testing-with-a-frsky-receiver)),
  link quality at 200 Hz.
- Telemetry: receive the receiver's link statistics in its slot (an RX path for
  LoRa).
- Bind mode, for receivers without a phrase.
- 2.4 GHz, once the SX1280's LI coding is learned ([lora.md](lora.md)).
