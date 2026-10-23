/* Multiprotocol runner (mpm_core.cpp): starts protocols and runs their callbacks
 * from the timer interrupt. Call mpm_start, mpm_stop and anything else that
 * touches the protocol or the radio with interrupts off (irq_save). */
#ifndef MPM_H
#define MPM_H

#include <stdint.h>

void mpm_init(void);
void mpm_set_id(uint32_t id);
/* proto/sub as in Multiprotocol (FlySky = 1, FrSky D = 3, FrSky X = 15, AFHDS2A = 28). 0 ok, -1 unknown
 * protocol, -2 invalid sub-protocol. */
int  mpm_start(int proto, int sub, int rx_num, int option, int bind);
void mpm_stop(void);
void mpm_bind_done(void);
int  mpm_running(void);
void mpm_set_channel(int ch, int value); /* 0..2047, 1024 = centre */
uint16_t mpm_channel(int ch);
/* the RF band a protocol uses, for radio_set_band (0 = 2.4 GHz) */
double mpm_band_hz(int proto);
/* option byte (FrSky: frequency fine tune) while running; protocols that re-read it
 * in their callback (CC2500_SetFreqOffset) apply it on the next packet */
void mpm_set_option(int opt);
void mpm_print_status(void);
void mpm_pause(int on);                  /* hold the callbacks (no input signal) */
int  mpm_binding(void);
int  mpm_protocol(void);                 /* proto | sub << 8 | RX << 16, or -1 */

/* Multiprotocol serial frames (input.c). mpm_serial_frame applies the flags,
 * option and channels; it returns 1 when the protocol must be (re)started as in
 * *m, 0 otherwise, -1 for a bad frame. Call with interrupts off. */
struct mpm_model {
	int proto, sub, rx, option, bind;
};
int  mpm_serial_frame(const uint8_t *frame, int len, struct mpm_model *m);
void mpm_serial_reset(void);             /* the next frame counts as a new model */
/* PPM channel widths (0.5 us units) -> channels 1..n, as Multiprotocol maps
 * its PPM input. Call with interrupts off. */
void mpm_set_ppm(const uint16_t *half_us, int n);
void mpm_set_ppm_range(int min_us, int max_us);   /* +-100% end points (default 988..2012) */
void mpm_get_ppm_range(int *min_us, int *max_us);
void mpm_list(void);
int  mpm_proto_by_name(const char *name);   /* name or number -> protocol, -1 */
const uint8_t *mpm_hopping_table(int *n);
/* AFHDS2A receiver ID per RX number (big endian: first byte on air first) */
void mpm_set_afhds2a_rxid(int rx_num, uint32_t id);
uint32_t mpm_get_afhds2a_rxid(int rx_num);

#endif
