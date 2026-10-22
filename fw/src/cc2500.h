/* Virtual CC2500: a byte-level SPI slave that behaves like TI's CC2500 towards
 * Multiprotocol's CC2500_SPI.ino, and transmits through the radio layer.
 * See cc2500.c for what is emulated. */
#ifndef CC2500_H
#define CC2500_H

#include <stdint.h>

void    cc2500_csn(int level);          /* chip select pin: 0 = selected */
uint8_t cc2500_spi(uint8_t mosi);       /* one SPI byte; returns MISO */
void    cc2500_poll(void);              /* end of TX -> next state (call often) */

/* for the shell */
struct cc2500_info {
	uint8_t  marcstate;
	uint32_t tx_packets;
	uint32_t tx_errors;
	uint32_t strobes;
	double   last_freq_hz;
	double   baud, dev_hz;
	int      last_len;
	uint8_t  last_pkt[64];
};
const struct cc2500_info *cc2500_get_info(void);
const uint8_t *cc2500_regs(void);

#endif
