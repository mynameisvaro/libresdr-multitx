# Design notes

Why MultiTX is built the way it is. These are working notes; they change as the
project learns things on real hardware.

## Reuse the protocol code, replace the radios

Multiprotocol's protocol files (`*_cc2500.ino`, `*_nrf24l01.ino`, `*_a7105.ino`,
`*_cyrf6936.ino`, `*_sx1276.ino`, …) call a small per-chip API: `CC2500_WriteReg`,
`CC2500_Strobe`, `NRF24L01_WriteReg`, `A7105_WriteData`, … They don't just send
packets. They write raw register values (`MDMCFG4`, `RF_SETUP`, sync words, channel
registers) and rely on chip features such as FIFOs, hardware CRC, auto-ack and RSSI.

So each `*_SPI.ino` replacement is a **virtual chip**. It keeps a shadow register
file, decodes it into a modem configuration (data rate, deviation, sync word, packet
length, CRC, whitening, channel), and emulates the behaviour the protocol code
expects. The protocol files stay unmodified, so upstream updates can be merged.

| Chip | Modem | Notes |
|---|---|---|
| CC2500 | GFSK / MSK up to 500 kbps | FrSky D8/D16, Futaba SFHSS; frequency fine-tune is a plain NCO offset |
| A7105 | FSK / GFSK | FlySky AFHDS / AFHDS2A, Hubsan |
| NRF24L01 | GFSK 250 kbps – 2 Mbps | XN297 emulation already lives in Multiprotocol's code; Enhanced ShockBurst auto-ack needs a ~130 µs turnaround |
| CYRF6936 | DSSS with GFSK | DSM2/DSMX, Devo; the PN code tables are in Multiprotocol |
| SX1276 | LoRa + FSK | FrSky R9 etc.; LoRa receive is the hardest DSP block |

Sending is the easy half. Telemetry, bind replies and auto-ack need the receive path,
which roughly doubles the work, so TX-only protocols come first.

## Frequency hopping: coarse LO + fine NCO

Storing a fastlock profile per channel and loading one from FPGA memory on every hop
would work, but it puts SPI traffic and synthesizer settling on the critical path.
Instead:

- The 2.4 GHz ISM band (2400–2483.5 MHz) is split into **2–4 slices** that each fit
  inside the AD9361's instantaneous bandwidth (2 at 61.44 MSPS, 3–4 at 30.72 MSPS).
- A **digital NCO/DDC** picks the channel inside a slice. Those hops are effectively
  instant.
- Hops **between slices** recall one of the AD9361's 8 built-in **fastlock** profiles
  per synthesizer, so there's no external profile table and no calibration on a hop.
- Each LO sits between channels, so no channel falls on the DC / LO-leakage notch.
- The sub-GHz bands (433, 868, 915 MHz) each fit under a single LO.

Fastlock profiles are temperature sensitive. Re-store them if the board's
temperature drifts a lot.

## Full duplex with digital TX gating

The AD9361 runs in **FDD mode** with both synthesizers locked, and TX on/off is done
by gating the digital samples. That gives a near-instant TX→RX turnaround for
telemetry and NRF24 auto-ack. The board has separate TX/RX and RX2 ports (UHD's
"full duplex" switch state, `0xE5`).

## ExpressLRS

ExpressLRS is a separate codebase (PlatformIO, C++). The same approach applies, but
at the driver-class level (`SX127xDriver`, `SX1280Driver`, `LR1121Driver`) rather
than raw SPI, since the SX1280 is command-based.

- **LoRa** is well understood (see gr-lora_sdr: whitening, Hamming, interleaving,
  headers, SF5–12). The TX chirp generator is easy; demodulation is a solid DSP block.
- **FLRC** (ELRS 1000 Hz at 2.4 GHz) is not fully documented and would need
  reverse-engineering.
- Packet periods of 1–4 ms with strict telemetry slots fit the hop engine above.

## No JTAG, no flash

The B220's FX3 can configure the FPGA (slave serial) but isn't wired to its JTAG. So
the design carries its own **USB debug bridge**: an AXI master driven over the FX3
data endpoints (Ettus' GPIF II code). With it, the PC can read and write anything on
the bus, hold the CPU in reset and load its code, and read sample captures. That
replaces the MicroBlaze debugger and the ILA until a JTAG cable is used.

The MicroBlaze code RAM is dual-ported for the same reason. The instruction LMB is
on one port and the AXI bus on the other, so new firmware loads in seconds over USB
without rebuilding the bitstream.

Without configuration flash, a stand-alone module needs something to load the FPGA
at power-up. A Pi Zero 2 W next to the board could do that, and could also bridge the
handset link.

## Output power

The AD9361 alone gives about +5 dBm at 2.4 GHz, against 20–30 dBm for commercial
modules. Range testing needs an external PA/LNA with a T/R switch, which CN2's GPIOs
can drive. Mind the legal limits.
