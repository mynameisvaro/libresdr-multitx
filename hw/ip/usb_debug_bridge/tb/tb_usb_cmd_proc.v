// Testbench for usb_cmd_proc: drives command packets, checks the response stream
// against an AXI4-Lite memory model (addresses >= 0x8000_0000 answer DECERR).
//   ./run_sim.sh

`timescale 1ns / 1ps

module tb_usb_cmd_proc;

  reg clk = 0, rst = 1;
  always #5 clk = ~clk;

  // streams
  reg  [63:0] i_tdata;  reg i_tlast, i_tvalid;  wire i_tready;
  wire [63:0] o_tdata;  wire o_tlast, o_tvalid; reg  o_tready;

  // AXI
  wire [31:0] awaddr, wdata, araddr;  wire [3:0] wstrb;
  wire awvalid, wvalid, bready, arvalid, rready;
  reg  awready, wready, bvalid, arready, rvalid;
  reg  [1:0] bresp, rresp;  reg [31:0] rdata;
  wire [31:0] ctrl_reg;

  usb_cmd_proc #(.VERSION(32'hCAFE_0001)) dut (
    .clk(clk), .rst(rst),
    .i_tdata(i_tdata), .i_tlast(i_tlast), .i_tvalid(i_tvalid), .i_tready(i_tready),
    .o_tdata(o_tdata), .o_tlast(o_tlast), .o_tvalid(o_tvalid), .o_tready(o_tready),
    .m_axi_awaddr(awaddr), .m_axi_awvalid(awvalid), .m_axi_awready(awready),
    .m_axi_wdata(wdata), .m_axi_wstrb(wstrb), .m_axi_wvalid(wvalid), .m_axi_wready(wready),
    .m_axi_bresp(bresp), .m_axi_bvalid(bvalid), .m_axi_bready(bready),
    .m_axi_araddr(araddr), .m_axi_arvalid(arvalid), .m_axi_arready(arready),
    .m_axi_rdata(rdata), .m_axi_rresp(rresp), .m_axi_rvalid(rvalid), .m_axi_rready(rready),
    .ctrl_reg(ctrl_reg), .status_in(32'h0000_00A5)
  );

  // ---- AXI4-Lite memory model with random stalls ----
  reg [31:0] mem [0:1023];
  reg        aw_got, w_got;  reg [31:0] aw_a, w_d;
  always @(posedge clk) begin
    if (rst) begin
      awready <= 0; wready <= 0; bvalid <= 0; arready <= 0; rvalid <= 0;
      aw_got <= 0; w_got <= 0;
    end else begin
      awready <= 0; wready <= 0; arready <= 0;
      if (awvalid && !awready && !aw_got && ($random & 1)) begin awready <= 1; aw_got <= 1; aw_a <= awaddr; end
      if (wvalid  && !wready  && !w_got  && ($random & 1)) begin wready  <= 1; w_got  <= 1; w_d  <= wdata;  end
      if (aw_got && w_got && !bvalid) begin
        if (aw_a[31]) bresp <= 2'b11; else begin bresp <= 2'b00; mem[aw_a[11:2]] <= w_d; end
        bvalid <= 1; aw_got <= 0; w_got <= 0;
      end
      if (bvalid && bready) bvalid <= 0;
      if (arvalid && !arready && !rvalid && ($random & 1)) begin
        arready <= 1; rvalid <= 1;
        rdata <= araddr[31] ? 32'hDEAD_DEAD : mem[araddr[11:2]];
        rresp <= araddr[31] ? 2'b11 : 2'b00;
      end
      if (rvalid && rready) rvalid <= 0;
    end
  end

  // ---- stimulus helpers ----
  reg [63:0] pkt [0:63];  integer pkt_len;
  reg [63:0] rsp [0:63];  integer rsp_len;
  integer errors = 0;

  task send_packet;
    integer k;
    begin
      for (k = 0; k < pkt_len; k = k + 1) begin
        i_tdata = pkt[k]; i_tlast = (k == pkt_len - 1); i_tvalid = 1;
        @(posedge clk); while (!i_tready) @(posedge clk);
        #1;
      end
      i_tvalid = 0; i_tlast = 0;
    end
  endtask

  // collect one response packet (with random back-pressure)
  task get_response;
    begin
      rsp_len = 0;
      while (1) begin
        o_tready = $random & 1;
        @(posedge clk);
        if (o_tvalid && o_tready) begin
          rsp[rsp_len] = o_tdata; rsp_len = rsp_len + 1;
          if (o_tlast) begin #1; o_tready = 0; disable get_response; end
        end
        #1;
      end
    end
  endtask

  task expect64(input integer idx, input [63:0] val, input [8*24-1:0] what);
    if (rsp[idx] !== val) begin
      $display("FAIL %0s: word %0d = %016h, expected %016h", what, idx, rsp[idx], val);
      errors = errors + 1;
    end
  endtask

  function [63:0] hdr(input [11:0] seq, input integer words);
    hdr = {4'h0, seq, 16'(words * 8), 32'h5553_4244};
  endfunction
  function [63:0] cmd(input [7:0] op, input [7:0] flags, input [15:0] cnt, input [31:0] a);
    cmd = {op, flags, cnt, a};
  endfunction

  initial begin
    i_tvalid = 0; i_tlast = 0; i_tdata = 0; o_tready = 0;
    repeat (5) @(posedge clk); rst = 0; repeat (2) @(posedge clk); #1;

    // 1) write 5 words at 0x100, read 5 back, read 4 back
    pkt[0] = hdr(12'h001, 8);
    pkt[1] = cmd(8'h01, 0, 5, 32'h100);
    pkt[2] = {32'h1111_1111, 32'h2222_2222};
    pkt[3] = {32'h3333_3333, 32'h4444_4444};
    pkt[4] = {32'h5555_5555, 32'h0};
    pkt[5] = cmd(8'h02, 0, 5, 32'h100);
    pkt[6] = cmd(8'h02, 0, 4, 32'h104);
    pkt[7] = cmd(8'h04, 0, 0, 0);
    pkt_len = 8;
    fork send_packet; get_response; join
    expect64(0, {32'h5553_4252, 20'h0, 12'h001}, "rsp header");
    expect64(1, {8'h81, 8'h00, 16'd5, 32'h100}, "write status");
    expect64(2, {32'h1111_1111, 32'h2222_2222}, "read5 w0");
    expect64(3, {32'h3333_3333, 32'h4444_4444}, "read5 w1");
    expect64(4, {32'h5555_5555, 32'h0}, "read5 w2 (odd)");
    expect64(5, {8'h82, 8'h00, 16'd5, 32'h100}, "read5 status");
    expect64(6, {32'h2222_2222, 32'h3333_3333}, "read4 w0");
    expect64(7, {32'h4444_4444, 32'h5555_5555}, "read4 w1");
    expect64(8, {8'h82, 8'h00, 16'd4, 32'h104}, "read4 status");
    expect64(9, {32'hCAFE_0001, 32'h0000_00A5}, "ctrl_rd data");
    expect64(10, {8'h84, 8'h00, 16'd0, 32'h0}, "ctrl_rd status");
    expect64(11, {32'h454E_4421, 32'h0}, "end word");
    if (rsp_len != 12) begin $display("FAIL: response length %0d, expected 12", rsp_len); errors = errors + 1; end

    // 2) ctrl write, fixed-address write, DECERR read, unknown op
    pkt[0] = hdr(12'h002, 6);
    pkt[1] = cmd(8'h03, 0, 0, 32'h0000_0001);
    pkt[2] = cmd(8'h01, 8'h01, 2, 32'h200);             // two writes to the same address
    pkt[3] = {32'hAAAA_AAAA, 32'hBBBB_BBBB};
    pkt[4] = cmd(8'h02, 0, 1, 32'h8000_0000);
    pkt[5] = cmd(8'h7E, 0, 0, 0);
    pkt_len = 6;
    fork send_packet; get_response; join
    if (ctrl_reg !== 32'h1) begin $display("FAIL: ctrl_reg = %h", ctrl_reg); errors = errors + 1; end
    expect64(1, {8'h83, 8'h00, 16'd0, 32'h1}, "ctrl_wr status");
    expect64(2, {8'h81, 8'h00, 16'd2, 32'h200}, "fixed write status");
    expect64(3, {32'hDEAD_DEAD, 32'h0}, "decerr read data");
    expect64(4, {8'h82, 8'h02, 16'd1, 32'h8000_0000}, "decerr status");
    expect64(5, {8'hFE, 8'h08, 16'd0, 32'h0}, "bad op status");
    expect64(6, {32'h454E_4421, 32'h0A}, "end word with errors");
    if (mem[12'h200 >> 2] !== 32'hBBBB_BBBB) begin $display("FAIL: fixed-address write"); errors = errors + 1; end

    // 3) bad magic: packet is drained, answered with header + end
    pkt[0] = {4'h0, 12'h003, 16'd24, 32'h1234_5678};
    pkt[1] = cmd(8'h01, 0, 2, 32'h300);
    pkt[2] = {32'h9999_9999, 32'h8888_8888};
    pkt_len = 3;
    fork send_packet; get_response; join
    expect64(1, {32'h454E_4421, 32'h08}, "bad magic end");
    if (rsp_len != 2) begin $display("FAIL: bad magic response length %0d", rsp_len); errors = errors + 1; end
    if (mem[12'h300 >> 2] === 32'h9999_9999) begin $display("FAIL: bad packet was executed"); errors = errors + 1; end

    // 4) write that runs past the end of the packet
    pkt[0] = hdr(12'h004, 3);
    pkt[1] = cmd(8'h01, 0, 4, 32'h400);
    pkt[2] = {32'h7777_7777, 32'h6666_6666};
    pkt_len = 3;
    fork send_packet; get_response; join
    expect64(1, {8'h81, 8'h04, 16'd4, 32'h400}, "short write status");

    if (errors == 0) $display("PASS: usb_cmd_proc, all checks");
    else             $display("FAILED: %0d errors", errors);
    $finish;
  end

  initial begin #2_000_000; $display("FAIL: timeout"); $finish; end

endmodule
