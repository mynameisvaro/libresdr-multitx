/* ppm_in driver: PPM from a radio's trainer port (hw/ip/ppm_in). */
#ifndef PPM_H
#define PPM_H

#include <stdint.h>

int      ppm_present(void);                 /* the bitstream has ppm_in */
void     ppm_enable(int on, int falling);   /* decode on the pin; rising or falling edges */
int      ppm_enabled(void);
/* built-in generator in place of the pin (n channels in us, frame length in us);
 * n = 0 switches back to the pin */
void     ppm_test(int n, const uint16_t *us, uint32_t frame_us);
int      ppm_testing(void);
uint16_t ppm_frames(void);                  /* complete frames so far (wraps) */
int      ppm_channels(uint16_t *half_us);   /* last frame, in 0.5 us units; returns the count */
int      ppm_pin_level(void);

#endif
