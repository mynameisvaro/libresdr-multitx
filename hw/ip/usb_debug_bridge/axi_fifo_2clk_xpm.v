// Drop-in replacement for Ettus' axi_fifo_2clk (same module name and ports).
//
// The Ettus version wraps two Vivado FIFO Generator IP cores (fifo_short_2clk,
// fifo_4k_2clk). This one uses the XPM macro instead, so the GPIF bridge can be
// used as a self-contained RTL module in the block design. First-word-fall-through,
// like the original. PIPELINE and DEVICE are accepted but ignored.

`timescale 1ns / 1ps
`default_nettype none

module axi_fifo_2clk #(
  parameter WIDTH    = 69,
  parameter SIZE     = 9,          // log2 of the FIFO depth
  parameter PIPELINE = "NONE",
  parameter DEVICE   = "7SERIES"
)(
  input  wire             reset,
  input  wire             i_aclk,
  input  wire [WIDTH-1:0] i_tdata,
  input  wire             i_tvalid,
  output wire             i_tready,
  input  wire             o_aclk,
  output wire [WIDTH-1:0] o_tdata,
  output wire             o_tvalid,
  input  wire             o_tready
);

  localparam DEPTH = (SIZE < 4) ? 16 : (1 << SIZE);

  // xpm_fifo_async wants its reset synchronous to wr_clk
  (* ASYNC_REG = "TRUE" *) reg [2:0] rst_sync = 3'b111;
  always @(posedge i_aclk or posedge reset)
    if (reset) rst_sync <= 3'b111;
    else       rst_sync <= {rst_sync[1:0], 1'b0};

  wire full, empty, wr_rst_busy, rd_rst_busy;

  xpm_fifo_async #(
    .FIFO_MEMORY_TYPE  ((SIZE <= 5) ? "distributed" : "block"),
    .FIFO_WRITE_DEPTH  (DEPTH),
    .WRITE_DATA_WIDTH  (WIDTH),
    .READ_DATA_WIDTH   (WIDTH),
    .READ_MODE         ("fwft"),
    .FIFO_READ_LATENCY (0),
    .CDC_SYNC_STAGES   (2),
    .RELATED_CLOCKS    (0),
    .ECC_MODE          ("no_ecc"),
    .FULL_RESET_VALUE  (1),
    .USE_ADV_FEATURES  ("0000"),
    .DOUT_RESET_VALUE  ("0"),
    .SIM_ASSERT_CHK    (0),
    .WAKEUP_TIME       (0)
  ) fifo (
    .rst           (rst_sync[2]),
    .wr_clk        (i_aclk),
    .wr_en         (i_tvalid & i_tready),
    .din           (i_tdata),
    .full          (full),
    .wr_rst_busy   (wr_rst_busy),
    .rd_clk        (o_aclk),
    .rd_en         (o_tvalid & o_tready),
    .dout          (o_tdata),
    .empty         (empty),
    .rd_rst_busy   (rd_rst_busy),
    .sleep         (1'b0),
    .injectsbiterr (1'b0),
    .injectdbiterr (1'b0),
    .prog_full     (), .wr_data_count (), .overflow   (), .almost_full  (), .wr_ack (),
    .prog_empty    (), .rd_data_count (), .underflow  (), .almost_empty (), .data_valid (),
    .sbiterr       (), .dbiterr       ()
  );

  assign i_tready = ~full  & ~wr_rst_busy;
  assign o_tvalid = ~empty & ~rd_rst_busy;

endmodule

`default_nettype wire
