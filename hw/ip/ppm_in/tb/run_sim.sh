#!/bin/bash
# ppm_in testbench in xsim (Vivado 2022.2)
set -e
cd "$(dirname "$0")"
source /opt/AMD/2022.2/Vivado/2022.2/settings64.sh
mkdir -p work && cd work
xvlog ../../ppm_in.v ../tb_ppm_in.v > xvlog.log
xelab -debug off tb_ppm_in -s tb > xelab.log
xsim tb -R | grep -E "PASS|FAIL|SIM_DONE|ERROR"
