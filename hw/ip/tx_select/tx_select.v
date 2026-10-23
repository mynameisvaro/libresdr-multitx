// tx_select: picks the modulator that is transmitting for axi_ad9361's TX1
// inputs. Both modulators output zero when idle, and the firmware only starts
// one at a time; if both are active, lora_tx wins.

`timescale 1ns / 1ps
`default_nettype none

module tx_select (
  (* X_INTERFACE_INFO = "xilinx.com:signal:clock:1.0 clk CLK" *)
  input  wire        clk,
  input  wire [15:0] gfsk_i,
  input  wire [15:0] gfsk_q,
  input  wire        lora_active,
  input  wire [15:0] lora_i,
  input  wire [15:0] lora_q,
  output reg  [15:0] dac_i,
  output reg  [15:0] dac_q
);

  always @(posedge clk) begin
    dac_i <= lora_active ? lora_i : gfsk_i;
    dac_q <= lora_active ? lora_q : gfsk_q;
  end

endmodule

`default_nettype wire
