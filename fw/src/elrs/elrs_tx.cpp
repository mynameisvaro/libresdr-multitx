// ExpressLRS 4.x transmitter for the 900 MHz LoRa rates.
//
// The over-the-air format is ExpressLRS's own code, built unmodified from
// third_party/expresslrs: OTA.cpp (packet layouts, channel packing, CRC),
// FHSS.cpp and random.cpp (hop sequence and frequencies), crc.cpp. This file
// replaces tx_main.cpp, which is tied to ExpressLRS's hardware and timers: the
// rate table (common.cpp, RADIO_SX127X) and the slot logic of timerCallback(),
// SendRCdataToRF(), TXdoneISR() and GenerateSyncPacketData() are adapted from
// it (GPL-3.0, the ExpressLRS contributors).
//
// What a real ExpressLRS TX does that this one doesn't (yet): receive telemetry
// (so it stays "disconnected" and sends a sync packet every
// SyncPktIntervalDisconnected), bind mode, MSP data, sync spam on rate changes,
// Gemini, LBT.

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "OTA.h"
#include "FHSS.h"
#include "options.h"
#include "SX127xDriver.h"

#include "elrs.h"

extern "C" void md5(const void *data, size_t len, uint8_t out[16]);

firmware_options_t firmwareOptions;
const char version[] = "MultiTX";

// ---------------------------------------------------------------- rates
// common.cpp, RADIO_SX127X: ExpressLRS_AirRateConfig and ExpressLRS_AirRateRFperf
// (all at 500 kHz). cr is the LoRa coding rate 4/(4+cr).

// expresslrs_RFrates_e, expresslrs_tlm_ratio_e (common.h)
enum {
	RATE_LORA_900_25HZ = 0, RATE_LORA_900_50HZ, RATE_LORA_900_100HZ, RATE_LORA_900_100HZ_8CH,
	RATE_LORA_900_150HZ, RATE_LORA_900_200HZ, RATE_LORA_900_200HZ_8CH, RATE_LORA_900_250HZ,
	RATE_LORA_900_333HZ_8CH, RATE_LORA_900_500HZ, RATE_LORA_900_50HZ_DVDA,
};
enum {
	TLM_RATIO_STD = 0, TLM_RATIO_NO_TLM, TLM_RATIO_1_128, TLM_RATIO_1_64, TLM_RATIO_1_32,
	TLM_RATIO_1_16, TLM_RATIO_1_8, TLM_RATIO_1_4, TLM_RATIO_1_2,
};
enum { AUX1 = 4, CRSF_NUM_CHANNELS = 16 };       // eAuxChannels

struct rate {
	const char *name;
	uint8_t  enum_rate, sf, cr, preamble, tlm, hop;
	uint32_t interval_us;
	uint8_t  size, sends;
	uint16_t toa_us, sync_disc_ms, sync_conn_ms;
};

static const struct rate rates[ELRS_RATES] = {
	{ "200Hz",      RATE_LORA_900_200HZ,     6, 3,  8, TLM_RATIO_1_64, 4,  5000, OTA4_PACKET_SIZE, 1,  4380, 600, 5000 },
	{ "100Hz Full", RATE_LORA_900_100HZ_8CH, 6, 4,  8, TLM_RATIO_1_32, 4, 10000, OTA8_PACKET_SIZE, 1,  6690, 600, 5000 },
	{ "100Hz",      RATE_LORA_900_100HZ,     7, 3,  8, TLM_RATIO_1_32, 4, 10000, OTA4_PACKET_SIZE, 1,  8770, 600, 5000 },
	{ "50Hz",       RATE_LORA_900_50HZ,      8, 3, 10, TLM_RATIO_1_16, 4, 20000, OTA4_PACKET_SIZE, 1, 18560, 600, 5000 },
	{ "25Hz",       RATE_LORA_900_25HZ,      9, 3, 10, TLM_RATIO_1_8,  2, 40000, OTA4_PACKET_SIZE, 1, 29950, 600, 5000 },
	{ "D50",        RATE_LORA_900_50HZ_DVDA, 6, 3,  8, TLM_RATIO_1_64, 2,  5000, OTA4_PACKET_SIZE, 4,  4380, 600, 5000 },
};

// FHSS.cpp's domains[], in its order (firmwareOptions.domain indexes it). The
// table itself has internal linkage there, so the names and the first and last
// channel (for the radio's band centre) are repeated here.
static const struct {
	const char *name;
	double first_hz, last_hz;
} domains_info[] = {
	{ "AU915",  915500000, 926900000 },
	{ "FCC915", 903500000, 926900000 },
	{ "EU868",  863275000, 869575000 },
	{ "IN866",  865375000, 866950000 },
	{ "AU433",  433420000, 434420000 },
	{ "EU433",  433100000, 434450000 },
	{ "US433",  433250000, 438000000 },
	{ "US433W", 423500000, 438000000 },
};
#define NUM_DOMAINS (sizeof(domains_info) / sizeof(domains_info[0]))

static uint8_t tlm_to_denom(uint8_t e)            // TLMratioEnumToValue
{
	return e == TLM_RATIO_NO_TLM ? 1 : 1 << (8 + TLM_RATIO_NO_TLM - e);
}

// ---------------------------------------------------------------- configuration
static uint8_t cfg_domain = 1;                     // FCC915
static uint8_t cfg_switch_mode = smWideOr8ch;
static int     cfg_tlm = ELRS_TLM_STD;
static int     cfg_model_match;
static double  cfg_dbm = -10;

// the UID of the binding phrase "multitx" until one is set
static uint8_t cfg_uid[UID_LEN] = { 0 };
static int     have_uid;

extern "C" void elrs_set_uid(const uint8_t uid[6])
{
	memcpy(cfg_uid, uid, UID_LEN);
	have_uid = 1;
}

extern "C" void elrs_set_phrase(const char *phrase)
{
	// python/build_flags.py: md5('-DMY_BINDING_PHRASE="' + phrase + '"')[0:6]
	static const char pre[] = "-DMY_BINDING_PHRASE=\"";
	char buf[160];
	uint8_t d[16];
	size_t n = strlen(phrase);
	if (n > sizeof(buf) - sizeof(pre) - 1)
		n = sizeof(buf) - sizeof(pre) - 1;
	memcpy(buf, pre, sizeof(pre) - 1);
	memcpy(buf + sizeof(pre) - 1, phrase, n);
	buf[sizeof(pre) - 1 + n] = '"';
	md5(buf, sizeof(pre) + n, d);
	elrs_set_uid(d);
}

extern "C" void elrs_get_uid(uint8_t uid[6])
{
	if (!have_uid)
		elrs_set_phrase("multitx");
	memcpy(uid, cfg_uid, UID_LEN);
}

extern "C" int elrs_set_domain(const char *name)
{
	for (unsigned i = 0; i < NUM_DOMAINS; i++) {
		if (!strcasecmp(name, domains_info[i].name)) {
			cfg_domain = i;
			return 0;
		}
	}
	return -1;
}

extern "C" const char *elrs_domain_name(void)
{
	return domains_info[cfg_domain].name;
}

extern "C" double elrs_band_hz(void)
{
	return (domains_info[cfg_domain].first_hz + domains_info[cfg_domain].last_hz) / 2;
}

extern "C" void elrs_set_switch_mode(int mode) { cfg_switch_mode = mode; }
extern "C" void elrs_set_model_match(int on)   { cfg_model_match = on; }
extern "C" void elrs_set_power(double dbm)     { cfg_dbm = dbm; }

extern "C" void elrs_set_tlm(int denom)
{
	cfg_tlm = ELRS_TLM_STD;
	for (uint8_t e = TLM_RATIO_NO_TLM; e <= TLM_RATIO_1_2; e++)
		if (tlm_to_denom(e) == denom)
			cfg_tlm = e;
}

extern "C" const char *elrs_rate_name(int rate)
{
	return rate >= 0 && rate < ELRS_RATES ? rates[rate].name : "?";
}

// ---------------------------------------------------------------- link state
static const struct rate *cur;
static struct elrs_air air;
static uint8_t  tlm_enum, tlm_denom, model_id, sync_slot, rate_index;
static uint8_t  switch_mode;
static bool     tlm_gap;                         // TelemetryRcvPhase == ttrpPreReceiveGap
static uint32_t sync_last_ms;
static uint32_t ChannelData[CRSF_NUM_CHANNELS];
static uint32_t n_rc, n_sync, n_tlm, n_fail;
static double   last_freq;
static uint32_t t_start_ms, max_pack_us, max_send_us;
static volatile bool capture_sync;

static double freq_hz(uint32_t reg)              // register units (FREQ_STEP) -> Hz
{
	return reg * FREQ_STEP;
}

static uint32_t cur_freq_reg;

// SetRFLinkRate(): rate, CRC seed, hop sequence, start on the sync channel
extern "C" int elrs_init(int rate, int mid)
{
	if (rate < 0 || rate >= ELRS_RATES)
		return -2;
	cur = &rates[rate];
	rate_index = rate;
	elrs_get_uid(UID);
	firmwareOptions.domain = cfg_domain;
	model_id = mid & MODELMATCH_MASK;

	OtaUpdateCrcInitFromUid();
	FHSSrandomiseFHSSsequence(OtaGetUidSeed());
	switch_mode = cfg_switch_mode;
	if (cur->size == OTA4_PACKET_SIZE && switch_mode > smHybridOr16ch)
		switch_mode = smWideOr8ch;               // adjustSwitchModeForAirRate
	OtaUpdateSerializers((OtaSwitchMode_e)switch_mode, cur->size);

	tlm_enum = cfg_tlm == ELRS_TLM_STD ? cur->tlm : cfg_tlm;
	tlm_denom = tlm_to_denom(tlm_enum);
	air.sf = cur->sf;
	air.cr = cur->cr;
	air.preamble = cur->preamble;
	air.bw = 500e3;

	cur_freq_reg = FHSSgetInitialFreq();
	FHSSsetCurrIndex(0);
	OtaNonce = 0;
	sync_slot = 0;
	sync_last_ms = elrs_hw_millis() - cur->sync_disc_ms - 1;   // a sync soon after start
	tlm_gap = false;
	n_rc = n_sync = n_tlm = n_fail = 0;
	t_start_ms = elrs_hw_millis();
	max_pack_us = max_send_us = 0;
	linkStats.uplink_TX_Power = 1;               // CRSF power level: 10 mW (the closest)
	return 0;
}

// GenerateSyncPacketData(), without sync spam or rate changes
static void sync_data(OTA_Sync_s *s)
{
	s->fhssIndex = FHSSgetCurrIndex();
	s->nonce = OtaNonce;
	s->rfRateEnum = cur->enum_rate;
	s->switchEncMode = switch_mode;
	s->newTlmRatio = tlm_enum - TLM_RATIO_NO_TLM;
	s->geminiMode = 0;
	s->otaProtocol = 0;                          // TX_NORMAL_MODE
	s->UID4 = UID[4];
	s->UID5 = UID[5];
	if (cfg_model_match)
		s->UID5 ^= (~model_id) & MODELMATCH_MASK;
}

// the channels from the Multiprotocol runner, in ExpressLRS's CRSF scale (as its
// PPM input maps 988..2012 us), and the arm switch on CH5 (AUX1)
static void get_channels(void)
{
	for (int i = 0; i < CRSF_NUM_CHANNELS; i++)
		ChannelData[i] = fmap(elrs_hw_channel(i), 204, 1844, CRSF_CHANNEL_VALUE_STD_MIN,
		                      CRSF_CHANNEL_VALUE_STD_MAX);
	isArmed = CRSF_to_BIT(ChannelData[AUX1]);
}

// timerCallback() + SendRCdataToRF() + TXdoneISR()
extern "C" uint16_t elrs_callback(void)
{
	OtaNonce++;
	if (tlm_gap) {                               // the receiver's telemetry slot: silent
		tlm_gap = false;
		n_tlm++;
		return cur->interval_us;
	}

	uint32_t t0 = elrs_hw_micros();
	WORD_ALIGNED_ATTR OTA_Packet_s pkt;
	memset(&pkt, 0, sizeof(pkt));
	uint32_t now = elrs_hw_millis();
	uint8_t hop_res = OtaNonce % cur->hop;
	if ((sync_slot / 2) <= hop_res && now - sync_last_ms > cur->sync_disc_ms && FHSSonSyncChannel()) {
		pkt.std.type = PACKET_TYPE_SYNC;
		sync_data(OtaIsFullRes ? &pkt.full.sync.sync : &pkt.std.sync);
		sync_last_ms = now;
		sync_slot = (sync_slot + 1) % (cur->hop * 2);
		n_sync++;
		if (capture_sync) {
			capture_sync = false;
			elrs_hw_capture_next();
		}
	} else {
		get_channels();
		OtaPackChannelData(&pkt, ChannelData, false);
		n_rc++;
	}
	OtaGeneratePacketCrc(&pkt);

	last_freq = freq_hz(cur_freq_reg);
	uint32_t t1 = elrs_hw_micros();
	if (elrs_hw_send(last_freq, &air, cfg_dbm, (const uint8_t *)&pkt, cur->size))
		n_fail++;
	uint32_t t2 = elrs_hw_micros();
	if (n_rc + n_sync > 2) {                     // (the first packet also sets the TX gain)
		if (t1 - t0 > max_pack_us)
			max_pack_us = t1 - t0;
		if (t2 - t1 > max_send_us)
			max_send_us = t2 - t1;
	}

	// TXdoneISR(): hop for the next packet, and leave the next slot to telemetry
	if ((OtaNonce + 1) % cur->hop == 0)
		cur_freq_reg = FHSSgetNextFreq();
	if (tlm_denom != 1 && (OtaNonce + 1) % tlm_denom == 0)
		tlm_gap = true;
	return cur->interval_us;
}

extern "C" void elrs_capture_sync(void)
{
	capture_sync = true;
}

extern "C" void elrs_print_status(void)
{
	uint8_t u[UID_LEN];
	elrs_get_uid(u);
	printf("elrs UID %u,%u,%u,%u,%u,%u, domain %s, switch mode %u, tlm 1:%u%s, model match %s, %.1f dBm\n",
	       u[0], u[1], u[2], u[3], u[4], u[5], domains_info[cfg_domain].name, cfg_switch_mode,
	       tlm_to_denom(cfg_tlm == ELRS_TLM_STD ? (cur ? cur->tlm : TLM_RATIO_1_64) : cfg_tlm),
	       cfg_tlm == ELRS_TLM_STD ? " (rate default)" : "", cfg_model_match ? "on" : "off", cfg_dbm);
	if (!cur)
		return;
	printf("elrs rate %s (%u), SF%u CR4/%u, %lu us, nonce %u, fhss %u/%u, %.4f MHz\n",
	       cur->name, rate_index, cur->sf, 4 + cur->cr, (unsigned long)cur->interval_us, OtaNonce,
	       FHSSgetCurrIndex(), FHSSgetSequenceCount(), last_freq / 1e6);
	printf("elrs packets: %lu rc, %lu sync, %lu telemetry slots, %lu not sent (radio busy), "
	       "in %lu ms\n", (unsigned long)n_rc, (unsigned long)n_sync, (unsigned long)n_tlm,
	       (unsigned long)n_fail, (unsigned long)(elrs_hw_millis() - t_start_ms));
	printf("elrs slot time: packet %lu us, encode + start %lu us (longest)\n",
	       (unsigned long)max_pack_us, (unsigned long)max_send_us);
}
