#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ad9361_spi.h"
#include "axi_ad9361.h"
#include "console.h"
#include "gpio.h"
#include "hw.h"
#include "rf.h"
#include "shell.h"
#include "timer.h"
#include "version.h"

#define LINE_MAX  128
#define ARGS_MAX  10
#define PROMPT    "mtx> "

static char line[LINE_MAX];
static int  line_len;

static uint32_t num(const char *s)
{
	return strtoul(s, NULL, 0);
}

static int cmd_help(int argc, char **argv);

static int cmd_info(int argc, char **argv)
{
	extern char _code_end[], _heap_start[], _heap_end[];

	printf("MultiTX firmware %s, built %s\n", FW_VERSION, FW_BUILD_DATE);
	printf("uptime   %lu ms\n", (unsigned long)timer_ms());
	printf("code     %lu bytes of 252 KB\n", (unsigned long)_code_end);
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

static int cmd_reboot(int argc, char **argv)
{
	printf("rebooting\n");
	delay_ms(10);
	((void (*)(void))0)();
	return 0;
}

static const struct shell_cmd cmds[] = {
	{ "help",    "",               "this list",                       cmd_help },
	{ "info",    "",               "version, uptime, memory",         cmd_info },
	{ "md",      "ADDR [N]",       "read N 32-bit words",             cmd_md },
	{ "mw",      "ADDR VALUE...",  "write 32-bit words",              cmd_mw },
	{ "ad",      "REG [VALUE]",    "AD9361 register read / write",    cmd_ad },
	{ "adreset", "",               "pulse AD9361 RESETB, read the ID", cmd_adreset },
	{ "rf",      "SUBCMD ...",     "AD9361: init | rate [HZ] | freq rx|tx [HZ] |\n"
	  "                             gain rx|tx DB | bw rx|tx HZ | rssi | temp | loopback 0|1", cmd_rf },
	{ "dds",     "HZ [DBFS] | off", "TX1 test tone from the FPGA DDS (default -12 dBFS)", cmd_dds },
	{ "reboot",  "",               "restart the firmware",            cmd_reboot },
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

	for (char *tok = strtok(s, " \t"); tok && argc < ARGS_MAX; tok = strtok(NULL, " \t"))
		argv[argc++] = tok;
	if (!argc)
		return;
	for (unsigned i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
		if (!strcmp(argv[0], cmds[i].name)) {
			if (cmds[i].fn(argc, argv) < 0)
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
