/* LoRa coding (lora_code.c): payload -> chirp symbol values. */
#ifndef LORA_CODE_H
#define LORA_CODE_H

#include <stdint.h>

struct lora_code {
	uint8_t sf;        /* 5..12 */
	uint8_t cr;        /* 1..4: 4/5..4/8 */
	uint8_t crc;       /* payload CRC-16 appended */
	uint8_t implicit;  /* no header (length, CR, CRC known to the receiver) */
	uint8_t ldro;      /* low data rate optimisation */
};

/* Data symbols (after the preamble, sync word and down-chirps) into syms[];
 * returns how many, or -1 (bad parameters, or more than max). */
int      lora_encode(const uint8_t *payload, int len, const struct lora_code *c,
                     uint16_t *syms, int max);
uint16_t lora_crc16(const uint8_t *payload, int len);

#endif
