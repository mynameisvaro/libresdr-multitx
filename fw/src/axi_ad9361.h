/* ADI axi_ad9361 core (FPGA side of the AD9361 data port): set-up and DDS. */
#ifndef AXI_AD9361_H
#define AXI_AD9361_H

void   axi_ad9361_init(void);                 /* after the AD9361 is clocking the port */
double axi_ad9361_clk_hz(void);               /* measured interface clock = sample rate */
void   axi_ad9361_dds(double rate_hz, double tone_hz, double dbfs);  /* complex tone on TX1 */
void   axi_ad9361_dds_off(void);              /* TX1 sends zeros */

#endif
