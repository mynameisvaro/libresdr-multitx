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
/* Centre of the protocol's channels (0: 2.4 GHz): the calibrations run there
 * and an LO slice is centred on it. Retunes with calibrations if running. */
int  radio_set_band(double hz);
void radio_rx_follow(int on);        /* tune the RX LO with the TX LO (for captures) */
/* dB added to the power every protocol asks for (the output isn't calibrated) */
void radio_set_power_offset(double db);
double radio_power_offset(void);
/* TX attenuation to maximum between packets (radio_tx_idle, called when a
 * virtual chip turns to receive) */
void radio_set_quiet(int on);
int radio_quiet(void);
void radio_tx_idle(void);
void radio_capture_next(void);       /* start adc_capture with the next packet */
void radio_stop(void);               /* front end off, silent */
int  radio_running(void);
/* Send nbits from data (MSB first) at freq_hz. Returns 0 when started. */
int  radio_tx(double freq_hz, const struct radio_modem *m, double dbm,
              const uint8_t *data, int nbits);
int  radio_tx_busy(void);
struct gfsk_rx_cfg;
/* Listen for a GFSK packet at freq_hz (gfsk_rx): retunes the RX LO if the
 * channel is outside its slice. 0 when armed; gfsk_rx_done() then tells. */
int  radio_rx(double freq_hz, const struct gfsk_rx_cfg *c);
void radio_rx_stop(void);
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
	uint32_t captures;        /* adc_capture starts (radio_capture_next) */
	uint32_t rx_arms;         /* radio_rx calls that armed the receiver */
	double   rx_lo_hz;
};
const struct radio_stats *radio_get_stats(void);

#endif
