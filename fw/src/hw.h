/* MultiTX platform: address map and register helpers (see docs/architecture.md). */
#ifndef HW_H
#define HW_H

#include <stdint.h>

#define CODE_RAM_BASE    0x00000000u   /* 512 KB, instruction LMB + AXI */
#define DATA_RAM_BASE    0x00100000u   /* 64 KB, data LMB */
#define GPIO_FE_BASE     0x40000000u   /* RF switch / LED word per channel */
#define GPIO_CTRL_BASE   0x40010000u   /* AD9361 control lines */
#define GPIO_DAC_BASE    0x40020000u   /* TCXO tuning word */
#define UART_BASE        0x40600000u   /* UART Lite, CN2 */
#define INTC_BASE        0x41200000u
#define TIMER_BASE       0x41C00000u
#define ADC_CAPTURE_BASE 0x44000000u
#define GFSK_TX_BASE     0x44040000u   /* GFSK modulator + channel NCO */
#define LORA_TX_BASE     0x44050000u   /* LoRa chirp modulator + channel NCO */
#define PPM_IN_BASE      0x44060000u   /* PPM input (CN2 GPIO_0) */
#define AD9361_SPI_BASE  0x44A00000u   /* AXI Quad SPI */
#define AXI_AD9361_BASE  0x79020000u   /* ADI axi_ad9361 core */

#define CPU_HZ           100000000u

/* AXI GPIO */
#define GPIO_DATA        0x00
#define GPIO_TRI         0x04
#define GPIO2_DATA       0x08
#define GPIO2_TRI        0x0C

/* gpio_ctrl channel 1 bits */
#define CTRL_RESETB      (1u << 0)
#define CTRL_EN_AGC      (1u << 1)
#define CTRL_SYNC        (1u << 2)
#define CTRL_IN_SHIFT    3
#define CTRL_ENABLE      (1u << 7)
#define CTRL_TXNRX       (1u << 8)
#define CTRL_LED         (1u << 9)

static inline uint32_t rd32(uint32_t addr)
{
	return *(volatile uint32_t *)addr;
}

static inline void wr32(uint32_t addr, uint32_t value)
{
	*(volatile uint32_t *)addr = value;
}

#endif
