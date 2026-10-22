/* MultiTX MicroBlaze firmware: start-up and main loop. */
#include <stdio.h>

#include "ad9361_spi.h"
#include "console.h"
#include "gpio.h"
#include "hw.h"
#include "mpm.h"
#include "shell.h"
#include "timer.h"
#include "version.h"

/* Interrupts are not enabled yet; the vector at 0x10 lands here. */
void __attribute__((interrupt_handler)) irq_handler(void)
{
}

int main(void)
{
	timer_init();
	console_init();
	gpio_init();
	ad9361_spi_init();
	mpm_init();

	printf("\nMultiTX firmware %s (%s)\n", FW_VERSION, FW_BUILD_DATE);
	shell_init();

	uint32_t last_blink = 0;
	for (;;) {
		uint32_t now = timer_ms();

		mailbox.uptime_ms = now;
		if (now - last_blink >= 500) {
			last_blink = now;
			if (gpio_ctrl_get() & CTRL_LED)
				gpio_ctrl_clear(CTRL_LED);
			else
				gpio_ctrl_set(CTRL_LED);
		}
		shell_poll();
		mpm_poll();
		console_poll();
	}
}
