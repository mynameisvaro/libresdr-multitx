/* AD9361 control for the firmware's C code. Underneath is UHD's AD9361 driver
 * (third_party/uhd_ad9361), configured like a B210. Every function returns 0, or
 * -1 after printing the driver's error message. Channel 1 only for now. */
#ifndef RF_H
#define RF_H

#ifdef __cplusplus
extern "C" {
#endif

enum rf_dir { RF_RX = 0, RF_TX = 1 };

int rf_init(void);                                    /* reset + full init, RX1/TX1 on, FPGA core set up */
int rf_ready(void);                                   /* rf_init succeeded */
int rf_set_rate(double hz, double *actual);           /* sample rate = DATA_CLK (CMOS 1R1T) */
int rf_get_rate(double *hz);
int rf_tune(enum rf_dir dir, double hz, double *actual);
int rf_get_freq(enum rf_dir dir, double *hz);
int rf_set_gain(enum rf_dir dir, double db, double *actual);   /* TX: 0..89.75 dB (attenuation = 89.75 - gain) */
int rf_set_bw(enum rf_dir dir, double hz, double *actual);
int rf_rssi(double *db);
int rf_temperature(double *deg_c);
int rf_loopback(int on);                              /* AD9361 digital TX -> RX loopback */

#ifdef __cplusplus
}
#endif

#endif
