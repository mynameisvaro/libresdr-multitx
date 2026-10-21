# libresdr_multitx

Port of DIY-Multiprotocol-TX-Module (and later ExpressLRS) to a LibreSDR B220 mini
(Artix-7 XC7A200T + AD9361 + FX3). The protocol code runs unmodified on a MicroBlaze
inside the FPGA; the per-chip SPI drivers (CC2500, NRF24L01, A7105, CYRF6936, SX1276)
are replaced by "virtual radio" shims that drive FPGA modems and the AD9361.
Frequency hopping: a few coarse LO positions (AD9361 fastlock profiles) plus a
digital NCO for the channel.

Status: phase 1 platform (MicroBlaze, AD9361 interface, USB debug bridge) built and
verified on hardware. See [docs/architecture.md](docs/architecture.md).

## Layout

| Path | |
|---|---|
| `hw/multitx/` | our Vivado 2022.2 block design (`create_project.tcl`) |
| `hw/ref_b220/` | f4tnk reference design rebuilt in Vivado 2022.2 (baseline) |
| `hw/ip/` | RTL blocks used by the block design |
| `tools/` | host tools: `usbdbg.py` (USB debug bridge), `ad9361_spi.py` |
| `remote_usb/` | USB/IP setup: the board sits on a remote VM, tunnelled to this machine |
| `reference/` | git submodule: f4tnk/LibreSDR_UHD_B220_Mini_FPGA at tag v20e |
| `docs/` | architecture notes, board schematic, reference constraints |

## Setup

```bash
git submodule update --init           # reference design (large)
```

The ADI library (`axi_ad9361`) is expected at `~/VARO/hdl/library`, ADI hdl tag
`2022_r2_p1`, with its IP packaged (`make` in `library/axi_ad9361`).

Build (Vivado 2022.2, Artix-7 device support needed):

```bash
source /opt/AMD/2022.2/Vivado/2022.2/settings64.sh
vivado -mode batch -source hw/multitx/create_project.tcl -tclargs build
```

`images/` is not versioned. Repopulate it with
`uhd_images_downloader -t b2xx -i images` (FX3 firmware), the f4tnk release
`libresdr_b220_v20e.bin` (linked as `usrp_b210_fpga.bin`), and our builds
(`hw/*/vivado/*.runs/impl_1/*.bin`).

## Licences

`hw/ip/usb_debug_bridge/ettus/` and `hw/ip/tcxo_dac/` are Ettus Research code
(LGPL-3.0-or-later), copied from the reference design with the changes noted in
each file header. The reference design itself is a submodule under its own licence.
