# Creates the LibreSDR B220 MultiTX Vivado 2022.2 project (block design based).
#
#   vivado -mode batch -source create_project.tcl                ;# create only
#   vivado -mode batch -source create_project.tcl -tclargs build  ;# create + bitstream
#
# Open the result with: vivado hw/multitx/vivado/multitx.xpr  (then Open Block Design)

set here [file normalize [file dirname [info script]]]
set hw   [file normalize $here/..]
set PRJ  $here/vivado
set ADI_LIB [file normalize ~/VARO/hdl/library]   ;# ADI hdl, tag 2022_r2_p1 (axi_ad9361)

create_project -force multitx $PRJ -part xc7a200tfbg484-2
set_property target_language Verilog [current_project]
set_property ip_repo_paths [list $ADI_LIB] [current_project]
update_ip_catalog

# ---- RTL used by the block design (module references) and the top level ----
set bridge $hw/ip/usb_debug_bridge
add_files -norecurse [concat \
  [glob $bridge/ettus/*.v] \
  [list $bridge/axi_fifo_2clk_xpm.v \
        $bridge/usb_cmd_proc.v \
        $bridge/usb_debug_bridge.v \
        $hw/ip/adc_capture/adc_capture.v \
        $hw/ip/gfsk_tx/gfsk_tx.v \
        $hw/ip/gfsk_tx/gfsk_tx_sincos.mem \
        $hw/ip/lora_tx/lora_tx.v \
        $hw/ip/tx_select/tx_select.v \
        $hw/ip/ppm_in/ppm_in.v \
        $hw/ip/tcxo_dac/DACx311_auto_spi.v \
        $here/system_top.v]]
update_compile_order -fileset sources_1

# ---- block design ----
source $here/system_bd.tcl
set bd_file [get_files system.bd]
generate_target all $bd_file
add_files -norecurse [make_wrapper -files $bd_file -top]

set_property top system_top [current_fileset]
add_files -fileset constrs_1 -norecurse [list $here/pins.xdc $here/timing.xdc]
update_compile_order -fileset sources_1

set_property STEPS.WRITE_BITSTREAM.ARGS.BIN_FILE true [get_runs impl_1]

if {[lindex $argv 0] eq "build"} {
  launch_runs impl_1 -to_step write_bitstream -jobs 8
  wait_on_run impl_1
  open_run impl_1
  report_timing_summary -file $PRJ/timing_summary.rpt
  report_utilization    -file $PRJ/utilization.rpt
  report_utilization -hierarchical -hierarchical_depth 2 -file $PRJ/utilization_hier.rpt
  puts "IMPL_STATUS: [get_property STATUS [get_runs impl_1]]"
  puts "WNS: [get_property STATS.WNS [get_runs impl_1]]  WHS: [get_property STATS.WHS [get_runs impl_1]]"
}
