#include <stdio.h>

#include "console.h"
#include "input.h"
#include "irq.h"
#include "mpm.h"
#include "ppm.h"
#include "radio.h"
#include "timer.h"

/* Handset input, from one of:
 *  - USB: Multiprotocol serial frames (what EdgeTX sends a Multi module) that the
 *    PC writes into the mailbox (tools/multi_link.py). They take control when they
 *    arrive; console commands (mpm start / stop) take it back.
 *  - PPM: a radio's trainer port on CN2 (ppm_in), selected with `input ppm`. The
 *    channels come from it, the protocol is chosen on the console, as on a
 *    Multiprotocol module in PPM mode.
 * Without a frame for the timeout (Multiprotocol: 70 ms) the protocol is paused,
 * so the receiver goes to failsafe, and resumes with the next frame. */

#define DEFAULT_TIMEOUT_MS 70

enum { IN_CONSOLE, IN_USB, IN_PPM };
static int mode = IN_CONSOLE, signal_ok, bad_proto;
static uint16_t ppm_seen;
static uint32_t timeout_ms = DEFAULT_TIMEOUT_MS;
static uint64_t last_frame;
static uint32_t restarts;

_Static_assert(__builtin_offsetof(struct mailbox, in_head) == 0xC40, "mailbox layout");
_Static_assert(__builtin_offsetof(struct mailbox, in) == 0xC60, "mailbox layout");
_Static_assert(sizeof(struct mailbox) <= 0x1000, "mailbox size");

void input_init(void)
{
	mailbox.in_tail = mailbox.in_head;      /* frames left from before a reload */
	mailbox.in_status = 0;
	mailbox.in_model = 0xFFFFFFFFu;
	mailbox.in_frames = 0;
	mailbox.in_errors = 0;
}

/* mpm start / stop on the console: USB frames give up control (PPM keeps the
 * channels: there the console picks the protocol anyway) */
void input_console(void)
{
	if (mode == IN_USB)
		input_select_console();
}

void input_select_console(void)
{
	mode = IN_CONSOLE;
	signal_ok = 0;
	bad_proto = 0;
	mpm_pause(0);
}

int input_select_ppm(int falling)
{
	if (!ppm_present())
		return -1;
	ppm_enable(1, falling);
	ppm_seen = ppm_frames();
	mode = IN_PPM;
	signal_ok = 0;
	bad_proto = 0;
	mpm_pause(1);               /* until the first frame */
	return 0;
}

void input_set_timeout(uint32_t ms)
{
	timeout_ms = ms;
}

/* (Re)start the protocol a frame asked for. The radio start takes ~0.8 s the
 * first time; frames arriving meanwhile wait in the ring. */
static void start(const struct mpm_model *m)
{
	uint32_t msr = irq_save();
	mpm_stop();
	bad_proto = 0;
	if (radio_set_band(mpm_band_hz(m->proto)))
		bad_proto = 1;
	else if (!radio_running() && radio_start())
		bad_proto = 1;
	else if (mpm_start(m->proto, m->sub, m->rx, m->option, m->bind))
		bad_proto = 1;
	restarts++;
	irq_restore(msr);
}

static void frame(const uint8_t *b, int len)
{
	struct mpm_model m;

	if (mode != IN_USB) {
		mode = IN_USB;
		mpm_serial_reset();     /* the first frame selects the model */
	}
	uint32_t msr = irq_save();
	int r = mpm_serial_frame(b, len, &m);
	irq_restore(msr);
	if (r < 0) {
		mailbox.in_errors++;
		return;
	}
	mailbox.in_frames++;
	last_frame = timer_ticks();
	if (!signal_ok) {
		signal_ok = 1;
		mpm_pause(0);
	}
	if (r == 1)
		start(&m);
}

void input_poll(void)
{
	uint8_t buf[INPUT_FRAME_MAX];

	while (mailbox.in_tail != mailbox.in_head) {
		uint32_t t = mailbox.in_tail;
		if (mailbox.in_head - t > INPUT_SLOTS) {    /* overrun: the newest one */
			mailbox.in_errors++;
			t = mailbox.in_head - 1;
		}
		volatile typeof(mailbox.in[0]) *s = &mailbox.in[t % INPUT_SLOTS];
		uint32_t len = s->len;
		for (uint32_t i = 0; i < len && i < INPUT_FRAME_MAX; i++)
			buf[i] = s->data[i];
		mailbox.in_tail = t + 1;
		if (len > INPUT_FRAME_MAX)
			mailbox.in_errors++;
		else
			frame(buf, (int)len);
	}

	if (mode == IN_PPM && ppm_frames() != ppm_seen) {
		uint16_t half_us[16];
		ppm_seen = ppm_frames();
		int n = ppm_channels(half_us);
		uint32_t msr = irq_save();
		mpm_set_ppm(half_us, n);
		irq_restore(msr);
		mailbox.in_frames++;
		last_frame = timer_ticks();
		if (!signal_ok) {
			signal_ok = 1;
			mpm_pause(0);
		}
	}

	if (mode != IN_CONSOLE && signal_ok && timer_ticks() - last_frame > TIMER_MS(timeout_ms)) {
		signal_ok = 0;
		mpm_pause(1);
	}

	uint32_t st = 0;
	if (mode == IN_USB)
		st |= IN_ST_USB;
	if (mode == IN_PPM)
		st |= IN_ST_PPM;
	if (mode != IN_CONSOLE)
		st |= signal_ok ? IN_ST_SIGNAL : IN_ST_PAUSED;
	if (mpm_running())
		st |= IN_ST_RUNNING;
	if (mpm_binding())
		st |= IN_ST_BINDING;
	if (bad_proto)
		st |= IN_ST_BAD_PROTO;
	mailbox.in_status = st;
	mailbox.in_model = (uint32_t)mpm_protocol();
}

void input_print_status(void)
{
	static const char *names[] = { "console", "usb", "ppm" };
	uint32_t ago = mode != IN_CONSOLE && last_frame ? (uint32_t)((timer_ticks() - last_frame) / TIMER_MS(1)) : 0;

	printf("input %s%s, timeout %lu ms, %lu frames, %lu errors, %lu protocol starts", names[mode],
	       mode != IN_CONSOLE && !signal_ok ? ", no signal (paused)" : "",
	       (unsigned long)timeout_ms, (unsigned long)mailbox.in_frames,
	       (unsigned long)mailbox.in_errors, (unsigned long)restarts);
	if (mode != IN_CONSOLE)
		printf(", last frame %lu ms ago", (unsigned long)ago);
	if (bad_proto)
		printf(", protocol not built in or failed to start");
	printf("\n");
}
