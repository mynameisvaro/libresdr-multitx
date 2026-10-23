/* Handset input from the PC: Multiprotocol serial frames in the mailbox. */
#ifndef INPUT_H
#define INPUT_H

#include <stdint.h>

void input_init(void);
void input_poll(void);                 /* main loop */
void input_console(void);              /* console commands take control back */
void input_set_timeout(uint32_t ms);   /* no frame for this long: protocol paused */
void input_print_status(void);

#endif
