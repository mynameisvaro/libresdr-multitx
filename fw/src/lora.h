/* lora_tx driver: LoRa frames (hw/ip/lora_tx). */
#ifndef LORA_H
#define LORA_H

#include <stdint.h>
#include "lora_code.h"

struct lora_cfg {
	struct lora_code code;
	double  bw;          /* Hz: 812.5e3 (SX1280 "800"), 500e3, 125e3, ... */
	uint8_t preamble;    /* up-chirps before the sync word */
	uint8_t sync;        /* sync word (SX127x: 0x12 private, 0x34 public) */
	uint8_t invert_iq;
};

/* Encode and start one frame at sample rate fs, offset chan_hz from the LO,
 * dbfs below full scale. 0 when started, -1 busy / bad parameters. */
int  lora_send(double fs, double chan_hz, double dbfs, const struct lora_cfg *c,
               const uint8_t *payload, int len);
int  lora_present(void);                /* the bitstream has lora_tx */
int  lora_busy(void);
int  lora_last_symbols(void);           /* chirps in the last frame */

#endif
