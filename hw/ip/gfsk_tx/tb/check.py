#!/usr/bin/env python3
"""Simulates gfsk_tx with xsim and demodulates its output.

  check.py      runs FrSky D8 and D16 style packets, prints PASS/FAIL per case
"""

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


def stim_for(fs, baud, dev, chan, bt, data, ampl=16000, step=200):
    r = gfsk.regs(fs, baud, dev, chan)
    w = [(0x10, r["baud_inc"]), (0x14, r["dev_inc"]), (0x18, r["chan_inc"]),
         (0x1C, (step << 16) | ampl), (0x20, 0), (0x08, 8 * len(data))]
    table = gfsk.shape_table(bt)
    for k in range(3):
        for p in range(256):
            w.append((0x1000 + 4 * (256 * k + p), table[k][p] & 0xFFFF))
    for i, b in enumerate(data):
        w.append((0x2000 + 4 * i, b))
    return w


def build():
    os.makedirs(WORK, exist_ok=True)
    cmd = (f"source {VIVADO} && cd {WORK} && cp ../../gfsk_tx_sincos.mem . && "
           f"xvlog ../../gfsk_tx.v ../tb_gfsk_tx.v > xvlog.log && "
           f"xelab -debug off tb_gfsk_tx -s tb > xelab.log")
    subprocess.run(["bash", "-c", cmd], check=True)


def run(name, fs, baud, dev, chan, bt, data):
    with open(os.path.join(WORK, "stim.mem"), "w") as f:
        for a, d in stim_for(fs, baud, dev, chan, bt, data):
            f.write(f"{a:04x}{d & 0xFFFFFFFF:08x}\n")
    period = 1e9 / fs
    out = subprocess.run(["bash", "-c", f"source {VIVADO} && cd {WORK} && "
                          f"xelab -debug off tb_gfsk_tx -generic_top DAC_PERIOD={period} -s tb > xelab.log && "
                          f"xsim tb -R"], capture_output=True, text=True)
    log = out.stdout
    fails = [l for l in log.splitlines() if "FAIL" in l]
    iq = np.loadtxt(os.path.join(WORK, "iq.txt"))
    x = iq[:, 0] + 1j * iq[:, 1]
    res = gfsk.demod(x, fs, baud, chan, gfsk.bytes_to_bits(data))
    peak = np.abs(x).max()
    ok = not fails and res["errors"] == 0 and abs(res["dev_meas"] / dev - 1) < 0.15 \
        and 15000 < peak < 16100
    print(f"[{'PASS' if ok else 'FAIL'}] {name:34s} {res['nbits']} bits, {res['errors']} errors, "
          f"deviation {res['dev_meas'] / 1e3:.1f} kHz (set {dev / 1e3:.1f}), peak {peak:.0f}, "
          f"{len(x)} samples {' '.join(fails)}")
    return ok


def main():
    build()
    random.seed(1)
    pre = [0xAA] * 4
    d8 = pre + [0xD3, 0x91, 0x11] + [random.randrange(256) for _ in range(17)] + [0x12, 0x34]
    d16 = pre + [0xD3, 0x91, 0xD3, 0x91, 0x1D] + [random.randrange(256) for _ in range(29)]
    cases = [
        ("D8: 31 kbaud, BT 1, +1 MHz", 4e6, gfsk.cc2500_baud(0xAA, 0x39), gfsk.cc2500_dev(0x42), 1e6, 1.0, d8),
        ("D16: 70 kbaud, BT 1, -1.5 MHz", 4e6, gfsk.cc2500_baud(0x7B, 0x61), gfsk.cc2500_dev(0x51), -1.5e6, 1.0, d16),
        ("D16 at 30.72 MSPS, BT 0.5, +10 MHz", 30.72e6, gfsk.cc2500_baud(0x7B, 0x61), gfsk.cc2500_dev(0x51), 10e6, 0.5, d16[:16]),
        ("2-FSK 250 kbaud, -5 MHz", 30.72e6, 250e3, 160e3, -5e6, None, d8[:12]),
    ]
    results = [run(*c) for c in cases]
    print(f"\n{sum(results)}/{len(results)} passed")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
