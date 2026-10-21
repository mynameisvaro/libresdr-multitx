// adc_capture: grabs a burst of AD9361 I/Q samples into block RAM for debugging.
//
// AXI4-Lite register map (byte offsets):
//   0x00 CTRL    W: bit0 = 1 starts a capture, bit1 = channel (0: RX1, 1: RX2)
//   0x04 STATUS  R: bit0 busy, bit1 done, [31:16] = buffer size in samples
//   0x08 LENGTH  RW: number of samples to capture (1 .. buffer size)
//   0x0C ID      R: "ADCC"
//   0x10000 ..   R: sample memory, one word per sample = {Q[15:0], I[15:0]}
//
// Samples are written in the AD9361 clock domain; everything else runs on s_axi_aclk.

`timescale 1ns / 1ps
`default_nettype none

module adc_capture #(
  parameter ADDR_BITS = 14              // buffer = 2^ADDR_BITS samples
)(
  (* X_INTERFACE_INFO = "xilinx.com:signal:clock:1.0 adc_clk CLK" *)
  input  wire        adc_clk,
  // plain wires: stop Vivado from guessing a FIFO interface from the names
  (* X_INTERFACE_IGNORE = "true" *) input  wire        adc_valid,
  (* X_INTERFACE_IGNORE = "true" *) input  wire [15:0] adc_data_i0,
  (* X_INTERFACE_IGNORE = "true" *) input  wire [15:0] adc_data_q0,
  (* X_INTERFACE_IGNORE = "true" *) input  wire [15:0] adc_data_i1,
  (* X_INTERFACE_IGNORE = "true" *) input  wire [15:0] adc_data_q1,

  (* X_INTERFACE_INFO = "xilinx.com:signal:clock:1.0 s_axi_aclk CLK" *)
  (* X_INTERFACE_PARAMETER = "ASSOCIATED_BUSIF s_axi, ASSOCIATED_RESET s_axi_aresetn" *)
  input  wire        s_axi_aclk,
  input  wire        s_axi_aresetn,
  input  wire [16:0] s_axi_awaddr,
  input  wire [ 2:0] s_axi_awprot,
  input  wire        s_axi_awvalid,
  output reg         s_axi_awready,
  input  wire [31:0] s_axi_wdata,
  input  wire [ 3:0] s_axi_wstrb,
  input  wire        s_axi_wvalid,
  output reg         s_axi_wready,
  output wire [ 1:0] s_axi_bresp,
  output reg         s_axi_bvalid,
  input  wire        s_axi_bready,
  input  wire [16:0] s_axi_araddr,
  input  wire [ 2:0] s_axi_arprot,
  input  wire        s_axi_arvalid,
  output reg         s_axi_arready,
  output reg  [31:0] s_axi_rdata,
  output wire [ 1:0] s_axi_rresp,
  output reg         s_axi_rvalid,
  input  wire        s_axi_rready
);

  localparam DEPTH = 1 << ADDR_BITS;

  assign s_axi_bresp = 2'b00;
  assign s_axi_rresp = 2'b00;

  // ---------------- AXI side (s_axi_aclk) ----------------
  reg [ADDR_BITS:0] length;     // up to DEPTH
  reg               chan;
  reg               arm_tgl;    // toggles once per CTRL start
  wire              done_tgl_s; // done toggle, synchronised
  reg               done_tgl_seen;
  reg               busy, done;

  // write channel: take address and data together
  always @(posedge s_axi_aclk) begin
    if (!s_axi_aresetn) begin
      s_axi_awready <= 1'b0;
      s_axi_wready  <= 1'b0;
      s_axi_bvalid  <= 1'b0;
      length        <= DEPTH;
      chan          <= 1'b0;
      arm_tgl       <= 1'b0;
      busy          <= 1'b0;
      done          <= 1'b0;
      done_tgl_seen <= 1'b0;
    end else begin
      s_axi_awready <= 1'b0;
      s_axi_wready  <= 1'b0;
      if (s_axi_bvalid && s_axi_bready) s_axi_bvalid <= 1'b0;

      if (s_axi_awvalid && s_axi_wvalid && !s_axi_awready && !s_axi_bvalid) begin
        s_axi_awready <= 1'b1;
        s_axi_wready  <= 1'b1;
        s_axi_bvalid  <= 1'b1;
        if (s_axi_awaddr[16] == 1'b0) begin
          case (s_axi_awaddr[7:2])
            6'h00: if (s_axi_wdata[0] && !busy) begin
                     chan    <= s_axi_wdata[1];
                     arm_tgl <= ~arm_tgl;
                     busy    <= 1'b1;
                     done    <= 1'b0;
                   end
            6'h02: length <= (s_axi_wdata == 0 || s_axi_wdata > DEPTH) ? DEPTH : s_axi_wdata[ADDR_BITS:0];
            default: ;
          endcase
        end
      end

      if (done_tgl_s != done_tgl_seen) begin
        done_tgl_seen <= done_tgl_s;
        busy          <= 1'b0;
        done          <= 1'b1;
      end
    end
  end

  // read channel: one cycle for the block RAM, one to answer
  (* ram_style = "block" *) reg [31:0] mem [0:DEPTH-1];
  reg [31:0]          mem_q;
  reg [16:0]          raddr;
  reg                 rpend;

  always @(posedge s_axi_aclk) mem_q <= mem[raddr[ADDR_BITS+1:2]];

  always @(posedge s_axi_aclk) begin
    if (!s_axi_aresetn) begin
      s_axi_arready <= 1'b0;
      s_axi_rvalid  <= 1'b0;
      rpend         <= 1'b0;
    end else begin
      s_axi_arready <= 1'b0;
      if (s_axi_rvalid && s_axi_rready) s_axi_rvalid <= 1'b0;

      if (s_axi_arvalid && !s_axi_arready && !rpend && !s_axi_rvalid) begin
        s_axi_arready <= 1'b1;
        raddr         <= s_axi_araddr;
        rpend         <= 1'b1;
      end else if (rpend) begin
        rpend        <= 1'b0;
        s_axi_rvalid <= 1'b1;
        if (raddr[16])
          s_axi_rdata <= mem_q;
        else case (raddr[7:2])
          6'h01:   s_axi_rdata <= {DEPTH[15:0], 14'd0, done, busy};
          6'h02:   s_axi_rdata <= length;
          6'h03:   s_axi_rdata <= 32'h4144_4343; // "ADCC"
          default: s_axi_rdata <= 32'd0;
        endcase
      end
    end
  end

  // ---------------- sample side (adc_clk) ----------------
  (* ASYNC_REG = "TRUE" *) reg [2:0] arm_sync = 3'b000;
  reg                 capturing = 1'b0;
  reg                 done_tgl  = 1'b0;
  reg [ADDR_BITS:0]   wcount, wlen;
  reg                 wchan;

  always @(posedge adc_clk) begin
    arm_sync <= {arm_sync[1:0], arm_tgl};
    if (arm_sync[2] != arm_sync[1]) begin
      // length/chan were written before the start bit and are stable here
      capturing <= 1'b1;
      wcount    <= 0;
      wlen      <= length;
      wchan     <= chan;
    end else if (capturing && adc_valid) begin
      mem[wcount[ADDR_BITS-1:0]] <= wchan ? {adc_data_q1, adc_data_i1} : {adc_data_q0, adc_data_i0};
      wcount <= wcount + 1'b1;
      if (wcount + 1'b1 == wlen) begin
        capturing <= 1'b0;
        done_tgl  <= ~done_tgl;
      end
    end
  end

  (* ASYNC_REG = "TRUE" *) reg [1:0] done_sync = 2'b00;
  always @(posedge s_axi_aclk) done_sync <= {done_sync[0], done_tgl};
  assign done_tgl_s = done_sync[1];

endmodule

`default_nettype wire
