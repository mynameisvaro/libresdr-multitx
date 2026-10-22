/* Virtual CC2500 (TI CC2500 data sheet, SWRS040). Emulated:
 *
 *  - SPI framing: header byte {R/W, burst, address}, chip status byte returned
 *    on every header, burst auto-increment, strobes, status registers,
 *    PATABLE and the FIFOs
 *  - configuration registers 0x00-0x2E with their reset values
 *  - transmit: on STX the TX FIFO becomes a packet built like the chip does it:
 *    preamble (MDMCFG1.NUM_PREAMBLE), sync word (MDMCFG2.SYNC_MODE, twice for
 *    30/32 modes), fixed / variable / infinite length (PKTCTRL0), CRC-16
 *    (PKTCTRL0.CRC_EN) and PN9 whitening (PKTCTRL0.WHITE_DATA); frequency from
 *    FREQ + CHANNR * channel spacing + FSCTRL0 offset; data rate (MDMCFG4/3),
 *    deviation (DEVIATN), modulation (2-FSK, GFSK, MSK) and power (PATABLE[0])
 *  - after TX the state follows MCSM1.TXOFF_MODE
 *
 * Not emulated (yet): the receiver (RXBYTES stays 0, RSSI reads -100 dBm), OOK,
 * Manchester coding, FEC, wake-on-radio. Calibration strobes are accepted and
 * do nothing: the AD9361 driver calibrates the real synthesiser.
 */
#include <string.h>

#include "cc2500.h"
#include "radio.h"

#define FXOSC 26e6

/* register addresses used here */
enum {
	SYNC1 = 0x04, SYNC0 = 0x05, PKTLEN = 0x06, PKTCTRL1 = 0x07, PKTCTRL0 = 0x08,
	CHANNR = 0x0A, FSCTRL0 = 0x0C, FREQ2 = 0x0D, FREQ1 = 0x0E, FREQ0 = 0x0F,
	MDMCFG4 = 0x10, MDMCFG3 = 0x11, MDMCFG2 = 0x12, MDMCFG1 = 0x13, MDMCFG0 = 0x14,
	DEVIATN = 0x15, MCSM1 = 0x17, FREND0 = 0x22, FSCAL1 = 0x25,
	PATABLE = 0x3E, FIFO = 0x3F,
};
/* strobes */
enum {
	SRES = 0x30, SFSTXON, SXOFF, SCAL, SRX, STX, SIDLE, SAFC, SWOR, SPWD, SFRX,
	SFTX, SWORRST, SNOP,
};
/* status registers (read with the burst bit) */
enum {
	PARTNUM = 0x30, VERSION, FREQEST, LQI, RSSI, MARCSTATE, WORTIME1, WORTIME0,
	PKTSTATUS, VCO_VC_DAC, TXBYTES, RXBYTES, RCCTRL1_STATUS, RCCTRL0_STATUS,
};
/* MARCSTATE values */
enum { M_SLEEP = 0x00, M_IDLE = 0x01, M_FSTXON = 0x12, M_RX = 0x0D, M_TX = 0x13 };

static const uint8_t reset_regs[0x2F] = {
	0x29, 0x2E, 0x3F, 0x07, 0xD3, 0x91, 0xFF, 0x04,   /* 00 IOCFG2 .. 07 PKTCTRL1 */
	0x45, 0x00, 0x00, 0x0F, 0x00, 0x5E, 0xC4, 0xEC,   /* 08 PKTCTRL0 .. 0F FREQ0 */
	0x8C, 0x22, 0x02, 0x22, 0xF8, 0x47, 0x07, 0x30,   /* 10 MDMCFG4 .. 17 MCSM1 */
	0x04, 0x36, 0x6C, 0x03, 0x40, 0x91, 0x87, 0x6B,   /* 18 MCSM0 .. 1F WOREVT0 */
	0xF8, 0x56, 0x10, 0xA9, 0x0A, 0x20, 0x0D, 0x41,   /* 20 WORCTRL .. 27 RCCTRL1 */
	0x00, 0x59, 0x7F, 0x3F, 0x88, 0x31, 0x0B,         /* 28 RCCTRL0 .. 2E TEST0 */
};

/* PATABLE setting -> output power (dBm), from the data sheet's table */
static const struct { uint8_t pa; int8_t dbm; } pa_dbm[] = {
	{ 0x00, -55 }, { 0x50, -30 }, { 0x44, -28 }, { 0xC0, -26 }, { 0x84, -24 },
	{ 0x81, -22 }, { 0x46, -20 }, { 0x93, -18 }, { 0x55, -16 }, { 0x8D, -14 },
	{ 0xC6, -12 }, { 0x97, -10 }, { 0x6E, -8 },  { 0x7F, -6 },  { 0xA9, -4 },
	{ 0xBB, -2 },  { 0xFE, 0 },   { 0xFF, 1 },
};

/* GFSK pulse shaping. The CC2500 data sheet gives no BT for GFSK; BT = 1 is an
 * assumption (the receiver is not sensitive to it). */
#define GFSK_BT 1.0

#define FIFO_SIZE 64

static uint8_t regs[0x2F];
static uint8_t patable[8];
static uint8_t txfifo[FIFO_SIZE];
static int     txlen;
static uint8_t marc = M_IDLE;
static uint8_t pa_index;

/* SPI frame state */
static int     selected;
static int     header_seen;
static uint8_t addr;
static int     rw_read, burst;

static struct cc2500_info info;
static uint8_t pkt[256];            /* bit stream for gfsk_tx, MSB first */

static void reset(void)
{
	memcpy(regs, reset_regs, sizeof(regs));
	memset(patable, 0xC6, sizeof(patable));
	patable[0] = 0xC6;
	txlen = 0;
	marc = M_IDLE;
}

static double pa_to_dbm(uint8_t pa)
{
	for (unsigned i = 0; i < sizeof(pa_dbm) / sizeof(pa_dbm[0]); i++)
		if (pa_dbm[i].pa == pa)
			return pa_dbm[i].dbm;
	return -10;    /* unlisted setting: a middling level */
}

static double rf_freq(void)
{
	uint32_t freq = ((uint32_t)regs[FREQ2] << 16) | (regs[FREQ1] << 8) | regs[FREQ0];
	double spacing = FXOSC / (1 << 18) * (256 + regs[MDMCFG0]) * (1 << (regs[MDMCFG1] & 3));
	int8_t off = (int8_t)regs[FSCTRL0];
	return FXOSC / 65536.0 * freq + regs[CHANNR] * spacing + FXOSC / 16384.0 * off;
}

static uint16_t crc16(const uint8_t *d, int n)
{
	uint16_t crc = 0xFFFF;

	for (int i = 0; i < n; i++) {
		crc ^= (uint16_t)d[i] << 8;
		for (int b = 0; b < 8; b++)
			crc = (crc & 0x8000) ? (crc << 1) ^ 0x8005 : crc << 1;
	}
	return crc;
}

static void whiten(uint8_t *d, int n)
{
	uint16_t pn9 = 0x1FF;

	for (int i = 0; i < n; i++) {
		d[i] ^= pn9 & 0xFF;
		for (int b = 0; b < 8; b++)
			pn9 = (pn9 >> 1) | (((pn9 ^ (pn9 >> 5)) & 1) << 8);
	}
}

static void transmit(void)
{
	static const uint8_t npre[8] = { 2, 3, 4, 6, 8, 12, 16, 24 };
	uint8_t mod = (regs[MDMCFG2] >> 4) & 7;
	uint8_t sync_mode = regs[MDMCFG2] & 7;
	uint8_t length_cfg = regs[PKTCTRL0] & 3;
	int n = 0, body, len;

	/* payload as the chip would send it, from the TX FIFO */
	if (length_cfg == 1)
		len = txlen ? txfifo[0] + 1 : 0;        /* length byte + data */
	else if (length_cfg == 0)
		len = regs[PKTLEN];
	else
		len = txlen;
	if (len == 0 || len > txlen || mod == 3) {  /* underflow or OOK */
		info.tx_errors++;
		txlen = 0;
		marc = M_IDLE;
		return;
	}

	if (sync_mode != 0 && sync_mode != 4) {
		for (int i = 0; i < npre[(regs[MDMCFG1] >> 4) & 7]; i++)
			pkt[n++] = 0xAA;
		int words = (sync_mode == 3 || sync_mode == 7) ? 2 : 1;
		for (int i = 0; i < words; i++) {
			pkt[n++] = regs[SYNC1];
			pkt[n++] = regs[SYNC0];
		}
	}
	body = n;
	memcpy(pkt + n, txfifo, len);
	n += len;
	if (regs[PKTCTRL0] & 0x04) {
		uint16_t crc = crc16(pkt + body, len);
		pkt[n++] = crc >> 8;
		pkt[n++] = crc & 0xFF;
	}
	if (regs[PKTCTRL0] & 0x40)
		whiten(pkt + body, n - body);

	struct radio_modem m;
	m.baud = (256 + regs[MDMCFG3]) * (FXOSC / 268435456.0) * (1 << (regs[MDMCFG4] & 0x0F));
	if (mod == 7) {                              /* MSK: deviation = rate / 4 */
		m.dev_hz = m.baud / 4;
		m.bt = 0;
	} else {
		uint8_t d = regs[DEVIATN];
		m.dev_hz = FXOSC / 131072.0 * (8 + (d & 7)) * (1 << ((d >> 4) & 7));
		m.bt = mod == 1 ? GFSK_BT : 0;
	}
	double f = rf_freq();
	if (radio_tx(f, &m, pa_to_dbm(patable[pa_index]), pkt, 8 * n)) {
		info.tx_errors++;
		marc = M_IDLE;
	} else {
		info.tx_packets++;
		marc = M_TX;
	}
	info.last_freq_hz = f;
	info.baud = m.baud;
	info.dev_hz = m.dev_hz;
	info.last_len = n < (int)sizeof(info.last_pkt) ? n : (int)sizeof(info.last_pkt);
	memcpy(info.last_pkt, pkt, info.last_len);
	txlen = 0;          /* the FIFO empties as the packet goes out */
}

void cc2500_poll(void)
{
	if (marc == M_TX && !radio_tx_busy()) {
		switch (regs[MCSM1] & 3) {              /* TXOFF_MODE */
		case 1:  marc = M_FSTXON; break;
		case 3:  marc = M_RX;     break;
		default: marc = M_IDLE;   break;
		}
	}
}

static void strobe(uint8_t s)
{
	info.strobes++;
	cc2500_poll();
	switch (s) {
	case SRES:    reset(); break;
	case SFSTXON: if (marc == M_IDLE) marc = M_FSTXON; break;
	case STX:     if (marc != M_TX) transmit(); break;
	case SRX:     if (marc != M_TX) marc = M_RX; break;
	case SIDLE:   if (marc != M_TX) marc = M_IDLE; break;   /* a packet in flight completes */
	case SFTX:    txlen = 0; break;
	case SPWD:    marc = M_SLEEP; break;
	default:      break;                                     /* SCAL, SFRX, SNOP, ... */
	}
}

static uint8_t status_byte(void)
{
	uint8_t st;

	cc2500_poll();
	switch (marc) {
	case M_RX:     st = 1; break;
	case M_TX:     st = 2; break;
	case M_FSTXON: st = 3; break;
	default:       st = 0; break;
	}
	int avail = rw_read ? 0 : FIFO_SIZE - txlen;
	return (st << 4) | (avail > 15 ? 15 : avail);
}

static uint8_t read_status_reg(uint8_t a)
{
	cc2500_poll();
	switch (a) {
	case PARTNUM:   return 0x80;
	case VERSION:   return 0x03;
	case RSSI:      return 0xC8;                 /* (-100 dBm + 72) * 2 */
	case LQI:       return 0x7F;
	case MARCSTATE: return marc;
	case TXBYTES:   return txlen;
	case RXBYTES:   return 0;
	case PKTSTATUS: return 0;
	default:        return 0;
	}
}

void cc2500_csn(int level)
{
	if (!level && !selected) {
		selected = 1;
		header_seen = 0;
	} else if (level) {
		selected = 0;
	}
}

uint8_t cc2500_spi(uint8_t mosi)
{
	static int inited;

	if (!inited) {
		reset();
		inited = 1;
	}
	if (!selected)
		return 0xFF;

	if (!header_seen) {
		header_seen = 1;
		rw_read = mosi >> 7;
		burst = (mosi >> 6) & 1;
		addr = mosi & 0x3F;
		uint8_t st = status_byte();
		if (addr >= 0x30 && addr <= 0x3D && !(rw_read && burst)) {
			strobe(addr);
			header_seen = 0;                     /* another command may follow */
		} else if (addr == PATABLE) {
			pa_index = 0;
		}
		return st;
	}

	/* data bytes */
	uint8_t out = status_byte();
	if (addr < 0x2F) {
		if (rw_read)
			out = regs[addr];
		else
			regs[addr] = mosi;
		if (burst)
			addr++;
	} else if (addr >= 0x30 && addr <= 0x3D) {
		out = read_status_reg(addr);
	} else if (addr == PATABLE) {
		if (rw_read)
			out = patable[pa_index];
		else
			patable[pa_index] = mosi;
		pa_index = (pa_index + 1) & 7;
	} else if (addr == FIFO) {
		if (rw_read)
			out = 0;                             /* no receiver yet */
		else if (txlen < FIFO_SIZE)
			txfifo[txlen++] = mosi;
	}
	if (!burst)
		header_seen = 0;                         /* single access: next byte is a header */
	return out;
}

const struct cc2500_info *cc2500_get_info(void)
{
	info.marcstate = marc;
	return &info;
}

const uint8_t *cc2500_regs(void)
{
	return regs;
}
