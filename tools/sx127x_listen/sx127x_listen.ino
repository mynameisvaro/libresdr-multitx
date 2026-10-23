// LoRa listener for an ESP32 + SX1278 (or SX1276), and the SBUS monitor in the
// same sketch. Prints every LoRa packet with a timestamp, RSSI, SNR and the
// chip's frequency error estimate, so tools/sx127x_listen.py can check what
// MultiTX sends with a real Semtech chip: ExpressLRS packets (CRC, nonce, hop
// timing) and the LoRa framing itself (SF6 implicit header, sync word).
//
// Wiring (classic ESP32 DevKit, Ai-Thinker Ra-02 pin names):
//   SX1278 3.3V -> 3V3 (not 5 V)    SX1278 GND  -> GND
//   SX1278 SCK  -> GPIO18           SX1278 MISO -> GPIO19
//   SX1278 MOSI -> GPIO23           SX1278 NSS  -> GPIO5
//   SX1278 RST  -> GPIO14           SX1278 DIO0 -> GPIO26
//   receiver SBUS -> GPIO16 (optional, as in sbus_monitor)
// Fit an antenna. The same pins suit ExpressLRS's Unified_ESP32_900_RX firmware.
//
// Commands (115200 baud, one per line):
//   f HZ                         listen on HZ (default 433100000, EU433's first channel)
//   lora SF BW CR LEN [crc] [explicit] [sync HEX] [pre N] [iqinv]
//                                LoRa settings; BW in Hz (7800..500000), CR 5..8
//   elrs                         ExpressLRS 900 MHz-style: SF6, 500 kHz, 4/7, 8 bytes,
//                                implicit, no CRC, sync 0x12 (the default)
//   status                       chip version, settings, counters
// Output:
//   pkt t=MICROS len=N rssi=DBM snr=DB fei=HZ crcerr=0|1 HEXBYTES
//   fps ...                      SBUS, once a second, as sbus_monitor prints it

#include <SPI.h>

#define PIN_SCK  18
#define PIN_MISO 19
#define PIN_MOSI 23
#define PIN_NSS  5
#define PIN_RST  14
#define PIN_DIO0 26

#define SBUS_RX_PIN 16
#define SBUS_TX_PIN 17
#define FRAME_GAP_US 2000

#define FXOSC 32000000.0

// SX127x LoRa registers
#define REG_FIFO          0x00
#define REG_OP_MODE       0x01
#define REG_FRF_MSB       0x06
#define REG_LNA           0x0C
#define REG_FIFO_ADDR_PTR 0x0D
#define REG_FIFO_RX_BASE  0x0F
#define REG_FIFO_RX_CUR   0x10
#define REG_IRQ_FLAGS     0x12
#define REG_RX_NB_BYTES   0x13
#define REG_PKT_SNR       0x19
#define REG_PKT_RSSI      0x1A
#define REG_MODEM_CONFIG1 0x1D
#define REG_MODEM_CONFIG2 0x1E
#define REG_PREAMBLE_MSB  0x20
#define REG_PREAMBLE_LSB  0x21
#define REG_PAYLOAD_LEN   0x22
#define REG_MODEM_CONFIG3 0x26
#define REG_FEI_MSB       0x28
#define REG_DETECT_OPT    0x31
#define REG_INVERT_IQ     0x33
#define REG_DETECT_THRESH 0x37
#define REG_SYNC_WORD     0x39
#define REG_INVERT_IQ2    0x3B
#define REG_DIO_MAPPING1  0x40
#define REG_VERSION       0x42

#define MODE_LORA   0x80
#define MODE_LF     0x08        // LowFrequencyModeOn (registers of the LF port, < 525 MHz)
#define MODE_SLEEP  0x00
#define MODE_STDBY  0x01
#define MODE_RXCONT 0x05

static const long bw_table[] = { 7800, 10400, 15600, 20800, 31250, 41700, 62500, 125000, 250000, 500000 };

struct cfg {
	uint32_t freq = 433100000;
	uint8_t sf = 6, cr = 7, len = 8, sync = 0x12;
	uint16_t preamble = 8;
	long bw = 500000;
	bool crc = false, explicit_hdr = false, iqinv = false;
} cfg;

static SPIClass spi(VSPI);
static volatile uint32_t irq_us;
static volatile bool irq_pending;
static uint32_t n_pkts, n_crcerr;
static uint8_t version;

static uint8_t rd(uint8_t a)
{
	spi.beginTransaction(SPISettings(8000000, MSBFIRST, SPI_MODE0));
	digitalWrite(PIN_NSS, LOW);
	spi.transfer(a & 0x7F);
	uint8_t v = spi.transfer(0);
	digitalWrite(PIN_NSS, HIGH);
	spi.endTransaction();
	return v;
}

static void wr(uint8_t a, uint8_t v)
{
	spi.beginTransaction(SPISettings(8000000, MSBFIRST, SPI_MODE0));
	digitalWrite(PIN_NSS, LOW);
	spi.transfer(a | 0x80);
	spi.transfer(v);
	digitalWrite(PIN_NSS, HIGH);
	spi.endTransaction();
}

static void IRAM_ATTR dio0_isr()
{
	irq_us = micros();
	irq_pending = true;
}

static uint8_t mode_bits()
{
	return MODE_LORA | (cfg.freq < 525000000 ? MODE_LF : 0);
}

// Settings as ExpressLRS's SX127x driver makes them (src/lib/SX127xDriver)
static void configure()
{
	wr(REG_OP_MODE, MODE_SLEEP);
	wr(REG_OP_MODE, mode_bits() | MODE_SLEEP);     // LoRa mode is set in sleep
	wr(REG_OP_MODE, mode_bits() | MODE_STDBY);

	uint64_t frf = ((uint64_t)cfg.freq << 19) / 32000000ULL;
	wr(REG_FRF_MSB, frf >> 16);
	wr(REG_FRF_MSB + 1, frf >> 8);
	wr(REG_FRF_MSB + 2, frf);

	int bwi = 9;
	for (int i = 0; i < 10; i++)
		if (bw_table[i] == cfg.bw)
			bwi = i;
	bool sf6 = cfg.sf == 6;
	bool implicit = sf6 || !cfg.explicit_hdr;      // SF6 only works with an implicit header
	wr(REG_MODEM_CONFIG1, bwi << 4 | (cfg.cr - 4) << 1 | (implicit ? 1 : 0));
	wr(REG_MODEM_CONFIG2, cfg.sf << 4 | (cfg.crc ? 0x04 : 0));
	wr(REG_MODEM_CONFIG3, 0x04);                    // AGC on, LDRO off
	wr(REG_PREAMBLE_MSB, cfg.preamble >> 8);
	wr(REG_PREAMBLE_LSB, cfg.preamble);
	wr(REG_PAYLOAD_LEN, cfg.len);
	wr(REG_SYNC_WORD, cfg.sync);
	wr(REG_LNA, 0x23);                              // max gain, boost on
	// SF6 detection settings, plus errata 2.3 (bit 7) at 500 kHz
	wr(REG_DETECT_OPT, (cfg.bw == 500000 ? 0x80 : 0) | 0x40 | (sf6 ? 0x05 : 0x03));
	wr(REG_DETECT_THRESH, sf6 ? 0x0C : 0x0A);
	// errata 2.1: sensitivity at 500 kHz
	if (cfg.bw == 500000) {
		wr(0x36, 0x02);
		wr(0x3A, cfg.freq < 525000000 ? 0x7F : 0x64);
	} else {
		wr(0x36, 0x03);
	}
	wr(REG_INVERT_IQ, cfg.iqinv ? 0x67 : 0x27);
	wr(REG_INVERT_IQ2, cfg.iqinv ? 0x19 : 0x1D);
	wr(REG_FIFO_RX_BASE, 0);
	wr(REG_DIO_MAPPING1, 0x00);                     // DIO0 = RxDone
	wr(REG_IRQ_FLAGS, 0xFF);
	wr(REG_OP_MODE, mode_bits() | MODE_RXCONT);
}

static void print_cfg()
{
	Serial.printf("cfg f=%lu sf=%u bw=%ld cr=4/%u len=%u crc=%u header=%s sync=0x%02x pre=%u iq=%s\n",
	              (unsigned long)cfg.freq, cfg.sf, cfg.bw, cfg.cr, cfg.len, cfg.crc,
	              (cfg.sf == 6 || !cfg.explicit_hdr) ? "implicit" : "explicit", cfg.sync, cfg.preamble,
	              cfg.iqinv ? "inverted" : "normal");
}

static void read_packet()
{
	uint8_t flags = rd(REG_IRQ_FLAGS);
	wr(REG_IRQ_FLAGS, 0xFF);
	if (!(flags & 0x40))
		return;
	bool crcerr = flags & 0x20;
	uint8_t n = (cfg.sf == 6 || !cfg.explicit_hdr) ? cfg.len : rd(REG_RX_NB_BYTES);
	wr(REG_FIFO_ADDR_PTR, rd(REG_FIFO_RX_CUR));
	uint8_t b[256];
	for (int i = 0; i < n; i++)
		b[i] = rd(REG_FIFO);
	int8_t snr_q = (int8_t)rd(REG_PKT_SNR);
	int rssi = (cfg.freq < 525000000 ? -164 : -157) + rd(REG_PKT_RSSI);
	int32_t fei = ((int32_t)(rd(REG_FEI_MSB) & 0x0F) << 16) | rd(REG_FEI_MSB + 1) << 8 | rd(REG_FEI_MSB + 2);
	if (fei & 0x80000)
		fei -= 0x100000;
	double fei_hz = fei * (double)(1 << 24) / FXOSC * (cfg.bw / 500000.0);
	n_pkts++;
	n_crcerr += crcerr;
	char line[600];
	int k = snprintf(line, sizeof(line), "pkt t=%lu len=%u rssi=%d snr=%.2f fei=%.0f crcerr=%d ",
	                 (unsigned long)irq_us, n, rssi, snr_q / 4.0, fei_hz, crcerr ? 1 : 0);
	for (int i = 0; i < n && k < (int)sizeof(line) - 3; i++)
		k += snprintf(line + k, sizeof(line) - k, "%02x", b[i]);
	Serial.println(line);
}

// ---------------------------------------------------------------- SBUS (sbus_monitor)
static uint8_t frame[25];
static int pos;
static uint32_t last_byte_us;
static uint16_t ch[16];
static uint8_t sflags;
static uint32_t n_frames, n_lost, n_failsafe, n_bad;
static bool sbus_seen;

static void sbus_decode()
{
	uint32_t acc = 0;
	int bits = 0, c = 0;
	for (int i = 1; i < 23 && c < 16; i++) {
		acc |= (uint32_t)frame[i] << bits;
		bits += 8;
		while (bits >= 11 && c < 16) {
			ch[c++] = acc & 0x7FF;
			acc >>= 11;
			bits -= 11;
		}
	}
	sflags = frame[23];
}

static void sbus_poll()
{
	static uint32_t last_print_ms;

	while (Serial2.available()) {
		uint32_t now = micros();
		uint8_t b = Serial2.read();
		if (now - last_byte_us > FRAME_GAP_US)
			pos = 0;
		last_byte_us = now;
		if (pos < (int)sizeof(frame))
			frame[pos++] = b;
		if (pos == sizeof(frame)) {
			if (frame[0] == 0x0F && frame[24] == 0x00) {
				sbus_decode();
				n_frames++;
				n_lost += (sflags >> 2) & 1;
				n_failsafe += (sflags >> 3) & 1;
				sbus_seen = true;
				pos = 0;
			} else {
				n_bad++;
				pos++;
			}
		}
	}
	if (millis() - last_print_ms >= 1000) {
		last_print_ms += 1000;
		if (sbus_seen) {        // (quiet when no receiver was ever connected)
			char line[160];
			int n = snprintf(line, sizeof(line), "fps %3lu  lost %lu (%.1f%%)  failsafe %lu",
			                 (unsigned long)n_frames, (unsigned long)n_lost,
			                 n_frames ? 100.0 * n_lost / n_frames : 0.0, (unsigned long)n_failsafe);
			if (n_bad)
				n += snprintf(line + n, sizeof(line) - n, "  bad %lu", (unsigned long)n_bad);
			n += snprintf(line + n, sizeof(line) - n, "  |");
			for (int i = 0; i < 8; i++)
				n += snprintf(line + n, sizeof(line) - n, " %4u", ch[i]);
			Serial.println(n_frames ? line : "no SBUS frames");
		}
		n_frames = n_lost = n_failsafe = n_bad = 0;
	}
}

// ---------------------------------------------------------------- commands
static void command(char *s)
{
	char *argv[16];
	int argc = 0;
	for (char *t = strtok(s, " \t\r"); t && argc < 16; t = strtok(NULL, " \t\r"))
		argv[argc++] = t;
	if (!argc)
		return;
	if (!strcmp(argv[0], "f") && argc > 1) {
		cfg.freq = strtoul(argv[1], NULL, 0);
	} else if (!strcmp(argv[0], "elrs")) {
		cfg.sf = 6; cfg.bw = 500000; cfg.cr = 7; cfg.len = 8; cfg.sync = 0x12;
		cfg.preamble = 8; cfg.crc = false; cfg.explicit_hdr = false; cfg.iqinv = false;
	} else if (!strcmp(argv[0], "lora") && argc > 4) {
		cfg.sf = atoi(argv[1]);
		cfg.bw = atol(argv[2]);
		cfg.cr = atoi(argv[3]);
		cfg.len = atoi(argv[4]);
		cfg.crc = cfg.explicit_hdr = cfg.iqinv = false;
		for (int i = 5; i < argc; i++) {
			if (!strcmp(argv[i], "crc")) cfg.crc = true;
			else if (!strcmp(argv[i], "explicit")) cfg.explicit_hdr = true;
			else if (!strcmp(argv[i], "iqinv")) cfg.iqinv = true;
			else if (!strcmp(argv[i], "sync") && i + 1 < argc) cfg.sync = strtoul(argv[++i], NULL, 16);
			else if (!strcmp(argv[i], "pre") && i + 1 < argc) cfg.preamble = atoi(argv[++i]);
		}
	} else if (!strcmp(argv[0], "status")) {
		Serial.printf("sx127x version 0x%02x, %lu packets, %lu CRC errors\n", version,
		              (unsigned long)n_pkts, (unsigned long)n_crcerr);
		print_cfg();
		return;
	} else {
		Serial.println("commands: f HZ | lora SF BW CR LEN [crc] [explicit] [sync HEX] [pre N] [iqinv] | elrs | status");
		return;
	}
	configure();
	print_cfg();
}

void setup()
{
	Serial.begin(115200);
	Serial2.begin(100000, SERIAL_8E2, SBUS_RX_PIN, SBUS_TX_PIN, true);
	pinMode(PIN_NSS, OUTPUT);
	digitalWrite(PIN_NSS, HIGH);
	pinMode(PIN_RST, OUTPUT);
	digitalWrite(PIN_RST, LOW);
	delay(2);
	digitalWrite(PIN_RST, HIGH);
	delay(10);
	spi.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_NSS);
	version = rd(REG_VERSION);
	Serial.printf("sx127x_listen: SX127x version 0x%02x%s\n", version,
	              version == 0x12 ? "" : " (expected 0x12: check the wiring and 3.3 V)");
	pinMode(PIN_DIO0, INPUT);
	attachInterrupt(digitalPinToInterrupt(PIN_DIO0), dio0_isr, RISING);
	configure();
	print_cfg();
}

void loop()
{
	static char buf[128];
	static int n;

	if (irq_pending) {
		irq_pending = false;
		read_packet();
	}
	while (Serial.available()) {
		char c = Serial.read();
		if (c == '\n' || c == '\r') {
			buf[n] = 0;
			if (n)
				command(buf);
			n = 0;
		} else if (n < (int)sizeof(buf) - 1) {
			buf[n++] = c;
		}
	}
	sbus_poll();
}
