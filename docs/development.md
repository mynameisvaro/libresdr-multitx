# Development workflow

## Prerequisites

| Tool | Version | Notes |
|---|---|---|
| Vivado | **2022.2** with Artix-7 device support | matches ADI hdl `2022_r2` |
| ADI hdl | tag `2022_r2_p1` | `axi_ad9361` packaged (`make` in `library/axi_ad9361`); path set by `ADI_LIB` in `hw/multitx/create_project.tcl` (default `~/VARO/hdl/library`) |
| UHD | 4.x (`uhd-host`) | loads the FX3 firmware and FPGA images; compat numbers are the same from 4.6 to 4.11 |
| Python 3 + pyusb | `python3-usb` | host tools |

```bash
git clone --recurse-submodules <this repo>     # or: git submodule update --init
```

Submodules: `reference/` (the B220 FPGA design) and `fw/third_party/multiprotocol`
(Multiprotocol's sources, built into the firmware).

## Build

```bash
source /opt/AMD/2022.2/Vivado/2022.2/settings64.sh
vivado -mode batch -source hw/multitx/create_project.tcl -tclargs build
```

The output is `hw/multitx/vivado/multitx.runs/impl_1/system_top.bin`, with the
timing summary and utilization reports next to the project. Without `-tclargs build`
the script only creates the project. Open `hw/multitx/vivado/multitx.xpr` and use
*Open Block Design* to explore it.

The project is generated from scripts, so `hw/multitx/vivado/` is disposable and not
versioned. To change the design, edit `system_bd.tcl`, the RTL in `hw/ip/` or the
XDC files, then re-run the script. If you change the block design in the GUI, port
the change back to `system_bd.tcl` (or `write_bd_tcl` and merge it).

The reference design rebuilds the same way:
`vivado -mode batch -source hw/ref_b220/create_project.tcl -tclargs build`.

### Last build (phase 3: 512 KB code_ram)

| | |
|---|---|
| Timing | met, WNS +0.83 ns, WHS +0.036 ns |
| Utilization | 9.5% LUT, 52% BRAM, 4.7% DSP |

## Simulate

```bash
hw/ip/usb_debug_bridge/tb/run_sim.sh        # usb_cmd_proc unit test (xsim)
python3 tools/test_usbdbg_cosim.py          # usbdbg.py packets through the RTL
python3 hw/ip/gfsk_tx/tb/check.py           # gfsk_tx: simulate FrSky-style packets, demodulate
```

## Load

The FPGA is loaded through the FX3 by UHD. Our bitstreams don't speak UHD's control
protocol, so UHD reports an error right after loading. That is expected.

```bash
uhd_usrp_probe --init-only --args "type=b200,fpga=/path/to/system_top.bin"
```

If the board is on another machine over USB/IP, load it on that machine; see
[remote-usb.md](remote-usb.md).

## Firmware

The MicroBlaze firmware lives in [`fw/`](../fw) and loads over the USB bridge, with
no FPGA reload:

```bash
make -C fw load                              # build, load, run
python3 tools/usbdbg.py console              # interactive console
python3 tools/usbdbg.py console info "ad 0x37"
```

See [fw/README.md](../fw/README.md) for the memory layout and the console mailbox.

## Verify and debug

```bash
python3 tools/selftest.py               # bridge, memory, registers, AD9361 SPI
python3 tools/usbdbg.py id
python3 tools/usbdbg.py read 0x40000000 2
python3 tools/usbdbg.py write 0x40000000 0x4C
python3 tools/ad9361_spi.py read 0x037
python3 tools/usbdbg.py load firmware.bin    # halt CPU, load code_ram, verify, run
python3 tools/usbdbg.py capture 4096 rx.csv
```

`usbdbg.py` is also a Python module (`from usbdbg import Bridge`). One `transact()`
call can batch many commands into a single USB round trip.

`selftest.py` output on the phase 1 bitstream (board on USB/IP):

```
[PASS] usb bridge             version 0x00010000, status 0x00000000
[PASS] adc_capture id         0x41444343
[PASS] code_ram               16 KB, 0 mismatches, 2.4 s
[PASS] gpio_fe reset          0x48 0x48
[PASS] gpio_dac reset         0x7fff
[PASS] axi_ad9361             ADC core 10.03, DAC core 9.01, scratch 0xa5a55a5a
[PASS] axi_timer              0x45534ac -> 0x567f256
[PASS] AD9361 SPI             product ID 0x0a (rev 2)

8/8 passed
```

`--full` tests the whole code RAM (512 KB since phase 3; the 256 KB version passed with 0 mismatches during bring-up).
