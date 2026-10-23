#!/usr/bin/env python3
"""Simulates gfsk_rx with xsim on GFSK packets made here, and checks the bits.

  check.py      A7105 (FlySky) and CC2500 (FrSky) style packets: channel and
                carrier offsets, low and high levels, noise, and noise alone

Each case builds a packet (preamble, sync word, random payload), modulates it
(Gaussian-filtered FSK) at 61.44 MSPS, moves it to a channel offset, adds a
carrier error and noise, and runs the RTL with the register values the firmware
uses (rx_regs below, the same maths as fw/src/gfsk_rx.c). PASS: every payload
bit captured, and nothing found in noise. Samples come on every other clock
with garbage in between, as from the AD9361 core.
"""

import math
import os
import random
import subprocess
import sys

import numpy as np

here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(here, "../../../../tools"))
import gfsk  # noqa: E402

VIVADO = "/opt/AMD/2022.2/Vivado/2022.2/settings64.sh"
WORK = os.path.join(here, "work")
FS = 61.44e6


def rx_regs(fs, baud, chan_hz, sync, nbits, mask=0xFFFFFFFF, maxerr=2, invert=False):
    """Register values for gfsk_rx (fw/src/gfsk_rx.c does the same)."""
    r = max(1, min(256, int(fs / (baud * 8))))       # about 8 samples per bit
    shift = max(0, math.ceil(3 * math.log2(r)) - 6)   # full scale stays inside 18 bits
    fs_dec = fs / r
    spb = fs_dec / baud
    tc = max(1, int(round(math.log2(spb * 4))))       # offset tracker: about 4 bits
    gain = 2
    return [
        (0x10, int(round(chan_hz / fs * 2 ** 32)) & 0xFFFFFFFF),
        (0x14, shift << 16 | r),
        (0x18, int(round(baud / fs_dec * 2 ** 32)) & 0xFFFFFFFF),
        (0x1C, sync),
        (0x20, mask),
        (0x24, gain << 24 | tc << 16 | int(invert) << 8 | maxerr),
        (0x08, nbits),
    ]


def modulate(bits, fs, baud, dev, bt, lead=200e-6, tail=100e-6):
    sps = fs / baud
    n = int(len(bits) * sps)
    nrz = np.array([1.0 if b else -1.0 for b in bits])
    x = nrz[np.minimum((np.arange(n) / sps).astype(int), len(bits) - 1)]
    if bt > 0:
        # Gaussian filter over +-2 symbols
        t = np.arange(-2 * sps, 2 * sps + 1) / sps
        sigma = math.sqrt(math.log(2)) / (2 * math.pi * bt)
        h = np.exp(-t ** 2 / (2 * sigma ** 2))
        x = np.convolve(x, h / h.sum(), mode="same")
    phase = 2 * math.pi * dev * np.cumsum(x) / fs
    s = np.exp(1j * phase)
    pad0 = np.zeros(int(lead * fs), complex)
    pad1 = np.zeros(int(tail * fs), complex)
    return np.concatenate([pad0, s, pad1])


def bits_of(data):
    return [(b >> (7 - i)) & 1 for b in data for i in range(8)]


def build():
    os.makedirs(WORK, exist_ok=True)
    cmd = (f"source {VIVADO} && cd {WORK} && cp ../../../gfsk_tx/gfsk_tx_sincos.mem . && "
           f"xvlog ../../gfsk_rx.v ../tb_gfsk_rx.v > xvlog.log")
    subprocess.run(["bash", "-c", cmd], check=True)


def run(name, baud, dev, bt, pre, sync_bytes, payload_bits, chan, cfo, ampl, noise, expect=True, every=2):
    rng = np.random.default_rng(abs(hash(name)) % 2 ** 32)
    bits = pre + bits_of(sync_bytes) + payload_bits + [0, 1, 0, 1]
    x = modulate(bits, FS, baud, dev, bt) * ampl if expect else np.zeros(int(1.2e-3 * FS), complex)
    t = np.arange(len(x)) / FS
    x = x * np.exp(2j * math.pi * (chan + cfo) * t)
    x = x + noise * (rng.standard_normal(len(x)) + 1j * rng.standard_normal(len(x)))
    i = np.clip(np.round(x.real), -2048, 2047).astype(int) & 0xFFFF
    q = np.clip(np.round(x.imag), -2048, 2047).astype(int) & 0xFFFF
    with open(os.path.join(WORK, "iq.mem"), "w") as f:
        for a, b in zip(i, q):
            f.write(f"{a:04x}{b:04x}\n")
    sync = int.from_bytes(sync_bytes[-4:], "big")
    with open(os.path.join(WORK, "stim.mem"), "w") as f:
        for a, d in rx_regs(FS, baud, chan, sync, len(payload_bits)):
            f.write(f"{a:04x}{d:08x}\n")
    out = subprocess.run(["bash", "-c", f"source {VIVADO} && cd {WORK} && "
                          f"xelab -debug off tb_gfsk_rx -generic_top ADC_PERIOD={1e9 / FS / every} "
                          f"-generic_top NSAMP={len(x)} -generic_top VALID_EVERY={every} -s tb > xelab.log && xsim tb -R"],
                         capture_output=True, text=True)
    log = out.stdout
    res = {}
    got = []
    for line in log.splitlines():
        p = line.split()
        if p and p[0] in ("STATUS", "TSTAMP", "FOFS", "LEVEL", "SYNCERR"):
            res[p[0]] = p[1]
        if p and p[0] == "BYTE":
            got.append(int(p[2], 16) if "x" not in p[2] else 0)
    if "SIM_DONE" not in log:
        print(f"[FAIL] {name}: simulation did not finish\n{log[-2000:]}")
        return False
    status = int(res["STATUS"], 16)
    done = bool(status & 2)
    got_bits = [(b >> (7 - k)) & 1 for b in got for k in range(8)][:len(payload_bits)]
    nerr = sum(a != b for a, b in zip(got_bits, payload_bits)) if done else len(payload_bits)
    fs_dec = FS / max(1, min(256, int(FS / (baud * 8))))
    fofs_hz = int(res["FOFS"]) / 65536 * fs_dec
    if expect:
        ok = done and nerr == 0
        detail = (f"{len(payload_bits)} bits, {nerr} wrong, sync errors {res['SYNCERR']}, "
                  f"offset estimate {fofs_hz / 1e3:+.1f} kHz (true {cfo / 1e3:+.1f}), level {res['LEVEL']}")
    else:
        ok = not done
        detail = "nothing found" if ok else "FALSE DETECTION"
    print(f"[{'PASS' if ok else 'FAIL'}] {name:52s} {detail}", flush=True)
    return ok


def main():
    build()
    random.seed(3)
    flysky_id = bytes([0x54, 0x75, 0xC5, 0x2A])
    a7105_pre = bits_of(b"\x55" * 4)
    cc_pre = bits_of(b"\xAA" * 4)
    cc_sync = bytes([0xD3, 0x91, 0xD3, 0x91])
    cc_baud = gfsk.cc2500_baud(0x7B, 0x61)
    p_fly = [random.randrange(2) for _ in range(39 * 14)]      # AFHDS2A RX packet + CRC, FEC
    p_cc = [random.randrange(2) for _ in range(30 * 8)]
    cases = [
        ("A7105 500k, +5 MHz, cfo +40 kHz, level 400", 500e3, 186e3, 0.7, a7105_pre, flysky_id, p_fly,
         5e6, 40e3, 400, 8),
        ("A7105 500k, -12 MHz, cfo -60 kHz, level 40", 500e3, 186e3, 0.7, a7105_pre, flysky_id, p_fly,
         -12e6, -60e3, 40, 4),
        ("A7105 500k, +15 MHz, level 1500 (strong)", 500e3, 186e3, 0.7, a7105_pre, flysky_id, p_fly,
         15e6, 10e3, 1500, 8),
        (f"CC2500 {cc_baud / 1e3:.0f}k, +3 MHz, cfo +30 kHz, level 300", cc_baud, 57129, 1.0, cc_pre, cc_sync,
         p_cc, 3e6, 30e3, 300, 8),
        (f"CC2500 {cc_baud / 1e3:.0f}k, -8 MHz, cfo -45 kHz, level 50", cc_baud, 57129, 1.0, cc_pre, cc_sync,
         p_cc, -8e6, -45e3, 50, 4),
    ]
    results = [run(*c) for c in cases]
    results.append(run("A7105, a sample on every clock", 500e3, 186e3, 0.7, a7105_pre, flysky_id, p_fly,
                       5e6, 40e3, 400, 8, every=1))
    results.append(run("noise only (A7105 settings)", 500e3, 186e3, 0.7, a7105_pre, flysky_id, p_fly,
                       5e6, 0, 0, 30, expect=False))
    print(f"\n{sum(results)}/{len(results)} passed")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
