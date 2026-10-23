/* LoRa coding: payload bytes to chirp symbols (the SX127x scheme). The same
 * steps as tools/lora.py, which was checked symbol for symbol against real
 * SX127x recordings; fw/test/test_lora_code.py compares the two. No hardware
 * access here, so the file also builds on the PC. */
#include "lora_code.h"

static uint8_t whiten_next(uint8_t w)
{
	uint8_t bit = (w >> 7 ^ w >> 5 ^ w >> 4 ^ w >> 3) & 1;
	return (uint8_t)(w << 1 | bit);
}

/* CRC-16/CCITT (poly 0x1021, init 0) over all but the last two bytes, then XOR
 * with those two bytes, as an SX127x appends it. */
uint16_t lora_crc16(const uint8_t *p, int len)
{
	uint16_t crc = 0;

	if (len < 2)
		return len == 1 ? p[0] : 0;
	for (int i = 0; i < len - 2; i++) {
		crc ^= (uint16_t)p[i] << 8;
		for (int k = 0; k < 8; k++)
			crc = (crc & 0x8000) ? (uint16_t)(crc << 1 ^ 0x1021) : (uint16_t)(crc << 1);
	}
	return crc ^ p[len - 1] ^ (uint16_t)(p[len - 2] << 8);
}

static void header_nibbles(int len, int cr, int crc, uint8_t *n)
{
#define B(v, i) ((v) >> (i) & 1)
	n[0] = len >> 4;
	n[1] = len & 0xF;
	n[2] = (uint8_t)(cr << 1 | (crc ? 1 : 0));
	int c4 = B(n[0], 3) ^ B(n[0], 2) ^ B(n[0], 1) ^ B(n[0], 0);
	int c3 = B(n[0], 3) ^ B(n[1], 3) ^ B(n[1], 2) ^ B(n[1], 1) ^ B(n[2], 0);
	int c2 = B(n[0], 2) ^ B(n[1], 3) ^ B(n[1], 0) ^ B(n[2], 3) ^ B(n[2], 1);
	int c1 = B(n[0], 1) ^ B(n[1], 2) ^ B(n[1], 0) ^ B(n[2], 2) ^ B(n[2], 1) ^ B(n[2], 0);
	int c0 = B(n[0], 0) ^ B(n[1], 1) ^ B(n[2], 3) ^ B(n[2], 2) ^ B(n[2], 1) ^ B(n[2], 0);
	n[3] = (uint8_t)c4;
	n[4] = (uint8_t)(c3 << 3 | c2 << 2 | c1 << 1 | c0);
#undef B
}

/* codeword bits, first sent first: d0 d1 d2 d3, then the parity bits */
static int hamming(uint8_t nib, int cr, uint8_t *bits)
{
	int d0 = nib & 1, d1 = nib >> 1 & 1, d2 = nib >> 2 & 1, d3 = nib >> 3 & 1;
	uint8_t p[4] = { d0 ^ d1 ^ d2, d1 ^ d2 ^ d3, d0 ^ d1 ^ d3, d0 ^ d2 ^ d3 };

	bits[0] = d0; bits[1] = d1; bits[2] = d2; bits[3] = d3;
	if (cr == 1) {
		bits[4] = d0 ^ d1 ^ d2 ^ d3;
		return 5;
	}
	for (int i = 0; i < cr; i++)
		bits[4 + i] = p[i];
	return 4 + cr;
}

static uint16_t gray_inv(uint16_t g)
{
	uint16_t v = g;

	for (uint16_t s = g >> 1; s; s >>= 1)
		v ^= s;
	return v;
}

int lora_encode(const uint8_t *payload, int len, const struct lora_code *c,
                uint16_t *syms, int max)
{
	/* nibbles: [header] whitened payload, low nibble first, [CRC] */
	uint8_t nib[5 + 2 * 255 + 4];
	int nn = 0;

	if (len < 0 || len > 255 || c->sf < 5 || c->sf > 12 || c->cr < 1 || c->cr > 4)
		return -1;
	if (!c->implicit) {
		header_nibbles(len, c->cr, c->crc, nib);
		nn = 5;
	}
	uint8_t w = 0xFF;
	for (int i = 0; i < len; i++) {
		uint8_t x = payload[i] ^ w;
		nib[nn++] = x & 0xF;
		nib[nn++] = x >> 4;
		w = whiten_next(w);
	}
	if (c->crc) {
		uint16_t crc = lora_crc16(payload, len);
		for (int k = 0; k < 4; k++)
			nib[nn++] = crc >> (4 * k) & 0xF;
	}

	int ns = 0, pos = 0, first = 1, n = 1 << c->sf;
	while (pos < nn || first) {
		int reduced = first || c->ldro;
		int sf_app = reduced ? c->sf - 2 : c->sf;
		int cw_cr = first ? 4 : c->cr;
		int cw_len = 4 + cw_cr;
		uint8_t cw[12][8];
		for (int r = 0; r < sf_app; r++)
			hamming(pos + r < nn ? nib[pos + r] : 0, cw_cr, cw[r]);
		pos += sf_app;
		if (ns + cw_len > max)
			return -1;
		/* diagonal interleaver: symbol i takes bit i of codeword (i - j - 1) */
		for (int i = 0; i < cw_len; i++) {
			uint16_t v = 0;
			int par = 0;
			for (int j = 0; j < sf_app; j++) {
				int r = ((i - j - 1) % sf_app + sf_app) % sf_app;
				v = (uint16_t)(v << 1 | cw[r][i]);
				par ^= cw[r][i];
			}
			if (reduced)                  /* reduced rate: a parity bit and a zero */
				v = (uint16_t)(v << 2 | par << 1);
			syms[ns++] = (uint16_t)((gray_inv(v) + 1) & (n - 1));
		}
		first = 0;
	}
	return ns;
}
