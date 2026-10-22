#!/usr/bin/env python3
"""End-to-end check of the gfsk_tx modulator: send a FrSky-style packet and
demodulate what comes back.

  gfsk_test.py [--mode loopback|rf] [--proto d8|d16] [--count N] [--chan HZ]

loopback: AD9361 digital loopback (no RF). rf: out of TX/RX, back in on RX2
(cable + 30 dB attenuator, as for rf_tone.py). The sample rate is 2 MSPS so one
16K-sample capture holds a whole packet.

Needs the firmware running (make -C fw load), a bitstream with gfsk_tx, numpy.
"""

import argparse
import random
import struct
import sys
import time

import numpy as np

import gfsk
from usbdbg import ADC_CAPTURE, Bridge, Console

FS = 2e6
N = 16384
PA_SETTLE_S = 0.6


def packet(proto, rng):
    """Preamble, sync word(s), length and random payload, as the CC2500 sends them."""
    pre = [0xAA] * 4
    if proto == "d8":
        return pre + [0xD3, 0x91, 0x11] + [rng.randrange(256) for _ in range(19)]
    return pre + [0xD3, 0x91, 0xD3, 0x91, 0x1D] + [rng.randrange(256) for _ in range(29)]


def capture_packet(br, con, data):
    br.write(ADC_CAPTURE + 0x08, [N])
    reply = con.run("gfsk send -c " + "".join(f"{b:02x}" for b in data))
    if "sending" not in reply:
        raise RuntimeError(reply.strip())
    for _ in range(200):
        if br.read(ADC_CAPTURE + 0x04)[0] & 2:
            break
        time.sleep(0.005)
    raw = br.read(ADC_CAPTURE + 0x10000, N)
    iq = np.array([struct.unpack("<hh", struct.pack("<I", s)) for s in raw], dtype=float)
    return iq[:, 0] + 1j * iq[:, 1]


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--mode", choices=["loopback", "rf"], default="loopback")
    ap.add_argument("--proto", choices=["d8", "d16"], default="d16")
    ap.add_argument("--count", type=int, default=5, help="packets (default 5)")
    ap.add_argument("--chan", type=float, default=300e3, help="NCO offset (default 300 kHz)")
    ap.add_argument("--freq", type=float, default=2440e6, help="LO for --mode rf")
    ap.add_argument("--tx-gain", type=float, default=60)
    ap.add_argument("--rx-gain", type=float, default=40)
    args = ap.parse_args(argv[1:])

    br = Bridge()
    con = Console(br)
    if "not initialised" in con.run("rf rate"):
        print(con.run("rf init"), end="")
    setup = [f"rf rate {FS}", f"rf loopback {1 if args.mode == 'loopback' else 0}",
             f"gfsk cc2500 {args.proto}", f"gfsk chan {args.chan}", "gfsk level -6 20"]
    if args.mode == "rf":
        setup += [f"rf freq tx {args.freq}", f"rf freq rx {args.freq}",
                  f"rf gain tx {args.tx_gain}", f"rf gain rx {args.rx_gain}", "fe 1 fdd"]
    for c in setup:
        print(f"  {c:28s} {con.run(c).strip()}")
    if args.mode == "rf":
        time.sleep(PA_SETTLE_S)

    baud = gfsk.cc2500_baud(0x7B, 0x61) if args.proto == "d16" else gfsk.cc2500_baud(0xAA, 0x39)
    dev = gfsk.cc2500_dev(0x51) if args.proto == "d16" else gfsk.cc2500_dev(0x42)
    rng = random.Random(1)
    ok = 0
    try:
        for i in range(args.count):
            data = packet(args.proto, rng)
            x = capture_packet(br, con, data)
            r = gfsk.demod(x, FS, baud, args.chan, gfsk.bytes_to_bits(data))
            good = r["errors"] == 0
            ok += good
            level = 20 * np.log10(np.abs(x).max() / 2048 + 1e-9)
            print(f"[{'PASS' if good else 'FAIL'}] packet {i}: {r['nbits']} bits, {r['errors']} errors, "
                  f"deviation {r.get('dev_meas', 0) / 1e3:.1f} kHz (set {dev / 1e3:.1f}), "
                  f"peak {level:.1f} dBFS")
    finally:
        con.run("rf loopback 0")
        if args.mode == "rf":
            con.run("fe 1 off")
    print(f"\n{ok}/{args.count} packets without errors")
    return 0 if ok == args.count else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
