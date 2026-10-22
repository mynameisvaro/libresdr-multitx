// SBUS monitor for an ESP32: reads a receiver's SBUS output and prints, once a
// second, how many frames arrived, how many the receiver flagged as lost, the
// failsafe flag and channels 1-8. Used to check MultiTX's link to a receiver
// that has no servo outputs (FrSky XM+, R-XSR, ...).
//
// Wiring (classic ESP32 DevKit; any free pin works, set SBUS_RX_PIN):
//   receiver +5V  -> ESP32 5V / VIN (USB 5 V)
//   receiver GND  -> ESP32 GND
//   receiver SBUS -> GPIO16
// FrSky receivers drive SBUS at 3.3 V, so no level shifter is needed.
//
// SBUS: 100000 baud, 8E2, inverted (the ESP32 UART inverts it itself). A frame
// is 25 bytes: 0x0F, 16 channels x 11 bits (LSB first), flags (bit 2 = frame
// lost, bit 3 = failsafe), 0x00. Frames are separated by a gap of a few ms,
// which is how the start is found.
//
// Output, 115200 baud:
//   fps 111  lost 2 (1.8%)  failsafe 0  |  992  992  172  992  992  992  992  992

#define SBUS_RX_PIN 16
#define SBUS_TX_PIN 17      // unused, the UART wants one
#define FRAME_GAP_US 2000

static uint8_t frame[25];
static int pos;
static uint32_t last_byte_us;
static uint16_t ch[16];
static uint8_t flags;
static uint32_t n_frames, n_lost, n_failsafe, n_bad;

static void decode(void)
{
	uint32_t acc = 0;
	int bits = 0, c = 0;
	for (int i = 1; i < 23 && c < 16; i++) {
		acc |= (uint32_t)frame[i] << bits;
		bits += 8;
		while (bits >= 11 && c < 16) {
			ch[c++] = acc & 0x7FF;
			acc >>= 11;
			bits -= 11;
		}
	}
	flags = frame[23];
}

void setup()
{
	Serial.begin(115200);
	Serial2.begin(100000, SERIAL_8E2, SBUS_RX_PIN, SBUS_TX_PIN, true);
	Serial.printf("sbus_monitor: waiting for SBUS on GPIO%d\n", SBUS_RX_PIN);
}

void loop()
{
	static uint32_t last_print_ms;

	while (Serial2.available()) {
		uint32_t now = micros();
		uint8_t b = Serial2.read();
		if (now - last_byte_us > FRAME_GAP_US)
			pos = 0;
		last_byte_us = now;
		if (pos < (int)sizeof(frame))
			frame[pos++] = b;
		if (pos == sizeof(frame)) {
			if (frame[0] == 0x0F && frame[24] == 0x00) {
				decode();
				n_frames++;
				n_lost += (flags >> 2) & 1;
				n_failsafe += (flags >> 3) & 1;
				pos = 0;        // frames read back to back still line up
			} else {
				n_bad++;
				pos++;          // out of step: ignore the rest until the next gap
			}
		}
	}

	if (millis() - last_print_ms >= 1000) {
		last_print_ms += 1000;
		char line[160];
		int n = snprintf(line, sizeof(line), "fps %3lu  lost %lu (%.1f%%)  failsafe %lu",
		                 (unsigned long)n_frames, (unsigned long)n_lost,
		                 n_frames ? 100.0 * n_lost / n_frames : 0.0, (unsigned long)n_failsafe);
		if (n_bad)
			n += snprintf(line + n, sizeof(line) - n, "  bad %lu", (unsigned long)n_bad);
		n += snprintf(line + n, sizeof(line) - n, "  |");
		for (int i = 0; i < 8; i++)
			n += snprintf(line + n, sizeof(line) - n, " %4u", ch[i]);
		Serial.println(n_frames ? line : "no SBUS frames");
		n_frames = n_lost = n_failsafe = n_bad = 0;
	}
}
