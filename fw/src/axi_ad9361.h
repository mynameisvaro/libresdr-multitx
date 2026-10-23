/* ADI axi_ad9361 core (FPGA side of the AD9361 data port): set-up and DDS. */
#ifndef AXI_AD9361_H
#define AXI_AD9361_H

void   axi_ad9361_init(void);                 /* after the AD9361 is clocking the port */
double axi_ad9361_clk_hz(void);               /* measured interface clock = sample rate */
void   axi_ad9361_dds(double rate_hz, double tone_hz, double dbfs);  /* complex tone on TX1 */
void   axi_ad9361_dds_off(void);              /* TX1 sends zeros */

enum axi_ad9361_tx_src { AXI_AD9361_TX_DDS = 0, AXI_AD9361_TX_MOD = 2, AXI_AD9361_TX_ZERO = 3 };
/* MOD = the modulators (gfsk_tx, lora_tx through tx_select) via the DMA inputs */
void   axi_ad9361_tx_source(enum axi_ad9361_tx_src src);

#endif
