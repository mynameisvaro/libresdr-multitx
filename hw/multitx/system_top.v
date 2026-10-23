// Top level for the LibreSDR B220 mini (XC7A200T) MultiTX platform.
//
// Everything lives in the block design (system.bd). This file only maps board
// pins to it: GPIF data-bus I/O buffers, AD9361 SPI/control lines, RF switches,
// LEDs. Pin names follow the board's reference design (b210.xdc).

`timescale 1ns / 1ps
`default_nettype none

module system_top (
  input  wire        CLK_40MHz_FPGA,

  // AD9361 data port (CMOS, P0 = RX, P1 = TX)
  input  wire        CAT_DCLK_P,
  input  wire        CAT_RX_FR_P,
  input  wire [11:0] CAT_P0_D,
  output wire        CAT_FBCLK_P,
  output wire        CAT_TX_FR_P,
  output wire [11:0] CAT_P1_D,

  // AD9361 control
  output wire        CAT_SPI_EN,
  output wire        CAT_SPI_DI,     // MOSI
  output wire        CAT_SPI_CLK,
  input  wire        CAT_SPI_DO,     // MISO
  output wire        CAT_RESETn,
  output wire        CAT_EN,
  output wire        CAT_EN_AGC,
  output wire        CAT_TXnRX,
  output wire        CAT_SYNC,
  output wire [3:0]  CAT_CTL_IN,
  input  wire [7:0]  CAT_CTL_OUT,

  // FX3 GPIF II
  output wire        IFCLK,
  inout  wire [31:0] GPIF_D,
  output wire        GPIF_CTL0, GPIF_CTL1, GPIF_CTL2, GPIF_CTL3, GPIF_CTL7,
  output wire        GPIF_CTL11, GPIF_CTL12,
  input  wire        GPIF_CTL4, GPIF_CTL5, GPIF_CTL6, GPIF_CTL8, GPIF_CTL9,

  // debug UART and PPM input (accessory connector CN2)
  input  wire        FPGA_RXD0,
  output wire        FPGA_TXD0,
  input  wire        FPGA_GPIO_0,

  // 40 MHz TCXO tuning DAC
  output wire        CLK_40M_DAC_nSYNC,
  output wire        CLK_40M_DAC_SCLK,
  output wire        CLK_40M_DAC_DIN,
  output wire        REF_CLK_REQ,

  // RF switches and PA enables
  output wire        SFDX1_RX, SFDX1_TX, SRX1_RX, SRX1_TX, tx_enable1,
  output wire        SFDX2_RX, SFDX2_TX, SRX2_RX, SRX2_TX, tx_enable2,

  // LEDs (the *_inv ones are active low)
  output wire        LED_RX1_inv, LED_TXRX1_RX_inv, LED_TXRX1_TX_inv,
  output wire        LED_RX2_inv, LED_TXRX2_RX_inv, LED_TXRX2_TX_inv,
  output wire        LED_RX1_R, LED_RX1_B, LED_RX2_R, LED_RX2_B, LED_TXRX1_B, LED_TXRX2_B,
  output wire        LED_USER_R,
  output wire        PPS_LED_inv, REF_LOCKED_inv, REF_IS_10M_detect_inv
);

  // ---- GPIF data bus ----
  wire [31:0] gpif_d_i, gpif_d_o, gpif_d_t;
  genvar n;
  generate
    for (n = 0; n < 32; n = n + 1) begin : gpif_iobuf
      IOBUF iobuf (.IO (GPIF_D[n]), .I (gpif_d_o[n]), .O (gpif_d_i[n]), .T (gpif_d_t[n]));
    end
  endgenerate

  // ---- GPIO words ----
  wire [7:0]  fe0_gpio, fe1_gpio;
  wire [15:0] ctrl_gpio;

  assign {tx_enable1, SFDX1_RX, SFDX1_TX, SRX1_RX, SRX1_TX} = fe0_gpio[7:3];
  assign {tx_enable2, SFDX2_RX, SFDX2_TX, SRX2_RX, SRX2_TX} = fe1_gpio[7:3];
  assign {LED_RX1_inv, LED_TXRX1_RX_inv, LED_TXRX1_TX_inv}  = ~fe0_gpio[2:0];
  assign {LED_RX2_inv, LED_TXRX2_RX_inv, LED_TXRX2_TX_inv}  = ~fe1_gpio[2:0];

  assign CAT_RESETn = ctrl_gpio[0];
  assign CAT_EN_AGC = ctrl_gpio[1];
  assign CAT_SYNC   = ctrl_gpio[2];
  assign CAT_CTL_IN = ctrl_gpio[6:3];
  assign LED_USER_R = ~ctrl_gpio[9];

  // unused LEDs off, external reference buffer on (as in the reference design)
  assign {LED_RX1_R, LED_RX1_B, LED_RX2_R, LED_RX2_B, LED_TXRX1_B, LED_TXRX2_B} = 6'b111111;
  assign {PPS_LED_inv, REF_LOCKED_inv, REF_IS_10M_detect_inv} = 3'b111;
  assign REF_CLK_REQ = 1'b1;

  system_wrapper i_system (
    .clk_40m          (CLK_40MHz_FPGA),

    .rx_clk_in        (CAT_DCLK_P),
    .rx_frame_in      (CAT_RX_FR_P),
    .rx_data_in       (CAT_P0_D),
    .tx_clk_out       (CAT_FBCLK_P),
    .tx_frame_out     (CAT_TX_FR_P),
    .tx_data_out      (CAT_P1_D),
    .enable           (CAT_EN),
    .txnrx            (CAT_TXnRX),

    .ad9361_spi_mosi  (CAT_SPI_DI),
    .ad9361_spi_miso  (CAT_SPI_DO),
    .ad9361_spi_sck   (CAT_SPI_CLK),
    .ad9361_spi_csn   (CAT_SPI_EN),

    .fe0_gpio         (fe0_gpio),
    .fe1_gpio         (fe1_gpio),
    .ctrl_gpio_o      (ctrl_gpio),
    .ctrl_gpio_i      (CAT_CTL_OUT),

    .tcxo_dac_sclk    (CLK_40M_DAC_SCLK),
    .tcxo_dac_mosi    (CLK_40M_DAC_DIN),
    .tcxo_dac_sync_n  (CLK_40M_DAC_nSYNC),

    .uart_rxd         (FPGA_RXD0),
    .uart_txd         (FPGA_TXD0),
    .ppm_in           (FPGA_GPIO_0),

    .gpif_ifclk       (IFCLK),
    .gpif_d_i         (gpif_d_i),
    .gpif_d_o         (gpif_d_o),
    .gpif_d_t         (gpif_d_t),
    .gpif_ctl0        (GPIF_CTL0),
    .gpif_ctl1        (GPIF_CTL1),
    .gpif_ctl2        (GPIF_CTL2),
    .gpif_ctl3        (GPIF_CTL3),
    .gpif_ctl7        (GPIF_CTL7),
    .gpif_ctl11       (GPIF_CTL11),
    .gpif_ctl12       (GPIF_CTL12),
    .gpif_ctl4        (GPIF_CTL4),
    .gpif_ctl5        (GPIF_CTL5),
    .gpif_ctl6        (GPIF_CTL6),
    .gpif_ctl8        (GPIF_CTL8),
    .gpif_ctl9        (GPIF_CTL9)
  );

endmodule

`default_nettype wire
