// usb_debug_bridge: host access to the AXI bus over the B220's FX3 USB chip.
//
// The FX3 runs Ettus' stock B200 firmware (loaded by UHD), so the FPGA side is
// Ettus' GPIF II slave-FIFO block. Packets arriving on the USB data-OUT endpoint
// (0x02) are executed by usb_cmd_proc as AXI4-Lite reads/writes; results go back
// on the data-IN endpoint (0x86). The control endpoints are drained and unused.
//
// ctrl_reg bit 0 drives cpu_reset (hold the MicroBlaze while loading code).
// This block also forwards its clock to the FX3 (IFCLK).

`timescale 1ns / 1ps
`default_nettype none

module usb_debug_bridge #(
  parameter [31:0] VERSION      = 32'h0001_0000,
  parameter [31:0] CTRL_DEFAULT = 32'h0000_0000
)(
  (* X_INTERFACE_INFO = "xilinx.com:signal:clock:1.0 aclk CLK" *)
  (* X_INTERFACE_PARAMETER = "ASSOCIATED_BUSIF m_axi, ASSOCIATED_RESET aresetn" *)
  input  wire        aclk,          // 100 MHz, also the GPIF clock
  (* X_INTERFACE_INFO = "xilinx.com:signal:reset:1.0 aresetn RST" *)
  (* X_INTERFACE_PARAMETER = "POLARITY ACTIVE_LOW" *)
  input  wire        aresetn,

  // FX3 GPIF II (slave FIFO, 32 bit)
  output wire        ifclk,
  input  wire [31:0] gpif_d_i,
  output wire [31:0] gpif_d_o,
  output wire [31:0] gpif_d_t,
  input  wire        gpif_ctl4,     // FX3 flag: ready
  input  wire        gpif_ctl5,     // FX3 flag: watermark
  input  wire        gpif_ctl6,     // FX3 serial settings bus (unused)
  input  wire        gpif_ctl8,     // FX3 serial settings bus (unused)
  input  wire        gpif_ctl9,     // FX3 global reset, active high
  output wire        gpif_ctl0,     // n_slcs
  output wire        gpif_ctl1,     // n_slwr
  output wire        gpif_ctl2,     // n_sloe
  output wire        gpif_ctl3,     // n_slrd
  output wire        gpif_ctl7,     // n_pktend
  output wire        gpif_ctl11,    // fifo address [1]
  output wire        gpif_ctl12,    // fifo address [0]

  // AXI4-Lite master
  output wire [31:0] m_axi_awaddr,
  output wire [ 2:0] m_axi_awprot,
  output wire        m_axi_awvalid,
  input  wire        m_axi_awready,
  output wire [31:0] m_axi_wdata,
  output wire [ 3:0] m_axi_wstrb,
  output wire        m_axi_wvalid,
  input  wire        m_axi_wready,
  input  wire [ 1:0] m_axi_bresp,
  input  wire        m_axi_bvalid,
  output wire        m_axi_bready,
  output wire [31:0] m_axi_araddr,
  output wire [ 2:0] m_axi_arprot,
  output wire        m_axi_arvalid,
  input  wire        m_axi_arready,
  input  wire [31:0] m_axi_rdata,
  input  wire [ 1:0] m_axi_rresp,
  input  wire        m_axi_rvalid,
  output wire        m_axi_rready,

  (* X_INTERFACE_INFO = "xilinx.com:signal:reset:1.0 cpu_reset RST" *)
  (* X_INTERFACE_PARAMETER = "POLARITY ACTIVE_HIGH" *)
  output wire        cpu_reset
);

  // ---- reset: system reset or FX3 global reset ----
  (* ASYNC_REG = "TRUE" *) reg [1:0] fx3_rst_sync = 2'b11;
  reg [3:0] rst_hold = 4'hF;
  always @(posedge aclk) begin
    fx3_rst_sync <= {fx3_rst_sync[0], gpif_ctl9};
    if (!aresetn || fx3_rst_sync[1]) rst_hold <= 4'hF;
    else                             rst_hold <= {rst_hold[2:0], 1'b0};
  end
  wire rst = rst_hold[3];

  // ---- FX3 slave FIFO ----
  wire [63:0] cmd_tdata,  rsp_tdata,  ctrl_tdata;
  wire        cmd_tlast,  rsp_tlast,  ctrl_tlast;
  wire        cmd_tvalid, rsp_tvalid, ctrl_tvalid;
  wire        cmd_tready, rsp_tready, ctrl_tready;
  wire        slcs, slwr, sloe, slrd, pktend;
  wire [1:0]  fifoadr;

  gpif2_slave_fifo32 #(
    .DATA_RX_FIFO_SIZE (12),
    .DATA_TX_FIFO_SIZE (12)
  ) slave_fifo32 (
    .gpif_clk (aclk), .gpif_rst (rst), .gpif_enb (1'b1),
    .gpif_d_i (gpif_d_i), .gpif_d_o (gpif_d_o), .gpif_d_t (gpif_d_t),
    .gpif_ctl ({gpif_ctl8, gpif_ctl6, gpif_ctl5, gpif_ctl4}),
    .fifoadr  (fifoadr),
    .slwr (slwr), .sloe (sloe), .slcs (slcs), .slrd (slrd), .pktend (pktend),
    .fifo_clk (aclk), .fifo_rst (rst),
    // data endpoints carry our commands/responses
    .tx_tdata   (cmd_tdata), .tx_tlast (cmd_tlast), .tx_tvalid (cmd_tvalid), .tx_tready (cmd_tready),
    .rx_tdata   (rsp_tdata), .rx_tlast (rsp_tlast), .rx_tvalid (rsp_tvalid), .rx_tready (rsp_tready),
    // control endpoints: drain, never answer
    .ctrl_tdata (ctrl_tdata), .ctrl_tlast (ctrl_tlast), .ctrl_tvalid (ctrl_tvalid), .ctrl_tready (1'b1),
    .resp_tdata (64'd0), .resp_tlast (1'b0), .resp_tvalid (1'b0), .resp_tready (),
    .debug ()
  );

  assign gpif_ctl0  = slcs;
  assign gpif_ctl1  = slwr;
  assign gpif_ctl2  = sloe;
  assign gpif_ctl3  = slrd;
  assign gpif_ctl7  = pktend;
  assign gpif_ctl11 = fifoadr[1];
  assign gpif_ctl12 = fifoadr[0];

  // ---- command processor ----
  wire [31:0] ctrl_reg;

  usb_cmd_proc #(
    .VERSION      (VERSION),
    .CTRL_DEFAULT (CTRL_DEFAULT)
  ) cmd_proc (
    .clk (aclk), .rst (rst),
    .i_tdata (cmd_tdata), .i_tlast (cmd_tlast), .i_tvalid (cmd_tvalid), .i_tready (cmd_tready),
    .o_tdata (rsp_tdata), .o_tlast (rsp_tlast), .o_tvalid (rsp_tvalid), .o_tready (rsp_tready),
    .m_axi_awaddr (m_axi_awaddr), .m_axi_awvalid (m_axi_awvalid), .m_axi_awready (m_axi_awready),
    .m_axi_wdata  (m_axi_wdata),  .m_axi_wstrb   (m_axi_wstrb),   .m_axi_wvalid  (m_axi_wvalid),
    .m_axi_wready (m_axi_wready), .m_axi_bresp   (m_axi_bresp),   .m_axi_bvalid  (m_axi_bvalid),
    .m_axi_bready (m_axi_bready),
    .m_axi_araddr (m_axi_araddr), .m_axi_arvalid (m_axi_arvalid), .m_axi_arready (m_axi_arready),
    .m_axi_rdata  (m_axi_rdata),  .m_axi_rresp   (m_axi_rresp),   .m_axi_rvalid  (m_axi_rvalid),
    .m_axi_rready (m_axi_rready),
    .ctrl_reg  (ctrl_reg),
    .status_in (32'd0)
  );

  assign m_axi_awprot = 3'b000;
  assign m_axi_arprot = 3'b000;
  assign cpu_reset    = ctrl_reg[0];

  // ---- IFCLK: forward aclk to the FX3 (constraints refer to ifclk_oddr) ----
  ODDR #(
    .DDR_CLK_EDGE ("OPPOSITE_EDGE"),
    .INIT         (1'b0),
    .SRTYPE       ("SYNC")
  ) ifclk_oddr (
    .Q (ifclk), .C (aclk), .CE (1'b1), .D1 (1'b1), .D2 (1'b0), .R (1'b0), .S (1'b0)
  );

endmodule

`default_nettype wire
