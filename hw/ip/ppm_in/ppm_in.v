// ppm_in: PPM (pulse position) input from a radio's trainer port, decoded the
// way Multiprotocol decodes it (Multiprotocol.ino, PPM input capture).
//
// The time between two active edges is one interval, counted in 0.5 us units:
//   < 800 us   : glitch, the frame is spoiled until the next sync
//   > 2200 us  : sync (start of a frame); the frame before it is kept if it had
//                at least 4 channels
//   otherwise  : the next channel (up to 16)
// The last complete frame stays in CH[i] until the next one replaces it.
//
// A built-in generator sends a test frame instead of the pin (CTRL bit 2), so the
// whole input path can be checked without a radio: a 300 us pulse at the start
// of each channel slot, then the sync gap to the frame length.
//
// AXI4-Lite register map (byte offsets), all on s_axi_aclk (100 MHz):
//   0x00 CTRL   RW: bit0 enable, bit1 falling edge (default rising), bit2 test source
//   0x04 STATUS R: [15:0] frames received, [20:16] channels in CH[], bit24 pin level
//   0x0C ID     R: "PPMI"
//   0x10 GEN_N  RW: test generator: channels (1..16)
//   0x14 GEN_FRAME RW: test generator: frame length, 0.5 us units
//   0x40 + 4*i  R: CH[i], interval in 0.5 us units (i = 0..15)
//   0x80 + 4*i  W: test generator channel i, 0.5 us units

`timescale 1ns / 1ps
`default_nettype none

module ppm_in #(
  parameter CLK_HZ = 100000000
)(
  (* X_INTERFACE_IGNORE = "true" *) input wire ppm_pin,

  (* X_INTERFACE_INFO = "xilinx.com:signal:clock:1.0 s_axi_aclk CLK" *)
  (* X_INTERFACE_PARAMETER = "ASSOCIATED_BUSIF s_axi, ASSOCIATED_RESET s_axi_aresetn" *)
  input  wire        s_axi_aclk,
  input  wire        s_axi_aresetn,
  input  wire [15:0] s_axi_awaddr,
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
  input  wire [15:0] s_axi_araddr,
  input  wire [ 2:0] s_axi_arprot,
  input  wire        s_axi_arvalid,
  output reg         s_axi_arready,
  output reg  [31:0] s_axi_rdata,
  output wire [ 1:0] s_axi_rresp,
  output reg         s_axi_rvalid,
  input  wire        s_axi_rready
);

  localparam DIV       = CLK_HZ / 2000000;   // clocks per 0.5 us tick
  localparam T_GLITCH  = 16'd1600;           // 800 us
  localparam T_SYNC    = 16'd4400;           // 2200 us
  localparam MIN_CH    = 4;
  localparam MAX_CH    = 16;
  localparam T_PULSE   = 16'd600;            // generator: 300 us pulse

  assign s_axi_bresp = 2'b00;
  assign s_axi_rresp = 2'b00;

  wire clk = s_axi_aclk;

  // ---------------------------------------------------------------- registers
  reg [2:0]  ctrl;
  reg [4:0]  gen_n;
  reg [15:0] gen_frame;
  reg [15:0] gen_ch [0:MAX_CH-1];
  reg [15:0] ch [0:MAX_CH-1];                // last complete frame
  reg [4:0]  nch;
  reg [15:0] frames;
  reg        level;

  // ---------------------------------------------------------------- 0.5 us tick
  reg [7:0] div = 0;
  wire tick = div == DIV - 1;
  always @(posedge clk) div <= tick ? 8'd0 : div + 1'b1;

  // ---------------------------------------------------------------- test generator
  reg [15:0] g_t = 0;                        // time in the current slot
  reg [4:0]  g_i = 0;                        // slot (channel, or gen_n = sync gap)
  reg [15:0] g_used = 0;                     // time used by the channels so far
  reg        gen = 1'b0;
  wire [15:0] g_len = (g_i < gen_n) ? gen_ch[g_i[3:0]]
                    : (gen_frame > g_used ? gen_frame - g_used : T_SYNC + 16'd1);
  always @(posedge clk) if (tick) begin
    gen <= g_t < T_PULSE;
    if (g_t + 1'b1 >= g_len) begin
      g_t <= 0;
      if (g_i >= gen_n) begin
        g_i    <= 0;
        g_used <= 0;
      end else begin
        g_i    <= g_i + 1'b1;
        g_used <= g_used + g_len;
      end
    end else
      g_t <= g_t + 1'b1;
  end

  // ---------------------------------------------------------------- decoder
  (* ASYNC_REG = "TRUE" *) reg [1:0] pin_s = 2'b00;
  reg [3:0]  hist = 0;                       // last samples, for a 4-tick glitch filter
  reg        in_f = 1'b0, in_d = 1'b0;
  reg [15:0] t = 0;                          // since the last active edge, saturating
  reg [4:0]  idx = 0;
  reg        bad = 1'b1;                     // wait for a sync first
  reg [15:0] cur [0:MAX_CH-1];               // the frame being received
  integer k;

  wire src  = ctrl[2] ? gen : pin_s[1];
  wire edge_act = ctrl[1] ? (in_d & ~in_f) : (~in_d & in_f);

  always @(posedge clk) begin
    pin_s <= {pin_s[0], ppm_pin};
    level <= pin_s[1];
    if (tick) begin
      hist <= {hist[2:0], src};
      if (hist == 4'hF) in_f <= 1'b1;
      if (hist == 4'h0) in_f <= 1'b0;
      in_d <= in_f;
      if (t != 16'hFFFF) t <= t + 1'b1;

      if (ctrl[0] && edge_act) begin
        t <= 16'd1;
        if (t < T_GLITCH)
          bad <= 1'b1;
        else if (t > T_SYNC) begin           // sync: publish the frame before it
          if (!bad && idx >= MIN_CH) begin
            for (k = 0; k < MAX_CH; k = k + 1) ch[k] <= cur[k];
            nch    <= idx;
            frames <= frames + 1'b1;
          end
          idx <= 0;
          bad <= 1'b0;
        end else if (!bad) begin
          if (idx == MAX_CH)
            bad <= 1'b1;                     // more than 16: drop the frame
          else begin
            cur[idx[3:0]] <= t;
            idx <= idx + 1'b1;
          end
        end
      end
    end
    if (!s_axi_aresetn) begin
      frames <= 0;
      nch    <= 0;
      idx    <= 0;
      bad    <= 1'b1;
    end
  end

  // ---------------------------------------------------------------- AXI
  always @(posedge clk) begin
    if (!s_axi_aresetn) begin
      s_axi_awready <= 1'b0;
      s_axi_wready  <= 1'b0;
      s_axi_bvalid  <= 1'b0;
      ctrl          <= 3'd0;
      gen_n         <= 5'd8;
      gen_frame     <= 16'd45000;            // 22.5 ms
      for (k = 0; k < MAX_CH; k = k + 1) gen_ch[k] <= 16'd3000;
    end else begin
      s_axi_awready <= 1'b0;
      s_axi_wready  <= 1'b0;
      if (s_axi_bvalid && s_axi_bready) s_axi_bvalid <= 1'b0;
      if (s_axi_awvalid && s_axi_wvalid && !s_axi_awready && !s_axi_bvalid) begin
        s_axi_awready <= 1'b1;
        s_axi_wready  <= 1'b1;
        s_axi_bvalid  <= 1'b1;
        case (s_axi_awaddr[7:2])
          6'h00: ctrl      <= s_axi_wdata[2:0];
          6'h04: gen_n     <= (s_axi_wdata == 0) ? 5'd1 : (s_axi_wdata > MAX_CH) ? MAX_CH[4:0] : s_axi_wdata[4:0];
          6'h05: gen_frame <= s_axi_wdata[15:0];
          default: if (s_axi_awaddr[7:6] == 2'b10) gen_ch[s_axi_awaddr[5:2]] <= s_axi_wdata[15:0];
        endcase
      end
    end
  end

  always @(posedge clk) begin
    if (!s_axi_aresetn) begin
      s_axi_arready <= 1'b0;
      s_axi_rvalid  <= 1'b0;
    end else begin
      s_axi_arready <= 1'b0;
      if (s_axi_rvalid && s_axi_rready) s_axi_rvalid <= 1'b0;
      if (s_axi_arvalid && !s_axi_arready && !s_axi_rvalid) begin
        s_axi_arready <= 1'b1;
        s_axi_rvalid  <= 1'b1;
        case (s_axi_araddr[7:2])
          6'h00: s_axi_rdata <= {29'd0, ctrl};
          6'h01: s_axi_rdata <= {7'd0, level, 3'd0, nch, frames};
          6'h03: s_axi_rdata <= 32'h5050_4D49;   // "PPMI"
          6'h04: s_axi_rdata <= gen_n;
          6'h05: s_axi_rdata <= gen_frame;
          default: s_axi_rdata <= (s_axi_araddr[7:6] == 2'b01) ? {16'd0, ch[s_axi_araddr[5:2]]} : 32'd0;
        endcase
      end
    end
  end

endmodule

`default_nettype wire
