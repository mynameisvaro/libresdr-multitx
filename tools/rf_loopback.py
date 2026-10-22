#!/usr/bin/env python3
"""Check the FPGA <-> AD9361 sample interface with the AD9361's digital loopback.

The firmware's DDS sends a complex tone into the AD9361 TX data port; the AD9361
loops the port back to RX (no RF involved) and adc_capture records it. The tone
must come back at the right frequency (sign included) and amplitude.

  rf_loopback.py                      every rate in RATES
  rf_loopback.py RATE_HZ TONE_HZ      one rate

Needs the firmware running (make -C fw load) and numpy.
"""

import struct
import sys
import time

import numpy as np

from usbdbg import ADC_CAPTURE, Bridge, Console

RATES = [(5e6, 0.4e6), (20e6, 1e6), (30.72e6, 3e6), (40e6, -5e6), (61.44e6, 10e6)]
N = 4096
DBFS = -12.0


def capture(br, n):
    br.write(ADC_CAPTURE + 0x08, [n])
    br.write(ADC_CAPTURE + 0x00, [1])
    for _ in range(100):
        if br.read(ADC_CAPTURE + 0x04)[0] & 2:
            break
        time.sleep(0.01)
    raw = br.read(ADC_CAPTURE + 0x10000, n)
    iq = np.array([struct.unpack("<hh", struct.pack("<I", s)) for s in raw], dtype=float)
    return (iq[:, 0] + 1j * iq[:, 1])[1:]       # sample 0 can be stale


def check(br, con, rate, tone):
    con.run(f"rf rate {rate}")
    con.run(f"dds {tone} {DBFS}")
    x = capture(br, N)
    n = len(x)
    spec = np.abs(np.fft.fft(x * np.hanning(n)))
    freqs = np.fft.fftfreq(n, 1 / rate)
    k = int(np.argmax(spec))
    others = spec.copy()
    others[[(k + d) % n for d in range(-8, 9)]] = 0
    spur = 20 * np.log10(others.max() / spec[k])
    amp = np.sqrt(np.mean(np.abs(x) ** 2)) / 2048
    amp_db = 20 * np.log10(amp) if amp > 0 else -999
    ok = abs(freqs[k] - tone) < 2 * rate / n and abs(amp_db - DBFS) < 1.5 and spur < -50
    print(f"[{'PASS' if ok else 'FAIL'}] rate {rate / 1e6:6.2f} MHz  tone {tone / 1e6:+6.2f} MHz -> "
          f"{freqs[k] / 1e6:+6.2f} MHz, {amp_db:5.1f} dBFS, worst spur {spur:5.1f} dBc")
    return ok


def main(argv):
    br = Bridge()
    con = Console(br)
    if "not initialised" in con.run("rf rate"):
        print(con.run("rf init"), end="")
    cases = [(float(argv[1]), float(argv[2]))] if len(argv) > 2 else RATES
    con.run("rf loopback 1")
    try:
        results = [check(br, con, rate, tone) for rate, tone in cases]
    finally:
        con.run("dds off")
        con.run("rf loopback 0")
    print(f"\n{sum(results)}/{len(results)} passed")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
