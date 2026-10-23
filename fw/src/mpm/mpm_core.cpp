// Multiprotocol on MultiTX: one translation unit that builds Multiprotocol's
// protocol files (fw/third_party/multiprotocol, unmodified) against our
// Arduino shim and virtual radio chips.
//
// Multiprotocol.ino itself is AVR/STM32 specific (timers, USARTs, PPM input),
// so it is not compiled. This file provides what the protocol files need from
// it instead: the global variables, protocol start-up (after protocol_init()
// and modules_reset()), the ID helpers and a microsecond scheduler that runs
// the protocol callbacks (after loop()). Those parts are adapted from
// Multiprotocol.ino (GPL-3.0-or-later, Pascal Langer and contributors).

// ---- which radios and protocols are built in (normally _Config.h / Validate.h)
#define CC2500_INSTALLED
#define A7105_INSTALLED
#define FRSKYD_CC2500_INO
#define FRSKYX_CC2500_INO
#define FLYSKY_A7105_INO
#define AFHDS2A_A7105_INO
#define AETR                    // channel order, Multiprotocol's default (_Config.h)

#include "mpm_arduino.h"
#include "Multiprotocol.h"
#include "TX_Def.h"
#include "mpm_prototypes.h"

extern "C" {
#include "mpm.h"
}

// ---------------------------------------------------------------- globals
// (Multiprotocol.ino, "Global constants/variables")
uint32_t MProtocol_id;
uint32_t MProtocol_id_master;
uint32_t blink = 0, last_signal = 0;
uint16_t counter;
uint8_t  channel;
uint8_t  packet[50];
#define NUM_CHN 16
uint16_t Channel_data[NUM_CHN];
uint8_t  Channel_AUX;
uint8_t  cyrfmfg_id[6];
uint8_t  rx_tx_addr[5];
uint8_t  rx_id[5];
uint8_t  phase;
uint16_t bind_counter;
uint8_t  bind_phase;
uint8_t  binding_idx;
uint16_t packet_period;
uint8_t  packet_count;
uint8_t  packet_sent;
uint8_t  packet_length;
uint8_t  hopping_frequency[50];
uint8_t  *hopping_frequency_ptr;
uint8_t  hopping_frequency_no = 0;
uint8_t  rf_ch_num;
uint8_t  throttle, rudder, elevator, aileron;
uint8_t  flags;
uint16_t crc;
uint16_t crc16_polynomial;
uint8_t  crc8;
uint8_t  crc8_polynomial;
uint16_t seed;
uint16_t failsafe_count;
uint16_t state;
uint8_t  len;
uint8_t  armed, arm_flags, arm_channel_previous;
uint8_t  num_ch;
uint32_t pps_timer;
uint16_t pps_counter;
uint8_t  calData[50];
uint8_t CH_AETR[] = {AILERON, ELEVATOR, THROTTLE, RUDDER, CH5, CH6, CH7, CH8, CH9, CH10, CH11, CH12, CH13, CH14, CH15, CH16};
uint8_t CH_TAER[] = {THROTTLE, AILERON, ELEVATOR, RUDDER, CH5, CH6, CH7, CH8, CH9, CH10, CH11, CH12, CH13, CH14, CH15, CH16};
uint8_t CH_EATR[] = {ELEVATOR, AILERON, THROTTLE, RUDDER, CH5, CH6, CH7, CH8, CH9, CH10, CH11, CH12, CH13, CH14, CH15, CH16};
uint8_t mode_select;
uint8_t protocol_flags = 0, protocol_flags2 = 0, protocol_flags3 = 0;
uint8_t option_override;
uint8_t sub_protocol;
uint8_t protocol;
uint8_t option;
uint8_t cur_protocol[3];
uint8_t prev_option;
uint8_t prev_power = 0xFD;
uint8_t RX_num;
uint16_function_t remote_callback = 0;
#define TELEMETRY_BUFFER_SIZE 32
uint8_t packet_in[TELEMETRY_BUFFER_SIZE];
uint8_t multi_protocols_index = 0xFF;

// ---------------------------------------------------------------- EEPROM (RAM)
static uint8_t eeprom[2048];
uint8_t eeprom_read_byte(EE_ADDR a) { uintptr_t i = (uintptr_t)a; return i < sizeof(eeprom) ? eeprom[i] : 0xFF; }
void eeprom_write_byte(EE_ADDR a, uint8_t v) { uintptr_t i = (uintptr_t)a; if (i < sizeof(eeprom)) eeprom[i] = v; }

// ---------------------------------------------------------------- SPI bus
// Each virtual chip has its own chip select and ignores the bus when it isn't
// selected (returns 0xFF), so the bytes go to both and the selected one answers.
uint8_t SPI_Read(void) { return cc2500_spi(0x00) & a7105_spi(0x00); }
uint8_t SPI_SDI_Read(void) { return SPI_Read(); }
void SPI_Write(uint8_t b) { cc2500_spi(b); a7105_spi(b); }

// ---------------------------------------------------------------- helpers
// (Multiprotocol.ino)
static void set_rx_tx_addr(uint32_t id)
{
	rx_tx_addr[0] = (id >> 24) & 0xFF;
	rx_tx_addr[1] = (id >> 16) & 0xFF;
	rx_tx_addr[2] = (id >>  8) & 0xFF;
	rx_tx_addr[3] = (id >>  0) & 0xFF;
	rx_tx_addr[4] = (rx_tx_addr[2] & 0xF0) | (rx_tx_addr[3] & 0x0F);
}

static uint8_t __attribute__((unused)) bit_reverse(uint8_t b_in)
{
	uint8_t b_out = 0;
	for (uint8_t i = 0; i < 8; ++i) {
		b_out = (b_out << 1) | (b_in & 1);
		b_in >>= 1;
	}
	return b_out;
}

static void __attribute__((unused)) crc16_update(uint8_t a, uint8_t bits)
{
	crc ^= a << 8;
	while (bits--)
		crc = (crc & 0x8000) ? (crc << 1) ^ crc16_polynomial : crc << 1;
}

static void __attribute__((unused)) crc8_update(uint8_t byte)
{
	crc8 = crc8 ^ byte;
	for (uint8_t j = 0; j < 8; j++)
		crc8 = (crc8 & 0x80) ? (crc8 << 1) ^ crc8_polynomial : crc8 << 1;
}

// ---------------------------------------------------------------- protocol files
#include "Convert.ino"
#include "CC2500_SPI.ino"
#include "A7105_SPI.ino"
#include "FrSkyDVX_common.ino"
#include "FrSkyD_cc2500.ino"
#include "FrSkyX_cc2500.ino"
#include "FlySky_a7105.ino"
#include "AFHDS2A_a7105.ino"
#include "Multi_Protos.ino"

// ---------------------------------------------------------------- start-up + scheduler
// The TX ID would live in EEPROM on a Multiprotocol module. This board has no
// non-volatile storage the firmware can write, so the ID is fixed (it can be
// changed with mpm_set_id) and receivers stay bound across reboots.
#define MPM_DEFAULT_ID 0x4D545831u      // "MTX1"

static uint64_t next_cb;                // timer tick of the next callback
static uint32_t cb_count, cb_late, cb_max_late_us, cb_first_late_us, cb_max_us;

static void mpm_alarm(void);
static void mpm_stop_callbacks(void);

static void modules_reset(void)
{
	CC2500_Reset();
	A7105_Reset();
	prev_power = 0xFD;
}

extern "C" void mpm_init(void)
{
	timer_alarm_fn(mpm_alarm);
	MProtocol_id_master = MPM_DEFAULT_ID;
	for (int i = 0; i < NUM_CHN; i++)
		Channel_data[i] = CHANNEL_MID;
	Channel_data[THROTTLE] = CHANNEL_MIN_100;
}

extern "C" void mpm_set_id(uint32_t id)
{
	MProtocol_id_master = id;
}

extern "C" int mpm_start(int proto, int sub, int rx_num, int opt, int bind)
{
	mpm_stop_callbacks();
	protocol = proto;
	sub_protocol = sub;
	RX_num = rx_num & 0x3F;
	option = opt;
	crc16_polynomial = 0x1021;
	crc8_polynomial = 0x31;
	prev_option = option;
	multi_protocols_index = 0xFF;
	binding_idx = 0;
	MProtocol_id = RX_num + MProtocol_id_master;
	set_rx_tx_addr(MProtocol_id);
	SUB_PROTO_INVALID;
	option_override = 0xFF;
	if (bind)
		BIND_IN_PROGRESS;
	else
		BIND_DONE;

	for (uint8_t i = 0; multi_protocols[i].protocol != 0xFF; i++) {
		if (multi_protocols[i].protocol != protocol)
			continue;
		multi_protocols_index = i;
		if ((sub_protocol & 0x07) == 0 || (sub_protocol & 0x07) < multi_protocols[i].nbrSubProto)
			SUB_PROTO_VALID;
		if (!IS_SUB_PROTO_VALID)
			return -2;
		modules_reset();
		multi_protocols[i].Init();
		if (!IS_SUB_PROTO_VALID)
			return -2;
		cb_count = cb_late = cb_max_late_us = cb_first_late_us = cb_max_us = 0;
		next_cb = timer_ticks() + TIMER_US(5000);   // first callback in 5 ms
		remote_callback = multi_protocols[i].CallBack;
		timer_alarm_at(next_cb);
		return 0;
	}
	return -1;
}

// The callbacks run from the timer interrupt, so the shell holds interrupts off
// (irq_save) around the commands that change the protocol or touch the radio.
static void mpm_stop_callbacks(void)
{
	remote_callback = 0;
	timer_alarm_cancel();
}

extern "C" void mpm_stop(void)
{
	mpm_stop_callbacks();
	CC2500_Strobe(CC2500_SIDLE);
	A7105_Strobe(A7105_STANDBY);
}

extern "C" void mpm_bind_done(void)
{
	BIND_DONE;          // End_Bind() for the FrSky protocols; AFHDS2A sends its last
	                    // bind packets (with the RX ID it has) and starts
}

// AFHDS2A keeps the receiver's ID (learned while binding) in EEPROM, per RX
// number (AFHDS2A_init). The EEPROM is RAM here, so it can be set by hand.
static uint16_t afhds2a_rxid_addr(int rx_num)
{
	return rx_num < 16 ? AFHDS2A_EEPROM_OFFSET + rx_num * 4 : AFHDS2A_EEPROM_OFFSET2 + (rx_num - 16) * 4;
}

extern "C" void mpm_set_afhds2a_rxid(int rx_num, uint32_t id)
{
	uint16_t a = afhds2a_rxid_addr(rx_num & 0x3F);
	for (int i = 0; i < 4; i++)
		eeprom_write_byte((EE_ADDR)(uintptr_t)(a + i), id >> (24 - 8 * i));
}

extern "C" uint32_t mpm_get_afhds2a_rxid(int rx_num)
{
	uint16_t a = afhds2a_rxid_addr(rx_num & 0x3F);
	uint32_t id = 0;
	for (int i = 0; i < 4; i++)
		id = id << 8 | eeprom_read_byte((EE_ADDR)(uintptr_t)(a + i));
	return id;
}

extern "C" int mpm_running(void)
{
	return remote_callback != 0;
}

// Run the protocol callback when its time has come: the timer alarm calls this
// from the interrupt, so the console and the shell can't delay it. The callbacks
// return the delay to the next one in microseconds, measured from the previous
// deadline (like OCR1A += next_callback), so timing does not drift.
static void mpm_alarm(void)
{
	if (!remote_callback)
		return;
	uint64_t now = timer_ticks();
	if (now < next_cb) {            // (an alarm longer than the 32-bit timer)
		timer_alarm_at(next_cb);
		return;
	}
	uint32_t late_us = (uint32_t)((now - next_cb) / (CPU_HZ / 1000000u));
	if (late_us > cb_max_late_us)
		cb_max_late_us = late_us;
	if (late_us > 100)
		cb_late++;
	if (!cb_count++)
		cb_first_late_us = late_us;
	cc2500_poll();
	a7105_poll();
	uint16_t next = remote_callback();
	uint32_t took_us = (uint32_t)((timer_ticks() - now) / (CPU_HZ / 1000000u));
	if (took_us > cb_max_us)
		cb_max_us = took_us;
	next_cb += TIMER_US(next);
	if (next_cb < now)              // fell behind: resynchronise
		next_cb = now + TIMER_US(next);
	timer_alarm_at(next_cb);
}

extern "C" void mpm_set_channel(int ch, int value)
{
	if (ch >= 0 && ch < NUM_CHN)
		Channel_data[ch] = value < 0 ? 0 : value > 2047 ? 2047 : value;
}

extern "C" void mpm_set_option(int opt)
{
	option = opt;
}

extern "C" void mpm_print_status(void)
{
	if (multi_protocols_index != 0xFF && remote_callback) {
		const mm_protocol_definition *p = &multi_protocols[multi_protocols_index];
		printf("protocol %s (%d), sub %d, rx %d, option %d, %s\n", p->ProtoString, protocol,
		       sub_protocol, RX_num, (int8_t)option, IS_BIND_IN_PROGRESS ? "binding" : "bound");
	} else {
		printf("no protocol running\n");
	}
	printf("TX ID %08lx (rx_tx_addr %02x %02x %02x %02x)\n", (unsigned long)MProtocol_id,
	       rx_tx_addr[0], rx_tx_addr[1], rx_tx_addr[2], rx_tx_addr[3]);
	if (protocol == PROTO_AFHDS2A)
		printf("AFHDS2A RX ID %02x%02x%02x%02x (stored for RX %d)\n", rx_id[0], rx_id[1], rx_id[2],
		       rx_id[3], RX_num);
	printf("callbacks %lu, late (>100 us) %lu, worst %lu us\n", (unsigned long)cb_count,
	       (unsigned long)cb_late, (unsigned long)cb_max_late_us);
	printf("first callback %lu us late, longest callback %lu us\n",
	       (unsigned long)cb_first_late_us, (unsigned long)cb_max_us);
	printf("channels");
	for (int i = 0; i < 8; i++)
		printf(" %d", Channel_data[i]);
	printf("\n");
}

extern "C" void mpm_list(void)
{
	for (uint8_t i = 0; multi_protocols[i].protocol != 0xFF; i++) {
		const mm_protocol_definition *p = &multi_protocols[i];
		printf("  %3d  %-8s", p->protocol, p->ProtoString);
		uint8_t n = p->SubProtoString ? p->SubProtoString[0] : 0;
		for (uint8_t s = 0; n && s < p->nbrSubProto; s++)
			printf("  %d:%.*s", s, n, p->SubProtoString + 1 + s * n);
		printf("\n");
	}
}

// "frskyd", "FrSky X", "15" -> protocol number (-1 if unknown)
extern "C" int mpm_proto_by_name(const char *name)
{
	char *end;
	long n = strtol(name, &end, 0);
	if (*name && !*end)
		return n;
	for (uint8_t i = 0; multi_protocols[i].protocol != 0xFF; i++) {
		const char *a = multi_protocols[i].ProtoString, *b = name;
		for (;;) {
			while (*a == ' ') a++;
			while (*b == ' ') b++;
			if (!*a || !*b || (*a | 0x20) != (*b | 0x20))
				break;
			a++, b++;
		}
		if (!*a && !*b)
			return multi_protocols[i].protocol;
	}
	return -1;
}

extern "C" const uint8_t *mpm_hopping_table(int *n)
{
	*n = 50;
	return hopping_frequency;
}
