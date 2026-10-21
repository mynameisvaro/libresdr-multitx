// Co-simulation harness for tools/test_usbdbg_cosim.py: replays command words from
// cmd.txt ("<tlast> <64-bit hex>" per line) into usb_cmd_proc and logs every
// response word to rsp.txt in the same format. AXI side: 4 KB memory model,
// addresses with bit 31 set answer DECERR.

`timescale 1ns / 1ps

module tb_cosim;

  reg clk = 0, rst = 1;
  always #5 clk = ~clk;

  reg  [63:0] i_tdata = 0; reg i_tlast = 0, i_tvalid = 0; wire i_tready;
  wire [63:0] o_tdata;     wire o_tlast, o_tvalid;        reg  o_tready = 1;

  wire [31:0] awaddr, wdata, araddr;  wire [3:0] wstrb;
  wire awvalid, wvalid, bready, arvalid, rready;
  reg  awready, wready, bvalid, arready, rvalid;
  reg  [1:0] bresp, rresp;  reg [31:0] rdata;

  usb_cmd_proc #(.VERSION(32'h0001_0000)) dut (
    .clk(clk), .rst(rst),
    .i_tdata(i_tdata), .i_tlast(i_tlast), .i_tvalid(i_tvalid), .i_tready(i_tready),
    .o_tdata(o_tdata), .o_tlast(o_tlast), .o_tvalid(o_tvalid), .o_tready(o_tready),
    .m_axi_awaddr(awaddr), .m_axi_awvalid(awvalid), .m_axi_awready(awready),
    .m_axi_wdata(wdata), .m_axi_wstrb(wstrb), .m_axi_wvalid(wvalid), .m_axi_wready(wready),
    .m_axi_bresp(bresp), .m_axi_bvalid(bvalid), .m_axi_bready(bready),
    .m_axi_araddr(araddr), .m_axi_arvalid(arvalid), .m_axi_arready(arready),
    .m_axi_rdata(rdata), .m_axi_rresp(rresp), .m_axi_rvalid(rvalid), .m_axi_rready(rready),
    .ctrl_reg(), .status_in(32'h0)
  );

  reg [31:0] mem [0:1023];
  reg aw_got, w_got; reg [31:0] aw_a, w_d;
  always @(posedge clk) begin
    if (rst) begin
      awready <= 0; wready <= 0; bvalid <= 0; arready <= 0; rvalid <= 0; aw_got <= 0; w_got <= 0;
    end else begin
      awready <= 0; wready <= 0; arready <= 0;
      if (awvalid && !awready && !aw_got) begin awready <= 1; aw_got <= 1; aw_a <= awaddr; end
      if (wvalid  && !wready  && !w_got)  begin wready  <= 1; w_got  <= 1; w_d  <= wdata;  end
      if (aw_got && w_got && !bvalid) begin
        if (aw_a[31]) bresp <= 2'b11; else begin bresp <= 2'b00; mem[aw_a[11:2]] <= w_d; end
        bvalid <= 1; aw_got <= 0; w_got <= 0;
      end
      if (bvalid && bready) bvalid <= 0;
      if (arvalid && !arready && !rvalid) begin
        arready <= 1; rvalid <= 1;
        rdata <= araddr[31] ? 32'hDEAD_DEAD : mem[araddr[11:2]];
        rresp <= araddr[31] ? 2'b11 : 2'b00;
      end
      if (rvalid && rready) rvalid <= 0;
    end
  end

  integer fin, fout, r, last;
  reg [63:0] w;

  always @(posedge clk)
    if (o_tvalid && o_tready) $fdisplay(fout, "%0d %016h", o_tlast, o_tdata);

  initial begin
    fin  = $fopen("cmd.txt", "r");
    fout = $fopen("rsp.txt", "w");
    repeat (5) @(posedge clk); rst = 0; repeat (2) @(posedge clk); #1;
    while (!$feof(fin)) begin
      r = $fscanf(fin, "%d %h\n", last, w);
      if (r == 2) begin
        i_tdata = w; i_tlast = last; i_tvalid = 1;
        @(posedge clk); while (!i_tready) @(posedge clk);
        #1;
      end
    end
    i_tvalid = 0; i_tlast = 0;
    repeat (20000) @(posedge clk);
    $fclose(fout);
    $finish;
  end

endmodule
