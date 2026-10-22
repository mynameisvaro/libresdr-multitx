/* 64-bit time base: sys_timer's two counters cascaded, counting at CPU_HZ. */
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

#endif
