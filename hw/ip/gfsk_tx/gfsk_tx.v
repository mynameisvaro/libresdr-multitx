// gfsk_tx: packet GFSK/FSK modulator with a channel NCO, feeding axi_ad9361's TX
// data inputs (select them with DAC channel data_sel = 2).
//
// The CPU writes the packet as bytes (preamble, sync, payload, CRC: the modem
// only shifts bits out) and the modulation parameters as phase increments:
//
//   freq(t)  = CHAN_INC + DEV_INC * shape(t)            [phase units per sample]
//   shape(t) = sum over k = 0..2 of  +-SHAPE[k][pos]    (sign = bit n-1+k)
//   phase   += freq(t);  out = AMPL * exp(j * phase)
//
// pos is the position inside the current symbol (top 8 bits of the baud NCO).
// SHAPE[0] is the tail of the previous symbol's frequency pulse, SHAPE[1] the
// current symbol's own, SHAPE[2] the head of the next one, so the table holds a
// frequency pulse spanning 3 symbols (Gaussian for GFSK, a plain step for FSK).
// The CPU computes it; see fw/src/gfsk.c.
//
// AXI4-Lite register map (byte offsets):
//   0x00 CTRL     W: bit0 = 1 sends the packet; bit1 = CW (unmodulated carrier
//                    at CHAN_INC while set)
//   0x04 STATUS   R: bit0 busy, bit1 done (cleared by the next start),
//                    [31:16] buffer size in bytes
//   0x08 NBITS    RW: bits to send (1 .. 8 * buffer size)
//   0x0C ID       R: "GFSK"
//   0x10 BAUD_INC RW: symbol rate / sample rate * 2^32
//   0x14 DEV_INC  RW: deviation / sample rate * 2^32 (signed)
//   0x18 CHAN_INC RW: channel offset / sample rate * 2^32 (signed)
//   0x1C AMPL     RW: [15:0] amplitude (32767 = full scale), [31:16] ramp step
//                     per sample (0 = no ramp)
//   0x20 FLAGS    RW: bit0 LSB first (default MSB first), bit1 invert bits
//   0x1000 + 4*(256*k + pos)  W: SHAPE[k][pos], signed Q1.15 (k = 0..2)
//   0x2000 + 4*i              W: packet byte i in bits [7:0]
//
// Config registers take effect when a packet starts, or at once when idle (so
// CW can be retuned live). The sample side runs on dac_clk = the AD9361 data
// clock; dac_valid is one sample request.

`timescale 1ns / 1ps
`default_nettype none

module gfsk_tx #(
  parameter BUF_BITS = 8                // packet buffer = 2^BUF_BITS bytes
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

  localparam BUF_BYTES = 1 << BUF_BITS;
  localparam NBITS_W   = BUF_BITS + 4;  // holds 8 * BUF_BYTES

  assign s_axi_bresp = 2'b00;
  assign s_axi_rresp = 2'b00;

  // ================================================================ AXI side
  reg [NBITS_W-1:0] nbits;
  reg [31:0] baud_inc, dev_inc, chan_inc, ampl;
  reg [ 1:0] flags;
  reg        cw;
  reg        start_tgl, cfg_tgl;
  reg        busy, done;
  wire       done_tgl_s;
  reg        done_tgl_seen;

  // shaping table and packet buffer: written here, read on dac_clk
  (* ram_style = "distributed" *) reg signed [15:0] shape0 [0:255];
  (* ram_style = "distributed" *) reg signed [15:0] shape1 [0:255];
  (* ram_style = "distributed" *) reg signed [15:0] shape2 [0:255];
  (* ram_style = "distributed" *) reg        [ 7:0] pkt    [0:BUF_BYTES-1];

  always @(posedge s_axi_aclk) begin
    if (!s_axi_aresetn) begin
      s_axi_awready <= 1'b0;
      s_axi_wready  <= 1'b0;
      s_axi_bvalid  <= 1'b0;
      nbits         <= 0;
      baud_inc      <= 0;
      dev_inc       <= 0;
      chan_inc      <= 0;
      ampl          <= 0;
      flags         <= 0;
      cw            <= 1'b0;
      start_tgl     <= 1'b0;
      cfg_tgl       <= 1'b0;
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
        case (s_axi_awaddr[15:12])
          4'h0: begin
            case (s_axi_awaddr[7:2])
              6'h00: begin
                cw      <= s_axi_wdata[1];
                cfg_tgl <= ~cfg_tgl;
                if (s_axi_wdata[0] && !busy && nbits != 0) begin
                  start_tgl <= ~start_tgl;
                  busy      <= 1'b1;
                  done      <= 1'b0;
                end
              end
              6'h02: nbits <= (s_axi_wdata > 8 * BUF_BYTES) ? 8 * BUF_BYTES : s_axi_wdata[NBITS_W-1:0];
              6'h04: begin baud_inc <= s_axi_wdata; cfg_tgl <= ~cfg_tgl; end
              6'h05: begin dev_inc  <= s_axi_wdata; cfg_tgl <= ~cfg_tgl; end
              6'h06: begin chan_inc <= s_axi_wdata; cfg_tgl <= ~cfg_tgl; end
              6'h07: begin ampl     <= s_axi_wdata; cfg_tgl <= ~cfg_tgl; end
              6'h08: begin flags    <= s_axi_wdata[1:0]; cfg_tgl <= ~cfg_tgl; end
              default: ;
            endcase
          end
          4'h1: case (s_axi_awaddr[11:10])
                  2'd0: shape0[s_axi_awaddr[9:2]] <= s_axi_wdata[15:0];
                  2'd1: shape1[s_axi_awaddr[9:2]] <= s_axi_wdata[15:0];
                  2'd2: shape2[s_axi_awaddr[9:2]] <= s_axi_wdata[15:0];
                  default: ;
                endcase
          4'h2: pkt[s_axi_awaddr[BUF_BITS+1:2]] <= s_axi_wdata[7:0];
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
          14'h01:  s_axi_rdata <= {BUF_BYTES[15:0], 14'd0, done, busy};
          14'h02:  s_axi_rdata <= nbits;
          14'h03:  s_axi_rdata <= 32'h4746_534B;   // "GFSK"
          14'h04:  s_axi_rdata <= baud_inc;
          14'h05:  s_axi_rdata <= dev_inc;
          14'h06:  s_axi_rdata <= chan_inc;
          14'h07:  s_axi_rdata <= ampl;
          14'h08:  s_axi_rdata <= {30'd0, flags};
          default: s_axi_rdata <= 32'd0;
        endcase
      end
    end
  end

  // ================================================================ sample side
  (* ASYNC_REG = "TRUE" *) reg [2:0] start_sync = 3'b000;
  (* ASYNC_REG = "TRUE" *) reg [2:0] cfg_sync   = 3'b000;
  (* ASYNC_REG = "TRUE" *) reg [1:0] cw_sync    = 2'b00;
  wire start_ev = start_sync[2] != start_sync[1];
  wire cfg_ev   = cfg_sync[2]   != cfg_sync[1];
  wire cw_on    = cw_sync[1];

  // configuration, latched from the AXI side (stable there when the toggles arrive)
  reg [31:0]        r_baud = 0, r_chan = 0;
  reg signed [31:0] r_dev  = 0;
  reg [15:0]        r_ampl = 0, r_step = 0;
  reg [ 1:0]        r_flags = 0;
  reg [NBITS_W-1:0] r_nbits = 0;

  localparam S_IDLE = 2'd0, S_DATA = 2'd1, S_TAIL = 2'd2;
  reg [ 1:0] state = S_IDLE;
  reg        done_tgl = 1'b0;
  reg [32:0] baud_acc = 0;              // [32] = symbol boundary this sample
  reg [NBITS_W-1:0] bit_idx = 0;        // next bit to fetch
  reg [NBITS_W:0]   sym_cnt = 0;        // symbol boundaries seen in this packet
  reg        b_prev = 1'b0, b_cur = 1'b0, b_next = 1'b0;
  reg [15:0] ampl_now = 0;
  reg [31:0] phase = 0;

  // next bit from the packet buffer (bit_idx counts bits in transmit order)
  wire [7:0] cur_byte = pkt[bit_idx[NBITS_W-1:3]];
  wire [2:0] bsel     = r_flags[0] ? bit_idx[2:0] : 3'd7 - bit_idx[2:0];
  wire       raw_bit  = cur_byte[bsel] ^ r_flags[1];
  wire       have_bit = bit_idx < r_nbits;

  assign tx_active = (state != S_IDLE) || cw_on;

  // pipeline stage 1: frequency pulse at this position. pos is registered so it
  // lines up with the bit window, which shifts on the boundary sample.
  reg  [7:0] pos = 0;
  reg signed [17:0] shape = 0;
  always @(posedge dac_clk) if (dac_valid) begin
    pos   <= baud_acc[31:24];
    shape <= (b_prev ? shape0[pos] : -shape0[pos])
           + (b_cur  ? shape1[pos] : -shape1[pos])
           + (b_next ? shape2[pos] : -shape2[pos]);
  end

  // stage 2: frequency word, stage 3: phase
  reg signed [49:0] fdev = 0;
  reg               modulate = 1'b0;
  always @(posedge dac_clk) if (dac_valid) begin
    fdev  <= r_dev * shape;             // Q1.15 shape -> >>> 15 below
    phase <= phase + r_chan + (modulate ? fdev[46:15] : 32'd0);
  end

  // stage 4: sin/cos, stage 5: amplitude
  (* rom_style = "block" *) reg [31:0] sincos [0:4095];  // {sin, cos}, Q1.15
  initial $readmemh("gfsk_tx_sincos.mem", sincos);
  reg [31:0] sc = 0;
  reg signed [31:0] i_prod = 0, q_prod = 0;
  reg [15:0] ampl_d1 = 0, ampl_d2 = 0;
  always @(posedge dac_clk) if (dac_valid) begin
    sc      <= sincos[phase[31:20]];
    ampl_d1 <= ampl_now;
    ampl_d2 <= ampl_d1;
    i_prod  <= $signed(sc[15:0])  * $signed({1'b0, ampl_d2[14:0]});
    q_prod  <= $signed(sc[31:16]) * $signed({1'b0, ampl_d2[14:0]});
    dac_data_i <= i_prod[30:15];
    dac_data_q <= q_prod[30:15];
  end

  // control: symbol clock, bit fetch, ramps
  always @(posedge dac_clk) begin
    start_sync <= {start_sync[1:0], start_tgl};
    cfg_sync   <= {cfg_sync[1:0], cfg_tgl};
    cw_sync    <= {cw_sync[0], cw};

    if ((cfg_ev && state == S_IDLE) || start_ev) begin
      r_baud  <= baud_inc;
      r_dev   <= dev_inc;
      r_chan  <= chan_inc;
      r_ampl  <= ampl[15:0];
      r_step  <= ampl[31:16];
      r_flags <= flags;
      r_nbits <= nbits;
    end

    // Bit k enters the window as b_next at boundary k and is sent between
    // boundaries k+1 and k+2 (the first symbol is a lead-in during the ramp).
    if (start_ev) begin
      state    <= S_DATA;
      bit_idx  <= 0;
      sym_cnt  <= 0;
      baud_acc <= 33'h1_0000_0000;      // boundary on the first sample: loads bit 0
      b_prev   <= 1'b0;
      b_cur    <= 1'b0;
      b_next   <= 1'b0;
      modulate <= 1'b1;
    end else if (dac_valid) begin
      baud_acc <= {1'b0, baud_acc[31:0]} + r_baud;

      if (baud_acc[32] && state != S_IDLE) begin
        // shift the 3-bit window; after the last bit, keep repeating it
        b_prev  <= b_cur;
        b_cur   <= b_next;
        sym_cnt <= sym_cnt + 1'b1;
        if (have_bit) begin
          b_next  <= raw_bit;
          bit_idx <= bit_idx + 1'b1;
        end
      end

      case (state)
        S_DATA: begin
          if (r_step == 0 || ampl_now + r_step >= r_ampl) ampl_now <= r_ampl;
          else                                            ampl_now <= ampl_now + r_step;
          if (baud_acc[32] && sym_cnt == r_nbits + 1)  // last bit's symbol just ended
            state <= S_TAIL;
        end
        S_TAIL: begin                   // ramp down on the last bit's frequency
          if (r_step == 0 || ampl_now <= r_step) begin
            ampl_now <= 0;
            state    <= S_IDLE;
            modulate <= 1'b0;
            done_tgl <= ~done_tgl;
          end else
            ampl_now <= ampl_now - r_step;
        end
        default: begin                  // idle: silent, or CW at the channel offset
          ampl_now <= cw_on ? r_ampl : 16'd0;
          modulate <= 1'b0;
        end
      endcase
    end
  end

  (* ASYNC_REG = "TRUE" *) reg [1:0] done_sync = 2'b00;
  always @(posedge s_axi_aclk) done_sync <= {done_sync[0], done_tgl};
  assign done_tgl_s = done_sync[1];

endmodule

`default_nettype wire
