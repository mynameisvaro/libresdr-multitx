/* Stand-in for ExpressLRS's lib/OPTIONS: the firmware options FHSS.cpp reads
 * (the regulatory domain, an index into its domains[] table). Set by elrs_tx.cpp. */
#pragma once

#include "targets.h"

typedef struct {
	uint8_t domain;
} firmware_options_t;

extern firmware_options_t firmwareOptions;
extern const char version[];
