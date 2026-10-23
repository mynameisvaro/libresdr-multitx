/* Handset input from the PC: Multiprotocol serial frames in the mailbox. */
#ifndef INPUT_H
#define INPUT_H

#include <stdint.h>

void input_init(void);
void input_poll(void);                 /* main loop */
void input_console(void);              /* mpm start / stop: USB gives control back */
void input_select_console(void);       /* channels from the console only */
int  input_select_ppm(int falling);    /* channels from PPM on CN2; -1 without ppm_in */
void input_set_timeout(uint32_t ms);   /* no frame for this long: protocol paused */
void input_print_status(void);

#endif
