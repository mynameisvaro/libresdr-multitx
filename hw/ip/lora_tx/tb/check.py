#!/usr/bin/env python3
"""Simulates lora_tx with xsim and decodes its output with tools/lora.py.

  check.py      LoRa frames at several SF / CR / sample rates, prints PASS/FAIL

The frames are built with lora.py's encoder (checked symbol for symbol against
real SX127x recordings) and decoded by its receiver, so a PASS means the RTL's
chirps carry exactly the symbols the encoder asked for.
"""

import os
import random
import subprocess
import sys

import numpy as np

here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(here, "../../../../tools"))
import lora  # noqa: E402

VIVADO = "/opt/AMD/2022.2/Vivado/2022.2/settings64.sh"
WORK = os.path.join(here, "work")


def inc(hz, fs):
    return int(round(hz / fs * 2 ** 32)) & 0xFFFFFFFF


def stim_for(fs, bw, sf, chan, entries, invert_iq, ampl=16000, step=400):
    w = [(0x10, inc(bw, fs)), (0x14, sf), (0x18, inc(chan, fs)), (0x1C, step << 16 | ampl),
         (0x20, int(invert_iq)), (0x08, len(entries))]
    w += [(0x1000 + 4 * i, t << 12 | v) for i, (t, v) in enumerate(entries)]
    return w


def build():
    os.makedirs(WORK, exist_ok=True)
    cmd = (f"source {VIVADO} && cd {WORK} && cp ../../../gfsk_tx/gfsk_tx_sincos.mem . && "
           f"xvlog ../../lora_tx.v ../tb_lora_tx.v > xvlog.log")
    subprocess.run(["bash", "-c", cmd], check=True)


def run(name, fs, bw, sf, cr, chan, payload, implicit, crc, invert_iq=False, preamble=8, sync=0x12):
    syms = lora.encode(payload, sf, cr, crc, implicit)
    entries = lora.frame_entries(syms, preamble, sync)
    with open(os.path.join(WORK, "stim.mem"), "w") as f:
        for a, d in stim_for(fs, bw, sf, chan, entries, invert_iq):
            f.write(f"{a:04x}{d & 0xFFFFFFFF:08x}\n")
    period = 1e9 / fs
    out = subprocess.run(["bash", "-c", f"source {VIVADO} && cd {WORK} && "
                          f"xelab -debug off tb_lora_tx -generic_top DAC_PERIOD={period} -s tb > xelab.log && "
                          f"xsim tb -R"], capture_output=True, text=True)
    log = out.stdout
    fails = [l for l in log.splitlines() if "FAIL" in l]
    iq = np.loadtxt(os.path.join(WORK, "iq.txt"))
    x = iq[:, 0] + 1j * iq[:, 1]
    frames = lora.decode_iq(x, fs, bw, sf, offset=chan, sync=sync, cr=cr if implicit else None,
                            crc=crc if implicit else None, implicit=len(payload) if implicit else None,
                            invert_iq=invert_iq)
    got = frames[0] if frames else None
    sym_ok = got is not None and got[3][:len(syms)] == syms
    ok = not fails and got is not None and got[0] == payload and got[2] in (None, True) and sym_ok
    # expected length: preamble + 4.25 + data symbols, plus the ramp-down symbol
    nsym = preamble + 4.25 + len(syms) + 1
    expect = nsym * (1 << sf) * fs / bw
    print(f"[{'PASS' if ok else 'FAIL'}] {name:44s} {len(syms)} data symbols "
          f"{'equal' if sym_ok else 'DIFFER'}, payload {'ok' if got and got[0] == payload else 'WRONG'}"
          f"{', crc ok' if got and got[2] else ''}, {len(x)} samples (expected ~{expect:.0f}), "
          f"peak {np.abs(x).max():.0f} {' '.join(fails)}", flush=True)
    return ok


def main():
    build()
    random.seed(1)
    p8 = bytes(random.randrange(256) for _ in range(8))
    p20 = bytes(random.randrange(256) for _ in range(20))
    cases = [
        ("SF7 BW 500k CR 4/8 explicit CRC, 4 MSPS", 4e6, 500e3, 7, 4, 0.0, p20, False, True),
        ("SF8 BW 125k CR 4/5 explicit, +300 kHz", 2e6, 125e3, 8, 1, 300e3, p8, False, False),
        ("ELRS-like: SF5 BW 812.5k 4/6 implicit 8 B, IQ inv, 61.44 MSPS, -7 MHz",
         61.44e6, 812.5e3, 5, 2, -7e6, p8, True, False, True, 12),
        ("ELRS-like: SF6 BW 812.5k 4/8 implicit 8 B, 61.44 MSPS, +10 MHz",
         61.44e6, 812.5e3, 6, 4, 10e6, p8, True, False, False, 14),
    ]
    results = [run(*c) for c in cases]
    print(f"\n{sum(results)}/{len(results)} passed")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
