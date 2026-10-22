#!/usr/bin/env python3
"""First over-the-air check: a DDS tone out of TX/RX, received on RX2.

Connect TX/RX to RX2 with a cable and an attenuator (30 dB or more), or put
antennas on both. The TX and RX LOs are offset so every impairment lands on its
own frequency:

  TX LO = F, tone = F + 1 MHz, RX LO = F - 2 MHz
  -> tone at +3 MHz, TX LO leakage at +2 MHz, TX sideband image at +1 MHz,
     RX DC at 0, RX image of the tone at -3 MHz

  rf_tone.py [--freq HZ] [--tx-gain DB] [--rx-gain DB] [--chan 1|2] [--csv FILE]
             [--plot FILE.png]

AD9361 chain 1 (TX1A/RX1A) is wired to front-end channel 1 (gpio_fe word 1).

Needs the firmware running (make -C fw load) and numpy.
"""

import argparse
import struct
import sys
import time

import numpy as np

from usbdbg import ADC_CAPTURE, Bridge, Console

RATE = 30.72e6
N = 16384
PA_SETTLE_S = 0.6       # TX_ENABLE -> full output takes ~450 ms on the B220 mini


def capture(br, n):
    br.write(ADC_CAPTURE + 0x08, [n])
    br.write(ADC_CAPTURE + 0x00, [1])
    for _ in range(100):
        if br.read(ADC_CAPTURE + 0x04)[0] & 2:
            break
        time.sleep(0.01)
    raw = br.read(ADC_CAPTURE + 0x10000, n)
    iq = np.array([struct.unpack("<hh", struct.pack("<I", s)) for s in raw], dtype=float)
    return (iq[:, 0] + 1j * iq[:, 1])[1:] / 2048.0      # full scale = 1.0


def level_dbfs(spec, freqs, f, width=20e3):
    """Peak level near frequency f (dBFS, window-corrected)."""
    sel = np.abs(freqs - f) <= width
    return 20 * np.log10(spec[sel].max() + 1e-12)


def plot(path, freqs, spec, rows, args):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    order = np.argsort(freqs)
    fig, ax = plt.subplots(figsize=(10, 4.5))
    ax.plot(freqs[order] / 1e6, 20 * np.log10(spec[order] + 1e-12), lw=0.7)
    for name, f in rows:
        ax.axvline(f / 1e6, color="0.6", lw=0.6, ls="--")
        ax.annotate(name, (f / 1e6, -10), rotation=90, fontsize=8, ha="right", va="top")
    ax.set_xlabel(f"baseband frequency (MHz), RX LO = {(args.freq - 2e6) / 1e6:.1f} MHz")
    ax.set_ylabel("dBFS")
    ax.set_ylim(-120, 0)
    ax.set_xlim(-RATE / 2e6, RATE / 2e6)
    ax.grid(alpha=0.3)
    ax.set_title(f"B220 mini: TX/RX -> RX2, tone at {(args.freq + 1e6) / 1e6:.1f} MHz, "
                 f"TX gain {args.tx_gain:g} dB, RX gain {args.rx_gain:g} dB")
    fig.tight_layout()
    fig.savefig(path, dpi=120)
    print(f"  spectrum -> {path}")


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--freq", type=float, default=2440e6, help="TX LO (default 2440 MHz)")
    ap.add_argument("--tx-gain", type=float, default=50, help="0..89.75 dB (default 50)")
    ap.add_argument("--rx-gain", type=float, default=30, help="0..76 dB (default 30)")
    ap.add_argument("--chan", type=int, default=1, help="front-end channel (default 1)")
    ap.add_argument("--dbfs", type=float, default=-6, help="DDS level (default -6 dBFS)")
    ap.add_argument("--csv", help="save the capture (i,q)")
    ap.add_argument("--plot", help="save the spectrum as an image (needs matplotlib)")
    args = ap.parse_args(argv[1:])

    br = Bridge()
    con = Console(br)
    if "not initialised" in con.run("rf rate"):
        print(con.run("rf init"), end="")
    tone_off, rx_off = 1e6, -2e6
    for cmd in (f"rf loopback 0", f"rf rate {RATE}",
                f"rf freq tx {args.freq}", f"rf freq rx {args.freq + rx_off}",
                f"rf gain tx {args.tx_gain}", f"rf gain rx {args.rx_gain}",
                f"fe {args.chan} fdd", f"dds {tone_off} {args.dbfs}"):
        print(f"  {cmd:28s} {con.run(cmd).strip()}")
    time.sleep(PA_SETTLE_S)
    try:
        x = capture(br, N)
        rssi = con.run("rf rssi").strip()
    finally:
        con.run("dds off")
        con.run(f"fe {args.chan} off")

    if args.csv:
        np.savetxt(args.csv, np.column_stack([x.real, x.imag]) * 2048, fmt="%d",
                   delimiter=",", header="i,q", comments="")
    n = len(x)
    win = np.blackman(n)
    spec = np.abs(np.fft.fft(x * win)) / win.sum()
    freqs = np.fft.fftfreq(n, 1 / RATE)
    peak = freqs[np.argmax(spec)]
    tone_bb = tone_off - rx_off
    tone = level_dbfs(spec, freqs, tone_bb)
    floor = 20 * np.log10(np.median(spec))
    print(f"\n  capture: {n} samples, peak at {peak / 1e6:+.3f} MHz, {rssi}")
    rows = [("tone (expected here)", tone_bb), ("TX LO leakage", -rx_off),
            ("TX sideband image", -rx_off - tone_off), ("RX DC", 0.0),
            ("RX image of the tone", -tone_bb)]
    for name, f in rows:
        lvl = level_dbfs(spec, freqs, f)
        rel = "" if f == tone_bb else f"  {lvl - tone:6.1f} dBc"
        print(f"  {name:22s} {f / 1e6:+6.2f} MHz  {lvl:6.1f} dBFS{rel}")
    print(f"  {'noise floor (median)':22s}              {floor:6.1f} dBFS/bin")

    if args.plot:
        plot(args.plot, freqs, spec, rows, args)

    found = abs(peak - tone_bb) < 50e3 and tone - floor > 20
    if not found:
        print("\nTone not found. Check the cable/attenuator (TX/RX -> RX2), try --chan 2, "
              "or raise --tx-gain / --rx-gain.")
    elif tone > -3:
        print("\nTone close to full scale: lower --rx-gain or --tx-gain.")
    return 0 if found else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
