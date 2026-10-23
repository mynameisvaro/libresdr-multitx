#!/usr/bin/env python3
"""Check LoRa frames from the board end to end, without a receiver.

The firmware sends LoRa frames (lora_tx, `lora send`) out of TX/RX; RX2 captures
each one (TX/RX -> cable + 30 dB attenuator -> RX2, as for rf_tone.py) and
tools/lora.py decodes it, the decoder that reproduces real SX127x recordings
symbol for symbol. Each case checks the payload and every data symbol.

  lora_check.py [--count N] [--rx-gain DB] [--case NAME]

Cases are ExpressLRS-like 2.4 GHz (SF5/SF6, BW 812.5 kHz, implicit 8-byte
frames; standard coding, since the SX1280's long-interleaving rates aren't
known) and 900 MHz (SF6, BW 500 kHz), plus an explicit-header frame with CRC.
Needs the lora_tx bitstream, the firmware and numpy/scipy.
"""

import argparse
import random
import re
import sys
import time

import lora
from a7105 import iq_from_raw
from usbdbg import ADC_CAPTURE, Bridge, Console

N = 16384

# name: (sample rate, lora cfg arguments, payload length, frequencies)
CASES = {
    "elrs2g4-sf5": (4e6, "5 812.5e3 2 nocrc implicit pre 12 sync 12 iqinv", 8,
                    [2405e6, 2440e6, 2478e6]),
    "elrs2g4-sf6": (4e6, "6 812.5e3 4 nocrc implicit pre 14 sync 12 iqnorm", 8,
                    [2410e6, 2465e6]),
    "elrs900-sf6": (2e6, "6 500e3 3 nocrc implicit pre 8 sync 12 iqnorm", 8,
                    [868.4e6, 915e6]),
    "explicit-crc": (4e6, "5 812.5e3 4 crc explicit pre 8 sync 34 iqnorm", 12,
                     [2440e6]),
}


def parse_cfg(text):
    t = text.split()
    sf, bw, cr = int(t[0]), float(t[1]), int(t[2])
    return dict(sf=sf, bw=bw, cr=cr, crc="crc" in t, implicit="implicit" in t,
                invert_iq="iqinv" in t, sync=int(t[t.index("sync") + 1], 16))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--count", type=int, default=2, help="frames per frequency")
    ap.add_argument("--rx-gain", type=float, default=30)
    ap.add_argument("--case", choices=sorted(CASES), action="append", help="(default: all)")
    a = ap.parse_args()

    br = Bridge()
    con = Console(br)
    random.seed(1)
    good = total = 0
    try:
        for name in a.case or CASES:
            fs, cfg, plen, freqs = CASES[name]
            p = parse_cfg(cfg)
            for c in ("mpm stop", "radio stop", f"radio rate {fs}", "radio rxfollow 1",
                      "radio start", f"rf gain rx {a.rx_gain}", f"lora cfg {cfg}"):
                out = con.run(c, timeout=30).strip()
                if out:
                    print(f"  {c:40s} {out}")
                if "usage" in out or "unknown" in out or "too many" in out:
                    raise SystemExit(f"'{c}' was not accepted")
            br.write(ADC_CAPTURE + 0x08, [N])
            for freq in freqs:
                for i in range(a.count):
                    payload = bytes(random.randrange(256) for _ in range(plen))
                    con.run("radio capture")
                    out = con.run(f"lora send {freq:.0f} {payload.hex()}").strip()
                    if out:
                        print(f"  lora send: {out}")
                    # the capture starts with the frame, before `lora send` returns
                    # (which clears its done bit): wait for done
                    t = time.time()
                    while not br.read(ADC_CAPTURE + 0x04)[0] & 2 and time.time() - t < 5:
                        time.sleep(0.002)
                    iq = iq_from_raw(br.read(ADC_CAPTURE + 0x10000, N))
                    st = con.run("mpm status")
                    lo = float(re.search(r"capture LO ([0-9.]+)", st).group(1))
                    frames = lora.decode_iq(iq, fs, p["bw"], p["sf"], offset=freq - lo, sync=p["sync"],
                                            cr=p["cr"] if p["implicit"] else None,
                                            crc=p["crc"] if p["implicit"] else None,
                                            implicit=plen if p["implicit"] else None,
                                            invert_iq=p["invert_iq"])
                    ref = lora.encode(payload, p["sf"], p["cr"], p["crc"], p["implicit"])
                    got = frames[0] if frames else None
                    sym_ok = got is not None and got[3][:len(ref)] == ref
                    ok = got is not None and got[0] == payload and got[2] in (None, True) and sym_ok
                    good += ok
                    total += 1
                    print(f"[{'PASS' if ok else 'FAIL'}] {name} {freq / 1e6:8.3f} MHz #{i}: "
                          + ("no frame found" if got is None else
                             f"payload {'ok' if got[0] == payload else got[0].hex()}"
                             f"{', crc ok' if got[2] else ''}, {len(ref)} symbols "
                             f"{'equal' if sym_ok else 'DIFFER'}"), flush=True)
    finally:
        con.run("radio stop")
        con.run("radio rxfollow 0")
        con.run("radio rate 61440000")
    print(f"\n{good}/{total} frames decoded and checked")
    return 0 if good == total else 1


if __name__ == "__main__":
    sys.exit(main())
