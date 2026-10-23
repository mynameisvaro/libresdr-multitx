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
| [`a7105.py`](a7105.py) | A7105 packet coding (CRC-16, (7,4) FEC) and demodulator, shared by the FlySky tools |
| [`flysky_check.py`](flysky_check.py) | the same for FlySky (AFHDS) and AFHDS2A on the virtual A7105 |
| [`a7105_sniff.py`](a7105_sniff.py) | learns the A7105's CRC and FEC coding from a real FlySky handset; `--apply` sets it on the board |
| [`afhds2a_bind.py`](afhds2a_bind.py) | binds an AFHDS2A receiver: catches its reply on RX2 and passes it to the virtual A7105 |
| [`sbus_monitor/`](sbus_monitor/sbus_monitor.ino) | ESP32 sketch: reads a receiver's SBUS and prints frame rate, lost frames, failsafe and channels 1–8 |
| [`multi_link.py`](multi_link.py) | the PC as the handset: streams Multiprotocol serial frames (protocol, flags, option, 16 channels) to the firmware over USB; also importable (`MultiLink`, `Frame`) |
| [`gamepad.py`](gamepad.py) | a gamepad as the sticks and switches for `multi_link.py --gamepad` (pygame; mapping, toggles, expo); run alone it shows your pad's input names |
| [`test_gamepad.py`](test_gamepad.py) | unit tests for the gamepad mapping (no hardware) |
| [`input_check.py`](input_check.py) | checks the handset input path: protocol start from a frame, exact channels, bind flag, link-loss pause and resume, protocol switch |
| [`sched_check.py`](sched_check.py) | runs a protocol while loading the console (busy, and with the output ring full) and checks that no callback is late |
| [`sbus_tune.py`](sbus_tune.py) | logs the SBUS monitor from its serial port and sweeps the FrSky frequency fine tune (`mpm option`) against the lost-frame rate |
| [`selftest.py`](selftest.py) | hardware self-test: bridge, memory, registers, AD9361 SPI |
| [`test_usbdbg_cosim.py`](test_usbdbg_cosim.py) | runs `usbdbg.py`'s packets through the RTL in xsim to check that both agree on the wire format |

```bash
python3 tools/selftest.py
python3 tools/usbdbg.py read 0x79020000      # axi_ad9361 version register
python3 tools/ad9361_spi.py id
```

The bridge protocol is documented in [docs/architecture.md](../docs/architecture.md#usb-debug-bridge).
