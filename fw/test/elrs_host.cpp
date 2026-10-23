// PC harness for fw/src/elrs (built by test_elrs.py with ExpressLRS's own OTA,
// FHSS and CRC code): runs the transmitter for a number of slots and prints what
// it sends, for comparison with tools/elrs.py.
//
//   elrs_host uid PHRASE
//   elrs_host run PHRASE RATE DOMAIN SWITCHMODE TLM MODELMATCH RXNUM N SEED T0_MS
//     prints one line per slot: nonce freq_hz hex-packet (or "-" for a silent slot),
//     the channels being random Multiprotocol values from SEED (CH5 too)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "elrs.h"

static uint64_t now_us;
static uint16_t chans[16];
static int sent;
static double sent_freq;
static uint8_t sent_pkt[16];
static int sent_len;

extern "C" int elrs_hw_send(double freq_hz, const struct elrs_air *air, double dbm,
                            const uint8_t *pkt, int len)
{
	sent = 1;
	sent_freq = freq_hz;
	memcpy(sent_pkt, pkt, len);
	sent_len = len;
	return 0;
}

extern "C" uint32_t elrs_hw_millis(void)
{
	return (uint32_t)(now_us / 1000);
}

extern "C" uint16_t elrs_hw_channel(int ch)
{
	return chans[ch];
}

extern "C" uint32_t elrs_hw_micros(void)
{
	return (uint32_t)now_us;
}

extern "C" void elrs_hw_capture_next(void)
{
}

static uint32_t lcg = 1;
static uint32_t rnd(void)
{
	lcg = lcg * 1103515245u + 12345u;
	return lcg >> 8;
}

int main(int argc, char **argv)
{
	if (argc == 3 && !strcmp(argv[1], "uid")) {
		uint8_t u[6];
		elrs_set_phrase(argv[2]);
		elrs_get_uid(u);
		printf("%02x%02x%02x%02x%02x%02x\n", u[0], u[1], u[2], u[3], u[4], u[5]);
		return 0;
	}
	if (argc != 12 || strcmp(argv[1], "run"))
		return 2;
	elrs_set_phrase(argv[2]);
	int rate = atoi(argv[3]);
	if (elrs_set_domain(argv[4]))
		return 2;
	elrs_set_switch_mode(atoi(argv[5]));
	elrs_set_tlm(atoi(argv[6]) ? atoi(argv[6]) : ELRS_TLM_STD);
	elrs_set_model_match(atoi(argv[7]));
	int rx = atoi(argv[8]), n = atoi(argv[9]);
	lcg = strtoul(argv[10], NULL, 0);
	now_us = strtoull(argv[11], NULL, 0) * 1000;
	if (elrs_init(rate, rx))
		return 2;
	for (int i = 0; i < n; i++) {
		for (int c = 0; c < 16; c++)
			chans[c] = rnd() % 2048;
		sent = 0;
		uint16_t period = elrs_callback();
		if (sent) {
			printf("%.3f ", sent_freq);
			for (int k = 0; k < sent_len; k++)
				printf("%02x", sent_pkt[k]);
		} else {
			printf("-");
		}
		printf(" ");
		for (int c = 0; c < 16; c++)
			printf("%s%u", c ? "," : "", chans[c]);
		printf("\n");
		now_us += period;
	}
	return 0;
}
