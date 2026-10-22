/* Console: a mailbox in code_ram read by the host over the USB bridge
 * (tools/usbdbg.py console), mirrored to the UART on CN2. */
#ifndef CONSOLE_H
#define CONSOLE_H

#include <stdint.h>

#define MAILBOX_ADDR      0x0007F000u   /* top 4 KB of code_ram */
#define MAILBOX_MAGIC     0x434F4E53u    /* "CONS" */
#define CONSOLE_TX_SIZE   2048           /* bytes, power of two */
#define CONSOLE_RX_SIZE   256            /* characters, one per 32-bit word */

/* Layout shared with tools/usbdbg.py. Counters are free running; each one is
 * written by one side only (fw = firmware, host = PC). */
struct mailbox {
	uint32_t magic;       /* fw: MAILBOX_MAGIC once the rest is valid */
	uint32_t session;     /* fw: new value at every boot */
	uint32_t tx_head;     /* fw: bytes written to tx[] */
	uint32_t tx_tail;     /* host: bytes read from tx[] */
	uint32_t rx_head;     /* host: characters written to rx[] */
	uint32_t rx_tail;     /* fw: characters taken from rx[] */
	uint32_t tx_size;     /* fw: CONSOLE_TX_SIZE */
	uint32_t rx_size;     /* fw: CONSOLE_RX_SIZE */
	uint32_t tx_dropped;  /* fw: bytes lost while the host wasn't reading */
	uint32_t uptime_ms;   /* fw: updated from the main loop (alive check) */
	uint32_t reserved[6];
	uint8_t  tx[CONSOLE_TX_SIZE];   /* offset 0x40 */
	uint32_t rx[CONSOLE_RX_SIZE];   /* offset 0x840 */
};

extern volatile struct mailbox mailbox;

void console_init(void);
void console_write(const char *buf, int len);
int  console_getc(void);         /* -1 if nothing waiting */
int  console_from_uart(void);    /* last character came from the UART (needs echo) */
void console_poll(void);         /* drain the UART output buffer (main loop) */

#endif
