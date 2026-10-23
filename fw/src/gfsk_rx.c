#include <math.h>

#include "gfsk_rx.h"
#include "hw.h"

/* gfsk_rx registers (hw/ip/gfsk_rx/gfsk_rx.v) */
#define CTRL     0x00
#define STATUS   0x04
#define NBITS    0x08
#define ID       0x0C
#define CHAN_INC 0x10
#define DECIM    0x14
#define BAUD_INC 0x18
#define SYNC     0x1C
#define MASK     0x20
#define CFG      0x24
#define TSTAMP   0x28
#define FOFS     0x2C
#define LEVEL    0x30
#define SYNCERR  0x34
#define BUF(i)   (0x1000 + 4 * (i))

#define GFRX_ID  0x47465258u            /* "GFRX" */

int gfsk_rx_present(void)
{
	static int present = -1;

	if (present < 0)
		present = rd32(GFSK_RX_BASE + ID) == GFRX_ID;
	return present;
}

/* The rate-dependent settings (soft-float: computed when fs or the bit rate
 * change, not on every arm). Same maths as hw/ip/gfsk_rx/tb/check.py:
 * about 8 samples per bit after the CIC, its R^3 gain kept inside 18 bits,
 * the offset tracker over about 4 bits, DPLL gain 1/4. */
static double cur_fs, cur_baud, fs_dec;
static uint32_t decim_reg, baud_reg, tc;

static void rate_setup(double fs, double baud)
{
	if (fs == cur_fs && baud == cur_baud)
		return;
	int r = (int)(fs / (baud * 8));
	if (r < 1)
		r = 1;
	if (r > 256)
		r = 256;
	int shift = (int)ceil(3 * log2(r)) - 6;
	if (shift < 0)
		shift = 0;
	fs_dec = fs / r;
	decim_reg = (uint32_t)shift << 16 | (uint32_t)r;
	baud_reg = (uint32_t)llround(baud / fs_dec * 4294967296.0);
	tc = (uint32_t)lround(log2(fs_dec / baud * 4));
	if (tc < 1)
		tc = 1;
	cur_fs = fs;
	cur_baud = baud;
}

int gfsk_rx_arm(double fs, double offset_hz, const struct gfsk_rx_cfg *c)
{
	if (!gfsk_rx_present() || c->nbits <= 0 || c->nbits > 8 * GFSK_RX_BUF_BYTES)
		return -1;
	rate_setup(fs, c->baud);
	wr32(GFSK_RX_BASE + CHAN_INC, (uint32_t)(int32_t)lround(offset_hz / fs * 4294967296.0));
	wr32(GFSK_RX_BASE + DECIM, decim_reg);
	wr32(GFSK_RX_BASE + BAUD_INC, baud_reg);
	wr32(GFSK_RX_BASE + SYNC, c->sync);
	wr32(GFSK_RX_BASE + MASK, c->mask);
	wr32(GFSK_RX_BASE + CFG, 2u << 24 | tc << 16 | (c->invert ? 1u << 8 : 0) | (uint32_t)(c->maxerr & 0x3F));
	wr32(GFSK_RX_BASE + NBITS, (uint32_t)c->nbits);
	wr32(GFSK_RX_BASE + CTRL, 1);
	return 0;
}

void gfsk_rx_stop(void)
{
	if (gfsk_rx_present())
		wr32(GFSK_RX_BASE + CTRL, 2);
}

int gfsk_rx_searching(void)
{
	return gfsk_rx_present() && (rd32(GFSK_RX_BASE + STATUS) & 1);
}

int gfsk_rx_done(void)
{
	return gfsk_rx_present() && (rd32(GFSK_RX_BASE + STATUS) & 2);
}

int gfsk_rx_read(uint8_t *buf, int max, struct gfsk_rx_result *r)
{
	int n = ((int)rd32(GFSK_RX_BASE + NBITS) + 7) / 8;
	if (n > max)
		n = max;
	for (int i = 0; i < n; i++)
		buf[i] = (uint8_t)rd32(GFSK_RX_BASE + BUF(i));
	if (r) {
		r->tstamp = rd32(GFSK_RX_BASE + TSTAMP);
		r->offset_hz = (int16_t)rd32(GFSK_RX_BASE + FOFS) / 65536.0 * fs_dec;
		r->level = rd32(GFSK_RX_BASE + LEVEL);
		r->syncerr = (int)rd32(GFSK_RX_BASE + SYNCERR);
	}
	return n;
}
