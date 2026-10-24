#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "a7105.h"
#include "ad9361_spi.h"
#include "axi_ad9361.h"
#include "console.h"
#include "cc2500.h"
#include "elrs.h"
#include "gfsk.h"
#include "gfsk_rx.h"
#include "mpm.h"
#include "radio.h"
#include "gpio.h"
#include "hw.h"
#include "input.h"
#include "lora.h"
#include "ppm.h"
#include "irq.h"
#include "rf.h"
#include "shell.h"
#include "timer.h"
#include "version.h"

#define LINE_MAX  128
#define ARGS_MAX  16
#define PROMPT    "mtx> "

static char line[LINE_MAX];
static int  line_len;

static uint32_t num(const char *s)
{
	return strtoul(s, NULL, 0);
}

/* "0a1B2c" -> bytes; returns the count, -1 on a bad string */
static int hex_bytes(const char *s, uint8_t *out, int max)
{
	int n = 0;
	for (; s[0] && s[1] && n < max; s += 2) {
		char b[3] = { s[0], s[1], 0 }, *end;
		out[n++] = strtoul(b, &end, 16);
		if (*end)
			return -1;
	}
	return *s ? -1 : n;
}

static int cmd_help(int argc, char **argv);

static int cmd_info(int argc, char **argv)
{
	extern char _code_end[], _code_limit[], _heap_start[], _heap_end[];

	printf("MultiTX firmware %s, built %s\n", FW_VERSION, FW_BUILD_DATE);
	printf("uptime   %lu ms\n", (unsigned long)timer_ms());
	printf("code     %lu bytes of %lu\n", (unsigned long)_code_end, (unsigned long)_code_limit);
	printf("heap     %lu bytes at 0x%08lx\n",
	       (unsigned long)(_heap_end - _heap_start), (unsigned long)_heap_start);
	printf("console  session 0x%08lx, %lu bytes dropped\n",
	       (unsigned long)mailbox.session, (unsigned long)mailbox.tx_dropped);
	return 0;
}

static int cmd_md(int argc, char **argv)
{
	if (argc < 2)
		return -1;
	uint32_t addr = num(argv[1]) & ~3u;
	uint32_t n = argc > 2 ? num(argv[2]) : 1;

	for (uint32_t i = 0; i < n; i++) {
		if (i % 4 == 0)
			printf("%s%08lx:", i ? "\n" : "", (unsigned long)(addr + 4 * i));
		printf(" %08lx", (unsigned long)rd32(addr + 4 * i));
	}
	printf("\n");
	return 0;
}

static int cmd_mw(int argc, char **argv)
{
	if (argc < 3)
		return -1;
	uint32_t addr = num(argv[1]) & ~3u;

	for (int i = 2; i < argc; i++, addr += 4)
		wr32(addr, num(argv[i]));
	return 0;
}

static int cmd_ad(int argc, char **argv)
{
	if (argc < 2)
		return -1;
	uint16_t reg = num(argv[1]);

	if (!(gpio_ctrl_get() & CTRL_RESETB))
		printf("note: AD9361 is held in reset (run 'adreset')\n");
	if (argc > 2) {
		ad9361_spi_write(reg, num(argv[2]));
		return 0;
	}
	printf("0x%03x: 0x%02x\n", reg, ad9361_spi_read(reg));
	return 0;
}

static int cmd_adreset(int argc, char **argv)
{
	ad9361_hw_reset();
	uint8_t pid = ad9361_spi_read(0x037);
	printf("product ID 0x%02x%s\n", pid,
	       (pid & 0xF8) == 0x08 ? " (AD9361)" : " - not an AD9361 response");
	return 0;
}

static int rf_dir_arg(const char *s, enum rf_dir *dir)
{
	if (!strcmp(s, "rx"))
		*dir = RF_RX;
	else if (!strcmp(s, "tx"))
		*dir = RF_TX;
	else
		return -1;
	return 0;
}

static int cmd_rf(int argc, char **argv)
{
	enum rf_dir dir;
	double v;

	if (argc < 2)
		return -1;
	const char *sub = argv[1];

	if (!strcmp(sub, "init")) {
		uint32_t t0 = timer_ms();
		if (rf_init())
			return 0;
		printf("AD9361 initialised in %lu ms\n", (unsigned long)(timer_ms() - t0));
		return 0;
	}
	if (!strcmp(sub, "rate")) {
		int ret = argc > 2 ? rf_set_rate(strtod(argv[2], NULL), &v) : rf_get_rate(&v);
		if (!ret)
			printf("rate %.0f Hz (FPGA measures %.3f MHz)\n", v, axi_ad9361_clk_hz() / 1e6);
		return 0;
	}
	if (!strcmp(sub, "freq") && argc > 2 && !rf_dir_arg(argv[2], &dir)) {
		int ret = argc > 3 ? rf_tune(dir, strtod(argv[3], NULL), &v) : rf_get_freq(dir, &v);
		if (!ret)
			printf("%s LO %.0f Hz\n", argv[2], v);
		return 0;
	}
	if (!strcmp(sub, "gain") && argc > 3 && !rf_dir_arg(argv[2], &dir)) {
		if (!rf_set_gain(dir, strtod(argv[3], NULL), &v))
			printf("%s gain %.2f dB\n", argv[2], v);
		return 0;
	}
	if (!strcmp(sub, "bw") && argc > 3 && !rf_dir_arg(argv[2], &dir)) {
		if (!rf_set_bw(dir, strtod(argv[3], NULL), &v))
			printf("%s bandwidth %.0f Hz\n", argv[2], v);
		return 0;
	}
	if (!strcmp(sub, "fast") && argc > 3 && !rf_dir_arg(argv[2], &dir)) {
		/* rf fast rx|tx HZ: fast retune with a lock-register trace */
		uint8_t tr[300];
		int ntr = sizeof(tr);
		uint32_t us;
		if (rf_fast_tune(dir, strtod(argv[3], NULL), &v, &us, tr, &ntr)) {
			printf("fast tune failed\n");
			return 0;
		}
		printf("%s LO %.0f Hz; lock register every ~%lu us:", argv[2], v,
		       (unsigned long)(ntr ? us / ntr : 0));
		for (int i = 0; i < ntr; i++)
			if (i == 0 || tr[i] != tr[i - 1])
				printf(" [%u]=%02x", i, tr[i]);
		printf("\n");
		return 0;
	}
	if (!strcmp(sub, "rssi")) {
		if (!rf_rssi(&v))
			printf("RSSI %.2f dB\n", v);
		return 0;
	}
	if (!strcmp(sub, "temp")) {
		if (!rf_temperature(&v))
			printf("temperature %.1f C\n", v);
		return 0;
	}
	if (!strcmp(sub, "loopback") && argc > 2) {
		rf_loopback(num(argv[2]) != 0);
		return 0;
	}
	return -1;
}

static int cmd_dds(int argc, char **argv)
{
	double rate;

	if (argc < 2)
		return -1;
	if (!strcmp(argv[1], "off")) {
		axi_ad9361_dds_off();
		return 0;
	}
	if (rf_get_rate(&rate))
		return 0;
	double tone = strtod(argv[1], NULL);
	double dbfs = argc > 2 ? strtod(argv[2], NULL) : -12.0;
	axi_ad9361_dds(rate, tone, dbfs);
	printf("DDS %.0f Hz at %.1f dBFS (sample rate %.0f Hz)\n", tone, dbfs, rate);
	return 0;
}

/* RF front-end words (docs/hardware.md); same bit layout on both channels */
static const struct { const char *name; uint32_t word; } fe_states[] = {
	{ "off",  0x48 },   /* power-up default */
	{ "rx2",  0x4C },   /* RX from the RX2 port */
	{ "rx",   0x1A },   /* RX from the TX/RX port */
	{ "tx",   0xE1 },   /* TX on the TX/RX port */
	{ "fdd",  0xE5 },   /* TX on TX/RX + RX from RX2 */
};

static int cmd_fe(int argc, char **argv)
{
	if (argc < 3)
		return -1;
	int chan = num(argv[1]);
	if (chan < 1 || chan > 2)
		return -1;
	for (unsigned i = 0; i < sizeof(fe_states) / sizeof(fe_states[0]); i++) {
		if (!strcmp(argv[2], fe_states[i].name)) {
			gpio_fe_set(chan - 1, fe_states[i].word);
			return 0;
		}
	}
	gpio_fe_set(chan - 1, num(argv[2]));
	return 0;
}

static int hexval(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

/* hex string (spaces allowed between arguments) -> bytes; returns count or -1 */
static int parse_hex(int argc, char **argv, uint8_t *out, int max)
{
	int n = 0, half = -1;

	for (int a = 0; a < argc; a++) {
		for (const char *p = argv[a]; *p; p++) {
			int v = hexval(*p);
			if (v < 0)
				return -1;
			if (half < 0) {
				half = v;
			} else {
				if (n >= max)
					return -1;
				out[n++] = (half << 4) | v;
				half = -1;
			}
		}
	}
	return half < 0 ? n : -1;
}

static int cmd_gfsk(int argc, char **argv)
{
	static uint8_t pkt[GFSK_BUF_BYTES];
	double rate;

	if (argc < 2)
		return -1;
	const char *sub = argv[1];

	if (!strcmp(sub, "cfg") && argc > 3) {
		if (rf_get_rate(&rate))
			return 0;
		double bt = argc > 4 ? strtod(argv[4], NULL) : 0.5;
		gfsk_config(rate, strtod(argv[2], NULL), strtod(argv[3], NULL), bt);
		printf("%.0f baud, deviation %.0f Hz, %s (sample rate %.0f)\n",
		       strtod(argv[2], NULL), strtod(argv[3], NULL), bt > 0 ? "GFSK" : "2-FSK", rate);
		return 0;
	}
	if (!strcmp(sub, "cc2500") && argc > 2) {
		/* FrSky D8 / D16 settings from Multiprotocol's CC2500 tables */
		int d16 = !strcmp(argv[2], "d16");
		if (!d16 && strcmp(argv[2], "d8"))
			return -1;
		if (rf_get_rate(&rate))
			return 0;
		double baud = d16 ? cc2500_baud(0x7B, 0x61) : cc2500_baud(0xAA, 0x39);
		double dev = d16 ? cc2500_dev(0x51) : cc2500_dev(0x42);
		gfsk_config(rate, baud, dev, 1.0);
		printf("FrSky %s: %.1f baud, deviation %.1f Hz, GFSK BT 1\n", argv[2], baud, dev);
		return 0;
	}
	if (!strcmp(sub, "chan") && argc > 2) {
		gfsk_channel(strtod(argv[2], NULL));
		return 0;
	}
	if (!strcmp(sub, "level") && argc > 2) {
		gfsk_level(strtod(argv[2], NULL), argc > 3 ? strtod(argv[3], NULL) : 20.0);
		return 0;
	}
	if (!strcmp(sub, "cw") && argc > 2) {
		gfsk_cw(!strcmp(argv[2], "on"));
		return 0;
	}
	if (!strcmp(sub, "send") && argc > 2) {
		/* gfsk send [-c] HEX...: -c also starts adc_capture (length set by the host) */
		int cap = !strcmp(argv[2], "-c");
		int n = parse_hex(argc - 2 - cap, argv + 2 + cap, pkt, sizeof(pkt));
		if (n <= 0) {
			printf("bad hex data\n");
			return 0;
		}
		if (cap)
			wr32(ADC_CAPTURE_BASE, 1);
		if (gfsk_send(pkt, 8 * n))
			printf("busy\n");
		else
			printf("sending %d bytes\n", n);
		return 0;
	}
	if (!strcmp(sub, "status")) {
		printf("%s, sample rate %.0f\n", gfsk_busy() ? "busy" : "idle", gfsk_fs());
		return 0;
	}
	return -1;
}

static int cmd_mpm_locked(int argc, char **argv);

/* Runs live: status, ch and option don't pause the protocol (a channel value is
 * one 16-bit store), the rest take interrupts off. */
static int cmd_mpm(int argc, char **argv)
{
	if (argc < 2)
		return -1;
	const char *sub = argv[1];

	if (!strcmp(sub, "list")) {
		mpm_list();
		return 0;
	}
	if (!strcmp(sub, "ch") && argc > 3) {
		/* mpm ch N VALUE: channel 1..16, 0..2047 (1024 centre, 204/1844 = -/+100%) */
		mpm_set_channel(num(argv[2]) - 1, num(argv[3]));
		return 0;
	}
	if (!strcmp(sub, "option") && argc > 2) {
		/* mpm option N: -128..127, e.g. FrSky frequency fine tune while running */
		mpm_set_option(strtol(argv[2], NULL, 0));
		return 0;
	}
	if (!strcmp(sub, "hops") && argc > 3 && !strcmp(argv[2], "set")) {
		/* mpm hops set HEX: replace the running protocol's hop table */
		uint8_t h[50];
		int n = hex_bytes(argv[3], h, sizeof(h));
		if (n <= 0)
			return -1;
		uint32_t msr = irq_save();
		mpm_set_hopping_table(h, n);
		irq_restore(msr);
		printf("%d hop channels set\n", n);
		return 0;
	}
	if (!strcmp(sub, "hops")) {
		int n;
		const uint8_t *h = mpm_hopping_table(&n);
		for (int i = 0; i < n; i++)
			printf("%s%d", i ? " " : "", h[i]);
		printf("\n");
		return 0;
	}
	if (!strcmp(sub, "status")) {
		const struct radio_stats *r = radio_get_stats();
		const struct cc2500_info *c = cc2500_get_info();
		mpm_print_status();
		input_print_status();
		printf("radio %s, %lu packets, LO %.3f MHz, %lu retunes (last %lu us, max %lu us)\n",
		       radio_running() ? "on" : "off", (unsigned long)r->packets, r->lo_hz / 1e6,
		       (unsigned long)r->retunes, (unsigned long)r->last_retune_us,
		       (unsigned long)r->max_retune_us);
		printf("rate %.0f, slice %.0f, capture LO %.0f, capture freq %.0f, captures %lu\n", r->rate_hz,
		       r->slice_hz, r->cap_lo_hz, r->cap_freq_hz, (unsigned long)r->captures);
		printf("cc2500 state 0x%02x, %lu packets, %lu errors, last %.4f MHz, %.0f baud, dev %.0f Hz\n",
		       c->marcstate, (unsigned long)c->tx_packets, (unsigned long)c->tx_errors,
		       c->last_freq_hz / 1e6, c->baud, c->dev_hz);
		const struct a7105_info *a = a7105_get_info();
		printf("a7105 state 0x%02x, %lu packets, %lu errors, %lu received, last %.4f MHz, "
		       "%.0f baud, dev %.0f Hz, %d bits\n", a->state, (unsigned long)a->tx_packets,
		       (unsigned long)a->tx_errors, (unsigned long)a->rx_packets, a->last_freq_hz / 1e6,
		       a->baud, a->dev_hz, a->last_bits);
		return 0;
	}
	uint32_t msr = irq_save();
	int ret = cmd_mpm_locked(argc, argv);
	irq_restore(msr);
	return ret;
}

static int cmd_mpm_locked(int argc, char **argv)
{
	const char *sub = argv[1];

	if (!strcmp(sub, "start") && argc > 2) {
		/* mpm start PROTO [SUB [RX [OPTION [bind]]]] */
		int proto = mpm_proto_by_name(argv[2]);
		int subp = argc > 3 ? (int)num(argv[3]) : 0;
		int rx = argc > 4 ? (int)num(argv[4]) : 0;
		int opt = argc > 5 ? (int)strtol(argv[5], NULL, 0) : 0;
		int bind = argc > 6 && !strcmp(argv[6], "bind");
		if (proto < 0) {
			printf("unknown protocol '%s' (mpm list)\n", argv[2]);
			return 0;
		}
		input_console();
		if (radio_set_band(mpm_band_hz(proto))) {
			printf("radio band change failed\n");
			return 0;
		}
		if (!radio_running()) {
			printf("radio on (front end needs ~0.5 s)\n");
			if (radio_start()) {
				printf("radio start failed\n");
				return 0;
			}
		}
		int ret = mpm_start(proto, subp, rx, opt, bind);
		if (ret)
			printf("start failed (%s)\n", ret == -1 ? "protocol not built in" : "bad sub-protocol");
		else
			mpm_print_status();
		return 0;
	}
	if (!strcmp(sub, "binddone")) {
		mpm_bind_done();
		return 0;
	}
	if (!strcmp(sub, "stop")) {
		input_console();
		mpm_stop();
		radio_stop();
		return 0;
	}
	if (!strcmp(sub, "id") && argc > 2) {
		mpm_set_id(num(argv[2]));
		return 0;
	}
	if (!strcmp(sub, "rxid") && argc > 2) {
		/* mpm rxid HEX [RX]: AFHDS2A receiver ID (learned when binding) */
		mpm_set_afhds2a_rxid(argc > 3 ? (int)num(argv[3]) : 0, strtoul(argv[2], NULL, 16));
		return 0;
	}
	return -1;
}

static int cmd_radio(int argc, char **argv)
{
	if (argc > 2 && !strcmp(argv[1], "rate")) {
		if (radio_running()) {
			printf("stop the protocol first (mpm stop)\n");
			return 0;
		}
		radio_set_rate(strtod(argv[2], NULL));
		return 0;
	}
	if (argc > 2 && !strcmp(argv[1], "rxfollow")) {
		radio_rx_follow(num(argv[2]) != 0);
		return 0;
	}
	if (argc > 1 && !strcmp(argv[1], "poffset")) {
		if (argc > 2)
			radio_set_power_offset(strtod(argv[2], NULL));
		printf("power offset %+.1f dB\n", radio_power_offset());
		return 0;
	}
	if (argc > 1 && !strcmp(argv[1], "capture")) {
		radio_capture_next();
		return 0;
	}
	if (argc > 1 && !strcmp(argv[1], "start")) {
		/* radio on without a protocol, e.g. to listen with rf freq rx / adc_capture */
		if (!radio_running() && radio_start())
			printf("radio start failed\n");
		return 0;
	}
	if (argc > 1 && !strcmp(argv[1], "stop")) {
		if (mpm_running())
			printf("stop the protocol first (mpm stop)\n");
		else
			radio_stop();
		return 0;
	}
	return -1;
}

static int cmd_cc2500(int argc, char **argv)
{
	const struct cc2500_info *c = cc2500_get_info();
	const uint8_t *r = cc2500_regs();

	for (int i = 0; i < 0x2F; i++)
		printf("%s%02x", i % 16 ? " " : (i ? "\n" : ""), r[i]);
	printf("\nlast packet (%d bytes):", c->last_len);
	for (int i = 0; i < c->last_len; i++)
		printf(" %02x", c->last_pkt[i]);
	printf("\n");
	return 0;
}

static void a7105_print_coding(void)
{
	const struct a7105_coding *c = a7105_coding();
	printf("crc poly %04x init %04x reflect %d xorout %04x\nfec ", c->crc_poly, c->crc_init,
	       c->crc_reflect, c->crc_xorout);
	for (int i = 0; i < 16; i++)
		printf("%02x", c->fec[i]);
	printf(" %s\n", c->fec_low_first ? "lowfirst" : "highfirst");
}

static int cmd_a7105_locked(int argc, char **argv);

/* Runs live for the displays; changes to the chip take interrupts off. */
static int cmd_a7105(int argc, char **argv)
{
	if (argc == 1) {
		const struct a7105_info *a = a7105_get_info();
		const uint8_t *r = a7105_regs();
		for (int i = 0; i < 0x33; i++)
			printf("%s%02x", i % 16 ? " " : (i ? "\n" : ""), r[i]);
		printf("\nstate 0x%02x, %lu packets, %lu errors, %lu received\nlast payload (%d bytes):",
		       a->state, (unsigned long)a->tx_packets, (unsigned long)a->tx_errors,
		       (unsigned long)a->rx_packets, a->last_len);
		for (int i = 0; i < a->last_len; i++)
			printf(" %02x", a->last_pkt[i]);
		printf("\nreceive: %s, %lu times armed, %lu CRC errors, %lu FEC errors, offset %+.1f kHz, "
		       "level %lu\narmed %lu us after the packet start (max %lu)\nlast received (%d bytes):",
		       gfsk_rx_present() ? "gfsk_rx" : "no gfsk_rx",
		       (unsigned long)a->rx_arms, (unsigned long)a->rx_crc_errors,
		       (unsigned long)a->rx_fec_errors, a->rx_offset_hz / 1e3, (unsigned long)a->rx_level,
		       (unsigned long)a->rx_arm_us, (unsigned long)a->rx_arm_us_max, a->rx_len);
		for (int i = 0; i < a->rx_len; i++)
			printf(" %02x", a->last_rx[i]);
		printf("\n");
		a7105_print_coding();
		return 0;
	}
	if (!strcmp(argv[1], "coding")) {
		a7105_print_coding();
		return 0;
	}
	uint32_t msr = irq_save();
	int ret = cmd_a7105_locked(argc, argv);
	irq_restore(msr);
	return ret;
}

static int cmd_a7105_locked(int argc, char **argv)
{
	struct a7105_coding *c = a7105_coding();
	uint8_t b[64];

	if (!strcmp(argv[1], "crc") && argc > 3) {
		c->crc_poly = strtoul(argv[2], NULL, 16);
		c->crc_init = strtoul(argv[3], NULL, 16);
		c->crc_reflect = argc > 4 && num(argv[4]);
		c->crc_xorout = argc > 5 ? strtoul(argv[5], NULL, 16) : 0;
		return 0;
	}
	if (!strcmp(argv[1], "fec") && argc > 2) {
		if (!strcmp(argv[2], "default")) {
			a7105_coding_default();
			return 0;
		}
		if (hex_bytes(argv[2], b, 16) != 16) {
			printf("need 16 code words as 32 hex digits\n");
			return 0;
		}
		memcpy(c->fec, b, 16);
		c->fec_low_first = argc > 3 && !strcmp(argv[3], "lowfirst");
		return 0;
	}
	if (!strcmp(argv[1], "foffset")) {
		if (argc > 2)
			a7105_set_freq_offset(strtod(argv[2], NULL));
		printf("frequency offset %.0f Hz\n", a7105_freq_offset());
		return 0;
	}
	if (!strcmp(argv[1], "rx") && argc > 2) {
		int n = hex_bytes(argv[2], b, sizeof(b));
		if (n <= 0 || a7105_rx_queue(b, n))
			printf("bad packet\n");
		return 0;
	}
	return -1;
}

/* default: like ExpressLRS 2.4 GHz at 500 Hz (SF5, BW 812.5 kHz, implicit 8-byte
 * frames, 12 preamble chirps), but with the standard 4/6 coding: the SX1280's
 * long-interleaving 4/6 (ELRS) is not known yet */
static struct lora_cfg lora_cfg = {
	.code = { .sf = 5, .cr = 2, .crc = 0, .implicit = 1, .ldro = 0 },
	.bw = 812.5e3, .preamble = 12, .sync = 0x12, .invert_iq = 0,
};

static void lora_print_cfg(void)
{
	const struct lora_cfg *c = &lora_cfg;
	printf("SF%d, BW %.1f kHz, CR 4/%d, %s, %s, preamble %d, sync %02x, IQ %s%s; "
	       "%d chirps in the last frame, %s\n", c->code.sf, c->bw / 1e3, 4 + c->code.cr,
	       c->code.crc ? "crc" : "nocrc", c->code.implicit ? "implicit" : "explicit",
	       c->preamble, c->sync, c->invert_iq ? "inverted" : "normal", c->code.ldro ? ", ldro" : "",
	       lora_last_symbols(), lora_busy() ? "busy" : "idle");
}

static int cmd_lora(int argc, char **argv)
{
	struct lora_cfg *c = &lora_cfg;
	uint8_t b[255];

	if (argc == 1) {
		lora_print_cfg();
		if (!lora_present())
			printf("no lora_tx in this bitstream\n");
		return 0;
	}
	if (!strcmp(argv[1], "cfg") && argc > 4) {
		/* lora cfg SF BW CR [crc|nocrc] [implicit|explicit] [pre N] [sync HEX] [iqinv|iqnorm] [ldro] */
		c->code.sf = num(argv[2]);
		c->bw = strtod(argv[3], NULL);
		c->code.cr = num(argv[4]);
		c->code.ldro = 0;
		for (int i = 5; i < argc; i++) {
			if (!strcmp(argv[i], "crc"))           c->code.crc = 1;
			else if (!strcmp(argv[i], "nocrc"))    c->code.crc = 0;
			else if (!strcmp(argv[i], "implicit")) c->code.implicit = 1;
			else if (!strcmp(argv[i], "explicit")) c->code.implicit = 0;
			else if (!strcmp(argv[i], "iqinv"))    c->invert_iq = 1;
			else if (!strcmp(argv[i], "iqnorm"))   c->invert_iq = 0;
			else if (!strcmp(argv[i], "ldro"))     c->code.ldro = 1;
			else if (!strcmp(argv[i], "pre") && i + 1 < argc)  c->preamble = num(argv[++i]);
			else if (!strcmp(argv[i], "sync") && i + 1 < argc) c->sync = strtoul(argv[++i], NULL, 16);
			else return -1;
		}
		lora_print_cfg();
		return 0;
	}
	if (!strcmp(argv[1], "send") && argc > 3) {
		/* lora send FREQ HEX [DBM]: one frame (starts the radio if needed) */
		if (mpm_running()) {
			printf("stop the protocol first (mpm stop)\n");
			return 0;
		}
		int n = hex_bytes(argv[3], b, sizeof(b));
		if (n < 0) {
			printf("bad payload\n");
			return 0;
		}
		if (!radio_running() && radio_start()) {
			printf("radio start failed\n");
			return 0;
		}
		double dbm = argc > 4 ? strtod(argv[4], NULL) : -10;
		if (radio_tx_lora(strtod(argv[2], NULL), c, dbm, b, n))
			printf("send failed (busy, or bad parameters)\n");
		return 0;
	}
	return -1;
}

static int cmd_input(int argc, char **argv)
{
	if (argc == 1) {
		input_print_status();
		return 0;
	}
	if (argc > 2 && !strcmp(argv[1], "timeout")) {
		input_set_timeout(num(argv[2]));
		return 0;
	}
	if (!strcmp(argv[1], "console")) {
		input_select_console();
		return 0;
	}
	if (!strcmp(argv[1], "ppm")) {
		/* input ppm [falling]: channels from a radio's trainer port on CN2 */
		if (input_select_ppm(argc > 2 && !strcmp(argv[2], "falling")))
			printf("no ppm_in in this bitstream\n");
		return 0;
	}
	if (argc > 3 && !strcmp(argv[1], "range")) {
		/* input range MIN MAX: PPM pulse widths for -100% / +100% (default 988 2012) */
		mpm_set_ppm_range(num(argv[2]), num(argv[3]));
		return 0;
	}
	return -1;
}

/* Runs live: reads the decoder; the generator is plain register writes. */
static int cmd_ppm(int argc, char **argv)
{
	uint16_t v[16];

	if (!ppm_present()) {
		printf("no ppm_in in this bitstream\n");
		return 0;
	}
	if (argc == 1) {
		int lo, hi, n = ppm_channels(v);
		mpm_get_ppm_range(&lo, &hi);
		printf("decoder %s, source %s, pin %d, %u frames, %d channels:",
		       ppm_enabled() ? "on" : "off", ppm_testing() ? "test generator" : "pin",
		       ppm_pin_level(), (unsigned)ppm_frames(), n);
		for (int i = 0; i < n; i++)
			printf(" %u%s", v[i] / 2, v[i] & 1 ? ".5" : "");
		printf(" us (+-100%% = %d..%d)\n", lo, hi);
		return 0;
	}
	if (argc > 2 && !strcmp(argv[1], "test")) {
		/* ppm test US1 US2 ... [frame US] | ppm test off */
		if (!strcmp(argv[2], "off")) {
			ppm_test(0, NULL, 0);
			return 0;
		}
		int n = 0;
		uint32_t frame = 22500;
		for (int i = 2; i < argc; i++) {
			if (!strcmp(argv[i], "frame") && i + 1 < argc)
				frame = num(argv[++i]);
			else if (n < 16)
				v[n++] = num(argv[i]);
		}
		ppm_test(n, v, frame);
		if (!ppm_enabled())
			ppm_enable(1, 0);
		return 0;
	}
	return -1;
}

static int cmd_elrs(int argc, char **argv)
{
	if (argc == 1) {
		elrs_print_status();
		return 0;
	}
	const char *sub = argv[1];
	if (!strcmp(sub, "phrase") && argc > 2) {
		/* the words joined with single spaces */
		char p[128];
		size_t n = 0;
		p[0] = 0;
		for (int i = 2; i < argc && n < sizeof(p) - 1; i++) {
			int w = snprintf(p + n, sizeof(p) - n, "%s%s", i > 2 ? " " : "", argv[i]);
			n += w > 0 ? (size_t)w : 0;
		}
		elrs_set_phrase(p);
	} else if (!strcmp(sub, "uid") && argc > 2) {
		/* 6 bytes: 12 hex digits, or decimal with commas as ExpressLRS shows it */
		uint8_t u[6];
		if (strchr(argv[2], ',')) {
			char *s = argv[2];
			for (int i = 0; i < 6; i++) {
				u[i] = strtoul(s, &s, 10);
				if (i < 5 && *s++ != ',')
					return -1;
			}
		} else if (hex_bytes(argv[2], u, 6) != 6) {
			return -1;
		}
		elrs_set_uid(u);
	} else if (!strcmp(sub, "domain") && argc > 2) {
		if (elrs_set_domain(argv[2])) {
			printf("unknown domain (AU915 FCC915 EU868 IN866 AU433 EU433 US433 US433W)\n");
			return 0;
		}
	} else if (!strcmp(sub, "switch") && argc > 2) {
		elrs_set_switch_mode(num(argv[2]));
	} else if (!strcmp(sub, "tlm") && argc > 2) {
		elrs_set_tlm(!strcmp(argv[2], "std") ? ELRS_TLM_STD : !strcmp(argv[2], "off") ? 1 : (int)num(argv[2]));
	} else if (!strcmp(sub, "modelmatch") && argc > 2) {
		elrs_set_model_match(!strcmp(argv[2], "on") || !strcmp(argv[2], "1"));
	} else if (!strcmp(sub, "power") && argc > 2) {
		elrs_set_power(strtod(argv[2], NULL));
	} else if (!strcmp(sub, "capsync")) {
		elrs_capture_sync();            /* for tools/elrs_check.py */
		return 0;
	} else {
		return -1;
	}
	elrs_print_status();
	if (mpm_running())
		printf("(applies from the next mpm start, except power and model match)\n");
	return 0;
}

#define GRX_LOG_MAX   96
#define GRX_LOG_BYTES 72                /* an AFHDS2A packet with FEC: 40 x 14 bits */
struct grx_log_entry {
	uint32_t tstamp, level;
	int32_t  offset_hz;
	uint8_t  syncerr, len;
	uint8_t  bits[GRX_LOG_BYTES];
};
static struct grx_log_entry grx_log[GRX_LOG_MAX];
static int grx_log_n, grx_log_first;

static int cmd_grx(int argc, char **argv)
{
	if (!gfsk_rx_present()) {
		printf("no gfsk_rx in this bitstream\n");
		return 0;
	}
	if (argc == 1) {
		/* grx: status, and the bits kept (hex) when a packet is in */
		uint8_t b[GFSK_RX_BUF_BYTES];
		struct gfsk_rx_result r;
		int done = gfsk_rx_done();
		printf("%s", done ? "done" : gfsk_rx_searching() ? "searching" : "idle");
		if (done) {
			int n = gfsk_rx_read(b, sizeof(b), &r);
			printf(", sync errors %d, offset %+.1f kHz, level %lu, tstamp %lu\n", r.syncerr,
			       r.offset_hz / 1e3, (unsigned long)r.level, (unsigned long)r.tstamp);
			for (int i = 0; i < n; i++)
				printf("%02x", b[i]);
		}
		printf("\n");
		return 0;
	}
	if (!strcmp(argv[1], "arm") && argc > 5) {
		/* grx arm FREQ BAUD SYNC NBITS [MAXERR [MASK]] */
		struct gfsk_rx_cfg c = {
			.baud = strtod(argv[3], NULL), .sync = strtoul(argv[4], NULL, 16),
			.mask = argc > 7 ? strtoul(argv[7], NULL, 16) : 0xFFFFFFFF,
			.maxerr = argc > 6 ? (int)num(argv[6]) : 2, .invert = 0, .nbits = (int)num(argv[5]),
		};
		if (!radio_running() && radio_start()) {
			printf("radio start failed\n");
			return 0;
		}
		if (radio_rx(strtod(argv[2], NULL), &c))
			printf("arm failed\n");
		return 0;
	}
	if (!strcmp(argv[1], "log") && argc > 6) {
		/* grx log FREQ BAUD SYNC NBITS SECONDS [TRIG_BIT TRIG_HEX [POST]]: every
		 * packet in a row, re-armed right after each one (a few us), so a reply
		 * that follows a packet is caught too. A ring: it keeps the last
		 * GRX_LOG_MAX. With a trigger it stops POST packets (default 16) after
		 * the first one whose kept bits from TRIG_BIT on differ from TRIG_HEX.
		 * Read them, oldest first, with grx dump I. */
		struct gfsk_rx_cfg c = {
			.baud = strtod(argv[3], NULL), .sync = strtoul(argv[4], NULL, 16), .mask = 0xFFFFFFFF,
			.maxerr = 2, .invert = 0, .nbits = (int)num(argv[5]),
		};
		double freq = strtod(argv[2], NULL);
		int trig_bit = -1, trig_n = 0, post = 16, left = -1;
		uint8_t trig[16];
		if (argc > 8) {
			trig_bit = (int)num(argv[7]);
			trig_n = 4 * (int)strlen(argv[8]);
			if (trig_n > 8 * (int)sizeof(trig) || parse_hex(1, argv + 8, trig, sizeof(trig)) <= 0) {
				printf("bad trigger pattern\n");
				return 0;
			}
			if (argc > 9)
				post = (int)num(argv[9]);
		}
		if (!radio_running() && radio_start()) {
			printf("radio start failed\n");
			return 0;
		}
		grx_log_n = grx_log_first = 0;
		uint32_t seen = 0;
		int trig_at = -1;
		uint64_t end = timer_ticks() + TIMER_MS((uint64_t)(strtod(argv[6], NULL) * 1000));
		if (radio_rx(freq, &c)) {
			printf("arm failed\n");
			return 0;
		}
		while (left && timer_ticks() < end) {
			if (!gfsk_rx_done())
				continue;
			int slot = (grx_log_first + grx_log_n) % GRX_LOG_MAX;
			if (grx_log_n < GRX_LOG_MAX)
				grx_log_n++;
			else
				grx_log_first = (grx_log_first + 1) % GRX_LOG_MAX;
			struct grx_log_entry *e = &grx_log[slot];
			uint8_t b[GFSK_RX_BUF_BYTES];
			struct gfsk_rx_result r;
			int n = gfsk_rx_read(b, sizeof(b), &r);
			radio_rx(freq, &c);
			seen++;
			e->tstamp = r.tstamp;
			e->offset_hz = (int32_t)r.offset_hz;
			e->level = r.level;
			e->syncerr = (uint8_t)r.syncerr;
			e->len = (uint8_t)(n < GRX_LOG_BYTES ? n : GRX_LOG_BYTES);
			memcpy(e->bits, b, e->len);
			if (left > 0)
				left--;
			else if (trig_bit >= 0) {
				for (int k = 0; k < trig_n; k++) {
					int i = trig_bit + k;
					int got = i < 8 * n ? (b[i >> 3] >> (7 - (i & 7))) & 1 : 0;
					if (got != ((trig[k >> 3] >> (7 - (k & 7))) & 1)) {
						left = post;
						trig_at = (int)seen;
						break;
					}
				}
			}
		}
		radio_rx_stop();
		printf("%d packets kept of %lu", grx_log_n, (unsigned long)seen);
		if (trig_at > 0)
			printf(", trigger at packet %d (entry %d)", trig_at, grx_log_n - 1 - (int)(seen - (uint32_t)trig_at));
		printf("\n");
		return 0;
	}
	if (!strcmp(argv[1], "dump") && argc > 2) {
		/* grx dump I: tstamp (samples), offset Hz, level, sync errors, bits (hex) */
		int i = (int)num(argv[2]);
		if (i >= grx_log_n) {
			printf("no entry %d (%d logged)\n", i, grx_log_n);
			return 0;
		}
		struct grx_log_entry *e = &grx_log[(grx_log_first + i) % GRX_LOG_MAX];
		printf("%lu %ld %lu %u ", (unsigned long)e->tstamp, (long)e->offset_hz,
		       (unsigned long)e->level, e->syncerr);
		for (int k = 0; k < e->len; k++)
			printf("%02x", e->bits[k]);
		printf("\n");
		return 0;
	}
	if (!strcmp(argv[1], "stop")) {
		radio_rx_stop();
		return 0;
	}
	if (!strcmp(argv[1], "minlevel")) {
		if (argc > 2)
			gfsk_rx_set_min_level(num(argv[2]));
		printf("sync matches count from level %lu\n", (unsigned long)gfsk_rx_min_level());
		return 0;
	}
	return -1;
}

static int cmd_reboot(int argc, char **argv)
{
	printf("rebooting\n");
	delay_ms(10);
	((void (*)(void))0)();
	return 0;
}

static const struct shell_cmd cmds[] = {
	{ "help",    "",               "this list",                       cmd_help, SHELL_LIVE },
	{ "info",    "",               "version, uptime, memory",         cmd_info, SHELL_LIVE },
	{ "md",      "ADDR [N]",       "read N 32-bit words",             cmd_md, SHELL_LIVE },
	{ "mw",      "ADDR VALUE...",  "write 32-bit words",              cmd_mw, 0 },
	{ "ad",      "REG [VALUE]",    "AD9361 register read / write",    cmd_ad, 0 },
	{ "adreset", "",               "pulse AD9361 RESETB, read the ID", cmd_adreset, 0 },
	{ "rf",      "SUBCMD ...",     "AD9361: init | rate [HZ] | freq rx|tx [HZ] |\n"
	  "                             gain rx|tx DB | bw rx|tx HZ | rssi | temp | loopback 0|1", cmd_rf, 0 },
	{ "fe",      "1|2 STATE|WORD", "front end: off | rx2 | rx | tx | fdd, or a raw word", cmd_fe, 0 },
	{ "dds",     "HZ [DBFS] | off", "TX1 test tone from the FPGA DDS (default -12 dBFS)", cmd_dds, 0 },
	{ "gfsk",    "SUBCMD ...",     "modulator: cfg BAUD DEV [BT] | cc2500 d8|d16 | chan HZ |\n"
	  "                             level DBFS [RAMP_US] | cw on|off | send [-c] HEX | status", cmd_gfsk, 0 },
	{ "mpm",     "SUBCMD ...",     "Multiprotocol: list | start PROTO [SUB [RX [OPT [bind]]]] |\n"
	  "                             binddone | stop | ch N VALUE | option N | id HEX |\n"
	  "                             rxid HEX [RX] |\n"
	  "                             hops [set HEX] | status", cmd_mpm, SHELL_LIVE },
	{ "radio",   "SUBCMD ...",     "rate HZ | start | stop | rxfollow 0|1 | poffset [DB] |\n"
	  "                             capture (next packet)", cmd_radio, 0 },
	{ "cc2500",  "",               "virtual CC2500 registers and last packet", cmd_cc2500, SHELL_LIVE },
	{ "a7105",   "[SUBCMD ...]",   "virtual A7105: registers, last packet | coding |\n"
	  "                             crc POLY INIT [REFLECT [XOROUT]] | fec HEX32 [lowfirst] | fec default |\n"
	  "                             rx HEX (queue a received packet) | foffset [HZ]", cmd_a7105, SHELL_LIVE },
	{ "lora",    "[SUBCMD ...]",   "LoRa frames: cfg SF BW CR [crc|nocrc] [implicit|explicit] [pre N]\n"
	  "                             [sync HEX] [iqinv|iqnorm] [ldro] | send FREQ HEX [DBM]", cmd_lora, 0 },
	{ "input",   "[SUBCMD ...]",   "handset input: status | console | ppm [falling] | timeout MS |\n"
	  "                             range MIN MAX (PPM us for -/+100%)", cmd_input, SHELL_LIVE },
	{ "ppm",     "[test US... [frame US] | test off]", "PPM decoder (CN2 GPIO_0) and its test generator", cmd_ppm, SHELL_LIVE },
	{ "elrs",    "[SUBCMD ...]",   "ExpressLRS (mpm start elrs RATE [RX]): phrase WORDS | uid A,B,C,D,E,F |\n"
	  "                             domain NAME | switch 0|1|2 | tlm std|off|N | modelmatch on|off |\n"
	  "                             power DBM | capsync (capture the next sync packet)", cmd_elrs, SHELL_LIVE },
	{ "grx",     "[SUBCMD ...]",   "GFSK receiver: status (+ bits) | arm FREQ BAUD SYNC NBITS [MAXERR [MASK]] |\n"
	  "                             log FREQ BAUD SYNC NBITS SECONDS [TRIG_BIT TRIG_HEX [POST]] | dump I |\n"
	  "                             stop | minlevel [N]", cmd_grx, 0 },
	{ "reboot",  "",               "restart the firmware",            cmd_reboot, 0 },
};

static int cmd_help(int argc, char **argv)
{
	for (unsigned i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
		printf("  %-8s %-16s %s\n", cmds[i].name, cmds[i].args, cmds[i].help);
	return 0;
}

static void execute(char *s)
{
	char *argv[ARGS_MAX];
	int argc = 0;

	for (char *tok = strtok(s, " \t"); tok; tok = strtok(NULL, " \t")) {
		if (argc == ARGS_MAX) {
			printf("too many arguments (at most %d)\n", ARGS_MAX);
			return;
		}
		argv[argc++] = tok;
	}
	if (!argc)
		return;
	for (unsigned i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
		if (!strcmp(argv[0], cmds[i].name)) {
			uint32_t msr = cmds[i].flags & SHELL_LIVE ? 0 : irq_save();
			int ret = cmds[i].fn(argc, argv);
			if (!(cmds[i].flags & SHELL_LIVE))
				irq_restore(msr);
			if (ret < 0)
				printf("usage: %s %s\n", cmds[i].name, cmds[i].args);
			return;
		}
	}
	printf("unknown command '%s' (try 'help')\n", argv[0]);
}

void shell_init(void)
{
	line_len = 0;
	printf(PROMPT);
	fflush(stdout);
}

void shell_poll(void)
{
	int c;

	while ((c = console_getc()) >= 0) {
		int echo = console_from_uart();

		if (c == '\r' || c == '\n') {
			if (echo)
				printf("\n");
			line[line_len] = 0;
			execute(line);
			line_len = 0;
			printf(PROMPT);
		} else if (c == 0x08 || c == 0x7F) {
			if (line_len) {
				line_len--;
				if (echo)
					printf("\b \b");
			}
		} else if (c >= ' ' && line_len < LINE_MAX - 1) {
			line[line_len++] = c;
			if (echo)
				putchar(c);
		}
		fflush(stdout);
	}
}
