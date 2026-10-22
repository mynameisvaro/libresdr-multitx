/* gpio_ctrl (AD9361 control lines, user LED) and gpio_fe (RF front end). */
#ifndef GPIO_H
#define GPIO_H

#include <stdint.h>

void     gpio_init(void);
void     gpio_ctrl_set(uint32_t bits);
void     gpio_ctrl_clear(uint32_t bits);
uint32_t gpio_ctrl_get(void);         /* outputs as last written */
uint32_t gpio_ctrl_out(void);         /* CTRL_OUT[7:0] inputs */
void     gpio_fe_set(int chan, uint32_t value);

#endif
