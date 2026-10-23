/* ExpressLRS 4.x transmitter, 900 MHz LoRa rates (elrs_tx.cpp), run as a
 * protocol by the Multiprotocol runner (mpm_start(ELRS_PROTO, rate, ...)).
 *
 * The packets, CRCs and hop sequence come from ExpressLRS's own code
 * (third_party/expresslrs: OTA.cpp, FHSS.cpp, crc.cpp, random.cpp). The receiver
 * must have the same binding phrase (there is no bind mode) and ExpressLRS 4.x:
 * 3.x is different over the air. */
#ifndef ELRS_H
#define ELRS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ELRS_PROTO 200          /* protocol number in mpm_start / Multi serial frames
                                 * (above Multiprotocol's own, which end at 127) */
#define ELRS_RATES 6            /* sub-protocols: 0 200Hz, 1 100Hz Full, 2 100Hz,
                                 * 3 50Hz, 4 25Hz, 5 D50 (ExpressLRS's SX127x table) */
#define ELRS_TLM_STD -1         /* elrs_set_tlm: the rate's own telemetry ratio */

/* configuration (kept across protocol starts) */
void        elrs_set_uid(const uint8_t uid[6]);
void        elrs_set_phrase(const char *phrase);      /* UID = MD5 as ExpressLRS's build does */
void        elrs_get_uid(uint8_t uid[6]);
int         elrs_set_domain(const char *name);        /* FCC915, EU868, ...; -1 unknown */
const char *elrs_domain_name(void);
double      elrs_band_hz(void);                       /* centre of the domain's channels */
void        elrs_set_switch_mode(int mode);           /* 0 Wide / 8ch, 1 Hybrid / 16ch, 2 12ch */
void        elrs_set_tlm(int denom);                  /* 1 (off), 2..128, or ELRS_TLM_STD */
void        elrs_set_model_match(int on);             /* model ID = the Multi RX number */
void        elrs_set_power(double dbm);
const char *elrs_rate_name(int rate);

/* protocol (mpm_core.cpp) */
int      elrs_init(int rate, int model_id);           /* 0 ok, -2 bad rate */
uint16_t elrs_callback(void);                         /* one packet slot; returns the period, us */
void     elrs_print_status(void);
void     elrs_capture_sync(void);                     /* adc_capture the next sync packet */

/* what elrs_tx.cpp needs from the board (elrs_hw.c; the PC test has its own) */
struct elrs_air {
	uint8_t sf, cr, preamble;   /* LoRa SF, coding rate 4/(4+cr), preamble */
	double  bw;                 /* Hz */
};
int      elrs_hw_send(double freq_hz, const struct elrs_air *air, double dbm,
                      const uint8_t *pkt, int len);   /* 0 when started */
uint32_t elrs_hw_millis(void);
uint32_t elrs_hw_micros(void);
uint16_t elrs_hw_channel(int ch);                     /* Multiprotocol scale, 204..1844 = +-100% */
void     elrs_hw_capture_next(void);                  /* adc_capture with the next packet sent */

#ifdef __cplusplus
}
#endif

#endif
