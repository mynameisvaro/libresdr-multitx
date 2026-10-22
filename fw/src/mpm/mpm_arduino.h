// The slice of the Arduino API that Multiprotocol's protocol files use, on top
// of the MultiTX firmware. Included before any Multiprotocol source.
#pragma once

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
#include "cc2500.h"
#include "timer.h"
}

typedef bool boolean;
typedef uint8_t byte;

// AVR program memory: plain memory here
#define PROGMEM
#define pgm_read_byte(p)       (*(const uint8_t *)(p))
#define pgm_read_byte_near(p)  (*(const uint8_t *)(p))
#define pgm_read_word(p)       (*(const uint16_t *)(p))
#define pgm_read_word_near(p)  (*(const uint16_t *)(p))
#define pgm_read_dword(p)      (*(const uint32_t *)(p))
#define _BV(b)                 (1u << (b))
#define NOP()                  do { } while (0)
#define cli()                  do { } while (0)
#define sei()                  do { } while (0)

// time
static inline uint32_t millis(void) { return timer_ms(); }
static inline uint32_t micros(void) { return (uint32_t)(timer_ticks() / (CPU_HZ / 1000000u)); }
static inline void delayMicroseconds(uint32_t us) { delay_us(us); }
static inline void delayMilliseconds(uint32_t ms) { delay_ms(ms); }
static inline void delay(uint32_t ms) { delay_ms(ms); }
#define _delay_ms(ms)          delay_ms(ms)
#define _delay_us(us)          delay_us(us)

// random numbers (Arduino semantics: random(0xfefefefe) is a negative long and
// still returns random() % howbig)
static inline long random(long howbig) { return howbig ? (long)(rand() % howbig) : 0; }
static inline long random(long lo, long hi) { return hi > lo ? lo + random(hi - lo) : lo; }
static inline void randomSeed(unsigned long s) { srand(s); }

// EEPROM: kept in RAM for now (IDs and clone data are lost on reset)
typedef uint8_t *EE_ADDR;
uint8_t eeprom_read_byte(EE_ADDR a);
void    eeprom_write_byte(EE_ADDR a, uint8_t v);

// debug output from the protocol files
#ifdef MPM_DEBUG
	#define debug(...)        printf(__VA_ARGS__)
	#define debugln(...)      do { printf(__VA_ARGS__); printf("\n"); } while (0)
#else
	#define debug(...)        do { } while (0)
	#define debugln(...)      do { } while (0)
#endif

// SPI and chip selects: the radio chips are virtual (fw/src/cc2500.c etc.)
uint8_t SPI_Read(void);
void    SPI_Write(uint8_t b);
#define CC25_CSN_on            cc2500_csn(1)
#define CC25_CSN_off           cc2500_csn(0)
