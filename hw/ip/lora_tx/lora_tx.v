// lora_tx: LoRa (chirp spread spectrum) modulator with a channel NCO, feeding
// axi_ad9361's TX data inputs through tx_select.
//
// The CPU writes the whole frame as a list of chirps (preamble, sync word,
// down-chirps, data symbols: the LoRa coding is done in firmware, fw/src/lora.c)
// and the chip rate as a phase increment. A chirp sweeps the bandwidth BW once
// per symbol of N = 2^SF chips; an up-chirp for symbol value s starts at
// -BW/2 + s * BW/N and wraps from +BW/2 to -BW/2:
//
//   up:    freq(t) = BW * (((c + s) mod N + frac) / N) - BW/2
//   down:  freq(t) = BW/2 - BW * ((c + frac) / N)
//   phase += CHAN_INC + freq;  out = AMPL * exp(j * phase)
//
// c is the chip count within the symbol and frac the position inside the chip
// (the chip NCO), so the frequency follows the chip clock exactly and nothing
// drifts. Since the chip rate equals BW, one increment sets both.
//
// AXI4-Lite register map (byte offsets):
//   0x00 CTRL     W: bit0 = 1 sends the frame
//   0x04 STATUS   R: bit0 busy, bit1 done (cleared by the next start),
//                    [31:16] symbol memory entries
//   0x08 NSYM     RW: entries to send (1 .. memory size)
//   0x0C ID       R: "LORA"
//   0x10 CHIP_INC RW: BW / sample rate * 2^32 (BW < sample rate / 2)
//   0x14 SF       RW: spreading factor 5..12
//   0x18 CHAN_INC RW: channel offset / sample rate * 2^32 (signed)
//   0x1C AMPL     RW: [15:0] amplitude (32767 = full scale), [31:16] ramp step
//                     per sample (0 = no ramp)
//   0x20 FLAGS    RW: bit0 invert IQ (chirps run the other way)
//   0x1000 + 4*i  W: entry i: [11:0] start value s, [13:12] type
//                    (0 up-chirp, 1 down-chirp, 2 quarter down-chirp: N/4 chips)
//
// Config registers are latched when a frame starts. The sample side runs on
// dac_clk = the AD9361 data clock; dac_valid is one sample request.

`timescale 1ns / 1ps
`default_nettype none

module lora_tx #(
  parameter SYM_BITS = 10               // symbol memory = 2^SYM_BITS entries
)(
  (* X_INTERFACE_INFO = "xilinx.com:signal:clock:1.0 dac_clk CLK" *)
  input  wire        dac_clk,
  (* X_INTERFACE_IGNORE = "true" *) input  wire        dac_valid,
  (* X_INTERFACE_IGNORE = "true" *) output reg  [15:0] dac_data_i,
  (* X_INTERFACE_IGNORE = "true" *) output reg  [15:0] dac_data_q,
  (* X_INTERFACE_IGNORE = "true" *) output wire        tx_active,

  (* X_INTERFACE_INFO = "xilinx.com:signal:interrupt:1.0 irq INTERRUPT" *)
  (* X_INTERFACE_PARAMETER = "SENSITIVITY LEVEL_HIGH" *)
  output wire        irq,

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

  localparam SYMS = 1 << SYM_BITS;

  assign s_axi_bresp = 2'b00;
  assign s_axi_rresp = 2'b00;

  // ================================================================ AXI side
  reg [SYM_BITS:0] nsym;
  reg [31:0] chip_inc, chan_inc, ampl;
  reg [ 3:0] sf;
  reg        flags;
  reg        start_tgl;
  reg        busy, done;
  wire       done_tgl_s;
  reg        done_tgl_seen;

  // frame: written here, read on dac_clk
  (* ram_style = "distributed" *) reg [13:0] syms [0:SYMS-1];

  always @(posedge s_axi_aclk) begin
    if (!s_axi_aresetn) begin
      s_axi_awready <= 1'b0;
      s_axi_wready  <= 1'b0;
      s_axi_bvalid  <= 1'b0;
      nsym          <= 0;
      chip_inc      <= 0;
      chan_inc      <= 0;
      ampl          <= 0;
      sf            <= 4'd7;
      flags         <= 1'b0;
      start_tgl     <= 1'b0;
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
        if (s_axi_awaddr[15:12] == 4'h1)
          syms[s_axi_awaddr[SYM_BITS+1:2]] <= s_axi_wdata[13:0];
        else if (s_axi_awaddr[15:12] == 4'h0)
          case (s_axi_awaddr[7:2])
            6'h00: if (s_axi_wdata[0] && !busy && nsym != 0) begin
                     start_tgl <= ~start_tgl;
                     busy      <= 1'b1;
                     done      <= 1'b0;
                   end
            6'h02: nsym     <= (s_axi_wdata > SYMS) ? SYMS : s_axi_wdata[SYM_BITS:0];
            6'h04: chip_inc <= s_axi_wdata;
            6'h05: sf       <= (s_axi_wdata < 5) ? 4'd5 : (s_axi_wdata > 12) ? 4'd12 : s_axi_wdata[3:0];
            6'h06: chan_inc <= s_axi_wdata;
            6'h07: ampl     <= s_axi_wdata;
            6'h08: flags    <= s_axi_wdata[0];
            default: ;
          endcase
      end

      if (done_tgl_s != done_tgl_seen) begin
        done_tgl_seen <= done_tgl_s;
        busy          <= 1'b0;
        done          <= 1'b1;
      end
    end
  end

  assign irq = done;

  always @(posedge s_axi_aclk) begin
    if (!s_axi_aresetn) begin
      s_axi_arready <= 1'b0;
      s_axi_rvalid  <= 1'b0;
    end else begin
      s_axi_arready <= 1'b0;
      if (s_axi_rvalid && s_axi_rready) s_axi_rvalid <= 1'b0;
      if (s_axi_arvalid && !s_axi_arready && !s_axi_rvalid) begin
        s_axi_arready <= 1'b1;
        s_axi_rvalid  <= 1'b1;
        case (s_axi_araddr[15:2])
          14'h01:  s_axi_rdata <= {SYMS[15:0], 14'd0, done, busy};
          14'h02:  s_axi_rdata <= nsym;
          14'h03:  s_axi_rdata <= 32'h4C4F_5241;   // "LORA"
          14'h04:  s_axi_rdata <= chip_inc;
          14'h05:  s_axi_rdata <= sf;
          14'h06:  s_axi_rdata <= chan_inc;
          14'h07:  s_axi_rdata <= ampl;
          14'h08:  s_axi_rdata <= {31'd0, flags};
          default: s_axi_rdata <= 32'd0;
        endcase
      end
    end
  end

  // ================================================================ sample side
  (* ASYNC_REG = "TRUE" *) reg [2:0] start_sync = 3'b000;
  wire start_ev = start_sync[2] != start_sync[1];

  // configuration, latched from the AXI side (stable there when the toggle arrives)
  reg [31:0]       r_chip = 0, r_chan = 0;
  reg [15:0]       r_ampl = 0, r_step = 0;
  reg [ 3:0]       r_sf = 7;
  reg              r_inv = 0;
  reg [SYM_BITS:0] r_nsym = 0;

  localparam S_IDLE = 2'd0, S_RUN = 2'd1, S_TAIL = 2'd2;
  localparam T_UP = 2'd0, T_DOWN = 2'd1, T_QDOWN = 2'd2;
  reg [ 1:0] state = S_IDLE;
  reg        done_tgl = 1'b0;
  reg [32:0] chip_acc = 0;              // [32] = chip boundary this sample
  reg [11:0] chip_cnt = 0;              // chip within the symbol
  reg [SYM_BITS:0] idx = 0;             // current entry
  reg [11:0] value = 0;                 // its start value
  reg [ 1:0] stype = T_UP;
  reg [15:0] ampl_now = 0;
  reg [31:0] phase = 0;

  // one read port (LUT RAM): entry 0 when a frame starts, else the next entry
  wire [SYM_BITS-1:0] rd_addr = start_ev ? {SYM_BITS{1'b0}} : idx[SYM_BITS-1:0] + 1'b1;
  wire [13:0] entry   = syms[rd_addr];
  wire [11:0] n_mask  = (12'd1 << r_sf) - 1'b1;
  wire [11:0] len_m1  = (stype == T_QDOWN) ? (n_mask >> 2) : n_mask;
  wire        sym_end = chip_acc[32] && chip_cnt == len_m1;

  assign tx_active = state != S_IDLE;

  // stage 1: position in the sweep as a 28-bit fraction of N (12 chip bits + 16
  // bits inside the chip, shifted so the scale doesn't depend on SF)
  wire [11:0] pos_up = (chip_cnt + value) & n_mask;
  reg  [27:0] frac = 0;
  reg         down = 1'b0;
  always @(posedge dac_clk) if (dac_valid) begin
    frac <= {(stype == T_UP ? pos_up : chip_cnt), chip_acc[31:16]} << (4'd12 - r_sf);
    down <= stype != T_UP;
  end

  // stage 2-3: BW * frac (DSP, pipelined), stage 4: frequency, stage 5: phase
  reg [59:0] prod = 0, prod_d = 0;
  reg        down_d1 = 0, down_d2 = 0;
  reg [31:0] freq = 0;
  always @(posedge dac_clk) if (dac_valid) begin
    prod    <= r_chip * frac;
    prod_d  <= prod;
    down_d1 <= down;
    down_d2 <= down_d1;
    // up: BW*frac - BW/2, down: BW/2 - BW*frac; inverted IQ: the other way
    freq    <= (down_d2 ^ r_inv) ? (r_chip >> 1) - prod_d[59:28] : prod_d[59:28] - (r_chip >> 1);
    phase   <= phase + r_chan + freq;
  end

  // stage 6: sin/cos, stage 7: amplitude
  (* rom_style = "block" *) reg [31:0] sincos [0:4095];  // {sin, cos}, Q1.15
  initial $readmemh("gfsk_tx_sincos.mem", sincos);
  reg [31:0] sc = 0;
  reg signed [31:0] i_prod = 0, q_prod = 0;
  reg [15:0] ampl_d [0:4];
  integer k;
  initial for (k = 0; k < 5; k = k + 1) ampl_d[k] = 0;
  always @(posedge dac_clk) if (dac_valid) begin
    sc       <= sincos[phase[31:20]];
    ampl_d[0] <= ampl_now;
    for (k = 1; k < 5; k = k + 1) ampl_d[k] <= ampl_d[k-1];
    i_prod   <= $signed(sc[15:0])  * $signed({1'b0, ampl_d[4][14:0]});
    q_prod   <= $signed(sc[31:16]) * $signed({1'b0, ampl_d[4][14:0]});
    dac_data_i <= i_prod[30:15];
    dac_data_q <= q_prod[30:15];
  end

  // control: chip clock, symbol sequence, ramps
  always @(posedge dac_clk) begin
    start_sync <= {start_sync[1:0], start_tgl};

    if (start_ev) begin
      r_chip  <= chip_inc;
      r_chan  <= chan_inc;
      r_ampl  <= ampl[15:0];
      r_step  <= ampl[31:16];
      r_sf    <= sf;
      r_inv   <= flags;
      r_nsym  <= nsym;
      state    <= S_RUN;
      idx      <= 0;
      value    <= entry[11:0];
      stype    <= entry[13:12];
      chip_acc <= 0;
      chip_cnt <= 0;
    end else if (dac_valid && state != S_IDLE) begin
      chip_acc <= {1'b0, chip_acc[31:0]} + r_chip;
      if (chip_acc[32])
        chip_cnt <= sym_end ? 12'd0 : chip_cnt + 1'b1;
      if (sym_end) begin
        if (state == S_RUN && idx + 1'b1 < r_nsym) begin
          idx   <= idx + 1'b1;
          value <= entry[11:0];
          stype <= entry[13:12];
        end else begin                  // ramp down on a plain up-chirp
          state <= S_TAIL;
          value <= 0;
          stype <= T_UP;
        end
      end

      case (state)
        S_RUN: begin
          if (r_step == 0 || ampl_now + r_step >= r_ampl) ampl_now <= r_ampl;
          else                                            ampl_now <= ampl_now + r_step;
        end
        default: begin                  // S_TAIL
          if (r_step == 0 || ampl_now <= r_step) begin
            ampl_now <= 0;
            state    <= S_IDLE;
            done_tgl <= ~done_tgl;
          end else
            ampl_now <= ampl_now - r_step;
        end
      endcase
    end
  end

  (* ASYNC_REG = "TRUE" *) reg [1:0] done_sync = 2'b00;
  always @(posedge s_axi_aclk) done_sync <= {done_sync[0], done_tgl};
  assign done_tgl_s = done_sync[1];

endmodule

`default_nettype wire
