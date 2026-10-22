#include "ad9361_spi.h"
#include "gpio.h"
#include "hw.h"
#include "timer.h"

/* AXI Quad SPI registers (PG153), standard mode, 16-deep FIFOs */
#define SRR    0x40
#define SPICR  0x60
#define SPISR  0x64
#define DTR    0x68
#define DRR    0x6C
#define SSR    0x70

#define CR_SPE         (1u << 1)
#define CR_MASTER      (1u << 2)
#define CR_CPHA        (1u << 4)   /* AD9361 samples on the falling edge */
#define CR_TX_RESET    (1u << 5)
#define CR_RX_RESET    (1u << 6)
#define CR_MANUAL_SS   (1u << 7)
#define CR_INHIBIT     (1u << 8)
#define CR_RUN         (CR_SPE | CR_MASTER | CR_CPHA | CR_MANUAL_SS)
#define SR_RX_EMPTY    (1u << 0)
#define FIFO_DEPTH     16
#define XFER_TIMEOUT_US 1000

/* The FIFO reset bits clear themselves; FIFO writes before that are lost. */
static void reset_fifos(void)
{
	wr32(AD9361_SPI_BASE + SPICR, CR_RUN | CR_INHIBIT | CR_TX_RESET | CR_RX_RESET);
	while (rd32(AD9361_SPI_BASE + SPICR) & (CR_TX_RESET | CR_RX_RESET))
		;
}

void ad9361_spi_init(void)
{
	wr32(AD9361_SPI_BASE + SRR, 0x0A);
	delay_us(1);
	wr32(AD9361_SPI_BASE + SSR, 0xFFFFFFFF);
	reset_fifos();
}

int ad9361_spi_xfer(uint8_t *buf, int len)
{
	if (len <= 0 || len > FIFO_DEPTH)
		return -1;

	int ret = 0;

	reset_fifos();
	for (int i = 0; i < len; i++)
		wr32(AD9361_SPI_BASE + DTR, buf[i]);
	wr32(AD9361_SPI_BASE + SSR, 0xFFFFFFFE);
	wr32(AD9361_SPI_BASE + SPICR, CR_RUN);
	/* one byte arrives in the RX FIFO per byte shifted out */
	uint64_t t0 = timer_ticks();
	for (int i = 0; i < len && !ret; i++) {
		while (rd32(AD9361_SPI_BASE + SPISR) & SR_RX_EMPTY) {
			if (timer_ticks() - t0 > TIMER_US(XFER_TIMEOUT_US)) {
				ret = -1;
				break;
			}
		}
		if (!ret)
			buf[i] = rd32(AD9361_SPI_BASE + DRR) & 0xFF;
	}
	wr32(AD9361_SPI_BASE + SSR, 0xFFFFFFFF);
	wr32(AD9361_SPI_BASE + SPICR, CR_RUN | CR_INHIBIT);
	return ret;
}

/* 16-bit instruction: [15] write, [14:12] byte count - 1, [9:0] address */
uint8_t ad9361_spi_read(uint16_t reg)
{
	uint8_t buf[3] = { (reg >> 8) & 0x03, reg & 0xFF, 0 };

	ad9361_spi_xfer(buf, 3);
	return buf[2];
}

void ad9361_spi_write(uint16_t reg, uint8_t value)
{
	uint8_t buf[3] = { 0x80 | ((reg >> 8) & 0x03), reg & 0xFF, value };

	ad9361_spi_xfer(buf, 3);
}

void ad9361_hw_reset(void)
{
	gpio_ctrl_clear(CTRL_RESETB);
	delay_ms(1);
	gpio_ctrl_set(CTRL_RESETB);
	delay_ms(1);
}
