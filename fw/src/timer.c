#include "timer.h"

/* AXI Timer registers (PG079) */
#define TCSR0  0x00
#define TLR0   0x04
#define TCR0   0x08
#define TCSR1  0x10
#define TLR1   0x14
#define TCR1   0x18
#define TCSR_LOAD  (1u << 5)
#define TCSR_ENT   (1u << 7)
#define TCSR_CASC  (1u << 11)

void timer_init(void)
{
	wr32(TIMER_BASE + TCSR0, 0);
	wr32(TIMER_BASE + TCSR1, 0);
	wr32(TIMER_BASE + TLR0, 0);
	wr32(TIMER_BASE + TLR1, 0);
	wr32(TIMER_BASE + TCSR0, TCSR_LOAD);
	wr32(TIMER_BASE + TCSR1, TCSR_LOAD);
	wr32(TIMER_BASE + TCSR1, 0);
	/* cascade mode: timer 0 + timer 1 form one 64-bit up counter */
	wr32(TIMER_BASE + TCSR0, TCSR_CASC | TCSR_ENT);
}

uint64_t timer_ticks(void)
{
	uint32_t hi, lo;

	do {
		hi = rd32(TIMER_BASE + TCR1);
		lo = rd32(TIMER_BASE + TCR0);
	} while (rd32(TIMER_BASE + TCR1) != hi);
	return ((uint64_t)hi << 32) | lo;
}

uint32_t timer_ms(void)
{
	return (uint32_t)(timer_ticks() / (CPU_HZ / 1000u));
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
