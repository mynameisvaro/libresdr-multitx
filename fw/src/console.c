#include "console.h"
#include "hw.h"
#include "timer.h"

/* UART Lite registers */
#define UART_RX     0x00
#define UART_TX     0x04
#define UART_STAT   0x08
#define UART_CTRL   0x0C
#define STAT_RX_VALID  (1u << 0)
#define STAT_TX_FULL   (1u << 3)
#define CTRL_RST_FIFOS 0x03

/* How long to wait for the host to make room before dropping output. */
#define TX_WAIT_US  50000

volatile struct mailbox mailbox __attribute__((section(".mailbox")));

static int dropping;
static int last_from_uart;

void console_init(void)
{
	/* the mailbox isn't part of the loaded image, so it survives a reload */
	uint32_t session = mailbox.session + 1;

	mailbox.magic = 0;
	mailbox.tx_head = 0;
	mailbox.tx_tail = 0;
	mailbox.rx_head = 0;
	mailbox.rx_tail = 0;
	mailbox.tx_size = CONSOLE_TX_SIZE;
	mailbox.rx_size = CONSOLE_RX_SIZE;
	mailbox.tx_dropped = 0;
	mailbox.uptime_ms = 0;
	mailbox.session = session;
	mailbox.magic = MAILBOX_MAGIC;

	wr32(UART_BASE + UART_CTRL, CTRL_RST_FIFOS);
}

static void mailbox_putc(char c)
{
	uint32_t head = mailbox.tx_head;

	if (head - mailbox.tx_tail >= CONSOLE_TX_SIZE) {
		/* Full: give the host a moment to catch up, unless it already
		 * failed to (then drop until it reads again). */
		if (!dropping) {
			uint64_t t0 = timer_ticks();
			while (head - mailbox.tx_tail >= CONSOLE_TX_SIZE &&
			       timer_ticks() - t0 < TIMER_US(TX_WAIT_US))
				;
		}
		if (head - mailbox.tx_tail >= CONSOLE_TX_SIZE) {
			dropping = 1;
			mailbox.tx_dropped++;
			return;
		}
	}
	dropping = 0;
	mailbox.tx[head % CONSOLE_TX_SIZE] = c;
	mailbox.tx_head = head + 1;
}

static void uart_putc(char c)
{
	while (rd32(UART_BASE + UART_STAT) & STAT_TX_FULL)
		;
	wr32(UART_BASE + UART_TX, (uint8_t)c);
}

void console_write(const char *buf, int len)
{
	for (int i = 0; i < len; i++) {
		if (buf[i] == '\n') {
			mailbox_putc('\r');
			uart_putc('\r');
		}
		mailbox_putc(buf[i]);
		uart_putc(buf[i]);
	}
}

int console_getc(void)
{
	uint32_t tail = mailbox.rx_tail;

	if (mailbox.rx_head != tail) {
		int c = mailbox.rx[tail % CONSOLE_RX_SIZE] & 0xFF;
		mailbox.rx_tail = tail + 1;
		last_from_uart = 0;
		return c;
	}
	if (rd32(UART_BASE + UART_STAT) & STAT_RX_VALID) {
		last_from_uart = 1;
		return rd32(UART_BASE + UART_RX) & 0xFF;
	}
	return -1;
}

int console_from_uart(void)
{
	return last_from_uart;
}
