/* Virtual A7105: a byte-level SPI slave that behaves like AMICCOM's A7105
 * towards Multiprotocol's A7105_SPI.ino, and transmits through the radio layer.
 * See a7105.c for what is emulated. */
#ifndef A7105_H
#define A7105_H

#include <stdint.h>

void    a7105_csn(int level);           /* chip select pin: 0 = selected */
uint8_t a7105_spi(uint8_t mosi);        /* one SPI byte; returns MISO */
void    a7105_poll(void);               /* end of TX, queued RX packet (call often) */

/* A packet "received" off the air: it lands in the RX FIFO the next time the
 * chip is in RX (it waits there if the chip is busy). With gfsk_rx in the
 * bitstream the chip also receives for real; this is for tests and replays. */
int     a7105_rx_queue(const uint8_t *data, int len);

/* On-air coding the data sheet leaves open: the CRC-16 parameters and the
 * (7,4) Hamming FEC code words. Settable so they can be learned from a real
 * FlySky transmitter (tools/a7105_sniff.py). */
struct a7105_coding {
	uint16_t crc_poly;
	uint16_t crc_init;
	uint16_t crc_xorout;
	uint8_t  crc_reflect;       /* reflected input and output */
	uint8_t  fec[16];           /* code word (7 bits, sent MSB first) per nibble */
	uint8_t  fec_low_first;     /* low nibble of each byte first */
};
struct a7105_coding *a7105_coding(void);
void    a7105_coding_default(void);
void    a7105_set_freq_offset(double hz);
double  a7105_freq_offset(void);

/* for the shell */
struct a7105_info {
	uint8_t  state;             /* A7105_State strobe value of the current state */
	uint32_t tx_packets;
	uint32_t tx_errors;
	uint32_t rx_packets;
	uint32_t strobes;
	double   last_freq_hz;
	double   baud, dev_hz;
	int      last_bits;
	int      last_len;
	uint8_t  last_pkt[40];      /* last payload as written to the FIFO */
	/* receive (gfsk_rx) */
	uint32_t rx_arms;           /* RX entered with the receiver armed */
	uint32_t rx_crc_errors, rx_fec_errors;
	double   rx_offset_hz;      /* carrier offset of the last packet (rough) */
	uint32_t rx_level;
	uint32_t rx_arm_us, rx_arm_us_max;  /* receiver armed this long after the packet's start */
	int      rx_len;
	uint8_t  last_rx[40];       /* last packet received, as it went into the FIFO */
};
const struct a7105_info *a7105_get_info(void);
const uint8_t *a7105_regs(void);

#endif
