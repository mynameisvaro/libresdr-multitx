# Recreates the f4tnk LibreSDR B220 reference design (tag v20e) as a Vivado 2022.2
# project. The sources stay in the git clone (../../reference) and are referenced
# in place, so the clone is never modified.
#
#   vivado -mode batch -source create_project.tcl              ;# create only
#   vivado -mode batch -source create_project.tcl -tclargs build ;# create + bitstream

set here [file normalize [file dirname [info script]]]
set SRC  [file normalize $here/../../reference/src/libresdr_b210.srcs]
set PRJ  $here/vivado
set NAME ref_b220

create_project -force $NAME $PRJ -part xc7a200tfbg484-2
set_property target_language Verilog [current_project]

source $here/sources.tcl
set_property top libresdr_b210 [current_fileset]
set_property verilog_define {TARGET_B210=1} [current_fileset]

add_files -fileset constrs_1 -norecurse $SRC/constrs_2/imports/xdc/b210.xdc

source $here/ips.tcl
generate_target all [get_ips]

# Same run strategies as the original project
set_property strategy Flow_PerfOptimized_high [get_runs synth_1]
set_property strategy Performance_ExplorePostRoutePhysOpt [get_runs impl_1]
set_property -name {STEPS.ROUTE_DESIGN.ARGS.MORE OPTIONS} -value {-tns_cleanup} -objects [get_runs impl_1]
set_property STEPS.WRITE_BITSTREAM.ARGS.BIN_FILE true [get_runs impl_1]

if {[lindex $argv 0] eq "build"} {
    launch_runs impl_1 -to_step write_bitstream -jobs 8
    wait_on_run impl_1
    open_run impl_1
    report_timing_summary -file $PRJ/timing_summary.rpt
    report_utilization -file $PRJ/utilization.rpt
    puts "IMPL_STATUS: [get_property STATUS [get_runs impl_1]]"
    puts "WNS: [get_property STATS.WNS [get_runs impl_1]]  WHS: [get_property STATS.WHS [get_runs impl_1]]"
}
