/* Stand-in for ExpressLRS's SX127x driver header: FHSS.cpp needs the SX127x
 * frequency step (its sequence works in register units) and the power table
 * sizes (for the domain string only). The radio itself is lora_tx. */
#pragma once

#define FREQ_STEP 61.03515625              /* 32 MHz / 2^19 */
#define POWER_OUTPUT_VALUES_COUNT 1
#define POWER_OUTPUT_VALUES_DUAL_COUNT 0
