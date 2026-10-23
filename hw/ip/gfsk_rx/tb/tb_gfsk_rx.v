// gfsk_rx testbench: applies the AXI writes in stim.mem ({addr, data} per line,
// written by check.py), arms the receiver, plays the samples in iq.mem
// ({I, Q} as 16-bit hex per line) one per adc_clk, then prints the status, the
// results and the buffer for check.py.
`timescale 1ns / 1ps

module tb_gfsk_rx;
  parameter real ADC_PERIOD = 16.276;  // ns (61.44 MSPS), overridden by check.py
  parameter integer NSAMP = 1;

  reg aclk = 0, adc_clk = 0, aresetn = 0;
  always #5 aclk = ~aclk;
  always #(ADC_PERIOD / 2) adc_clk = ~adc_clk;

  reg  [15:0] awaddr = 0, araddr = 0;
  reg  [31:0] wdata = 0;
  reg         awvalid = 0, wvalid = 0, arvalid = 0;
  wire        awready, wready, bvalid, arready, rvalid;
  wire [31:0] rdata;
  wire [ 1:0] bresp, rresp;
  reg  [15:0] si = 0, sq = 0;
  reg         sv = 0;

  gfsk_rx dut (
    .adc_clk(adc_clk), .adc_valid(sv), .adc_data_i(si), .adc_data_q(sq),
    .s_axi_aclk(aclk), .s_axi_aresetn(aresetn),
    .s_axi_awaddr(awaddr), .s_axi_awprot(3'd0), .s_axi_awvalid(awvalid), .s_axi_awready(awready),
    .s_axi_wdata(wdata), .s_axi_wstrb(4'hF), .s_axi_wvalid(wvalid), .s_axi_wready(wready),
    .s_axi_bresp(bresp), .s_axi_bvalid(bvalid), .s_axi_bready(1'b1),
    .s_axi_araddr(araddr), .s_axi_arprot(3'd0), .s_axi_arvalid(arvalid), .s_axi_arready(arready),
    .s_axi_rdata(rdata), .s_axi_rresp(rresp), .s_axi_rvalid(rvalid), .s_axi_rready(1'b1));

  task axi_write(input [15:0] a, input [31:0] d);
    begin
      @(posedge aclk); awaddr <= a; wdata <= d; awvalid <= 1; wvalid <= 1;
      @(posedge aclk); while (!awready) @(posedge aclk);
      awvalid <= 0; wvalid <= 0;
      while (!bvalid) @(posedge aclk);
    end
  endtask

  task axi_read(input [15:0] a, output [31:0] d);
    begin
      @(posedge aclk); araddr <= a; arvalid <= 1;
      @(posedge aclk); while (!arready) @(posedge aclk);
      arvalid <= 0;
      while (!rvalid) @(posedge aclk);
      d = rdata;
    end
  endtask

  reg [47:0] stim [0:255];
  reg [31:0] iq [0:NSAMP-1];
  integer i, n;
  reg [31:0] st, v;
  reg playing = 0;

  initial begin
    for (i = 0; i < 256; i = i + 1) stim[i] = 48'hFFFF_FFFF_FFFF;
    $readmemh("stim.mem", stim);
    $readmemh("iq.mem", iq);
    repeat (10) @(posedge aclk);
    aresetn = 1;
    for (i = 0; i < 256 && stim[i] != 48'hFFFF_FFFF_FFFF; i = i + 1)
      axi_write(stim[i][47:32], stim[i][31:0]);
    axi_read(16'h000C, st);
    if (st != 32'h47465258) $display("FAIL: ID %h", st);
    axi_write(16'h0000, 32'h1);           // arm
    repeat (20) @(posedge adc_clk);
    playing = 1;
    wait (!playing);
    repeat (200) @(posedge adc_clk);
    axi_read(16'h0004, st);
    $display("STATUS %h", st);
    axi_read(16'h0028, v); $display("TSTAMP %0d", v);
    axi_read(16'h002C, v); $display("FOFS %0d", $signed(v[15:0]));
    axi_read(16'h0030, v); $display("LEVEL %0d", v);
    axi_read(16'h0034, v); $display("SYNCERR %0d", v);
    axi_read(16'h0008, n);
    for (i = 0; i < (n + 7) / 8; i = i + 1) begin
      axi_read(16'h1000 + 4 * i, v);
      $display("BYTE %0d %02x", i, v[7:0]);
    end
    $display("SIM_DONE");
    $finish;
  end

  integer k = 0;
  integer dbg;
  initial dbg = $fopen("debug.txt", "w");
  always @(posedge adc_clk) begin
    if (dut.f_valid) $fwrite(dbg, "F %0d %0d %0d %0d\n", $signed(dut.freq), $signed(dut.est[31:16]), dut.mag, $signed(dut.di));
    if (dut.bit_v)   $fwrite(dbg, "B %0d\n", dut.bit_val);
  end
  always @(posedge adc_clk) begin
    sv <= 1'b0;
    if (playing) begin
      if (k < NSAMP) begin
        si <= iq[k][31:16];
        sq <= iq[k][15:0];
        sv <= 1'b1;
        k  <= k + 1;
      end else
        playing <= 0;
    end
  end
endmodule
