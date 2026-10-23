/* Radio layer between the virtual chips and the hardware: picks an LO slice and
 * NCO offset for an RF frequency, caches the modem settings, sets the power and
 * hands packets to gfsk_tx. */
#ifndef RADIO_H
#define RADIO_H

#include <stdint.h>

struct radio_modem {
	double baud;
	double dev_hz;
	double bt;            /* Gaussian BT; <= 0 for 2-FSK */
};

int  radio_start(void);              /* rate, front end on (PA settles ~0.5 s); 0 = ok */
void radio_set_rate(double hz);      /* before radio_start; default 61.44 MSPS */
void radio_rx_follow(int on);        /* tune the RX LO with the TX LO (for captures) */
void radio_capture_next(void);       /* start adc_capture with the next packet */
void radio_stop(void);               /* front end off, silent */
int  radio_running(void);
/* Send nbits from data (MSB first) at freq_hz. Returns 0 when started. */
int  radio_tx(double freq_hz, const struct radio_modem *m, double dbm,
              const uint8_t *data, int nbits);
int  radio_tx_busy(void);
struct lora_cfg;
/* A LoRa frame at freq_hz (lora_tx); 0 when started. */
int  radio_tx_lora(double freq_hz, const struct lora_cfg *c, double dbm,
                   const uint8_t *payload, int len);

/* statistics for the shell */
struct radio_stats {
	uint32_t packets;
	uint32_t retunes;
	uint32_t last_retune_us;
	uint32_t max_retune_us;
	double   lo_hz;
	double   rate_hz;
	double   slice_hz;
	double   last_freq_hz;
	double   cap_lo_hz;       /* LO and channel of the last captured packet */
	double   cap_freq_hz;
};
const struct radio_stats *radio_get_stats(void);

#endif
