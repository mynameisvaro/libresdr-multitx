#include "irq.h"
#include "timer.h"

/* AXI Timer registers (PG079) */
#define TCSR0  0x00
#define TLR0   0x04
#define TCR0   0x08
#define TCSR1  0x10
#define TLR1   0x14
#define TCR1   0x18
#define TCSR_UDT   (1u << 1)    /* count down */
#define TCSR_ARHT  (1u << 4)    /* auto reload */
#define TCSR_LOAD  (1u << 5)
#define TCSR_ENIT  (1u << 6)
#define TCSR_ENT   (1u << 7)
#define TCSR_TINT  (1u << 8)    /* interrupt flag, write 1 to clear */

/* Timer 0 counts up from 0 and wraps every 43 s; its interrupt counts the
 * wraps in ticks_hi. Timer 1 is the alarm: a one-shot down counter. */
#define T0_MODE    (TCSR_ARHT | TCSR_ENIT | TCSR_ENT)
#define T1_MODE    (TCSR_UDT | TCSR_ENIT)

static volatile uint32_t ticks_hi;
static void (*alarm_fn)(void);

void timer_init(void)
{
	wr32(TIMER_BASE + TCSR0, TCSR_TINT);
	wr32(TIMER_BASE + TCSR1, TCSR_TINT);
	wr32(TIMER_BASE + TLR0, 0);
	wr32(TIMER_BASE + TCSR0, TCSR_LOAD);
	ticks_hi = 0;
	wr32(TIMER_BASE + TCSR0, T0_MODE);
	irq_enable_source(IRQ_TIMER);
}

uint64_t timer_ticks(void)
{
	uint32_t hi, lo, csr;

	do {
		hi = ticks_hi;
		lo = rd32(TIMER_BASE + TCR0);
		csr = rd32(TIMER_BASE + TCSR0);
	} while (hi != ticks_hi);
	/* wrapped, but the interrupt hasn't counted it yet (interrupts off, or
	 * it is about to run) */
	if ((csr & TCSR_TINT) && lo < 0x80000000u)
		hi++;
	return ((uint64_t)hi << 32) | lo;
}

uint32_t timer_ms(void)
{
	return (uint32_t)(timer_ticks() / (CPU_HZ / 1000u));
}

void timer_alarm_fn(void (*fn)(void))
{
	alarm_fn = fn;
}

void timer_alarm_at(uint64_t t)
{
	uint64_t now = timer_ticks();
	uint32_t n = t <= now ? 1 : t - now > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)(t - now);

	wr32(TIMER_BASE + TCSR1, TCSR_TINT);    /* stop, drop a pending alarm */
	wr32(TIMER_BASE + TLR1, n);
	wr32(TIMER_BASE + TCSR1, TCSR_LOAD);
	wr32(TIMER_BASE + TCSR1, T1_MODE | TCSR_ENT);
}

void timer_alarm_cancel(void)
{
	wr32(TIMER_BASE + TCSR1, TCSR_TINT);
}

/* From the interrupt handler, with interrupts off. */
void timer_irq(void)
{
	uint32_t csr = rd32(TIMER_BASE + TCSR0);

	if (csr & TCSR_TINT) {
		ticks_hi++;                     /* before the flag clears: see timer_ticks */
		wr32(TIMER_BASE + TCSR0, csr);
	}
	csr = rd32(TIMER_BASE + TCSR1);
	if (csr & TCSR_TINT) {
		wr32(TIMER_BASE + TCSR1, TCSR_TINT);   /* one-shot: stop and clear */
		if (alarm_fn)
			alarm_fn();
	}
}

void delay_us(uint32_t us)
{
	uint64_t t0 = timer_ticks();

	while (timer_ticks() - t0 < TIMER_US(us))
		;
}

void delay_ms(uint32_t ms)
{
	uint64_t t0 = timer_ticks();

	while (timer_ticks() - t0 < TIMER_MS(ms))
		;
}
