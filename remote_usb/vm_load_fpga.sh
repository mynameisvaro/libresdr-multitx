#!/bin/bash
# Run on the VM:  sudo ./vm_load_fpga.sh [fpga.bin]
#
# Loading a bitstream through the USB/IP tunnel is unusably slow: UHD sends it as
# ~11k 512-byte control transfers and each one waits a full network round-trip.
# This script takes the board back from Vega-1 for a moment, loads the FX3
# firmware + FPGA image locally (seconds), then exports it again.
#
# UHD stores a hash of the image *contents* in the FX3, so when Vega-1 opens the
# board with an identical file it skips the reload. Keep $IMAGES identical to
# ~/VARO/libresdr_multitx/images on Vega-1 (copy it with the scp line below).
#
# One-time setup on the VM:
#   sudo apt install uhd-host
#   sudo mkdir -p /opt/libresdr && sudo scp -r vega-1@192.168.192.71:VARO/libresdr_multitx/images /opt/libresdr/
set -euo pipefail

IMAGES="${IMAGES:-/opt/libresdr/images}"
FPGA="${1:-$IMAGES/usrp_b210_fpga.bin}"

[ "$(id -u)" -eq 0 ] || { echo "run with sudo"; exit 1; }
[ -f "$FPGA" ] || { echo "no FPGA image at $FPGA"; exit 1; }

restore() { systemctl start usbip-autobind; }
trap restore EXIT

echo "== Taking the board back from Vega-1"
systemctl stop usbip-autobind
for d in /sys/bus/usb/drivers/usbip-host/*-*; do
    [ -e "$d" ] || continue
    usbip unbind -b "$(basename "$d")"
done
sleep 2

echo "== Loading FX3 firmware + $(basename "$FPGA") locally"
# Our own (MultiTX) bitstreams don't answer UHD's control protocol, so UHD reports
# an error right after loading them. That is expected; the FPGA is configured.
UHD_IMAGES_DIR="$IMAGES" timeout 60 uhd_usrp_probe --init-only --args "type=b200,fpga=$FPGA" \
    || echo "== UHD stopped after loading (expected for MultiTX bitstreams, not for UHD images)"

echo "== Exporting the board to Vega-1 again (it re-attaches within a few seconds)"
