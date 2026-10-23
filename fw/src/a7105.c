/* Virtual A7105 (AMICCOM A7105 data sheet v1.4). Emulated:
 *
 *  - SPI framing: strobe commands (bit 7 set), register access {0, R/W,
 *    address}, multi-byte access to the FIFO (05h) and ID (06h) registers,
 *    FIFO pointer resets
 *  - control registers with their reset values; writing the mode register
 *    (00h) resets the chip; calibrations (02h) finish at once and pass
 *  - mode register status: TRER (TX/RX in progress), TRSR, CRCF, FECF
 *  - transmit: on the TX strobe the FIFO becomes a packet built like the chip
 *    does it: preamble (1-4 bytes, 0101.. or 1010.. to suit the ID), ID code
 *    (2 or 4 bytes), payload (FEP + 1 bytes), CRC-16, (7,4) Hamming FEC and
 *    PN7 whitening as enabled in code register I (1Fh). Frequency from the
 *    LO base (BIP/BFP), channel number and step (CHR, DBL, RRC); data rate
 *    from the clock and data rate registers, deviation from TX I/II,
 *    Gaussian filter (BT 0.7) from FS, power from TX test (28h)
 *  - after TX, and after a packet is received, the chip goes back to standby
 *    (the data sheet's auto-back)
 *  - receive: in RX the receiver (gfsk_rx) looks for the ID code on the
 *    channel, and the bits after it are decoded as the transmit side codes
 *    them (whitening, FEC, CRC); CRCF/FECF report the result and the chip
 *    goes back to standby. A packet queued with a7105_rx_queue() (console,
 *    tests) lands in the RX FIFO with a good CRC instead.
 *
 * The data sheet does not give the CRC-16 initial value or reflection, nor the
 * bit layout of the Hamming code words, so those are settable (struct
 * a7105_coding); the defaults were learned from an FS-i6X with
 * tools/a7105_sniff.py (see a7105_coding_default). The PN7 whitening is a guess
 * (no Multiprotocol protocol on this chip uses it).
 */
#include <string.h>

#include "a7105.h"
#include "gfsk.h"
#include "gfsk_rx.h"
#include "radio.h"

#define FXTAL 16e6

/* register addresses used here */
enum {
	MODE = 0x00, MODE_CONTROL = 0x01, CALC = 0x02, FIFOI = 0x03, FIFO_DATA = 0x05,
	ID_DATA = 0x06, CLOCK = 0x0D, DATA_RATE = 0x0E, PLL_I = 0x0F, PLL_II = 0x10,
	PLL_III = 0x11, PLL_IV = 0x12, PLL_V = 0x13, TX_I = 0x14, TX_II = 0x15,
	RSSI_THOLD = 0x1D, CODE_I = 0x1F, CODE_III = 0x21, IF_CALIB_I = 0x22,
	VCO_CURCAL = 0x24, VCO_SBCAL_I = 0x25, TX_TEST = 0x28, NREGS = 0x33,
};
/* strobe commands = the state they enter */
enum {
	S_SLEEP = 0x80, S_IDLE = 0x90, S_STANDBY = 0xA0, S_PLL = 0xB0, S_RX = 0xC0,
	S_TX = 0xD0, S_RST_WRPTR = 0xE0, S_RST_RDPTR = 0xF0,
};
/* mode register (read) */
#define TRER 0x01
#define TRSR 0x02
#define CRCF 0x20
#define FECF 0x40

/* reset values (data sheet chapter 9; reserved/unknown ones 0) */
static const uint8_t reset_regs[NREGS] = {
	0x00, 0x00, 0x00, 0x3F, 0x00, 0x00, 0x00, 0x00,   /* 00 MODE .. 07 RC_OSC_I */
	0x00, 0x00, 0x00, 0x01, 0x01, 0xF5, 0x00, 0x00,   /* 08 .. 0F PLL_I */
	0x9E, 0x4B, 0x00, 0x02, 0x16, 0x0B, 0x12, 0x00,   /* 10 PLL_II .. 17 DELAY_II */
	0x62, 0x80, 0x80, 0x00, 0x0A, 0x32, 0xC3, 0x07,   /* 18 RX .. 1F CODE_I */
	0x77, 0x2A, 0x00, 0x00, 0x00, 0x00, 0x3B, 0x00,   /* 20 CODE_II .. 27 BATTERY */
	0x17, 0x47, 0x80, 0x03, 0x01, 0x45, 0x18, 0x00,   /* 28 TX_TEST .. 2F */
	0x01, 0x0F, 0x00,                                 /* 30 .. 32 */
};

/* TX test register (PAC << 3 | TBG) -> output power, dBm. The points in
 * Multiprotocol.h's table: PAC 0 with TBG 0/1/2/4 = -23/-20/-16/-11, PAC 1 TBG 5
 * = -6, PAC 2 TBG 7 = 0, PAC 3 TBG 7 = +1; the rest is interpolated. */
static double tx_test_dbm(uint8_t v)
{
	static const int8_t tbg_dbm[8] = { -23, -20, -16, -13, -11, -9, -8, -7 };
	static const int8_t pac_db[4] = { 0, 3, 7, 8 };
	double dbm = tbg_dbm[v & 7] + pac_db[(v >> 3) & 3];
	return dbm > 1 ? 1 : dbm;
}

#define GAUSS_BT  0.7       /* TX I.FS (data sheet) */
#define FIFO_SIZE 64

static uint8_t regs[NREGS];
static uint8_t id[4];
static uint8_t txfifo[FIFO_SIZE], rxfifo[FIFO_SIZE];
static int     txlen, rxrd;
static uint8_t state = S_STANDBY, status;    /* status: CRCF/FECF from the last RX */
static uint8_t rxq[FIFO_SIZE];
static int     rxq_len;
static int     rx_armed;

/* SPI frame state */
static int     selected, header_seen, rw_read, nbyte;
static uint8_t addr;

static struct a7105_info info;
static struct a7105_coding coding;
static uint8_t pkt[GFSK_BUF_BYTES];

/* Learned from an FS-i6X (tools/a7105_sniff.py, 2026-10-24): 55 normal packets
 * fit only CRC-16 poly 0x1021 with start value 0x1D0F over the payload, and a
 * systematic (7,4) Hamming code: the data nibble in the top 4 bits, three parity
 * bits below, high nibble first. */
void a7105_coding_default(void)
{
	static const uint8_t fec[16] = {
		0x00, 0x0b, 0x16, 0x1d, 0x27, 0x2c, 0x31, 0x3a,
		0x45, 0x4e, 0x53, 0x58, 0x62, 0x69, 0x74, 0x7f,
	};
	coding.crc_poly = 0x1021;
	coding.crc_init = 0x1D0F;
	coding.crc_xorout = 0x0000;
	coding.crc_reflect = 0;
	memcpy(coding.fec, fec, sizeof(fec));
	coding.fec_low_first = 0;
}

struct a7105_coding *a7105_coding(void)
{
	return &coding;
}

static void reset(void)
{
	memcpy(regs, reset_regs, sizeof(regs));
	txlen = rxrd = 0;
	state = S_STANDBY;
	status = 0;
}

static double fpfd(void)
{
	return FXTAL * ((regs[PLL_II] >> 7) + 1) / (((regs[PLL_II] >> 5) & 3) + 1);
}

/* Added to the frequency the registers give (transmit and receive): a real
 * FlySky handset was measured below where the data sheet maths puts its
 * channels; settable while that is worked out (a7105 foffset HZ). */
static double freq_offset_hz;

void a7105_set_freq_offset(double hz)
{
	freq_offset_hz = hz;
}

double a7105_freq_offset(void)
{
	return freq_offset_hz;
}

static double rf_freq(void)
{
	unsigned bip = ((regs[PLL_II] & 1) << 8) | regs[PLL_III];
	unsigned bfp = (regs[PLL_IV] << 8) | regs[PLL_V];
	unsigned chr = (regs[PLL_II] >> 1) & 0x0F;
	return fpfd() * (bip + bfp / 65536.0) + regs[PLL_I] * fpfd() / (4.0 * (chr + 1)) + freq_offset_hz;
}

static double data_rate(void)
{
	static const int csc_div[4] = { 1, 2, 2, 4 };
	double fmclk = FXTAL * ((regs[PLL_II] >> 7) + 1);
	return fmclk / csc_div[(regs[CLOCK] >> 2) & 3] / 32 / (regs[DATA_RATE] + 1);
}

static double deviation(void)
{
	return fpfd() * 127 * ((regs[TX_II] & 0x1F) + 1) * (1 << (regs[TX_I] & 7)) / 16777216.0;
}

static uint8_t reflect8(uint8_t b)
{
	uint8_t r = 0;
	for (int i = 0; i < 8; i++)
		r |= ((b >> i) & 1) << (7 - i);
	return r;
}

static uint16_t crc16(const uint8_t *d, int n)
{
	uint16_t crc = coding.crc_init;

	for (int i = 0; i < n; i++) {
		crc ^= (uint16_t)(coding.crc_reflect ? reflect8(d[i]) : d[i]) << 8;
		for (int b = 0; b < 8; b++)
			crc = (crc & 0x8000) ? (crc << 1) ^ coding.crc_poly : crc << 1;
	}
	if (coding.crc_reflect)
		crc = reflect8(crc >> 8) | reflect8(crc & 0xFF) << 8;
	return crc ^ coding.crc_xorout;
}

/* bit writer, MSB first */
static int nbits;
static void put_bits(uint32_t v, int n)
{
	while (n--) {
		int bit = (v >> n) & 1;
		if (bit)
			pkt[nbits >> 3] |= 0x80 >> (nbits & 7);
		nbits++;
	}
}

static void transmit(void)
{
	uint8_t code = regs[CODE_I];
	int len = regs[FIFOI] + 1;           /* FEP + 1 (no segment FIFO / extension) */
	uint8_t body[FIFO_SIZE + 2];

	if (len > FIFO_SIZE)
		len = FIFO_SIZE;
	if (txlen < len)                     /* underflow: the chip sends what's in the FIFO */
		info.tx_errors++;
	memcpy(body, txfifo, len);
	int n = len;
	if (code & 0x08) {                   /* CRCS */
		uint16_t crc = crc16(body, len);
		body[n++] = crc >> 8;
		body[n++] = crc & 0xFF;
	}

	memset(pkt, 0, sizeof(pkt));
	nbits = 0;
	int idl = (code & 0x04) ? 4 : 2;
	for (int i = 0; i <= (code & 3); i++)
		put_bits((id[0] & 0x80) ? 0xAA : 0x55, 8);
	for (int i = 0; i < idl; i++)
		put_bits(id[i], 8);
	int start = nbits;
	for (int i = 0; i < n; i++) {
		if (code & 0x10) {               /* FECS: each nibble -> 7-bit code word */
			uint8_t hi = body[i] >> 4, lo = body[i] & 0x0F;
			put_bits(coding.fec[coding.fec_low_first ? lo : hi], 7);
			put_bits(coding.fec[coding.fec_low_first ? hi : lo], 7);
		} else {
			put_bits(body[i], 8);
		}
	}
	if (code & 0x20) {                   /* WHTS: PN7 (x^7 + x^4 + 1) from WS */
		uint8_t pn = regs[CODE_III] & 0x7F;
		if (!pn)
			pn = 0x7F;
		for (int i = start; i < nbits; i++) {
			if (pn & 0x40)
				pkt[i >> 3] ^= 0x80 >> (i & 7);
			pn = ((pn << 1) | (((pn >> 6) ^ (pn >> 3)) & 1)) & 0x7F;
		}
	}
	if (regs[TX_I] & 0x20)               /* TXDI: inverted data */
		for (int i = 0; i < (nbits + 7) / 8; i++)
			pkt[i] ^= 0xFF;

	struct radio_modem m;
	m.baud = data_rate();
	m.dev_hz = deviation();
	m.bt = (regs[TX_I] & 0x08) ? GAUSS_BT : 0;
	double f = rf_freq();
	if (radio_tx(f, &m, tx_test_dbm(regs[TX_TEST]), pkt, nbits)) {
		info.tx_errors++;
		state = S_STANDBY;
	} else {
		info.tx_packets++;
		state = S_TX;
	}
	info.last_freq_hz = f;
	info.baud = m.baud;
	info.dev_hz = m.dev_hz;
	info.last_bits = nbits;
	info.last_len = len < (int)sizeof(info.last_pkt) ? len : (int)sizeof(info.last_pkt);
	memcpy(info.last_pkt, body, info.last_len);
}

static void receive(void);
static void listen(void);

void a7105_poll(void)
{
	if (state == S_TX && !radio_tx_busy())
		state = S_STANDBY;
	if (state == S_RX && rx_armed && gfsk_rx_done()) {
		rx_armed = 0;
		receive();
		state = S_STANDBY;
	}
	if (state == S_RX && rxq_len) {
		memset(rxfifo, 0, sizeof(rxfifo));
		memcpy(rxfifo, rxq, rxq_len);
		rxq_len = 0;
		rxrd = 0;
		status = 0;                       /* CRC and FEC ok */
		info.rx_packets++;
		state = S_STANDBY;
	}
}

int a7105_rx_queue(const uint8_t *data, int len)
{
	if (len <= 0 || len > FIFO_SIZE)
		return -1;
	memcpy(rxq, data, len);
	rxq_len = len;
	return 0;
}

static void strobe(uint8_t s)
{
	info.strobes++;
	a7105_poll();
	switch (s & 0xF0) {
	case S_TX:
		if (state != S_TX)
			transmit();
		break;
	case S_RST_WRPTR: txlen = 0; break;
	case S_RST_RDPTR: rxrd = 0; break;
	default:
		/* a packet in flight completes (the modulator can't stop mid-packet) */
		if (state != S_TX) {
			uint8_t was = state;
			state = s & 0xF0;
			if (state == S_RX && was != S_RX)
				listen();
			else if (state != S_RX && rx_armed) {
				radio_rx_stop();
				rx_armed = 0;
			}
		}
		break;
	}
}

/* The packet as transmit() would send it: preamble, ID code, then the bits
 * gfsk_rx keeps (payload and CRC, FEC coded if on, whitened if on). */
static int rx_body_len(void)
{
	int len = regs[FIFOI] + 1;
	if (len > FIFO_SIZE)
		len = FIFO_SIZE;
	return len + ((regs[CODE_I] & 0x08) ? 2 : 0);
}

static void listen(void)
{
	if (!gfsk_rx_present() || !radio_running())
		return;
	uint8_t code = regs[CODE_I];
	struct gfsk_rx_cfg c;
	c.baud = data_rate();
	if (code & 0x04) {
		c.sync = (uint32_t)id[0] << 24 | (uint32_t)id[1] << 16 | (uint32_t)id[2] << 8 | id[3];
	} else {                                /* 2-byte ID: preamble before it */
		uint8_t pre = (id[0] & 0x80) ? 0xAA : 0x55;
		c.sync = (uint32_t)pre << 24 | (uint32_t)pre << 16 | (uint32_t)id[0] << 8 | id[1];
	}
	c.mask = 0xFFFFFFFF;
	c.maxerr = 2;
	c.invert = 0;
	c.nbits = rx_body_len() * ((code & 0x10) ? 14 : 8);
	if (!radio_rx(rf_freq(), &c)) {
		rx_armed = 1;
		info.rx_arms++;
	}
}

static int get_bits(const uint8_t *b, int *pos, int n)
{
	int v = 0;
	while (n--) {
		v = v << 1 | ((b[*pos >> 3] >> (7 - (*pos & 7))) & 1);
		(*pos)++;
	}
	return v;
}

/* nearest code word: the nibble, and whether it needed more than one fix */
static int fec_decode(int cw, int *bad)
{
	int best = 0, dist = 8;
	for (int n = 0; n < 16; n++) {
		int d = __builtin_popcount((cw ^ coding.fec[n]) & 0x7F);
		if (d < dist) {
			dist = d;
			best = n;
		}
	}
	if (dist > 1)
		*bad = 1;
	return best;
}

static void receive(void)
{
	uint8_t raw[GFSK_RX_BUF_BYTES], body[FIFO_SIZE + 2];
	struct gfsk_rx_result r;
	uint8_t code = regs[CODE_I];
	int n = rx_body_len();

	gfsk_rx_read(raw, sizeof(raw), &r);
	int nbits = n * ((code & 0x10) ? 14 : 8);
	if (code & 0x20) {                       /* undo the PN7 whitening */
		uint8_t pn = regs[CODE_III] & 0x7F;
		if (!pn)
			pn = 0x7F;
		for (int i = 0; i < nbits; i++) {
			if (pn & 0x40)
				raw[i >> 3] ^= 0x80 >> (i & 7);
			pn = ((pn << 1) | (((pn >> 6) ^ (pn >> 3)) & 1)) & 0x7F;
		}
	}
	int pos = 0, fec_bad = 0;
	for (int i = 0; i < n; i++) {
		if (code & 0x10) {
			int a = fec_decode(get_bits(raw, &pos, 7), &fec_bad);
			int b = fec_decode(get_bits(raw, &pos, 7), &fec_bad);
			body[i] = coding.fec_low_first ? (uint8_t)(b << 4 | a) : (uint8_t)(a << 4 | b);
		} else {
			body[i] = (uint8_t)get_bits(raw, &pos, 8);
		}
	}
	int len = n - ((code & 0x08) ? 2 : 0);
	int crc_bad = 0;
	if (code & 0x08) {
		uint16_t crc = crc16(body, len);
		crc_bad = body[len] != (crc >> 8) || body[len + 1] != (crc & 0xFF);
	}
	memset(rxfifo, 0, sizeof(rxfifo));
	memcpy(rxfifo, body, len);
	rxrd = 0;
	status = (crc_bad ? CRCF : 0) | (fec_bad ? FECF : 0);
	info.rx_packets++;
	info.rx_crc_errors += crc_bad;
	info.rx_fec_errors += fec_bad;
	info.rx_offset_hz = r.offset_hz;
	info.rx_level = r.level;
	info.rx_len = len < (int)sizeof(info.last_rx) ? len : (int)sizeof(info.last_rx);
	memcpy(info.last_rx, body, info.rx_len);
}

static uint8_t read_reg(uint8_t a)
{
	a7105_poll();
	switch (a) {
	case MODE:
		return status | (state == S_TX ? TRSR | TRER : 0) | (state == S_RX ? TRER : 0);
	case CALC:        return 0x00;            /* calibrations done */
	case IF_CALIB_I:  return 0x00;            /* FBCF = 0: pass */
	case VCO_CURCAL:  return 0x13;
	case VCO_SBCAL_I: return 0x03;            /* VBCF = 0, band 3 */
	case RSSI_THOLD:  return 0xA0;            /* weak signal (no receiver) */
	default:          return a < NREGS ? regs[a] : 0;
	}
}

static void write_reg(uint8_t a, uint8_t v)
{
	if (a == MODE) {
		reset();                              /* any write: software reset */
		return;
	}
	if (a < NREGS)
		regs[a] = v;
	if (a == CALC)
		regs[CALC] = 0;
}

void a7105_csn(int level)
{
	if (!level && !selected) {
		selected = 1;
		header_seen = 0;
	} else if (level) {
		selected = 0;
	}
}

uint8_t a7105_spi(uint8_t mosi)
{
	static int inited;

	if (!inited) {
		reset();
		a7105_coding_default();
		inited = 1;
	}
	if (!selected)
		return 0xFF;

	if (!header_seen) {
		if (mosi & 0x80) {                    /* strobe; another command may follow */
			strobe(mosi);
			return 0xFF;
		}
		header_seen = 1;
		rw_read = (mosi >> 6) & 1;
		addr = mosi & 0x3F;
		nbyte = 0;
		return 0xFF;
	}

	/* data bytes: the FIFO and ID registers take several, the others one
	 * (further bytes go to the same register) */
	uint8_t out = 0xFF;
	if (addr == FIFO_DATA) {
		if (rw_read)
			out = rxfifo[rxrd++ % FIFO_SIZE];
		else if (txlen < FIFO_SIZE)
			txfifo[txlen++] = mosi;
	} else if (addr == ID_DATA) {
		if (rw_read)
			out = id[nbyte & 3];
		else
			id[nbyte & 3] = mosi;
	} else if (rw_read) {
		out = read_reg(addr);
	} else {
		write_reg(addr, mosi);
	}
	nbyte++;
	return out;
}

const struct a7105_info *a7105_get_info(void)
{
	a7105_poll();
	info.state = state;
	return &info;
}

const uint8_t *a7105_regs(void)
{
	return regs;
}
