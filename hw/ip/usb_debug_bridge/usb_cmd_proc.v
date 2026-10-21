// usb_cmd_proc: runs host command packets as AXI4-Lite transactions.
//
// Streams are 64-bit words. On USB each word is two little-endian 32-bit words,
// high half [63:32] first (that is how the Ettus GPIF converters pack them).
//
// Command packet (host -> FPGA, USB data-OUT endpoint 0x02):
//   word 0      : [63:32] CHDR-style header {4'h0, seq[11:0], packet_bytes[15:0]}
//                 [31:0]  MAGIC_CMD ("USBD")
//   then any number of commands:
//   cmd word    : [63:56] op  [55:48] flags  [47:32] count  [31:0] addr / value
//     OP_WRITE  : followed by ceil(count/2) data words, hi half first; writes count
//                 32-bit words starting at addr (flags[0] = keep addr fixed)
//     OP_READ   : reads count 32-bit words starting at addr (flags[0] as above)
//     OP_CTRL_WR: ctrl_reg <= value
//     OP_CTRL_RD: returns {VERSION, status_in}
//
// Response packet (FPGA -> host, USB data-IN endpoint 0x86):
//   word 0      : {MAGIC_RSP ("USBR"), 20'h0, seq}
//   per command : [result data words] then a status word
//                 {op | 0x80, err, count, addr}   err: bit0 SLVERR, bit1 DECERR,
//                                                     bit2 packet ended early, bit3 bad op
//   last word   : {MAGIC_END ("END!"), 24'h0, OR of all err} with tlast
//
// The FX3 path holds whole packets: keep each packet and each response <= 16 KB.

`timescale 1ns / 1ps
`default_nettype none

module usb_cmd_proc #(
  parameter [31:0] VERSION      = 32'h0001_0000,
  parameter [31:0] CTRL_DEFAULT = 32'h0000_0000
)(
  input  wire        clk,
  input  wire        rst,

  // commands from the host
  input  wire [63:0] i_tdata,
  input  wire        i_tlast,
  input  wire        i_tvalid,
  output wire        i_tready,

  // responses to the host
  output reg  [63:0] o_tdata,
  output reg         o_tlast,
  output reg         o_tvalid,
  input  wire        o_tready,

  // AXI4-Lite master
  output reg  [31:0] m_axi_awaddr,
  output reg         m_axi_awvalid,
  input  wire        m_axi_awready,
  output reg  [31:0] m_axi_wdata,
  output wire [ 3:0] m_axi_wstrb,
  output reg         m_axi_wvalid,
  input  wire        m_axi_wready,
  input  wire [ 1:0] m_axi_bresp,
  input  wire        m_axi_bvalid,
  output wire        m_axi_bready,
  output reg  [31:0] m_axi_araddr,
  output reg         m_axi_arvalid,
  input  wire        m_axi_arready,
  input  wire [31:0] m_axi_rdata,
  input  wire [ 1:0] m_axi_rresp,
  input  wire        m_axi_rvalid,
  output wire        m_axi_rready,

  output reg  [31:0] ctrl_reg,
  input  wire [31:0] status_in
);

  localparam [31:0] MAGIC_CMD = 32'h5553_4244; // "USBD"
  localparam [31:0] MAGIC_RSP = 32'h5553_4252; // "USBR"
  localparam [31:0] MAGIC_END = 32'h454E_4421; // "END!"

  localparam [7:0] OP_WRITE   = 8'h01;
  localparam [7:0] OP_READ    = 8'h02;
  localparam [7:0] OP_CTRL_WR = 8'h03;
  localparam [7:0] OP_CTRL_RD = 8'h04;

  localparam [7:0] ERR_SLV   = 8'h01;
  localparam [7:0] ERR_DEC   = 8'h02;
  localparam [7:0] ERR_SHORT = 8'h04;
  localparam [7:0] ERR_OP    = 8'h08;

  localparam [3:0] S_HDR      = 4'd0,
                   S_CMD      = 4'd1,
                   S_WR_DATA  = 4'd2,
                   S_WR_ISSUE = 4'd3,
                   S_WR_WAIT  = 4'd4,
                   S_RD_ISSUE = 4'd5,
                   S_RD_WAIT  = 4'd6,
                   S_STATUS   = 4'd7,
                   S_END      = 4'd8,
                   S_DRAIN    = 4'd9,
                   S_EMIT     = 4'd10;

  reg [3:0]  state, ret_state;

  reg [11:0] seq;
  reg [7:0]  op, flags, err, pkt_err;
  reg [15:0] count, cmd_count;
  reg [31:0] addr, cmd_addr;
  reg        last_in;     // the word just consumed carried tlast
  reg [63:0] wbuf;        // write data word being executed
  reg        half;        // 0: high half next, 1: low half next
  reg [31:0] rbuf;        // first read word of a pair

  assign i_tready = (state == S_HDR) || (state == S_CMD) ||
                    (state == S_WR_DATA) || (state == S_DRAIN);

  assign m_axi_wstrb  = 4'hF;
  assign m_axi_bready = (state == S_WR_WAIT);
  assign m_axi_rready = (state == S_RD_WAIT);

  wire [7:0]  resp_err_b = (m_axi_bresp == 2'b10) ? ERR_SLV : (m_axi_bresp == 2'b11) ? ERR_DEC : 8'h00;
  wire [7:0]  resp_err_r = (m_axi_rresp == 2'b10) ? ERR_SLV : (m_axi_rresp == 2'b11) ? ERR_DEC : 8'h00;
  wire [31:0] next_addr  = flags[0] ? addr : addr + 32'd4;
  wire [3:0]  after_cmd  = last_in ? S_END : S_CMD;

  always @(posedge clk) begin
    if (rst) begin
      state         <= S_HDR;
      ret_state     <= S_HDR;
      o_tvalid      <= 1'b0;
      o_tlast       <= 1'b0;
      o_tdata       <= 64'd0;
      m_axi_awvalid <= 1'b0;
      m_axi_wvalid  <= 1'b0;
      m_axi_arvalid <= 1'b0;
      ctrl_reg      <= CTRL_DEFAULT;
      pkt_err       <= 8'h00;
      half          <= 1'b0;
    end else begin
      case (state)

        // packet header: answer with our own header
        S_HDR: if (i_tvalid) begin
          seq       <= i_tdata[59:48];
          pkt_err   <= 8'h00;
          last_in   <= i_tlast;
          o_tdata   <= {MAGIC_RSP, 20'h0, i_tdata[59:48]};
          o_tlast   <= 1'b0;
          o_tvalid  <= 1'b1;
          state     <= S_EMIT;
          if (i_tdata[31:0] != MAGIC_CMD) begin
            pkt_err   <= ERR_OP;
            ret_state <= i_tlast ? S_END : S_DRAIN;
          end else begin
            ret_state <= i_tlast ? S_END : S_CMD;
          end
        end

        // decode one command
        S_CMD: if (i_tvalid) begin
          op        <= i_tdata[63:56];
          flags     <= i_tdata[55:48];
          count     <= i_tdata[47:32];
          cmd_count <= i_tdata[47:32];
          addr      <= i_tdata[31:0];
          cmd_addr  <= i_tdata[31:0];
          last_in   <= i_tlast;
          err       <= 8'h00;
          half      <= 1'b0;
          case (i_tdata[63:56])
            OP_WRITE: begin
              if (i_tdata[47:32] == 16'd0)
                state <= S_STATUS;
              else if (i_tlast) begin
                err   <= ERR_SHORT;
                state <= S_STATUS;
              end else
                state <= S_WR_DATA;
            end
            OP_READ:
              state <= (i_tdata[47:32] == 16'd0) ? S_STATUS : S_RD_ISSUE;
            OP_CTRL_WR: begin
              ctrl_reg <= i_tdata[31:0];
              state    <= S_STATUS;
            end
            OP_CTRL_RD: begin
              o_tdata   <= {VERSION, status_in};
              o_tlast   <= 1'b0;
              o_tvalid  <= 1'b1;
              ret_state <= S_STATUS;
              state     <= S_EMIT;
            end
            default: begin
              err   <= ERR_OP;
              state <= S_STATUS;
            end
          endcase
        end

        // ---- writes: two 32-bit AXI writes per data word ----
        S_WR_DATA: if (i_tvalid) begin
          wbuf    <= i_tdata;
          last_in <= i_tlast;
          half    <= 1'b0;
          state   <= S_WR_ISSUE;
        end

        S_WR_ISSUE: begin
          m_axi_awaddr  <= addr;
          m_axi_wdata   <= half ? wbuf[31:0] : wbuf[63:32];
          m_axi_awvalid <= 1'b1;
          m_axi_wvalid  <= 1'b1;
          state         <= S_WR_WAIT;
        end

        S_WR_WAIT: begin
          if (m_axi_awready) m_axi_awvalid <= 1'b0;
          if (m_axi_wready)  m_axi_wvalid  <= 1'b0;
          if (m_axi_bvalid) begin
            err   <= err | resp_err_b;
            count <= count - 16'd1;
            addr  <= next_addr;
            if (count == 16'd1)
              state <= S_STATUS;
            else if (!half) begin
              half  <= 1'b1;
              state <= S_WR_ISSUE;
            end else if (last_in) begin
              err   <= err | resp_err_b | ERR_SHORT;
              state <= S_STATUS;
            end else
              state <= S_WR_DATA;
          end
        end

        // ---- reads: pack two 32-bit AXI reads per response word ----
        S_RD_ISSUE: begin
          m_axi_araddr  <= addr;
          m_axi_arvalid <= 1'b1;
          state         <= S_RD_WAIT;
        end

        S_RD_WAIT: begin
          if (m_axi_arready) m_axi_arvalid <= 1'b0;
          if (m_axi_rvalid) begin
            err   <= err | resp_err_r;
            count <= count - 16'd1;
            addr  <= next_addr;
            if (!half && count != 16'd1) begin
              rbuf  <= m_axi_rdata;
              half  <= 1'b1;
              state <= S_RD_ISSUE;
            end else begin
              o_tdata   <= half ? {rbuf, m_axi_rdata} : {m_axi_rdata, 32'h0};
              o_tlast   <= 1'b0;
              o_tvalid  <= 1'b1;
              half      <= 1'b0;
              ret_state <= (count == 16'd1) ? S_STATUS : S_RD_ISSUE;
              state     <= S_EMIT;
            end
          end
        end

        // ---- per-command status, end of packet ----
        S_STATUS: begin
          o_tdata   <= {op | 8'h80, err, cmd_count, cmd_addr};
          o_tlast   <= 1'b0;
          o_tvalid  <= 1'b1;
          pkt_err   <= pkt_err | err;
          ret_state <= after_cmd;
          state     <= S_EMIT;
        end

        S_END: begin
          o_tdata   <= {MAGIC_END, 24'h0, pkt_err};
          o_tlast   <= 1'b1;
          o_tvalid  <= 1'b1;
          ret_state <= S_HDR;
          state     <= S_EMIT;
        end

        S_DRAIN: if (i_tvalid && i_tlast) state <= S_END;

        // hold one output word until it is taken
        S_EMIT: if (o_tready) begin
          o_tvalid <= 1'b0;
          state    <= ret_state;
        end

        default: state <= S_HDR;
      endcase
    end
  end

endmodule

`default_nettype wire
