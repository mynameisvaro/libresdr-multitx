// Prototypes the Arduino builder would generate for Multiprotocol's .ino files
// (functions used before their definition in the concatenated sketch).
#pragma once

uint16_t limit_channel_100(uint8_t ch);
void A7105_Strobe(uint8_t address);
void A7105_SetTxRxMode(uint8_t mode);
void A7105_WriteReg(uint8_t address, uint8_t data);
uint8_t A7105_ReadReg(uint8_t address);
