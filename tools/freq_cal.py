#!/usr/bin/env python3
"""Calibrate the two ways we measure a FlySky transmitter's frequency.

The board's own modulator (gfsk_tx) sends an AFHDS2A bind packet (the FS-i6X's,
coded like the real chip) at known offsets from a channel, and both receive
methods measure it:

  grx      gfsk_rx's offset estimate (what a7105_sniff.py --live, the virtual
           A7105's `a7105` status and `grx` report), at 61.44 MSPS
  capture  adc_capture + a7105.demod with the RX LO 1 MHz below the channel,
           as a7105_sniff.py captures (4 MSPS)

Each line gives the offset sent and the offset measured. A method is right when
they agree to a few kHz; a constant difference is a bias, a sign change an
inversion. The deviation of the received packet is printed too (an FS-i6X
measures about 196 kHz median |f| with the capture method).

  freq_cal.py [--method grx|capture|both] [--count N] [--rx-gain DB]

TX/RX -> cable + 30 dB attenuator -> RX2 (as for rf_tone.py), or antennas.
Needs the gfsk_rx bitstream (multitx_p6 or later) and the firmware.
"""

import argparse
import re
import sys
import time

import numpy as np

import a7105
import gfsk
from usbdbg import ADC_CAPTURE, OP_WRITE, Bridge, Console

GFSK_TX = 0x44040000            # gfsk_tx: 0x00 CTRL, 0x08 NBITS, 0x2000 + 4 * i packet bytes
OFFSETS = [-750e3, -500e3, -250e3, -100e3, 0, 100e3, 250e3, 500e3, 750e3]
# the FS-i6X's first bind packet (logs/sniff_afhds2a_fsi6x.npz)
BIND = bytes.fromhex("bbcc05b6c4ffffffff0100151d6266326a8b44542d255d957f7b76ffffffffffffffffffff00")
FEC = [int(v, 16) for v in "00 0b 16 1d 27 2c 31 3a 45 4e 53 58 62 69 74 7f".split()]
CODING = a7105.Coding(0x1021, 0x1D0F, False, 0, FEC, False)
CAP_N = 16384
CAP_FS = 4e6
RX_OFFSET = 1e6                 # a7105_sniff.py's RX LO below the channel


def packet():
    """preamble + ID + FEC-coded bind packet with its CRC, as bytes"""
    crc = CODING.crc(BIND)
    body = CODING.encode(BIND + bytes([crc >> 8, crc & 0xFF]))
    bits = "01" * 16 + "".join(f"{b:08b}" for b in a7105.FLYSKY_ID) + body
    bits += "0" * (-len(bits) % 8)
    return bytes(int(bits[i:i + 8], 2) for i in range(0, len(bits), 8)), len(body)


def deviation(x, fs, off):
    f = gfsk.discriminator(x, fs) - off
    return float(np.median(np.abs(f[40:-40])))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--method", choices=["grx", "capture", "both"], default="both")
    ap.add_argument("--count", type=int, default=2, help="packets per offset")
    ap.add_argument("--rx-gain", type=float, default=30)
    a = ap.parse_args()

    br = Bridge()
    con = Console(br)
    pkt, nbody = packet()

    def run(c, timeout=30):
        out = con.run(c, timeout=timeout).strip()
        if "usage" in out or "unknown" in out or "failed" in out or "no gfsk_rx" in out:
            raise SystemExit(f"'{c}': {out}")
        return out

    def load_packet():
        br.write(GFSK_TX + 0x2000, list(pkt))
        br.write(GFSK_TX + 0x08, [8 * len(pkt)])

    rows = []
    try:
        if a.method in ("grx", "both"):
            for c in ("mpm stop", "radio stop", "radio rate 61440000", "radio start",
                      f"rf gain rx {a.rx_gain}", "rf gain tx 76", "gfsk cfg 500000 186000 0.7", "gfsk level -10"):
                run(c)
            lo = float(re.search(r"LO ([0-9.]+) MHz", run("mpm status")).group(1)) * 1e6
            chan = lo + 5e6                     # a channel inside the slice, away from the LO
            load_packet()
            print(f"grx: channel {chan / 1e6:.3f} MHz (TX LO {lo / 1e6:.3f})")
            for d in OFFSETS:
                for _ in range(a.count):
                    run(f"grx arm {chan:.0f} 500000 {a7105.FLYSKY_ID.hex()} {nbody}")
                    run(f"gfsk chan {chan + d - lo:.0f}")
                    br.write(GFSK_TX + 0x00, [1])
                    time.sleep(0.05)
                    lines = run("grx").splitlines()
                    m = re.search(r"offset ([-+0-9.]+) kHz", lines[0]) if lines else None
                    got = float(m.group(1)) * 1e3 if m and lines[0].startswith("done") else None
                    ok = ""
                    if got is not None and len(lines) > 1:
                        bits = "".join(f"{b:08b}" for b in bytes.fromhex(lines[1].strip()))
                        data, fixed = CODING.decode(bits, len(BIND) + 2)
                        ok = "packet exact" if data[:len(BIND)] == BIND and fixed == 0 else f"{fixed} bits fixed"
                    rows.append(("grx", d, got))
                    print(f"  sent {d / 1e3:+7.1f} kHz  measured "
                          f"{'   ---' if got is None else f'{got / 1e3:+7.1f}'} kHz  {ok or lines[0]}", flush=True)
            run("grx stop")

        if a.method in ("capture", "both"):
            chan = a7105.chan_freq(140)
            for c in ("mpm stop", "radio stop", f"radio rate {CAP_FS:.0f}", "radio start",
                      f"rf gain rx {a.rx_gain}", "rf gain tx 76", "gfsk cfg 500000 186000 0.7", "gfsk level -10"):
                run(c)
            tx = run(f"rf freq tx {chan:.0f}")
            rx = run(f"rf freq rx {chan - RX_OFFSET:.0f}")
            tx_lo = float(re.search(r"LO ([0-9.]+) Hz", tx).group(1))
            rx_lo = float(re.search(r"LO ([0-9.]+) Hz", rx).group(1))
            print(f"capture: channel 140 ({chan / 1e6:.3f} MHz), TX LO {tx_lo / 1e6:.6f}, RX LO {rx_lo / 1e6:.6f}")
            load_packet()
            br.write(ADC_CAPTURE + 0x08, [CAP_N])
            for d in OFFSETS:
                for _ in range(a.count):
                    run(f"gfsk chan {chan + d - tx_lo:.0f}")
                    # capture and packet start in one bridge transaction: microseconds apart
                    br._check(br.transact([(OP_WRITE, 0, 1, ADC_CAPTURE + 0x00, [1]),
                                           (OP_WRITE, 0, 1, GFSK_TX + 0x00, [1])]))
                    time.sleep(CAP_N / CAP_FS + 0.01)
                    x = a7105.iq_from_raw(br.read(ADC_CAPTURE + 0x10000, CAP_N))
                    # as a7105_sniff.py: the channel to 0 Hz with the nominal shift
                    x *= np.exp(-2j * np.pi * RX_OFFSET * np.arange(CAP_N) / CAP_FS)
                    b = a7105.bursts(x, int(100e-6 * CAP_FS))
                    if not b:
                        rows.append(("capture", d, None))
                        print(f"  sent {d / 1e3:+7.1f} kHz  no burst", flush=True)
                        continue
                    s, e = max(b, key=lambda se: se[1] - se[0])
                    xb = x[max(0, s - 20):e + 20]
                    bits, off = a7105.demod(xb, CAP_FS)
                    pos = a7105.find_id(bits)
                    ok = "no ID"
                    if pos >= 0:
                        data, fixed = CODING.decode(bits[pos:], len(BIND) + 2)
                        ok = "packet exact" if data[:len(BIND)] == BIND and fixed == 0 else f"{fixed} bits fixed"
                    rows.append(("capture", d, off))
                    print(f"  sent {d / 1e3:+7.1f} kHz  measured {off / 1e3:+7.1f} kHz  "
                          f"deviation {deviation(xb, CAP_FS, off) / 1e3:.0f} kHz  {ok}", flush=True)
    finally:
        con.run("grx stop")
        con.run("radio stop")

    print()
    for method in ("grx", "capture"):
        pts = [(d, g) for m, d, g in rows if m == method and g is not None]
        if len(pts) < 2:
            continue
        d, g = np.array(pts).T
        slope, bias = np.polyfit(d, g, 1)
        resid = g - (slope * d + bias)
        print(f"{method:7s}: measured = {slope:.4f} x sent {bias / 1e3:+.1f} kHz "
              f"(residual rms {np.std(resid) / 1e3:.1f} kHz, {len(pts)}/{sum(m == method for m, _, _ in rows)} received)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
