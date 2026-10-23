/* MultiTX MicroBlaze firmware: start-up and main loop. */
#include <stdio.h>

#include "ad9361_spi.h"
#include "console.h"
#include "gpio.h"
#include "hw.h"
#include "irq.h"
#include "mpm.h"
#include "shell.h"
#include "timer.h"
#include "version.h"

int main(void)
{
	irq_init();
	timer_init();
	console_init();
	gpio_init();
	ad9361_spi_init();
	mpm_init();

	printf("\nMultiTX firmware %s (%s)\n", FW_VERSION, FW_BUILD_DATE);
	shell_init();
	irq_on();           /* the protocol scheduler runs from the timer interrupt */

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
		console_poll();
	}
}
