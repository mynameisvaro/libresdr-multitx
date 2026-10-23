# Block design "system" for the LibreSDR B220 MultiTX platform (phase 1).
#
#   CLK_40MHz_FPGA ──► sys_clkgen ──► sys_clk 100 MHz (CPU, AXI, USB bridge)
#                                └──► delay_clk 200 MHz (AD9361 input delays)
#
#   sys_mb (MicroBlaze) ─ ILMB ─► code_ram port A   (code, 512 KB @ 0x0000_0000)
#                       ─ DLMB ─► data_ram          (stack/data, 64 KB @ 0x0010_0000)
#                       ─ M_AXI_DP ─┐
#   usb_debug_bridge ─ m_axi ───────┴─► axi_cpu_interconnect ─► peripherals,
#                                                    code_ram port B (code loading)
#
#   axi_ad9361 (ADI) ── AD9361 CMOS data port; samples ──► adc_capture
#   gfsk_tx   : packet GFSK modulator + channel NCO ──┐
#   lora_tx   : LoRa chirp modulator + channel NCO ───┴► tx_select ──► axi_ad9361 TX data
#   gpio_fe   : RF switch / LED word per channel (same bits as UHD's ATR)
#   gpio_ctrl : AD9361 reset/AGC/sync/CTRL_IN, ENABLE/TXNRX request, user LED
#   gpio_dac  : 40 MHz TCXO tuning word ──► tcxo_dac (SPI to the board's DAC)
#   ppm_in    : PPM from a radio's trainer port on CN2 GPIO_0
#   gfsk_rx   : GFSK receiver on the RX samples (sync word -> bits)
#
# Address map: see docs/architecture.md (kept in sync with assign_addresses below).

create_bd_design system

# ---------------------------------------------------------------- helpers
proc add_ip {name vlnv {props {}}} {
  set cell [create_bd_cell -type ip -vlnv $vlnv $name]
  if {[llength $props]} { set_property -dict $props $cell }
  return $cell
}
proc add_module {name module {props {}}} {
  set cell [create_bd_cell -type module -reference $module $name]
  if {[llength $props]} { set_property -dict $props $cell }
  return $cell
}
proc wire {from args} {
  set f [get_bd_pins -quiet $from]
  if {$f eq ""} { error "no pin $from" }
  foreach to $args {
    set t [get_bd_pins -quiet $to]
    if {$t eq ""} { error "no pin $to" }
    connect_bd_net $f $t
  }
}
proc wire_port {port args} {
  foreach to $args { connect_bd_net [get_bd_ports $port] [get_bd_pins $to] }
}
proc const {name width value} {
  add_ip $name xilinx.com:ip:xlconstant:1.1 [list CONFIG.CONST_WIDTH $width CONFIG.CONST_VAL $value]
}
proc slice {name from_pin bit} {
  add_ip $name xilinx.com:ip:xlslice:1.0 [list CONFIG.DIN_WIDTH 16 CONFIG.DIN_FROM $bit CONFIG.DIN_TO $bit]
  connect_bd_net [get_bd_pins $from_pin] [get_bd_pins $name/Din]
}

# ---------------------------------------------------------------- board ports
create_bd_port -dir I -type clk -freq_hz 40000000 clk_40m

create_bd_intf_port -mode Master -vlnv xilinx.com:interface:uart_rtl:1.0 uart
create_bd_port -dir O ad9361_spi_mosi
create_bd_port -dir I ad9361_spi_miso
create_bd_port -dir O ad9361_spi_sck
create_bd_port -dir O ad9361_spi_csn

create_bd_port -dir O -from 7  -to 0 fe0_gpio
create_bd_port -dir O -from 7  -to 0 fe1_gpio
create_bd_port -dir O -from 15 -to 0 ctrl_gpio_o
create_bd_port -dir I -from 7  -to 0 ctrl_gpio_i
create_bd_port -dir O tcxo_dac_sclk
create_bd_port -dir O tcxo_dac_mosi
create_bd_port -dir O tcxo_dac_sync_n
create_bd_port -dir I ppm_in

create_bd_port -dir I rx_clk_in
create_bd_port -dir I rx_frame_in
create_bd_port -dir I -from 11 -to 0 rx_data_in
create_bd_port -dir O tx_clk_out
create_bd_port -dir O tx_frame_out
create_bd_port -dir O -from 11 -to 0 tx_data_out
create_bd_port -dir O enable
create_bd_port -dir O txnrx

create_bd_port -dir O gpif_ifclk
create_bd_port -dir I -from 31 -to 0 gpif_d_i
create_bd_port -dir O -from 31 -to 0 gpif_d_o
create_bd_port -dir O -from 31 -to 0 gpif_d_t
foreach p {4 5 6 8 9}      { create_bd_port -dir I gpif_ctl$p }
foreach p {0 1 2 3 7 11 12} { create_bd_port -dir O gpif_ctl$p }

# ---------------------------------------------------------------- clocks and resets
# clk_out1 = sys_clk (100 MHz), clk_out2 = delay_clk (200 MHz)
add_ip sys_clkgen xilinx.com:ip:clk_wiz:6.0 {
  CONFIG.PRIM_IN_FREQ 40
  CONFIG.CLKOUT1_REQUESTED_OUT_FREQ 100
  CONFIG.CLKOUT2_USED true
  CONFIG.CLKOUT2_REQUESTED_OUT_FREQ 200
  CONFIG.USE_RESET false
}
wire_port clk_40m sys_clkgen/clk_in1

const const_1 1 1
add_ip sys_rstgen xilinx.com:ip:proc_sys_reset:5.0
wire sys_clkgen/clk_out1 sys_rstgen/slowest_sync_clk
wire sys_clkgen/locked  sys_rstgen/dcm_locked
wire const_1/dout       sys_rstgen/ext_reset_in

# ---------------------------------------------------------------- MicroBlaze
add_ip sys_mb xilinx.com:ip:microblaze:11.0 {
  CONFIG.C_DEBUG_ENABLED 1
  CONFIG.C_I_LMB 1
  CONFIG.C_D_LMB 1
  CONFIG.C_D_AXI 1
  CONFIG.C_USE_BARREL 1
  CONFIG.C_USE_HW_MUL 1
  CONFIG.C_USE_DIV 1
  CONFIG.C_USE_MSR_INSTR 1
  CONFIG.C_USE_PCMP_INSTR 1
  CONFIG.C_USE_REORDER_INSTR 1
}
add_ip sys_mdm xilinx.com:ip:mdm:3.2 {CONFIG.C_MB_DBG_PORTS 1}
connect_bd_intf_net [get_bd_intf_pins sys_mdm/MBDEBUG_0] [get_bd_intf_pins sys_mb/DEBUG]
wire sys_mdm/Debug_SYS_Rst sys_rstgen/mb_debug_sys_rst
wire sys_clkgen/clk_out1    sys_mb/Clk

# CPU reset = power-on reset OR "hold" from the USB bridge (while loading code)
add_ip sys_mb_reset xilinx.com:ip:util_vector_logic:2.0 {CONFIG.C_OPERATION or CONFIG.C_SIZE 1}
wire sys_rstgen/mb_reset sys_mb_reset/Op1
wire sys_mb_reset/Res    sys_mb/Reset

# ---------------------------------------------------------------- local memory
# code_ram: port A = instruction LMB, port B = AXI (data reads + code loading)
add_ip ilmb       xilinx.com:ip:lmb_v10:3.0
add_ip ilmb_cntlr xilinx.com:ip:lmb_bram_if_cntlr:4.0 {CONFIG.C_ECC 0}
add_ip code_ram   xilinx.com:ip:blk_mem_gen:8.4 {
  CONFIG.Memory_Type True_Dual_Port_RAM
  CONFIG.use_bram_block BRAM_Controller
}
add_ip code_ram_axi xilinx.com:ip:axi_bram_ctrl:4.1 {
  CONFIG.PROTOCOL AXI4
  CONFIG.SINGLE_PORT_BRAM 1
  CONFIG.DATA_WIDTH 32
}
connect_bd_intf_net [get_bd_intf_pins sys_mb/ILMB]          [get_bd_intf_pins ilmb/LMB_M]
connect_bd_intf_net [get_bd_intf_pins ilmb/LMB_Sl_0]        [get_bd_intf_pins ilmb_cntlr/SLMB]
connect_bd_intf_net [get_bd_intf_pins ilmb_cntlr/BRAM_PORT] [get_bd_intf_pins code_ram/BRAM_PORTA]
connect_bd_intf_net [get_bd_intf_pins code_ram_axi/BRAM_PORTA] [get_bd_intf_pins code_ram/BRAM_PORTB]

# data_ram: data LMB only (fast stack and variables)
add_ip dlmb       xilinx.com:ip:lmb_v10:3.0
add_ip dlmb_cntlr xilinx.com:ip:lmb_bram_if_cntlr:4.0 {CONFIG.C_ECC 0}
add_ip data_ram   xilinx.com:ip:blk_mem_gen:8.4 {
  CONFIG.Memory_Type Single_Port_RAM
  CONFIG.use_bram_block BRAM_Controller
}
connect_bd_intf_net [get_bd_intf_pins sys_mb/DLMB]          [get_bd_intf_pins dlmb/LMB_M]
connect_bd_intf_net [get_bd_intf_pins dlmb/LMB_Sl_0]        [get_bd_intf_pins dlmb_cntlr/SLMB]
connect_bd_intf_net [get_bd_intf_pins dlmb_cntlr/BRAM_PORT] [get_bd_intf_pins data_ram/BRAM_PORTA]

wire sys_clkgen/clk_out1 ilmb/LMB_Clk dlmb/LMB_Clk ilmb_cntlr/LMB_Clk dlmb_cntlr/LMB_Clk
wire sys_rstgen/bus_struct_reset ilmb/SYS_Rst dlmb/SYS_Rst ilmb_cntlr/LMB_Rst dlmb_cntlr/LMB_Rst

# ---------------------------------------------------------------- USB debug bridge
add_module usb_debug_bridge usb_debug_bridge
wire sys_clkgen/clk_out1           usb_debug_bridge/aclk
wire sys_rstgen/peripheral_aresetn usb_debug_bridge/aresetn
wire usb_debug_bridge/cpu_reset   sys_mb_reset/Op2

wire_port gpif_d_i usb_debug_bridge/gpif_d_i
connect_bd_net [get_bd_pins usb_debug_bridge/gpif_d_o] [get_bd_ports gpif_d_o]
connect_bd_net [get_bd_pins usb_debug_bridge/gpif_d_t] [get_bd_ports gpif_d_t]
connect_bd_net [get_bd_pins usb_debug_bridge/ifclk]    [get_bd_ports gpif_ifclk]
foreach p {4 5 6 8 9}       { wire_port gpif_ctl$p usb_debug_bridge/gpif_ctl$p }
foreach p {0 1 2 3 7 11 12} { connect_bd_net [get_bd_pins usb_debug_bridge/gpif_ctl$p] [get_bd_ports gpif_ctl$p] }

# ---------------------------------------------------------------- peripherals
add_ip sys_uart  xilinx.com:ip:axi_uartlite:2.0 {CONFIG.C_BAUDRATE 115200}
add_ip sys_timer xilinx.com:ip:axi_timer:2.0
add_ip sys_intc  xilinx.com:ip:axi_intc:4.1
connect_bd_intf_net [get_bd_intf_pins sys_uart/UART] [get_bd_intf_ports uart]

add_ip ad9361_spi xilinx.com:ip:axi_quad_spi:3.2 {
  CONFIG.C_USE_STARTUP 0
  CONFIG.C_NUM_SS_BITS 1
  CONFIG.C_SCK_RATIO 8
}
# fixed-direction lines on this board: export plain pins, not a tri-state interface
connect_bd_net [get_bd_pins ad9361_spi/io0_o]    [get_bd_ports ad9361_spi_mosi]
wire_port ad9361_spi_miso ad9361_spi/io1_i
connect_bd_net [get_bd_pins ad9361_spi/sck_o]    [get_bd_ports ad9361_spi_sck]
connect_bd_net [get_bd_pins ad9361_spi/ss_o]     [get_bd_ports ad9361_spi_csn]
wire sys_clkgen/clk_out1 ad9361_spi/ext_spi_clk

add_ip sys_irq xilinx.com:ip:xlconcat:2.1 {CONFIG.NUM_PORTS 5}
wire sys_uart/interrupt        sys_irq/In0
wire sys_timer/interrupt       sys_irq/In1
wire ad9361_spi/ip2intc_irpt   sys_irq/In2
wire sys_irq/dout              sys_intc/intr
connect_bd_intf_net [get_bd_intf_pins sys_intc/interrupt] [get_bd_intf_pins sys_mb/INTERRUPT]

# RF front end: one 8-bit word per channel, {tx_enable, SFDX_RX, SFDX_TX, SRX_RX,
# SRX_TX, LED_RX, LED_TXRX_RX, LED_TXRX_TX}. Reset value 0x48 = UHD's "RX off" state.
add_ip gpio_fe xilinx.com:ip:axi_gpio:2.0 {
  CONFIG.C_GPIO_WIDTH 8   CONFIG.C_ALL_OUTPUTS 1   CONFIG.C_DOUT_DEFAULT 0x00000048
  CONFIG.C_IS_DUAL 1
  CONFIG.C_GPIO2_WIDTH 8  CONFIG.C_ALL_OUTPUTS_2 1 CONFIG.C_DOUT_DEFAULT_2 0x00000048
}
connect_bd_net [get_bd_pins gpio_fe/gpio_io_o]  [get_bd_ports fe0_gpio]
connect_bd_net [get_bd_pins gpio_fe/gpio2_io_o] [get_bd_ports fe1_gpio]

# AD9361 control lines. Channel 1 (outputs): [0] RESETB, [1] EN_AGC, [2] SYNC,
# [6:3] CTRL_IN, [7] ENABLE request, [8] TXNRX request, [9] user LED.
# Channel 2 (inputs): CTRL_OUT[7:0]. Everything starts at 0 (AD9361 held in reset).
add_ip gpio_ctrl xilinx.com:ip:axi_gpio:2.0 {
  CONFIG.C_GPIO_WIDTH 16  CONFIG.C_ALL_OUTPUTS 1  CONFIG.C_DOUT_DEFAULT 0x00000000
  CONFIG.C_IS_DUAL 1
  CONFIG.C_GPIO2_WIDTH 8  CONFIG.C_ALL_INPUTS_2 1
}
connect_bd_net [get_bd_pins gpio_ctrl/gpio_io_o] [get_bd_ports ctrl_gpio_o]
wire_port ctrl_gpio_i gpio_ctrl/gpio2_io_i
slice ctrl_up_enable gpio_ctrl/gpio_io_o 7
slice ctrl_up_txnrx  gpio_ctrl/gpio_io_o 8

# 40 MHz TCXO tuning DAC (16 bit, mid-scale 0x7FFF at reset)
add_ip gpio_dac xilinx.com:ip:axi_gpio:2.0 {
  CONFIG.C_GPIO_WIDTH 16 CONFIG.C_ALL_OUTPUTS 1 CONFIG.C_DOUT_DEFAULT 0x00007FFF
}
add_module tcxo_dac DACx311_auto_spi
wire sys_clkgen/clk_out1   tcxo_dac/clk
wire gpio_dac/gpio_io_o   tcxo_dac/dat
connect_bd_net [get_bd_pins tcxo_dac/sclk]   [get_bd_ports tcxo_dac_sclk]
connect_bd_net [get_bd_pins tcxo_dac/mosi]   [get_bd_ports tcxo_dac_mosi]
connect_bd_net [get_bd_pins tcxo_dac/sync_n] [get_bd_ports tcxo_dac_sync_n]

# ---------------------------------------------------------------- AD9361
# CMOS dual-port interface (P0 = RX, P1 = TX), built-in DDS for test tones.
add_ip axi_ad9361 analog.com:user:axi_ad9361:1.0 {
  CONFIG.ID 0
  CONFIG.CMOS_OR_LVDS_N 1
  CONFIG.TDD_DISABLE 1
  CONFIG.DAC_DDS_TYPE 1
  CONFIG.DAC_DDS_CORDIC_DW 14
  CONFIG.DAC_DDS_CORDIC_PHASE_DW 13
}
wire_port rx_clk_in   axi_ad9361/rx_clk_in
wire_port rx_frame_in axi_ad9361/rx_frame_in
wire_port rx_data_in  axi_ad9361/rx_data_in
connect_bd_net [get_bd_pins axi_ad9361/tx_clk_out]   [get_bd_ports tx_clk_out]
connect_bd_net [get_bd_pins axi_ad9361/tx_frame_out] [get_bd_ports tx_frame_out]
connect_bd_net [get_bd_pins axi_ad9361/tx_data_out]  [get_bd_ports tx_data_out]
connect_bd_net [get_bd_pins axi_ad9361/enable]       [get_bd_ports enable]
connect_bd_net [get_bd_pins axi_ad9361/txnrx]        [get_bd_ports txnrx]

wire sys_clkgen/clk_out2     axi_ad9361/delay_clk
wire axi_ad9361/l_clk         axi_ad9361/clk
wire axi_ad9361/dac_sync_out  axi_ad9361/dac_sync_in
wire ctrl_up_enable/Dout      axi_ad9361/up_enable
wire ctrl_up_txnrx/Dout       axi_ad9361/up_txnrx

const const_0   1  0
const const_0x16 16 0
wire const_0/dout    ad9361_spi/io0_i ad9361_spi/sck_i
wire const_1/dout    ad9361_spi/ss_i
wire const_0/dout    axi_ad9361/adc_dovf axi_ad9361/dac_dunf axi_ad9361/tdd_sync axi_ad9361/gps_pps
wire const_0x16/dout axi_ad9361/dac_data_i1 axi_ad9361/dac_data_q1

# sample capture for debugging the receive path
add_module adc_capture adc_capture {CONFIG.ADDR_BITS 14}
wire axi_ad9361/l_clk        adc_capture/adc_clk
wire axi_ad9361/adc_valid_i0 adc_capture/adc_valid
wire axi_ad9361/adc_data_i0  adc_capture/adc_data_i0
wire axi_ad9361/adc_data_q0  adc_capture/adc_data_q0
wire axi_ad9361/adc_data_i1  adc_capture/adc_data_i1
wire axi_ad9361/adc_data_q1  adc_capture/adc_data_q1

# Modulators: GFSK (FrSky, FlySky) and LoRa (ExpressLRS); tx_select passes the
# active one to TX1 through the core's DMA inputs (DAC data_sel = 2). The core's
# DDS stays available for tests (data_sel = 0).
add_module gfsk_tx gfsk_tx
wire axi_ad9361/l_clk        gfsk_tx/dac_clk
wire axi_ad9361/dac_valid_i0 gfsk_tx/dac_valid
wire gfsk_tx/irq             sys_irq/In3

add_module lora_tx lora_tx
wire axi_ad9361/l_clk        lora_tx/dac_clk
wire axi_ad9361/dac_valid_i0 lora_tx/dac_valid
wire lora_tx/irq             sys_irq/In4

# GFSK receiver (telemetry, bind replies): RX samples -> sync word -> bits
add_module gfsk_rx gfsk_rx
wire axi_ad9361/l_clk        gfsk_rx/adc_clk
wire axi_ad9361/adc_valid_i0 gfsk_rx/adc_valid
wire axi_ad9361/adc_data_i0  gfsk_rx/adc_data_i
wire axi_ad9361/adc_data_q0  gfsk_rx/adc_data_q

# PPM input (a radio's trainer port on CN2 GPIO_0), decoded like Multiprotocol
add_module ppm_in ppm_in
wire_port ppm_in ppm_in/ppm_pin

add_module tx_select tx_select
wire axi_ad9361/l_clk        tx_select/clk
wire gfsk_tx/dac_data_i      tx_select/gfsk_i
wire gfsk_tx/dac_data_q      tx_select/gfsk_q
wire lora_tx/tx_active       tx_select/lora_active
wire lora_tx/dac_data_i      tx_select/lora_i
wire lora_tx/dac_data_q      tx_select/lora_q
wire tx_select/dac_i         axi_ad9361/dac_data_i0
wire tx_select/dac_q         axi_ad9361/dac_data_q0

# ---------------------------------------------------------------- AXI interconnect
set slaves {
  code_ram_axi/S_AXI
  sys_uart/S_AXI
  sys_intc/s_axi
  sys_timer/S_AXI
  gpio_fe/S_AXI
  gpio_ctrl/S_AXI
  gpio_dac/S_AXI
  ad9361_spi/AXI_LITE
  axi_ad9361/s_axi
  adc_capture/s_axi
  gfsk_tx/s_axi
  lora_tx/s_axi
  ppm_in/s_axi
  gfsk_rx/s_axi
}
add_ip axi_cpu_interconnect xilinx.com:ip:axi_interconnect:2.1 \
  [list CONFIG.NUM_SI 2 CONFIG.NUM_MI [llength $slaves]]
connect_bd_intf_net [get_bd_intf_pins sys_mb/M_AXI_DP]        [get_bd_intf_pins axi_cpu_interconnect/S00_AXI]
connect_bd_intf_net [get_bd_intf_pins usb_debug_bridge/m_axi] [get_bd_intf_pins axi_cpu_interconnect/S01_AXI]

set i 0
foreach s $slaves {
  set mi [format "M%02d" $i]
  connect_bd_intf_net [get_bd_intf_pins axi_cpu_interconnect/${mi}_AXI] [get_bd_intf_pins $s]
  wire sys_clkgen/clk_out1 axi_cpu_interconnect/${mi}_ACLK
  wire sys_rstgen/peripheral_aresetn axi_cpu_interconnect/${mi}_ARESETN
  incr i
}
wire sys_clkgen/clk_out1 axi_cpu_interconnect/ACLK axi_cpu_interconnect/S00_ACLK axi_cpu_interconnect/S01_ACLK
wire sys_rstgen/interconnect_aresetn axi_cpu_interconnect/ARESETN
wire sys_rstgen/peripheral_aresetn axi_cpu_interconnect/S00_ARESETN axi_cpu_interconnect/S01_ARESETN

foreach c {code_ram_axi sys_uart sys_intc sys_timer gpio_fe gpio_ctrl gpio_dac ad9361_spi axi_ad9361 adc_capture gfsk_tx lora_tx ppm_in gfsk_rx} {
  wire sys_clkgen/clk_out1            $c/s_axi_aclk
  wire sys_rstgen/peripheral_aresetn $c/s_axi_aresetn
}

# ---------------------------------------------------------------- address map
# Each slave gets one address, the same in every master that reaches it:
# I = MicroBlaze instruction side, D = MicroBlaze data side, U = USB bridge.
proc place {cell offset range masters} {
  array set space {I sys_mb/Instruction D sys_mb/Data U usb_debug_bridge/m_axi}
  foreach seg [get_bd_addr_segs -of_objects [get_bd_cells $cell]] {
    foreach m [split $masters ""] {
      assign_bd_address -target_address_space [get_bd_addr_spaces $space($m)] \
        -offset $offset -range $range $seg
    }
  }
}
place ilmb_cntlr   0x00000000 512K I
place code_ram_axi 0x00000000 512K DU
place dlmb_cntlr   0x00100000 64K  D
place gpio_fe      0x40000000 64K  DU
place gpio_ctrl    0x40010000 64K  DU
place gpio_dac     0x40020000 64K  DU
place sys_uart     0x40600000 64K  DU
place sys_intc     0x41200000 64K  DU
place sys_timer    0x41C00000 64K  DU
place adc_capture  0x44000000 128K DU
place gfsk_tx      0x44040000 64K  DU
place lora_tx      0x44050000 64K  DU
place ppm_in       0x44060000 64K  DU
place gfsk_rx      0x44070000 64K  DU
place ad9361_spi   0x44A00000 64K  DU
place axi_ad9361   0x79020000 64K  DU

# print the map (also lands in the build log)
foreach as {sys_mb/Data usb_debug_bridge/m_axi} {
  puts "Address space $as:"
  foreach seg [get_bd_addr_segs $as/*] { puts "  [get_property OFFSET $seg] [get_property RANGE $seg] $seg" }
}

validate_bd_design
save_bd_design
