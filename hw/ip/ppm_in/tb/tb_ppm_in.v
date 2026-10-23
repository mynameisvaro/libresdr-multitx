// ppm_in testbench: frames on the pin (both polarities), a glitch, a short
// frame, and the built-in generator; checks CH[] and the frame counter.
//   run_sim.sh   (prints PASS/FAIL per check, then SIM_DONE)
`timescale 1ns / 1ps

module tb_ppm_in;
  reg clk = 0, aresetn = 0, pin = 0;
  always #5 clk = ~clk;                  // 100 MHz

  reg  [15:0] awaddr = 0, araddr = 0;
  reg  [31:0] wdata = 0;
  reg         awvalid = 0, wvalid = 0, arvalid = 0;
  wire        awready, wready, bvalid, arready, rvalid;
  wire [31:0] rdata;
  wire [1:0]  bresp, rresp;

  ppm_in dut (
    .ppm_pin(pin), .s_axi_aclk(clk), .s_axi_aresetn(aresetn),
    .s_axi_awaddr(awaddr), .s_axi_awprot(3'd0), .s_axi_awvalid(awvalid), .s_axi_awready(awready),
    .s_axi_wdata(wdata), .s_axi_wstrb(4'hF), .s_axi_wvalid(wvalid), .s_axi_wready(wready),
    .s_axi_bresp(bresp), .s_axi_bvalid(bvalid), .s_axi_bready(1'b1),
    .s_axi_araddr(araddr), .s_axi_arprot(3'd0), .s_axi_arvalid(arvalid), .s_axi_arready(arready),
    .s_axi_rdata(rdata), .s_axi_rresp(rresp), .s_axi_rvalid(rvalid), .s_axi_rready(1'b1));

  task axi_write(input [15:0] a, input [31:0] d);
    begin
      @(posedge clk); awaddr <= a; wdata <= d; awvalid <= 1; wvalid <= 1;
      @(posedge clk); while (!awready) @(posedge clk);
      awvalid <= 0; wvalid <= 0;
      while (!bvalid) @(posedge clk);
    end
  endtask

  task axi_read(input [15:0] a, output [31:0] d);
    begin
      @(posedge clk); araddr <= a; arvalid <= 1;
      @(posedge clk); while (!arready) @(posedge clk);
      arvalid <= 0;
      while (!rvalid) @(posedge clk);
      d = rdata;
    end
  endtask

  integer fails = 0;
  task check(input [8*48-1:0] name, input ok);
    begin
      if (ok) $display("PASS %0s", name); else begin $display("FAIL %0s", name); fails = fails + 1; end
    end
  endtask

  // one PPM frame on the pin: a 300 us pulse (pol = level) at each slot start
  integer w [0:15];
  task frame(input integer n, input integer sync_us, input pol);
    integer i;
    begin
      for (i = 0; i < n; i = i + 1) begin
        pin = pol;  #(300 * 1000);
        pin = ~pol; #((w[i] - 300) * 1000);
      end
      pin = pol;  #(300 * 1000);           // the pulse that ends the last channel
      pin = ~pol; #((sync_us - 300) * 1000);
    end
  endtask

  // a frame is complete at the edge that ends its sync gap (the next frame's
  // start): one pulse, then a gap that is itself a sync, so the next frame's
  // first edge starts cleanly
  task flush(input pol);
    begin
      pin = pol;  #(300 * 1000);
      pin = ~pol; #(3000 * 1000);
    end
  endtask

  reg [31:0] st, v;
  integer i, f0, ok;
  initial begin
    repeat (10) @(posedge clk);
    aresetn = 1;
    axi_read(16'h000C, v);
    check("ID", v == 32'h50504D49);

    // positive pulses, rising edges, 8 channels
    axi_write(16'h0000, 32'h1);
    for (i = 0; i < 16; i = i + 1) w[i] = 1000 + 70 * i;
    #(3000 * 1000);                        // idle line, as before a real stream
    frame(8, 10000, 1);
    frame(8, 10000, 1);
    flush(1);
    axi_read(16'h0004, st);
    check("frames counted", st[15:0] == 2);
    check("8 channels", st[20:16] == 8);
    ok = 1;
    for (i = 0; i < 8; i = i + 1) begin
      axi_read(16'h0040 + 4 * i, v);
      if (v < 2 * w[i] - 2 || v > 2 * w[i] + 2) ok = 0;
    end
    check("channel widths (0.5 us units)", ok);

    // a glitch inside a frame spoils that frame only
    f0 = st[15:0];
    pin = 1; #(300 * 1000); pin = 0; #(700 * 1000);
    pin = 1; #(100 * 1000); pin = 0; #(100 * 1000);   // 200 us interval
    pin = 1; #(300 * 1000); pin = 0; #(9000 * 1000);  // sync
    flush(1);
    axi_read(16'h0004, st);
    check("glitched frame dropped", st[15:0] == f0);
    frame(8, 10000, 1);
    flush(1);
    axi_read(16'h0004, st);
    check("next frame accepted", st[15:0] == f0 + 1);

    // too few channels (3): ignored
    f0 = st[15:0];
    frame(3, 10000, 1);
    flush(1);
    axi_read(16'h0004, st);
    check("3-channel frame ignored", st[15:0] == f0);

    // negative pulses, falling edges, 16 channels at the extremes
    axi_write(16'h0000, 32'h3);
    for (i = 0; i < 16; i = i + 1) w[i] = (i & 1) ? 2100 : 900;
    pin = 1; #(5000 * 1000);                // idle high for negative pulses
    frame(16, 6000, 0);
    flush(0);
    axi_read(16'h0004, st);
    check("16 channels, falling edge", st[20:16] == 16);
    axi_read(16'h0040 + 4 * 15, v);
    check("channel 16 width", v >= 4198 && v <= 4202);

    // built-in generator: 6 channels
    axi_write(16'h0010, 6);
    axi_write(16'h0014, 40000);             // 20 ms frame
    for (i = 0; i < 6; i = i + 1) axi_write(16'h0080 + 4 * i, 2200 + 200 * i);
    axi_write(16'h0000, 32'h5);             // enable, rising edge, test source
    #(70_000_000);
    axi_read(16'h0004, st);
    check("generator: 6 channels", st[20:16] == 6);
    ok = 1;
    for (i = 0; i < 6; i = i + 1) begin
      axi_read(16'h0040 + 4 * i, v);
      if (v < 2200 + 200 * i - 2 || v > 2200 + 200 * i + 2) ok = 0;
    end
    check("generator: widths", ok);

    $display("SIM_DONE %0d failures", fails);
    $finish;
  end
endmodule
