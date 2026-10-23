#include "hw.h"
#include "irq.h"
#include "timer.h"

/* AXI INTC registers (PG099) */
#define INTC_ISR  0x00
#define INTC_IPR  0x04
#define INTC_IER  0x08
#define INTC_IAR  0x0C
#define INTC_MER  0x1C
#define MER_ME    (1u << 0)
#define MER_HIE   (1u << 1)

void irq_init(void)
{
	/* the INTC isn't reset when the host reloads the firmware */
	irq_save();
	wr32(INTC_BASE + INTC_IER, 0);
	wr32(INTC_BASE + INTC_IAR, 0xFFFFFFFFu);
	wr32(INTC_BASE + INTC_MER, MER_ME | MER_HIE);
}

void irq_enable_source(int n)
{
	wr32(INTC_BASE + INTC_IER, rd32(INTC_BASE + INTC_IER) | 1u << n);
}

/* The vector at 0x10 lands here, with interrupts off until it returns. */
void __attribute__((interrupt_handler)) irq_handler(void)
{
	uint32_t pending = rd32(INTC_BASE + INTC_IPR);

	if (pending & 1u << IRQ_TIMER)
		timer_irq();
	wr32(INTC_BASE + INTC_IAR, pending);
}
