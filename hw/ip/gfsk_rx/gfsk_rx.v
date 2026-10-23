// gfsk_rx: packet GFSK/FSK receiver on the AD9361's RX samples. The counterpart
// of gfsk_tx: it finds a sync word and hands the bits after it to the CPU,
// which decodes them like the chip it emulates (CRC, FEC, whitening).
//
//   samples -> mixer (channel NCO) -> CIC decimator (order 3, /R)
//           -> CORDIC (phase, magnitude) -> frequency = phase step
//           -> minus a running offset estimate (frozen at the sync word)
//           -> bit clock DPLL (nudged at zero crossings), integrate-and-dump
//           -> 32-bit sync correlator (masked, up to MAXERR bit errors)
//           -> NBITS bits into the buffer, MSB first
//
// AXI4-Lite register map (byte offsets):
//   0x00 CTRL     W: bit0 = 1 arms (search for the sync word, then capture),
//                    bit1 = 1 stops the search
//   0x04 STATUS   R: bit0 searching, bit1 done (a packet is in the buffer;
//                    cleared by the next arm), [31:16] buffer size in bytes
//   0x08 NBITS    RW: bits to capture after the sync word (1 .. 8 * buffer)
//   0x0C ID       R: "GFRX"
//   0x10 CHAN_INC RW: channel offset / sample rate * 2^32 (signed): the mixer
//                     moves this frequency to 0
//   0x14 DECIM    RW: [8:0] CIC decimation R (1..256), [20:16] output shift
//                     (the CIC gain is R^3: shift by about 3*log2(R) - 2)
//   0x18 BAUD_INC RW: bit rate / (sample rate / R) * 2^32
//   0x1C SYNC     RW: sync pattern, the last bit received in bit 0
//   0x20 MASK     RW: which SYNC bits count
//   0x24 CFG      RW: [5:0] max bit errors in the sync word, bit8 invert bits,
//                     [19:16] offset tracker time constant (log2 samples),
//                     [27:24] DPLL gain (log2: phase correction = error >> G)
//   0x28 TSTAMP   R: input sample counter when the sync word matched
//   0x2C FOFS     R: [15:0] frequency offset estimate at the sync word (signed,
//                    phase units (2^16 = one turn) per decimated sample)
//   0x30 LEVEL    R: [17:0] average magnitude at the sync word (CORDIC units)
//   0x34 SYNCERR  R: [5:0] bit errors in the matched sync word
//   0x1000 + 4*i  R: buffer byte i in bits [7:0]
//
// Configuration is latched on arm. The sample side runs on adc_clk (the AD9361
// data clock) with adc_valid marking samples; results are written there and
// read on s_axi_aclk once done is seen (they don't change until the next arm).

`timescale 1ns / 1ps
`default_nettype none

module gfsk_rx #(
  parameter BUF_BITS = 7                // buffer = 2^BUF_BITS bytes
)(
  (* X_INTERFACE_INFO = "xilinx.com:signal:clock:1.0 adc_clk CLK" *)
  input  wire        adc_clk,
  (* X_INTERFACE_IGNORE = "true" *) input  wire        adc_valid,
  (* X_INTERFACE_IGNORE = "true" *) input  wire [15:0] adc_data_i,
  (* X_INTERFACE_IGNORE = "true" *) input  wire [15:0] adc_data_q,

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
  reg [31:0] chan_inc, decim, baud_inc, sync, mask, cfg;
  reg        arm_tgl, stop_tgl;
  reg        busy, done;
  wire       done_tgl_s;
  reg        done_tgl_seen;

  // results, written on adc_clk while busy, stable once done
  (* ram_style = "distributed" *) reg [7:0] buf_mem [0:BUF_BYTES-1];
  reg [31:0] res_tstamp = 0;
  reg [15:0] res_fofs   = 0;
  reg [17:0] res_level  = 0;
  reg [ 5:0] res_err    = 0;

  always @(posedge s_axi_aclk) begin
    if (!s_axi_aresetn) begin
      s_axi_awready <= 1'b0;
      s_axi_wready  <= 1'b0;
      s_axi_bvalid  <= 1'b0;
      nbits         <= 8;
      chan_inc      <= 0;
      decim         <= 32'h000A_0010;   // R 16, shift 10
      baud_inc      <= 0;
      sync          <= 0;
      mask          <= 32'hFFFF_FFFF;
      cfg           <= 32'h0204_0002;   // 2 errors, tracker 2^4, DPLL gain 2^-2
      arm_tgl       <= 1'b0;
      stop_tgl      <= 1'b0;
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
        if (s_axi_awaddr[15:12] == 4'h0) begin
          case (s_axi_awaddr[7:2])
            6'h00: begin
              if (s_axi_wdata[0] && nbits != 0) begin
                arm_tgl <= ~arm_tgl;
                busy    <= 1'b1;
                done    <= 1'b0;
              end else if (s_axi_wdata[1]) begin
                stop_tgl <= ~stop_tgl;
                busy     <= 1'b0;
              end
            end
            6'h02: nbits    <= (s_axi_wdata > 8 * BUF_BYTES) ? 8 * BUF_BYTES : s_axi_wdata[NBITS_W-1:0];
            6'h04: chan_inc <= s_axi_wdata;
            6'h05: decim    <= s_axi_wdata;
            6'h06: baud_inc <= s_axi_wdata;
            6'h07: sync     <= s_axi_wdata;
            6'h08: mask     <= s_axi_wdata;
            6'h09: cfg      <= s_axi_wdata;
            default: ;
          endcase
        end
      end

      if (done_tgl_s != done_tgl_seen) begin
        done_tgl_seen <= done_tgl_s;
        if (busy) begin                 // (a stop may have raced the packet)
          busy <= 1'b0;
          done <= 1'b1;
        end
      end
    end
  end

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
        if (s_axi_araddr[15:12] == 4'h1)
          s_axi_rdata <= {24'd0, buf_mem[s_axi_araddr[BUF_BITS+1:2]]};
        else case (s_axi_araddr[7:2])
          6'h01:   s_axi_rdata <= {BUF_BYTES[15:0], 14'd0, done, busy};
          6'h02:   s_axi_rdata <= nbits;
          6'h03:   s_axi_rdata <= 32'h4746_5258;   // "GFRX"
          6'h04:   s_axi_rdata <= chan_inc;
          6'h05:   s_axi_rdata <= decim;
          6'h06:   s_axi_rdata <= baud_inc;
          6'h07:   s_axi_rdata <= sync;
          6'h08:   s_axi_rdata <= mask;
          6'h09:   s_axi_rdata <= cfg;
          6'h0A:   s_axi_rdata <= res_tstamp;
          6'h0B:   s_axi_rdata <= {16'd0, res_fofs};
          6'h0C:   s_axi_rdata <= {14'd0, res_level};
          6'h0D:   s_axi_rdata <= {26'd0, res_err};
          default: s_axi_rdata <= 32'd0;
        endcase
      end
    end
  end

  // ================================================================ sample side
  (* ASYNC_REG = "TRUE" *) reg [2:0] arm_sync  = 3'b000;
  (* ASYNC_REG = "TRUE" *) reg [2:0] stop_sync = 3'b000;
  wire arm_ev  = arm_sync[2]  != arm_sync[1];
  wire stop_ev = stop_sync[2] != stop_sync[1];

  reg [31:0] r_chan = 0, r_baud = 0, r_sync = 0, r_mask = 0;
  reg [ 8:0] r_decim = 16;
  reg [ 4:0] r_shift = 10;
  reg [ 5:0] r_maxerr = 0;
  reg        r_invert = 1'b0;
  reg [ 3:0] r_tc = 4, r_gain = 2;
  reg [NBITS_W-1:0] r_nbits = 8;

  always @(posedge adc_clk) begin
    arm_sync  <= {arm_sync[1:0], arm_tgl};
    stop_sync <= {stop_sync[1:0], stop_tgl};
    if (arm_ev) begin
      r_chan   <= chan_inc;
      r_decim  <= (decim[8:0] == 0) ? 9'd1 : decim[8:0];
      r_shift  <= decim[20:16];
      r_baud   <= baud_inc;
      r_sync   <= sync;
      r_mask   <= mask;
      r_maxerr <= cfg[5:0];
      r_invert <= cfg[8];
      r_tc     <= cfg[19:16];
      r_gain   <= cfg[27:24];
      r_nbits  <= nbits;
    end
  end

  reg [31:0] sample_cnt = 0;
  always @(posedge adc_clk) if (adc_valid) sample_cnt <= sample_cnt + 1'b1;

  // ---------------------------------------------------------------- mixer
  // x * exp(-j phase): the channel at +CHAN_INC moves to 0 Hz
  (* rom_style = "block" *) reg [31:0] sincos [0:4095];  // {sin, cos}, Q1.15 (gfsk_tx's table)
  initial $readmemh("gfsk_tx_sincos.mem", sincos);

  reg [31:0] nco = 0;
  reg [31:0] sc = 0;
  reg signed [15:0] xi1 = 0, xq1 = 0;
  reg               v1 = 1'b0, v2 = 1'b0, v3 = 1'b0;   // valid with xi1, p_*, m*
  reg signed [31:0] p_ic = 0, p_qs = 0, p_qc = 0, p_is = 0;
  reg signed [15:0] mi = 0, mq = 0;

  always @(posedge adc_clk) begin
    v1 <= adc_valid;
    v2 <= v1;
    v3 <= v2;
    if (adc_valid) nco <= nco + r_chan;
    // stage 1: table read, samples aligned with it
    sc  <= sincos[nco[31:20]];
    xi1 <= adc_data_i;
    xq1 <= adc_data_q;
    // stage 2: products
    p_ic <= xi1 * $signed(sc[15:0]);
    p_qs <= xq1 * $signed(sc[31:16]);
    p_qc <= xq1 * $signed(sc[15:0]);
    p_is <= xi1 * $signed(sc[31:16]);
    // stage 3: (I cos + Q sin) + j (Q cos - I sin), Q1.15 -> sample units
    mi <= (p_ic + p_qs) >>> 15;
    mq <= (p_qc - p_is) >>> 15;
  end

  // ---------------------------------------------------------------- CIC /R, order 3
  reg signed [39:0] int1i = 0, int2i = 0, int3i = 0, int1q = 0, int2q = 0, int3q = 0;
  reg signed [39:0] dly1i = 0, dly2i = 0, dly3i = 0, dly1q = 0, dly2q = 0, dly3q = 0;
  reg signed [39:0] c1i = 0, c2i = 0, c3i = 0, c1q = 0, c2q = 0, c3q = 0;
  reg [8:0]  dcnt = 0;
  reg        dv = 1'b0, cv1 = 1'b0, cv2 = 1'b0, cv3 = 1'b0;
  reg signed [17:0] di = 0, dq = 0;   // decimated, saturated
  reg               d_valid = 1'b0;

  function signed [17:0] sat18(input signed [39:0] v);
    sat18 = (v > 40'sd131071) ? 18'sd131071 : (v < -40'sd131072) ? -18'sd131072 : v[17:0];
  endfunction

  always @(posedge adc_clk) begin
    dv <= 1'b0;
    if (v3) begin                     // mi / mq hold a sample
      int1i <= int1i + mi;
      int2i <= int2i + int1i;
      int3i <= int3i + int2i;
      int1q <= int1q + mq;
      int2q <= int2q + int1q;
      int3q <= int3q + int2q;
      if (dcnt >= r_decim - 1) begin
        dcnt <= 0;
        dv   <= 1'b1;
      end else
        dcnt <= dcnt + 1'b1;
    end
    // comb stages at the output rate (one per clock, pipelined)
    cv1 <= dv;
    cv2 <= cv1;
    cv3 <= cv2;
    if (dv)  begin c1i <= int3i - dly1i; dly1i <= int3i; c1q <= int3q - dly1q; dly1q <= int3q; end
    if (cv1) begin c2i <= c1i - dly2i;   dly2i <= c1i;   c2q <= c1q - dly2q;   dly2q <= c1q;   end
    if (cv2) begin c3i <= c2i - dly3i;   dly3i <= c2i;   c3q <= c2q - dly3q;   dly3q <= c2q;   end
    d_valid <= cv3;
    if (cv3) begin
      di <= sat18(c3i >>> r_shift);
      dq <= sat18(c3q >>> r_shift);
    end
  end

  // ---------------------------------------------------------------- CORDIC
  // vectoring: rotate (x, y) onto the x axis; the angle turned is the phase.
  // 16-bit angles (65536 = one turn); x ends as 1.647 * magnitude.
  localparam CS = 14;
  reg signed [20:0] cx [0:CS];
  reg signed [20:0] cy [0:CS];
  reg        [15:0] cz [0:CS];
  reg               cvv [0:CS];
  function [15:0] atan_tab(input integer k);
    case (k)
      0: atan_tab = 16'd8192;  1: atan_tab = 16'd4836;  2: atan_tab = 16'd2555;
      3: atan_tab = 16'd1297;  4: atan_tab = 16'd651;   5: atan_tab = 16'd326;
      6: atan_tab = 16'd163;   7: atan_tab = 16'd81;    8: atan_tab = 16'd41;
      9: atan_tab = 16'd20;    10: atan_tab = 16'd10;   11: atan_tab = 16'd5;
      12: atan_tab = 16'd3;    default: atan_tab = 16'd1;
    endcase
  endfunction

  integer k;
  initial for (k = 0; k <= CS; k = k + 1) begin cx[k] = 0; cy[k] = 0; cz[k] = 0; cvv[k] = 0; end

  always @(posedge adc_clk) begin
    // stage 0: into the right half plane
    cvv[0] <= d_valid;
    if (di < 0) begin
      cx[0] <= -di;
      cy[0] <= -dq;
      cz[0] <= 16'h8000;
    end else begin
      cx[0] <= di;
      cy[0] <= dq;
      cz[0] <= 16'h0000;
    end
    for (k = 0; k < CS; k = k + 1) begin
      cvv[k+1] <= cvv[k];
      if (cy[k] < 0) begin
        cx[k+1] <= cx[k] - (cy[k] >>> k);
        cy[k+1] <= cy[k] + (cx[k] >>> k);
        cz[k+1] <= cz[k] - atan_tab(k);
      end else begin
        cx[k+1] <= cx[k] + (cy[k] >>> k);
        cy[k+1] <= cy[k] - (cx[k] >>> k);
        cz[k+1] <= cz[k] + atan_tab(k);
      end
    end
  end

  wire        ph_valid = cvv[CS];
  wire [15:0] ph       = cz[CS];
  wire [17:0] mag      = cx[CS][20:3];   // (magnitude * 1.647 / 8)

  // ---------------------------------------------------------------- frequency, offset, level
  reg  [15:0] ph_prev = 0;
  reg  signed [15:0] freq = 0;
  reg         f_valid = 1'b0;
  reg  signed [31:0] est = 0;          // offset estimate, 16 fractional bits
  reg  signed [27:0] lvl = 0;          // magnitude average, 8 fractional bits
  reg         freeze = 1'b0;

  always @(posedge adc_clk) begin
    f_valid <= 1'b0;
    if (ph_valid) begin
      freq    <= ph - ph_prev;
      ph_prev <= ph;
      f_valid <= 1'b1;
      lvl     <= lvl + (($signed({2'b00, mag, 8'd0}) - lvl) >>> 4);
    end
    if (f_valid && !freeze)
      est <= est + ((($signed({freq, 16'd0}) - est)) >>> r_tc);
  end

  wire signed [16:0] dfreq = $signed(freq) - $signed(est[31:16]);

  // ---------------------------------------------------------------- bit clock, bits
  // acc counts the bit (2^32 = one bit). At a zero crossing of dfreq the bit
  // boundary should be at acc = 0: move acc toward it by error >> G.
  reg  [31:0] acc = 0;
  wire signed [31:0] acc_s = acc;
  wire signed [31:0] corr  = acc_s >>> r_gain;   // (its own signed wire: inside an
                                                 // unsigned sum >>> would turn logical)
  reg  signed [16:0] dprev = 0;
  reg  signed [23:0] integ = 0;
  reg         bit_v = 1'b0, bit_val = 1'b0;

  // signed(acc): +x just after the boundary (we are early), -x before it. The
  // step stays positive (the correction is at most a quarter of acc), so the
  // boundary is where the sum wraps, with the correction included.
  wire        xing   = dfreq[16] != dprev[16];
  wire [31:0] acc_nx = acc + r_baud - (xing ? corr : 32'd0);

  always @(posedge adc_clk) begin
    bit_v <= 1'b0;
    if (f_valid) begin
      dprev <= dfreq;
      acc   <= acc_nx;
      if (acc_nx < acc) begin           // boundary: decide the bit just ended
        bit_v   <= 1'b1;
        bit_val <= (integ > 0) ^ r_invert;
        integ   <= dfreq;
      end else
        integ <= integ + dfreq;
    end
  end

  // ---------------------------------------------------------------- sync and capture
  localparam S_IDLE = 2'd0, S_SEARCH = 2'd1, S_CAPT = 2'd2;
  reg  [1:0]  state = S_IDLE;
  reg  [31:0] sr = 0;
  reg         sr_new = 1'b0;
  reg  [NBITS_W-1:0] bcnt = 0;
  reg  [7:0]  cur = 0;
  reg         done_tgl = 1'b0;

  // popcount of the masked difference (registered: one cycle after the bit)
  wire [31:0] diff = (sr ^ r_sync) & r_mask;
  function [5:0] popcount32(input [31:0] v);
    integer j;
    begin
      popcount32 = 0;
      for (j = 0; j < 32; j = j + 1) popcount32 = popcount32 + v[j];
    end
  endfunction

  always @(posedge adc_clk) begin
    sr_new <= 1'b0;
    if (arm_ev) begin
      state  <= S_SEARCH;
      sr     <= 0;
      freeze <= 1'b0;
    end else if (stop_ev) begin
      state  <= S_IDLE;
      freeze <= 1'b0;
    end else begin
      case (state)
        S_SEARCH: begin
          if (bit_v) begin
            sr     <= {sr[30:0], bit_val};
            sr_new <= 1'b1;
          end
          if (sr_new) begin
            if (popcount32(diff) <= r_maxerr) begin
              state      <= S_CAPT;
              freeze     <= 1'b1;
              bcnt       <= 0;
              cur        <= 0;
              res_tstamp <= sample_cnt;
              res_fofs   <= est[31:16];
              res_level  <= lvl[25:8];
              res_err    <= popcount32(diff);
            end
          end
        end
        S_CAPT: begin
          if (bit_v) begin
            cur  <= {cur[6:0], bit_val};
            bcnt <= bcnt + 1'b1;
            if (bcnt[2:0] == 3'd7)
              buf_mem[bcnt[NBITS_W-1:3]] <= {cur[6:0], bit_val};
            else if (bcnt + 1'b1 == r_nbits)   // last, partial byte: MSB aligned
              buf_mem[bcnt[NBITS_W-1:3]] <= {cur[6:0], bit_val} << (3'd7 - bcnt[2:0]);
            if (bcnt + 1'b1 == r_nbits) begin
              state    <= S_IDLE;
              freeze   <= 1'b0;
              done_tgl <= ~done_tgl;
            end
          end
        end
        default: freeze <= 1'b0;
      endcase
    end
  end

  (* ASYNC_REG = "TRUE" *) reg [1:0] done_sync = 2'b00;
  always @(posedge s_axi_aclk) done_sync <= {done_sync[0], done_tgl};
  assign done_tgl_s = done_sync[1];

endmodule

`default_nettype wire
