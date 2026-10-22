/* AD9361 SPI through the AXI Quad SPI (ad9361_spi) and its reset line. */
#ifndef AD9361_SPI_H
#define AD9361_SPI_H

#include <stdint.h>

void    ad9361_spi_init(void);
int     ad9361_spi_xfer(uint8_t *buf, int len);   /* full duplex, in place, len <= 16 */
uint8_t ad9361_spi_read(uint16_t reg);
void    ad9361_spi_write(uint16_t reg, uint8_t value);
void    ad9361_hw_reset(void);

#endif
