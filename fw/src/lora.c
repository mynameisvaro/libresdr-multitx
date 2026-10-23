#include <math.h>

#include "hw.h"
#include "lora.h"

/* lora_tx registers (hw/ip/lora_tx/lora_tx.v) */
#define CTRL      0x00
#define STATUS    0x04
#define NSYM      0x08
#define CHIP_INC  0x10
#define SF        0x14
#define CHAN_INC  0x18
#define AMPL      0x1C
#define FLAGS     0x20
#define ENTRY(i)  (0x1000 + 4 * (i))

#define T_UP      0
#define T_DOWN    1
#define T_QDOWN   2
#define MAX_SYMS  1024
#define RAMP_US   20.0

static int last_nsym;

static uint32_t phase_inc(double hz, double fs)
{
	return (uint32_t)(int32_t)lround(hz / fs * 4294967296.0);
}

static void entry(int i, int type, int value)
{
	wr32(LORA_TX_BASE + ENTRY(i), (uint32_t)(type << 12 | (value & 0xFFF)));
}

int lora_busy(void)
{
	return rd32(LORA_TX_BASE + STATUS) & 1;
}

int lora_last_symbols(void)
{
	return last_nsym;
}

int lora_send(double fs, double chan_hz, double dbfs, const struct lora_cfg *c,
              const uint8_t *payload, int len)
{
	static uint16_t syms[MAX_SYMS];

	if (lora_busy() || c->bw <= 0 || c->bw >= fs / 2)
		return -1;
	int head = c->preamble + 2 + 3;
	int n = lora_encode(payload, len, &c->code, syms, MAX_SYMS - head);
	if (n < 0)
		return -1;

	/* preamble, sync word (two chirps at 8x its nibbles), 2.25 down-chirps */
	int k = 0;
	for (int i = 0; i < c->preamble; i++)
		entry(k++, T_UP, 0);
	entry(k++, T_UP, (c->sync >> 4) * 8);
	entry(k++, T_UP, (c->sync & 0xF) * 8);
	entry(k++, T_DOWN, 0);
	entry(k++, T_DOWN, 0);
	entry(k++, T_QDOWN, 0);
	for (int i = 0; i < n; i++)
		entry(k++, T_UP, syms[i]);

	double a = 32767 * pow(10.0, dbfs / 20.0);
	if (a > 32767)
		a = 32767;
	uint32_t step = (uint32_t)ceil(a / (RAMP_US * 1e-6 * fs));
	if (step > 0xFFFF)
		step = 0;
	wr32(LORA_TX_BASE + CHIP_INC, phase_inc(c->bw, fs));
	wr32(LORA_TX_BASE + SF, c->code.sf);
	wr32(LORA_TX_BASE + CHAN_INC, phase_inc(chan_hz, fs));
	wr32(LORA_TX_BASE + AMPL, step << 16 | (uint32_t)a);
	wr32(LORA_TX_BASE + FLAGS, c->invert_iq ? 1 : 0);
	wr32(LORA_TX_BASE + NSYM, (uint32_t)k);
	wr32(LORA_TX_BASE + CTRL, 1);
	last_nsym = k;
	return 0;
}
