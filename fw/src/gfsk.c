#include <math.h>

#include "axi_ad9361.h"
#include "gfsk.h"
#include "hw.h"

/* gfsk_tx registers (hw/ip/gfsk_tx/gfsk_tx.v) */
#define CTRL      0x00
#define STATUS    0x04
#define NBITS     0x08
#define BAUD_INC  0x10
#define DEV_INC   0x14
#define CHAN_INC  0x18
#define AMPL      0x1C
#define FLAGS     0x20
#define SHAPE(k, pos) (0x1000 + 4 * (256 * (k) + (pos)))
#define PKT(i)    (0x2000 + 4 * (i))

#define CTRL_START  (1u << 0)
#define CTRL_CW     (1u << 1)
#define STATUS_BUSY (1u << 0)
#define TABLE_POS   256
#define FXOSC       26e6

static double cur_fs = 1.0;
static uint32_t ctrl_cw;

static uint32_t phase_inc(double hz, double fs)
{
	return (uint32_t)(int32_t)lround(hz / fs * 4294967296.0);
}

/* Gaussian-filtered rectangular frequency pulse, t in symbols, centred on 0 */
static double pulse(double t, double bt)
{
	if (bt <= 0)
		return (t >= -0.5 && t < 0.5) ? 1.0 : 0.0;
	double k = 2 * M_PI * bt / sqrt(log(2.0)) / sqrt(2.0);   /* 1 / (sigma sqrt 2) */
	return 0.5 * (erf(k * (t + 0.5)) - erf(k * (t - 0.5)));
}

/* A pulse table needs ~1500 soft-float erf() calls (~0.3 s on the MicroBlaze;
 * even 2-FSK's takes ~18 ms), far too long for a protocol callback, so tables are
 * computed ahead (gfsk_precompute) and kept, one per BT. */
#define NTABLES 3       /* the virtual chips use three BTs (radio.c) */
static struct {
	double  bt;
	int16_t t[3][TABLE_POS];
} tables[NTABLES];
static int ntables, next_slot;
static double loaded_bt = -2;    /* BT of the table in gfsk_tx's SHAPE memory */

static int16_t (*find_table(double bt))[TABLE_POS]
{
	for (int i = 0; i < ntables; i++)
		if (tables[i].bt == bt)
			return tables[i].t;
	return NULL;
}

void gfsk_precompute(double bt)
{
	if (find_table(bt))
		return;
	int slot = next_slot;
	next_slot = (next_slot + 1) % NTABLES;
	if (ntables < NTABLES)
		ntables++;
	int16_t (*table)[TABLE_POS] = tables[slot].t;
	/* SHAPE[k][pos]: previous (k = 0), current, next symbol's pulse at this
	 * position in the current symbol; renormalised so a run of ones gives 1.0 */
	for (int p = 0; p < TABLE_POS; p++) {
		double tau = (p + 0.5) / TABLE_POS, v[3], sum = 0;
		for (int k = 0; k < 3; k++) {
			v[k] = pulse(tau - (k - 1) - 0.5, bt);
			sum += v[k];
		}
		for (int k = 0; k < 3; k++)
			table[k][p] = (int16_t)lround(32767 * v[k] / sum);
	}
	tables[slot].bt = bt;
}

void gfsk_config(double fs, double baud, double dev_hz, double bt)
{
	cur_fs = fs;
	wr32(GFSK_TX_BASE + BAUD_INC, phase_inc(baud, fs));
	wr32(GFSK_TX_BASE + DEV_INC, phase_inc(dev_hz, fs));
	if (bt != loaded_bt) {
		gfsk_precompute(bt);      /* (immediate if it was precomputed) */
		int16_t (*table)[TABLE_POS] = find_table(bt);
		for (int k = 0; k < 3; k++)
			for (int p = 0; p < TABLE_POS; p++)
				wr32(GFSK_TX_BASE + SHAPE(k, p), (uint16_t)table[k][p]);
		loaded_bt = bt;
	}
	axi_ad9361_tx_source(AXI_AD9361_TX_MOD);
}

void gfsk_channel(double offset_hz)
{
	wr32(GFSK_TX_BASE + CHAN_INC, phase_inc(offset_hz, cur_fs));
}

void gfsk_level(double dbfs, double ramp_us)
{
	double a = 32767 * pow(10.0, dbfs / 20.0);
	if (a > 32767)
		a = 32767;
	double samples = ramp_us * 1e-6 * cur_fs;
	uint32_t step = samples >= 1 ? (uint32_t)ceil(a / samples) : 0;
	if (step > 0xFFFF)
		step = 0;
	wr32(GFSK_TX_BASE + AMPL, (step << 16) | (uint32_t)a);
}

void gfsk_bit_order(int lsb_first)
{
	wr32(GFSK_TX_BASE + FLAGS, lsb_first ? 1 : 0);
}

int gfsk_busy(void)
{
	return rd32(GFSK_TX_BASE + STATUS) & STATUS_BUSY;
}

int gfsk_send(const uint8_t *data, int nbits)
{
	if (nbits <= 0 || nbits > 8 * GFSK_BUF_BYTES || gfsk_busy())
		return -1;
	for (int i = 0; i < (nbits + 7) / 8; i++)
		wr32(GFSK_TX_BASE + PKT(i), data[i]);
	wr32(GFSK_TX_BASE + NBITS, nbits);
	wr32(GFSK_TX_BASE + CTRL, ctrl_cw | CTRL_START);
	return 0;
}

void gfsk_cw(int on)
{
	ctrl_cw = on ? CTRL_CW : 0;
	wr32(GFSK_TX_BASE + CTRL, ctrl_cw);
}

double gfsk_fs(void)
{
	return cur_fs;
}

double cc2500_baud(uint8_t mdmcfg4, uint8_t mdmcfg3)
{
	return (256 + mdmcfg3) * ldexp(FXOSC, (mdmcfg4 & 0x0F) - 28);
}

double cc2500_dev(uint8_t deviatn)
{
	return FXOSC / 131072.0 * (8 + (deviatn & 7)) * (1 << ((deviatn >> 4) & 7));
}
