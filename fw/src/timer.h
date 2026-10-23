/* 64-bit time base at CPU_HZ (sys_timer's timer 0 plus a wrap count) and a
 * one-shot alarm (timer 1) that calls a function from the interrupt. */
#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>
#include "hw.h"

#define TIMER_US(us) ((uint64_t)(us) * (CPU_HZ / 1000000u))
#define TIMER_MS(ms) ((uint64_t)(ms) * (CPU_HZ / 1000u))

void     timer_init(void);
uint64_t timer_ticks(void);
uint32_t timer_ms(void);
void     delay_us(uint32_t us);
void     delay_ms(uint32_t ms);

void     timer_alarm_fn(void (*fn)(void));  /* runs in the interrupt, interrupts off */
void     timer_alarm_at(uint64_t t);        /* at tick t (soon, if t has passed) */
void     timer_alarm_cancel(void);
void     timer_irq(void);                   /* from irq_handler */

#endif
