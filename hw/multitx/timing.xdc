# Timing constraints for system_top (LibreSDR B220 MultiTX platform).
# The 40 MHz TCXO clock is created by the clock wizard IP itself.

# ---- AD9361 data clock (CMOS DDR, max 61.44 MHz) ----
create_clock -name ad9361_clk -period 16.276 [get_ports CAT_DCLK_P]

# ---- FX3 GPIF II, slave FIFO at 100 MHz (numbers from the reference design V19b) ----
# The FPGA forwards its 100 MHz clock to the FX3 through the bridge's ODDR.
set ifclk_src [get_pins -hier -filter {NAME =~ */usb_debug_bridge/*ifclk_oddr/C}]
create_generated_clock -name gpif_ifclk -source $ifclk_src -divide_by 1 [get_ports IFCLK]

# FPGA -> FX3: data + strobes (FX3 tS 2.0 ns, tH 0.5 ns)
set gpif_out [get_ports {GPIF_D[*] GPIF_CTL0 GPIF_CTL1 GPIF_CTL2 GPIF_CTL3 GPIF_CTL7 GPIF_CTL11 GPIF_CTL12}]
set_output_delay -clock gpif_ifclk -max  2.000 $gpif_out
set_output_delay -clock gpif_ifclk -min -0.500 $gpif_out

# FX3 -> FPGA: data + flags (FX3 tCO 2.0..7.5 ns). The slave-FIFO state machine
# expects them one cycle later, hence the 2-cycle setup window.
set gpif_in [get_ports {GPIF_D[*] GPIF_CTL4 GPIF_CTL5}]
set_input_delay -clock gpif_ifclk -max 7.500 $gpif_in
set_input_delay -clock gpif_ifclk -min 2.000 $gpif_in
set_multicycle_path 2 -setup -from [get_clocks gpif_ifclk] -to [get_clocks -of_objects $ifclk_src]
set_multicycle_path 1 -hold  -from [get_clocks gpif_ifclk] -to [get_clocks -of_objects $ifclk_src]

# slow/static FX3 lines: serial settings bus and global reset
set_false_path -from [get_ports {GPIF_CTL6 GPIF_CTL8 GPIF_CTL9}]

# ---- unrelated clock domains ----
# AD9361 clock vs. everything derived from the 40 MHz TCXO. Crossings are the ADI
# core's own synchronisers and adc_capture's toggle handshakes.
set_clock_groups -name async_ad9361 -asynchronous \
  -group [get_clocks -include_generated_clocks ad9361_clk] \
  -group [get_clocks -include_generated_clocks -of_objects [get_ports CLK_40MHz_FPGA]]

# ---- slow outputs, no timing relationship needed ----
set_false_path -to [get_ports {SFDX* SRX* tx_enable* LED_* PPS_LED_inv REF_LOCKED_inv REF_IS_10M_detect_inv REF_CLK_REQ}]
set_false_path -to [get_ports {CLK_40M_DAC_* CAT_RESETn CAT_EN_AGC CAT_SYNC CAT_CTL_IN[*] FPGA_TXD0}]
set_false_path -from [get_ports {CAT_CTL_OUT[*] FPGA_RXD0}]

set_property BITSTREAM.GENERAL.COMPRESS TRUE [current_design]
