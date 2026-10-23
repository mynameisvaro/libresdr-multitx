/* Console: a mailbox in code_ram read by the host over the USB bridge
 * (tools/usbdbg.py console), mirrored to the UART on CN2. */
#ifndef CONSOLE_H
#define CONSOLE_H

#include <stdint.h>

#define MAILBOX_ADDR      0x0007F000u   /* top 4 KB of code_ram */
#define MAILBOX_MAGIC     0x434F4E53u    /* "CONS" */
#define CONSOLE_TX_SIZE   2048           /* bytes, power of two */
#define CONSOLE_RX_SIZE   256            /* characters, one per 32-bit word */
#define INPUT_SLOTS       8              /* Multi serial frames from the host (input.c) */
#define INPUT_FRAME_MAX   36

/* input status bits (mailbox.in_status) */
#define IN_ST_USB         (1u << 0)      /* the host's frames are in control */
#define IN_ST_SIGNAL      (1u << 1)      /* a frame arrived within the timeout */
#define IN_ST_PAUSED      (1u << 2)      /* no signal: the protocol isn't sending */
#define IN_ST_RUNNING     (1u << 3)      /* a protocol is running */
#define IN_ST_BINDING     (1u << 4)
#define IN_ST_BAD_PROTO   (1u << 5)      /* the frame's protocol isn't built in */
#define IN_ST_PPM         (1u << 6)      /* channels from PPM (CN2) */

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
	/* handset input: Multiprotocol serial frames (offset 0xC40) */
	uint32_t in_head;     /* host: frames written to in[] */
	uint32_t in_tail;     /* fw: frames taken from in[] */
	uint32_t in_status;   /* fw: IN_ST_* */
	uint32_t in_model;    /* fw: running protocol | sub << 8 | RX number << 16 */
	uint32_t in_frames;   /* fw: frames accepted */
	uint32_t in_errors;   /* fw: bad frames and overruns */
	uint32_t in_reserved[2];
	struct {
		uint32_t len;                     /* bytes in data[] */
		uint8_t  data[INPUT_FRAME_MAX];
	} in[INPUT_SLOTS];              /* offset 0xC60, 40 bytes each */
};

extern volatile struct mailbox mailbox;

void console_init(void);
void console_write(const char *buf, int len);
int  console_getc(void);         /* -1 if nothing waiting */
int  console_from_uart(void);    /* last character came from the UART (needs echo) */
void console_poll(void);         /* drain the UART output buffer (main loop) */

#endif
