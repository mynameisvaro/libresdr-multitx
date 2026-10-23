#!/usr/bin/env python3
"""Check the GFSK receiver (gfsk_rx) on the board through the loopback cable.

The board's own GFSK modulator (gfsk_tx) sends packets out of TX/RX while
gfsk_rx listens on RX2 (TX/RX -> cable + 30 dB attenuator -> RX2, as for
rf_tone.py): preamble, sync word, random payload. Each case checks that the
receiver found the sync word and kept every payload bit.

  gfsk_rx_check.py [--count N] [--rx-gain DB]

Cases: A7105 / FlySky settings (500 kbaud, 186 kHz, ID 5475C52A) and CC2500 /
FrSky D16 settings (70 kbaud, sync D391D391), at several channels of the 2.4 GHz
slice. Needs the gfsk_rx bitstream and the firmware.
"""

import argparse
import random
import re
import sys
import time

from usbdbg import Bridge, Console

LO_BAND = 2440e6
# name: (baud, deviation, BT, preamble byte, sync, payload bits, channels as offsets from the LO)
CASES = {
    "a7105": (500e3, 186e3, 0.7, 0x55, 0x5475C52A, 39 * 14, [-12e6, -3e6, 5e6, 15e6]),
    "cc2500": (None, None, 1.0, 0xAA, 0xD391D391, 30 * 8, [-10e6, 2e6, 9e6]),
}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--count", type=int, default=3, help="packets per channel")
    ap.add_argument("--rx-gain", type=float, default=30)
    a = ap.parse_args()

    con = Console(Bridge())
    random.seed(4)

    def run(c, timeout=30):
        out = con.run(c, timeout=timeout).strip()
        if "usage" in out or "unknown" in out or "failed" in out or "no gfsk_rx" in out:
            raise SystemExit(f"'{c}': {out}")
        return out

    results = []
    try:
        for c in ("mpm stop", "radio stop", "radio rate 61440000", "radio start", f"rf gain rx {a.rx_gain}"):
            run(c)
        lo = float(re.search(r"LO ([0-9.]+) MHz", run("mpm status")).group(1)) * 1e6
        for name, (baud, dev, bt, pre, sync, nbits, offsets) in CASES.items():
            if name == "cc2500":
                out = run("gfsk cc2500 d16")
                baud = float(re.search(r"([0-9.]+) baud", out).group(1))
            else:
                run(f"gfsk cfg {baud:.0f} {dev:.0f} {bt}")
            run("gfsk level -10")
            for off in offsets:
                freq = lo + off
                for i in range(a.count):
                    payload = [random.randrange(2) for _ in range(nbits)]
                    pbytes = int("".join(map(str, payload)) + "0" * (-nbits % 8), 2).to_bytes((nbits + 7) // 8, "big")
                    pkt = bytes([pre] * 4) + sync.to_bytes(4, "big") + pbytes + b"\x55"
                    run(f"grx arm {freq:.0f} {baud:.0f} {sync:08x} {nbits}")
                    run(f"gfsk chan {off:.0f}")
                    run("gfsk send " + pkt.hex())
                    time.sleep(0.05)
                    out = run("grx")
                    lines = out.splitlines()
                    ok = lines[0].startswith("done")
                    nerr = nbits
                    if ok:
                        got = bytes.fromhex(lines[1].strip())
                        gbits = [(b >> (7 - k)) & 1 for b in got for k in range(8)][:nbits]
                        nerr = sum(x != y for x, y in zip(gbits, payload))
                        ok = nerr == 0
                    results.append(ok)
                    info = lines[0] if lines else ""
                    print(f"[{'PASS' if ok else 'FAIL'}] {name:6s} {freq / 1e6:9.3f} MHz #{i}: {nbits} bits, "
                          f"{nerr} wrong; {info}", flush=True)
    finally:
        con.run("grx stop")
        con.run("radio stop")
    print(f"\n{sum(results)}/{len(results)} packets received exactly")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
