/* gfsk_tx driver: packet GFSK/FSK modulator + channel NCO (hw/ip/gfsk_tx). */
#ifndef GFSK_H
#define GFSK_H

#include <stdint.h>

#define GFSK_BUF_BYTES 256

/* Modulation for sample rate fs. bt > 0: Gaussian pulse with that BT; bt <= 0: 2-FSK. */
void   gfsk_config(double fs, double baud, double dev_hz, double bt);
void   gfsk_channel(double offset_hz);        /* NCO offset from the LO, |offset| < fs/2 */
void   gfsk_level(double dbfs, double ramp_us);
void   gfsk_bit_order(int lsb_first);
int    gfsk_send(const uint8_t *data, int nbits);   /* -1 if busy or too long */
int    gfsk_busy(void);
void   gfsk_cw(int on);                       /* unmodulated carrier at the channel offset */
double gfsk_fs(void);

/* CC2500 register maths (26 MHz crystal) */
double cc2500_baud(uint8_t mdmcfg4, uint8_t mdmcfg3);
double cc2500_dev(uint8_t deviatn);

#endif
