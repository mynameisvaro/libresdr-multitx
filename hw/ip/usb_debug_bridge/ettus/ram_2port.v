// libresdr_multitx: simplified replacement for Ettus' ram_2port (same ports and
// parameters). The original builds five variants by `including ram_2port_impl.vh
// with different macros, which Vivado's block-design module-reference parser
// cannot handle. All users in this design (axi_fifo_legacy, axi_fifo_bram,
// axi_packet_gate) write through port A and read through port B, so a plain
// read-first dual-port RAM is equivalent. RW_MODE and INIT_FILE are ignored;
// RAM_TYPE "LUTRAM"/"REG"/"BRAM" select the inference style.

`timescale 1ns / 1ps
`default_nettype none

module ram_2port #(
  parameter DWIDTH    = 32,
  parameter AWIDTH    = 9,
  parameter RW_MODE   = "READ-FIRST",
  parameter RAM_TYPE  = "AUTOMATIC",
  parameter OUT_REG   = 0,
  parameter INIT_FILE = ""
) (
  input  wire              clka,
  input  wire              ena,
  input  wire              wea,
  input  wire [AWIDTH-1:0] addra,
  input  wire [DWIDTH-1:0] dia,
  output wire [DWIDTH-1:0] doa,
  input  wire              clkb,
  input  wire              enb,
  input  wire              web,
  input  wire [AWIDTH-1:0] addrb,
  input  wire [DWIDTH-1:0] dib,
  output wire [DWIDTH-1:0] dob
);

  generate
    if (RAM_TYPE == "LUTRAM" || RAM_TYPE == "REG") begin : lut
      ram_2port_core #(.DWIDTH(DWIDTH), .AWIDTH(AWIDTH), .OUT_REG(OUT_REG), .STYLE(1)) core (
        .clka(clka), .ena(ena), .wea(wea), .addra(addra), .dia(dia), .doa(doa),
        .clkb(clkb), .enb(enb), .web(web), .addrb(addrb), .dib(dib), .dob(dob));
    end else if (RAM_TYPE == "BRAM") begin : bram
      ram_2port_core #(.DWIDTH(DWIDTH), .AWIDTH(AWIDTH), .OUT_REG(OUT_REG), .STYLE(2)) core (
        .clka(clka), .ena(ena), .wea(wea), .addra(addra), .dia(dia), .doa(doa),
        .clkb(clkb), .enb(enb), .web(web), .addrb(addrb), .dib(dib), .dob(dob));
    end else begin : auto
      ram_2port_core #(.DWIDTH(DWIDTH), .AWIDTH(AWIDTH), .OUT_REG(OUT_REG), .STYLE(0)) core (
        .clka(clka), .ena(ena), .wea(wea), .addra(addra), .dia(dia), .doa(doa),
        .clkb(clkb), .enb(enb), .web(web), .addrb(addrb), .dib(dib), .dob(dob));
    end
  endgenerate

endmodule


module ram_2port_core #(
  parameter DWIDTH  = 32,
  parameter AWIDTH  = 9,
  parameter OUT_REG = 0,
  parameter STYLE   = 0     // 0: automatic, 1: distributed, 2: block
) (
  input  wire              clka,
  input  wire              ena,
  input  wire              wea,
  input  wire [AWIDTH-1:0] addra,
  input  wire [DWIDTH-1:0] dia,
  output wire [DWIDTH-1:0] doa,
  input  wire              clkb,
  input  wire              enb,
  input  wire              web,
  input  wire [AWIDTH-1:0] addrb,
  input  wire [DWIDTH-1:0] dib,
  output wire [DWIDTH-1:0] dob
);

  localparam DEPTH = 1 << AWIDTH;

  reg [DWIDTH-1:0] doa_r = {DWIDTH{1'b0}}, dob_r = {DWIDTH{1'b0}};

  // same read-first logic three times, only the ram_style attribute differs
  generate
    if (STYLE == 1) begin : dist
      (* ram_style = "distributed" *) reg [DWIDTH-1:0] ram [0:DEPTH-1];
      integer i;
      initial for (i = 0; i < DEPTH; i = i + 1) ram[i] = {DWIDTH{1'b0}};
      always @(posedge clka) if (ena) begin if (wea) ram[addra] <= dia; doa_r <= ram[addra]; end
      always @(posedge clkb) if (enb) begin if (web) ram[addrb] <= dib; dob_r <= ram[addrb]; end
    end else if (STYLE == 2) begin : blk
      (* ram_style = "block" *) reg [DWIDTH-1:0] ram [0:DEPTH-1];
      integer i;
      initial for (i = 0; i < DEPTH; i = i + 1) ram[i] = {DWIDTH{1'b0}};
      always @(posedge clka) if (ena) begin if (wea) ram[addra] <= dia; doa_r <= ram[addra]; end
      always @(posedge clkb) if (enb) begin if (web) ram[addrb] <= dib; dob_r <= ram[addrb]; end
    end else begin : aut
      reg [DWIDTH-1:0] ram [0:DEPTH-1];
      integer i;
      initial for (i = 0; i < DEPTH; i = i + 1) ram[i] = {DWIDTH{1'b0}};
      always @(posedge clka) if (ena) begin if (wea) ram[addra] <= dia; doa_r <= ram[addra]; end
      always @(posedge clkb) if (enb) begin if (web) ram[addrb] <= dib; dob_r <= ram[addrb]; end
    end
  endgenerate

  generate
    if (OUT_REG == 1) begin : oreg
      reg [DWIDTH-1:0] doa_rr = {DWIDTH{1'b0}}, dob_rr = {DWIDTH{1'b0}};
      always @(posedge clka) if (ena) doa_rr <= doa_r;
      always @(posedge clkb) if (enb) dob_rr <= dob_r;
      assign doa = doa_rr;
      assign dob = dob_rr;
    end else begin : noreg
      assign doa = doa_r;
      assign dob = dob_r;
    end
  endgenerate

endmodule

`default_nettype wire
