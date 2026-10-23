#include <math.h>
#include <string.h>
#include <stdio.h>

#include "gfsk.h"
#include "gfsk_rx.h"
#include "gpio.h"
#include "hw.h"
#include "lora.h"
#include "radio.h"
#include "rf.h"
#include "timer.h"

/* The sample rate sets how far the NCO can reach from the LO. At 61.44 MSPS a
 * slice is 35.2 MHz wide (offsets up to +-17.6 MHz, inside the TX analog
 * filter), so two slices cover the 2.4 GHz channels FrSky uses (2404 - 2474.2
 * MHz). Lower rates (for captures) get slices of 80% of the rate. */
#define DEFAULT_RATE    61.44e6
#define MAX_SLICE_HZ    35.2e6
#define SLICE_BASE_HZ   2403.9e6    /* slice grid origin: two 61.44 MSPS slices cover
                                     * FrSky's 2404 - 2474.2 MHz */
#define BAND_CAL_HZ     2440e6      /* LO used for the start-up calibrations */
#define FE_CHAN         1           /* front-end word for AD9361 chain 1 */
#define FE_FDD          0xE5        /* TX on TX/RX, RX on RX2 (docs/hardware.md) */
#define FE_OFF          0x48
#define PA_SETTLE_MS    500
/* the pulse shapes the virtual chips use: CC2500 GFSK (cc2500.c), A7105 GFSK
 * (a7105.c), 2-FSK */
static const double precompute_bt[] = { 1.0, 0.7, 0 };

/* Output level: the DAC runs at a fixed level and the AD9361 attenuator sets the
 * power. Not calibrated: MAX_DBM is a rough estimate of the output at 0 dB
 * attenuation. */
#define DAC_DBFS        -3.0
#define MAX_DBM         3.0
#define MAX_TX_GAIN     89.75

static struct radio_stats stats;
static struct radio_modem cur_modem;
static double cur_gain = -1, lo_hz, band_hz = BAND_CAL_HZ;
static double rate = DEFAULT_RATE, slice = MAX_SLICE_HZ;
static int running, rx_follow, capture_next;

void radio_set_rate(double hz)
{
	rate = hz;
	slice = 0.8 * hz < MAX_SLICE_HZ ? 0.8 * hz : MAX_SLICE_HZ;
}

void radio_rx_follow(int on)
{
	rx_follow = on;
}

void radio_capture_next(void)
{
	capture_next = 1;
}

int radio_start(void)
{
	if (!rf_ready() && rf_init())
		return -1;
	/* (the driver's return value is the baseband bandwidth when the rate is
	 * unchanged, so keep the requested rate) */
	if (rf_set_rate(rate, NULL))
		return -1;
	if (rf_set_bw(RF_TX, 2 * slice, NULL))
		return -1;
	memset(&stats, 0, sizeof(stats));
	stats.rate_hz = rate;
	stats.slice_hz = slice;
	/* calibrate in the band now, not on the first packet (the driver runs its
	 * calibrations after a retune of more than 100 MHz); from here on the
	 * packets only need fast retunes */
	double actual;
	double actual_rx;
	if (rf_tune(RF_TX, band_hz, &actual) || rf_tune(RF_RX, band_hz, &actual_rx))
		return -1;
	stats.rx_lo_hz = actual_rx;
	cur_modem.baud = 0;              /* force a modem reload */
	cur_gain = -1;
	lo_hz = band_hz;
	stats.lo_hz = actual;
	gpio_fe_set(FE_CHAN - 1, FE_FDD);
	uint64_t t0 = timer_ticks();
	for (unsigned i = 0; i < sizeof(precompute_bt) / sizeof(precompute_bt[0]); i++)
		gfsk_precompute(precompute_bt[i]);   /* while the PA settles anyway */
	while (timer_ticks() - t0 < TIMER_MS(PA_SETTLE_MS))
		;
	running = 1;
	return 0;
}

void radio_stop(void)
{
	gpio_fe_set(FE_CHAN - 1, FE_OFF);
	running = 0;
}

int radio_running(void)
{
	return running;
}

int radio_tx_busy(void)
{
	return gfsk_busy() || lora_busy();
}

int radio_set_band(double hz)
{
	if (hz <= 0)
		hz = BAND_CAL_HZ;
	if (hz == band_hz)
		return 0;
	band_hz = hz;
	if (!running)
		return 0;
	/* a full tune: the driver runs its calibrations for the new band */
	double actual, actual_rx;
	if (rf_tune(RF_TX, band_hz, &actual) || rf_tune(RF_RX, band_hz, &actual_rx))
		return -1;
	lo_hz = band_hz;
	stats.lo_hz = actual;
	stats.rx_lo_hz = actual_rx;
	return 0;
}

/* centre of the slice holding freq_hz: on the 2.4 GHz grid, or with a slice
 * centred on the band (900 MHz: one slice holds a whole ExpressLRS domain at
 * 61.44 MSPS, so hops are NCO steps, no retune) */
static double slice_lo(double freq_hz)
{
	double base = band_hz == BAND_CAL_HZ ? SLICE_BASE_HZ : band_hz - slice / 2;
	return base + (floor((freq_hz - base) / slice) + 0.5) * slice;
}

/* LO and power for a packet at freq_hz: retune only when the channel leaves the
 * current slice, with a fast synthesiser-only retune (~0.3 ms; radio_start did
 * the calibrations). A full driver tune here took 2.4 ms, too long inside a
 * protocol callback. Returns the NCO offset from the LO in *offset. */
static int prepare(double freq_hz, double dbm, double *offset)
{
	if (!running || radio_tx_busy())
		return -1;
	double want = slice_lo(freq_hz);
	if (want != lo_hz) {
		uint64_t t0 = timer_ticks();
		double actual;
		if (rf_fast_tune(RF_TX, want, &actual, NULL, NULL, NULL))
			return -1;
		if (rx_follow && rf_fast_tune(RF_RX, want, &stats.rx_lo_hz, NULL, NULL, NULL))
			return -1;
		lo_hz = want;
		stats.lo_hz = actual;
		stats.last_retune_us = (uint32_t)((timer_ticks() - t0) / (CPU_HZ / 1000000u));
		if (stats.last_retune_us > stats.max_retune_us)
			stats.max_retune_us = stats.last_retune_us;
		stats.retunes++;
	}

	double gain = MAX_TX_GAIN - (MAX_DBM - dbm);
	if (gain < 0)
		gain = 0;
	if (gain > MAX_TX_GAIN)
		gain = MAX_TX_GAIN;
	if (gain != cur_gain) {
		rf_set_gain(RF_TX, gain, NULL);
		cur_gain = gain;
	}

	*offset = freq_hz - stats.lo_hz;
	stats.last_freq_hz = freq_hz;
	stats.packets++;
	return 0;
}

/* right before a packet starts: adc_capture, if the host asked for one */
static void capture_start(double freq_hz)
{
	if (capture_next) {
		wr32(ADC_CAPTURE_BASE, 1);      /* length set by the host */
		capture_next = 0;
		stats.cap_lo_hz = stats.lo_hz;
		stats.cap_freq_hz = freq_hz;
		stats.captures++;
	}
}

int radio_tx(double freq_hz, const struct radio_modem *m, double dbm,
             const uint8_t *data, int nbits)
{
	double offset;

	if (prepare(freq_hz, dbm, &offset))
		return -1;
	if (m->baud != cur_modem.baud || m->dev_hz != cur_modem.dev_hz || m->bt != cur_modem.bt) {
		gfsk_config(rate, m->baud, m->dev_hz, m->bt);
		gfsk_level(DAC_DBFS, 20.0);  /* the ramp length depends on the rate */
		cur_modem = *m;
	}
	gfsk_channel(offset);
	capture_start(freq_hz);
	return gfsk_send(data, nbits);
}

int radio_rx(double freq_hz, const struct gfsk_rx_cfg *c)
{
	if (!running)
		return -1;
	double offset = freq_hz - stats.rx_lo_hz;
	if (fabs(offset) > slice / 2) {
		double want = slice_lo(freq_hz);
		if (rf_fast_tune(RF_RX, want, &stats.rx_lo_hz, NULL, NULL, NULL))
			return -1;
		stats.retunes++;
		offset = freq_hz - stats.rx_lo_hz;
	}
	if (gfsk_rx_arm(rate, offset, c))
		return -1;
	stats.rx_arms++;
	return 0;
}

void radio_rx_stop(void)
{
	gfsk_rx_stop();
}

int radio_tx_lora(double freq_hz, const struct lora_cfg *c, double dbm,
                  const uint8_t *payload, int len)
{
	double offset;

	if (prepare(freq_hz, dbm, &offset))
		return -1;
	capture_start(freq_hz);
	return lora_send(rate, offset, DAC_DBFS, c, payload, len);
}

const struct radio_stats *radio_get_stats(void)
{
	return &stats;
}
