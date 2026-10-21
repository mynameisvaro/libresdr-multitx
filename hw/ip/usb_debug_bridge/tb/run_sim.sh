#!/bin/bash
# Simulates usb_cmd_proc with Vivado's xsim. Prints PASS or FAILED.
set -e
source /opt/AMD/2022.2/Vivado/2022.2/settings64.sh
here="$(cd "$(dirname "$0")" && pwd)"
work="$here/work"
mkdir -p "$work" && cd "$work"
xvlog -sv "$here/../usb_cmd_proc.v" "$here/tb_usb_cmd_proc.v" > xvlog.log
xelab -debug off tb_usb_cmd_proc -s tb > xelab.log
xsim tb -R | grep -E 'PASS|FAIL'
