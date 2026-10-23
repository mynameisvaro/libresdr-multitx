/* Stand-in for <Arduino.h>: what ExpressLRS's OTA, FHSS and CRC code (and the
 * targets.h they include) need from it, on the MicroBlaze and on the PC. */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>

#define IRAM_ATTR
#define constrain(amt, low, high) ((amt) < (low) ? (low) : ((amt) > (high) ? (high) : (amt)))
