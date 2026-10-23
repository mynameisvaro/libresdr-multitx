/* Interrupts: the AXI INTC in front of the MicroBlaze, and critical sections. */
#ifndef IRQ_H
#define IRQ_H

#include <stdint.h>

/* sys_irq inputs (hw/multitx/system_bd.tcl) */
#define IRQ_UART   0
#define IRQ_TIMER  1
#define IRQ_SPI    2
#define IRQ_GFSK   3

void irq_init(void);                  /* INTC on, all sources masked, CPU interrupts off */
void irq_enable_source(int n);

#define MSR_IE 0x2u

/* CPU interrupts off; returns the previous state for irq_restore. */
static inline uint32_t irq_save(void)
{
	uint32_t msr;

	__asm__ volatile ("msrclr %0, %1" : "=r"(msr) : "i"(MSR_IE) : "memory");
	return msr;
}

static inline void irq_restore(uint32_t msr)
{
	if (msr & MSR_IE)
		__asm__ volatile ("msrset r0, %0" : : "i"(MSR_IE) : "memory");
}

static inline void irq_on(void)
{
	__asm__ volatile ("msrset r0, %0" : : "i"(MSR_IE) : "memory");
}

#endif
