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
/* option byte (FrSky: frequency fine tune) while running; protocols that re-read it
 * in their callback (CC2500_SetFreqOffset) apply it on the next packet */
void mpm_set_option(int opt);
void mpm_print_status(void);
void mpm_list(void);
int  mpm_proto_by_name(const char *name);   /* name or number -> protocol, -1 */
const uint8_t *mpm_hopping_table(int *n);
/* AFHDS2A receiver ID per RX number (big endian: first byte on air first) */
void mpm_set_afhds2a_rxid(int rx_num, uint32_t id);
uint32_t mpm_get_afhds2a_rxid(int rx_num);

#endif
