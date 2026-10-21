# IP cores of the f4tnk v20e reference, recreated for Vivado 2022.2.
# The original .xci files were saved by 2025.2 and would be locked here, so each
# core is regenerated from its user-set parameters (derived values such as MMCM
# dividers and jitter are left for the wizard to compute).

# 40 MHz AD9361 always-on clock -> 40 MHz internal, 100 MHz bus/GPIF, 200 MHz ref PLL
create_ip -name clk_wiz -vendor xilinx.com -library ip -version 6.0 -module_name gen_clks
set_property -dict [list \
    CONFIG.PRIM_IN_FREQ {40} \
    CONFIG.CLKIN1_JITTER_PS {250.0} \
    CONFIG.PRIMARY_PORT {clk_in1_40} \
    CONFIG.NUM_OUT_CLKS {3} \
    CONFIG.CLKOUT2_USED {true} \
    CONFIG.CLKOUT3_USED {true} \
    CONFIG.CLK_OUT1_PORT {clk_out1_40_int} \
    CONFIG.CLK_OUT2_PORT {clk_out2_100_bus} \
    CONFIG.CLK_OUT3_PORT {clk_out3_200_ref_pll} \
    CONFIG.CLKOUT1_REQUESTED_OUT_FREQ {40} \
    CONFIG.CLKOUT2_REQUESTED_OUT_FREQ {100} \
    CONFIG.CLKOUT3_REQUESTED_OUT_FREQ {200} \
    CONFIG.USE_RESET {true} \
] [get_ips gen_clks]

# External 10 MHz reference -> 200 MHz; only its 'locked' output is used (10 MHz detect)
create_ip -name clk_wiz -vendor xilinx.com -library ip -version 6.0 -module_name clk_wiz_0
set_property -dict [list \
    CONFIG.PRIMITIVE {MMCM} \
    CONFIG.PRIM_IN_FREQ {10} \
    CONFIG.CLKIN1_JITTER_PS {1000.0} \
    CONFIG.PRIM_SOURCE {Global_buffer} \
    CONFIG.PRIMARY_PORT {clk_in1_10M} \
    CONFIG.CLK_OUT1_PORT {clk_out_200M} \
    CONFIG.CLKOUT1_REQUESTED_OUT_FREQ {200} \
    CONFIG.USE_PHASE_ALIGNMENT {false} \
    CONFIG.USE_RESET {false} \
    CONFIG.MMCM_BANDWIDTH {OPTIMIZED} \
    CONFIG.MMCM_COMPENSATION {ZHOLD} \
] [get_ips clk_wiz_0]

# 32 x 72 dual-clock FIFO in distributed RAM
create_ip -name fifo_generator -vendor xilinx.com -library ip -version 13.2 -module_name fifo_short_2clk
set_property -dict [list \
    CONFIG.Fifo_Implementation {Independent_Clocks_Distributed_RAM} \
    CONFIG.synchronization_stages {3} \
    CONFIG.Performance_Options {First_Word_Fall_Through} \
    CONFIG.Input_Data_Width {72} \
    CONFIG.Input_Depth {32} \
    CONFIG.Full_Flags_Reset_Value {1} \
    CONFIG.Use_Extra_Logic {true} \
    CONFIG.Write_Data_Count {true} \
    CONFIG.Write_Data_Count_Width {6} \
    CONFIG.Read_Data_Count {true} \
    CONFIG.Read_Data_Count_Width {6} \
] [get_ips fifo_short_2clk]

# 512 x 72 dual-clock FIFO in block RAM
create_ip -name fifo_generator -vendor xilinx.com -library ip -version 13.2 -module_name fifo_4k_2clk
set_property -dict [list \
    CONFIG.Fifo_Implementation {Independent_Clocks_Block_RAM} \
    CONFIG.Performance_Options {First_Word_Fall_Through} \
    CONFIG.Input_Data_Width {72} \
    CONFIG.Input_Depth {512} \
    CONFIG.Output_Data_Width {72} \
    CONFIG.Full_Flags_Reset_Value {1} \
    CONFIG.Use_Extra_Logic {true} \
    CONFIG.Write_Data_Count {true} \
    CONFIG.Read_Data_Count {true} \
    CONFIG.Disable_Timing_Violations {true} \
    CONFIG.Enable_Safety_Circuit {true} \
] [get_ips fifo_4k_2clk]

# Debug VIO (needs JTAG to use, harmless otherwise)
create_ip -name vio -vendor xilinx.com -library ip -version 3.0 -module_name vio_0
set_property -dict [list \
    CONFIG.C_NUM_PROBE_IN {3} \
    CONFIG.C_NUM_PROBE_OUT {0} \
    CONFIG.C_PROBE_IN0_WIDTH {16} \
    CONFIG.C_PROBE_IN1_WIDTH {2} \
    CONFIG.C_PROBE_IN2_WIDTH {2} \
] [get_ips vio_0]
