#include "hw.h"
#include "ppm.h"

/* ppm_in registers (hw/ip/ppm_in/ppm_in.v) */
#define CTRL      0x00
#define STATUS    0x04
#define ID        0x0C
#define GEN_N     0x10
#define GEN_FRAME 0x14
#define CH(i)     (0x40 + 4 * (i))
#define GEN_CH(i) (0x80 + 4 * (i))

#define CTRL_EN      (1u << 0)
#define CTRL_FALLING (1u << 1)
#define CTRL_TEST    (1u << 2)
#define PPM_ID       0x50504D49u           /* "PPMI" */

static uint32_t ctrl;

int ppm_present(void)
{
	static int present = -1;

	if (present < 0)
		present = rd32(PPM_IN_BASE + ID) == PPM_ID;
	return present;
}

static void write_ctrl(void)
{
	if (ppm_present())
		wr32(PPM_IN_BASE + CTRL, ctrl);
}

void ppm_enable(int on, int falling)
{
	ctrl = (ctrl & CTRL_TEST) | (on ? CTRL_EN : 0) | (falling ? CTRL_FALLING : 0);
	write_ctrl();
}

int ppm_enabled(void)
{
	return (ctrl & CTRL_EN) != 0;
}

void ppm_test(int n, const uint16_t *us, uint32_t frame_us)
{
	if (!ppm_present())
		return;
	if (n > 0) {
		for (int i = 0; i < n && i < 16; i++)
			wr32(PPM_IN_BASE + GEN_CH(i), 2u * us[i]);
		wr32(PPM_IN_BASE + GEN_N, (uint32_t)n);
		wr32(PPM_IN_BASE + GEN_FRAME, 2u * frame_us > 0xFFFF ? 0xFFFF : 2u * frame_us);
		ctrl |= CTRL_TEST;
	} else {
		ctrl &= ~CTRL_TEST;
	}
	write_ctrl();
}

int ppm_testing(void)
{
	return (ctrl & CTRL_TEST) != 0;
}

uint16_t ppm_frames(void)
{
	return ppm_present() ? (uint16_t)rd32(PPM_IN_BASE + STATUS) : 0;
}

int ppm_channels(uint16_t *half_us)
{
	if (!ppm_present())
		return 0;
	int n = (rd32(PPM_IN_BASE + STATUS) >> 16) & 0x1F;
	for (int i = 0; i < n; i++)
		half_us[i] = (uint16_t)rd32(PPM_IN_BASE + CH(i));
	return n;
}

int ppm_pin_level(void)
{
	return ppm_present() ? (rd32(PPM_IN_BASE + STATUS) >> 24) & 1 : 0;
}
