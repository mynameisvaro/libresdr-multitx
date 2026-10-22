#include "gpio.h"
#include "hw.h"

static uint32_t ctrl_shadow;

void gpio_init(void)
{
	/* keep whatever the host may have set (e.g. AD9361 already out of reset) */
	ctrl_shadow = rd32(GPIO_CTRL_BASE + GPIO_DATA);
}

void gpio_ctrl_set(uint32_t bits)
{
	ctrl_shadow |= bits;
	wr32(GPIO_CTRL_BASE + GPIO_DATA, ctrl_shadow);
}

void gpio_ctrl_clear(uint32_t bits)
{
	ctrl_shadow &= ~bits;
	wr32(GPIO_CTRL_BASE + GPIO_DATA, ctrl_shadow);
}

uint32_t gpio_ctrl_get(void)
{
	return ctrl_shadow;
}

uint32_t gpio_ctrl_out(void)
{
	return rd32(GPIO_CTRL_BASE + GPIO2_DATA) & 0xFF;
}

void gpio_fe_set(int chan, uint32_t value)
{
	wr32(GPIO_FE_BASE + (chan ? GPIO2_DATA : GPIO_DATA), value & 0xFF);
}
