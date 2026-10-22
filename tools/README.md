# tools/

Host-side Python tools. They need `pyusb` (`sudo apt install python3-usb`) and a
board running a MultiTX bitstream.

| Tool | |
|---|---|
| [`usbdbg.py`](usbdbg.py) | USB debug bridge client: read/write the AXI bus, halt/run the CPU, load firmware, capture RX samples, firmware console (`console`). Also importable (`from usbdbg import Bridge`) |
| [`ad9361_spi.py`](ad9361_spi.py) | AD9361 register access through the AXI Quad SPI (bring-up only; the firmware will own the chip) |
| [`rf_loopback.py`](rf_loopback.py) | FPGA ↔ AD9361 sample-interface check through the AD9361's digital loopback (needs the firmware and numpy) |
| [`rf_tone.py`](rf_tone.py) | over-the-air check: DDS tone out of TX/RX, received on RX2 (cable + attenuator), reports tone, LO leakage, images, DC; `--plot` saves the spectrum |
| [`gfsk_test.py`](gfsk_test.py) | sends FrSky D8/D16-style packets through `gfsk_tx` and demodulates them (digital loopback or RF) |
| [`gfsk.py`](gfsk.py) | GFSK helpers: pulse table, phase increments, CC2500 maths, demodulator; also writes `gfsk_tx`'s sin/cos ROM |
| [`frsky_check.py`](frsky_check.py) | runs Multiprotocol's FrSky D8/D16 on the board and decodes captured packets like a CC2500 (CRC, hop channel, offset) |
| [`selftest.py`](selftest.py) | hardware self-test: bridge, memory, registers, AD9361 SPI |
| [`test_usbdbg_cosim.py`](test_usbdbg_cosim.py) | runs `usbdbg.py`'s packets through the RTL in xsim to check that both agree on the wire format |

```bash
python3 tools/selftest.py
python3 tools/usbdbg.py read 0x79020000      # axi_ad9361 version register
python3 tools/ad9361_spi.py id
```

The bridge protocol is documented in [docs/architecture.md](../docs/architecture.md#usb-debug-bridge).
