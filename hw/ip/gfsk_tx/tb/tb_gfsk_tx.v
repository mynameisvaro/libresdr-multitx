// gfsk_tx testbench: applies the AXI writes in stim.mem ({addr, data} per line,
// written by check.py), starts the packet, dumps every output sample to iq.txt
// until the modulator is idle again.
`timescale 1ns / 1ps

module tb_gfsk_tx;
  parameter real DAC_PERIOD = 250.0;   // ns (4 MSPS), overridden by check.py

  reg aclk = 0, dac_clk = 0, aresetn = 0;
  always #5 aclk = ~aclk;
  always #(DAC_PERIOD / 2) dac_clk = ~dac_clk;

  reg  [15:0] awaddr = 0, araddr = 0;
  reg  [31:0] wdata = 0;
  reg         awvalid = 0, wvalid = 0, arvalid = 0;
  wire        awready, wready, bvalid, arready, rvalid;
  wire [31:0] rdata;
  wire [ 1:0] bresp, rresp;
  wire [15:0] di, dq;
  wire        tx_active, irq;

  gfsk_tx dut (
    .dac_clk(dac_clk), .dac_valid(1'b1), .dac_data_i(di), .dac_data_q(dq),
    .tx_active(tx_active), .irq(irq),
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

  reg [47:0] stim [0:4095];
  integer nstim, i, f, nsamp;
  reg [31:0] st;
  reg dumping = 0;

  always @(posedge dac_clk) if (dumping) begin
    $fwrite(f, "%0d %0d\n", $signed(di), $signed(dq));
    nsamp = nsamp + 1;
  end

  initial begin
    for (i = 0; i < 4096; i = i + 1) stim[i] = 48'hFFFF_FFFF_FFFF;
    $readmemh("stim.mem", stim);
    f = $fopen("iq.txt", "w");
    nsamp = 0;
    repeat (10) @(posedge aclk);
    aresetn = 1;
    for (i = 0; i < 4096 && stim[i] != 48'hFFFF_FFFF_FFFF; i = i + 1)
      axi_write(stim[i][47:32], stim[i][31:0]);
    axi_read(16'h000C, st);
    if (st != 32'h4746534B) $display("FAIL: ID %h", st);
    repeat (20) @(posedge dac_clk);
    dumping = 1;
    axi_write(16'h0000, 32'h1);           // start
    axi_read(16'h0004, st);
    if (!st[0]) $display("FAIL: not busy after start");
    st = 1;
    while (st[0]) begin repeat (200) @(posedge aclk); axi_read(16'h0004, st); end
    if (!st[1]) $display("FAIL: done not set");
    if (!irq)   $display("FAIL: irq not raised");
    repeat (50) @(posedge dac_clk);
    dumping = 0;
    $fclose(f);
    $display("SIM_DONE %0d samples, status %h", nsamp, st);
    $finish;
  end
endmodule
