#include <math.h>

#include "axi_ad9361.h"
#include "hw.h"

/* Register map: ADI up_adc_common / up_dac_common / up_*_channel (hdl 2022_r2) */
#define ADC            (AXI_AD9361_BASE + 0x0000)
#define DAC            (AXI_AD9361_BASE + 0x4000)
#define REG_RSTN       0x040     /* [1] MMCM reset release, [0] core reset release */
#define REG_ADC_CNTRL  0x044     /* [2] R1_MODE (1R1T) */
#define REG_DAC_SYNC   0x044     /* [0] SYNC: restart the DDS phase */
#define REG_DAC_CNTRL2 0x048     /* [5] R1_MODE (1R1T) */
#define REG_DAC_RATE   0x04C     /* valid every RATE + 1 clocks */
#define REG_CLK_FREQ   0x054     /* interface clock, 100 MHz * value / 2^16 */
#define REG_DAC_CLKSEL 0x060     /* [0] invert FB_CLK */
#define CHAN(n)        (0x400 + 0x40 * (n))

/* ADC channel control */
#define ADC_ENABLE       (1u << 0)
#define ADC_FMT_ENABLE   (1u << 4)
#define ADC_FMT_SIGNEXT  (1u << 6)

/* DAC channel registers (offsets inside CHAN(n)) */
#define DDS_SCALE_1    0x00
#define DDS_INIT_INCR_1 0x04     /* [31:16] phase offset, [15:0] phase increment */
#define DDS_SCALE_2    0x08
#define DDS_INIT_INCR_2 0x0C
#define DAC_DATA_SEL   0x18      /* enum axi_ad9361_tx_src */
#define DDS_FULL_SCALE 0x4000    /* scale 0x1000 gives a quarter of full scale */

void axi_ad9361_init(void)
{
	wr32(ADC + REG_RSTN, 3);
	wr32(ADC + REG_ADC_CNTRL, 1u << 2);
	for (int ch = 0; ch < 2; ch++)
		wr32(ADC + CHAN(ch), ADC_ENABLE | ADC_FMT_ENABLE | ADC_FMT_SIGNEXT);

	wr32(DAC + REG_RSTN, 3);
	wr32(DAC + REG_DAC_CNTRL2, 1u << 5);
	wr32(DAC + REG_DAC_RATE, 0);
	/* The core puts Q + frame low on FB_CLK's rising edge and I + frame high on
	 * the falling one; the AD9361 (configured like a B210) wants I on the rising
	 * edge, as Ettus' FPGA sends it. Inverting FB_CLK lines the two up. */
	wr32(DAC + REG_DAC_CLKSEL, 1);
	axi_ad9361_dds_off();
}

double axi_ad9361_clk_hz(void)
{
	return rd32(ADC + REG_CLK_FREQ) * (100e6 / 65536.0);
}

void axi_ad9361_dds(double rate_hz, double tone_hz, double dbfs)
{
	uint32_t incr = (uint32_t)lround(tone_hz / rate_hz * 65536.0) & 0xFFFF;
	double scale = DDS_FULL_SCALE * pow(10.0, dbfs / 20.0);

	if (scale > DDS_FULL_SCALE)
		scale = DDS_FULL_SCALE;
	/* I = cos, Q = sin: the DDS makes sines, so I starts 90 degrees ahead */
	for (int ch = 0; ch < 2; ch++) {
		uint32_t base = DAC + CHAN(ch);
		wr32(base + DDS_SCALE_1, (uint32_t)scale);
		wr32(base + DDS_INIT_INCR_1, ((ch == 0 ? 0x4000u : 0) << 16) | incr);
		wr32(base + DDS_SCALE_2, 0);
		wr32(base + DDS_INIT_INCR_2, 0);
	}
	axi_ad9361_tx_source(AXI_AD9361_TX_DDS);
	wr32(DAC + REG_DAC_SYNC, 1);
}

void axi_ad9361_dds_off(void)
{
	axi_ad9361_tx_source(AXI_AD9361_TX_ZERO);
}

void axi_ad9361_tx_source(enum axi_ad9361_tx_src src)
{
	for (int ch = 0; ch < 2; ch++)
		wr32(DAC + CHAN(ch) + DAC_DATA_SEL, src);
}
