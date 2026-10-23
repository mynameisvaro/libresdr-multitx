/* What the ExpressLRS transmitter (elrs_tx.cpp) needs from the board: a LoRa
 * frame through the radio layer, the time and the channels. ExpressLRS's 900 MHz
 * framing: sync word 0x12, implicit header, LoRa CRC off, IQ standard. */
#include "elrs.h"
#include "lora.h"
#include "mpm.h"
#include "radio.h"
#include "timer.h"

int elrs_hw_send(double freq_hz, const struct elrs_air *air, double dbm,
                 const uint8_t *pkt, int len)
{
	struct lora_cfg c = {
		.code = { .sf = air->sf, .cr = air->cr, .crc = 0, .implicit = 1, .ldro = 0 },
		.bw = air->bw,
		.preamble = air->preamble,
		.sync = 0x12,
		.invert_iq = 0,
	};
	/* The 200 Hz frames take 4.4 of the 5 ms. The first frame after a start also
	 * sets the TX gain (SPI) and starts late, so the next one may find it still
	 * on air: wait for it rather than drop a packet (at most 1 ms). */
	uint64_t t0 = timer_ticks();
	while (radio_tx_busy() && timer_ticks() - t0 < TIMER_US(1000))
		;
	return radio_tx_lora(freq_hz, &c, dbm, pkt, len);
}

uint32_t elrs_hw_millis(void)
{
	return timer_ms();
}

uint32_t elrs_hw_micros(void)
{
	return (uint32_t)(timer_ticks() / (CPU_HZ / 1000000u));
}

uint16_t elrs_hw_channel(int ch)
{
	return mpm_channel(ch);
}

void elrs_hw_capture_next(void)
{
	radio_capture_next();
}
