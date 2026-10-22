#!/usr/bin/env python3
"""GFSK helpers shared by the gfsk_tx simulation and the hardware tests.

- shape_table(): the 3 x 256 frequency-pulse table gfsk_tx uses (the firmware
  computes the same thing in fw/src/gfsk.c)
- regs(): phase increments for a symbol rate, deviation and channel offset
- demod(): FM discriminator + symbol timing + bit-error count against the
  expected bits
- cc2500_*(): CC2500 register maths (26 MHz crystal)

  gfsk.py sincos FILE.mem     write the sin/cos ROM for gfsk_tx
"""

import math
import sys

import numpy as np

TABLE_POS = 256          # positions per symbol in the shaping table
FXOSC = 26e6             # CC2500 crystal


# ------------------------------------------------------------------ modulator side
def pulse(t, bt):
    """Gaussian-filtered rectangular frequency pulse (t in symbols, centred on 0).
    bt = None gives plain FSK (a rectangle)."""
    if bt is None:
        return 1.0 if -0.5 <= t < 0.5 else 0.0
    sigma = math.sqrt(math.log(2)) / (2 * math.pi * bt)
    phi = lambda x: 0.5 * (1 + math.erf(x / (sigma * math.sqrt(2))))
    return phi(t + 0.5) - phi(t - 0.5)


def shape_table(bt):
    """SHAPE[k][pos] as Q1.15 ints; k = 0 previous, 1 current, 2 next symbol."""
    table = [[0] * TABLE_POS for _ in range(3)]
    for p in range(TABLE_POS):
        tau = (p + 0.5) / TABLE_POS                     # inside the current symbol
        vals = [pulse(tau - (k - 1) - 0.5, bt) for k in range(3)]
        total = sum(vals)                               # renormalise the truncation
        for k in range(3):
            table[k][p] = int(round(32767 * vals[k] / total))
    return table


def inc(hz, fs):
    """Phase increment (32-bit, two's complement) for a frequency at sample rate fs."""
    return int(round(hz / fs * 2**32)) & 0xFFFFFFFF


def regs(fs, baud, dev, chan):
    return {"baud_inc": inc(baud, fs), "dev_inc": inc(dev, fs), "chan_inc": inc(chan, fs)}


def bytes_to_bits(data, lsb_first=False):
    bits = []
    for b in data:
        order = range(8) if lsb_first else range(7, -1, -1)
        bits += [(b >> i) & 1 for i in order]
    return bits


def write_sincos(path):
    with open(path, "w") as f:
        for n in range(4096):
            a = 2 * math.pi * n / 4096
            c = int(round(32767 * math.cos(a))) & 0xFFFF
            s = int(round(32767 * math.sin(a))) & 0xFFFF
            f.write(f"{s:04x}{c:04x}\n")


# ------------------------------------------------------------------ CC2500 maths
def cc2500_baud(mdmcfg4, mdmcfg3):
    return (256 + mdmcfg3) * 2 ** (mdmcfg4 & 0x0F) * FXOSC / 2**28


def cc2500_dev(deviatn):
    return FXOSC / 2**17 * (8 + (deviatn & 7)) * 2 ** ((deviatn >> 4) & 7)


def cc2500_freq(freq2, freq1, freq0, chan=0, mdmcfg1=0x23, mdmcfg0=0x7A, fsctrl0=0):
    base = FXOSC / 2**16 * ((freq2 << 16) | (freq1 << 8) | freq0)
    spacing = FXOSC / 2**18 * (256 + mdmcfg0) * 2 ** (mdmcfg1 & 3)
    offset = FXOSC / 2**14 * (fsctrl0 - 256 if fsctrl0 & 0x80 else fsctrl0)
    return base + chan * spacing + offset


# ------------------------------------------------------------------ demodulator
def discriminator(x, fs):
    """Instantaneous frequency (Hz) of complex samples."""
    return np.angle(x[1:] * np.conj(x[:-1])) * fs / (2 * np.pi)


def demod(x, fs, baud, chan, expect_bits, dev=None):
    """Recover bits from a capture and compare them with expect_bits.

    Returns a dict: errors, nbits, offset (bit index where the packet was
    found), dev_meas (95th percentile of |f| over the packet), timing."""
    f = discriminator(x, fs) - chan
    sps = fs / baud
    # active part: where the amplitude is up
    amp = np.abs(x)
    on = np.nonzero(amp > 0.3 * amp.max())[0]
    if len(on) < 4 * sps:
        return {"errors": None, "nbits": 0, "why": "no signal"}
    f = f[on[0]:on[-1]]
    # integrate over half a symbol (the per-sample discriminator is noisy at high
    # sample rates), then pick the symbol timing with the widest eye
    k = max(1, int(round(sps / 2)))
    f = np.convolve(f, np.ones(k) / k, mode="same")
    best = None
    for o in np.linspace(0, sps, 32, endpoint=False):
        idx = np.round(o + np.arange(int((len(f) - 1 - o) / sps) + 1) * sps).astype(int)
        idx = idx[idx < len(f)]
        score = np.mean(np.abs(f[idx]))
        if best is None or score > best[0]:
            best = (score, o, idx)
    _, o, idx = best
    got = (f[idx] > 0).astype(int)
    exp = np.array(expect_bits)
    # align: the expected packet somewhere inside the decoded bits
    n = len(exp)
    res = {"errors": n, "offset": None}
    for s in range(0, max(1, len(got) - n + 1)):
        e = int(np.sum(got[s:s + n] != exp))
        if e < res["errors"]:
            res = {"errors": e, "offset": s}
    res["nbits"] = n
    res["dev_meas"] = float(np.percentile(np.abs(f), 99))
    res["timing"] = float(o)
    return res


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "sincos":
        write_sincos(sys.argv[2])
    else:
        print(__doc__)
