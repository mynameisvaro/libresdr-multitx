/* gfsk_rx driver: packet GFSK/FSK receiver (hw/ip/gfsk_rx). It finds a sync
 * word on the RX samples and keeps the bits after it; decoding them (CRC, FEC,
 * whitening) is up to the chip being emulated. */
#ifndef GFSK_RX_H
#define GFSK_RX_H

#include <stdint.h>

#define GFSK_RX_BUF_BYTES 128

struct gfsk_rx_cfg {
	double   baud;
	uint32_t sync;          /* the 32 bits before the payload, first received in bit 31 */
	uint32_t mask;          /* which sync bits count */
	int      maxerr;        /* bit errors allowed in the sync word */
	int      invert;
	int      nbits;         /* bits to keep after the sync word */
};

struct gfsk_rx_result {
	uint32_t tstamp;        /* sample counter at the sync word */
	double   offset_hz;     /* carrier offset estimate (rough) */
	uint32_t level;         /* average magnitude (CORDIC units) */
	int      syncerr;
};

int  gfsk_rx_present(void);             /* the bitstream has gfsk_rx */
/* A sync match only counts at this average level or more: noise makes
 * sync-like bit patterns (around 10-20 at RX gain 30; packets through the
 * loopback cable read ~25000). Default 60. */
void gfsk_rx_set_min_level(uint32_t level);
uint32_t gfsk_rx_min_level(void);
/* Search at offset_hz from the RX LO for sample rate fs; 0 when armed. */
int  gfsk_rx_arm(double fs, double offset_hz, const struct gfsk_rx_cfg *c);
void gfsk_rx_stop(void);
int  gfsk_rx_searching(void);
int  gfsk_rx_done(void);                /* a packet is in the buffer */
/* The kept bits, MSB first; returns the bytes copied. */
int  gfsk_rx_read(uint8_t *buf, int max, struct gfsk_rx_result *r);

#endif
