#!/usr/bin/env python3
"""Check Multiprotocol's FrSky D8 / D16 (X) output end to end, without a receiver.

Runs the protocol on the board, captures single packets on RX2 (TX/RX -> cable
+ 30 dB attenuator -> RX2, as for rf_tone.py) and decodes them like a CC2500
would: carrier offset, bits, sync word, length, payload, CRC. Each packet's
frequency is mapped back to a CC2500 channel and checked against the
protocol's hop table.

  frsky_check.py [--proto d8|d16] [--count N] [--bind] [--rx-gain DB]

The sample rate is low (2 or 3 MSPS) so a 16K capture holds a whole packet;
the LO slices are then narrow and the LO retunes on most hops.
Needs the firmware running (make -C fw load) and numpy.
"""

import argparse
import re
import struct
import sys
import time

import numpy as np

import gfsk
from usbdbg import ADC_CAPTURE, Bridge, Console

N = 16384
F0 = gfsk.cc2500_freq(0x5C, 0x76, 0x27)                 # channel 0
SPACING = gfsk.cc2500_freq(0x5C, 0x76, 0x27, 1) - F0     # 299.93 kHz
FRSKYX_SHORT = [0x0000, 0x1189, 0x2312, 0x329B, 0x4624, 0x57AD, 0x6536, 0x74BF,
                0x8C48, 0x9DC1, 0xAF5A, 0xBED3, 0xCA6C, 0xDBE5, 0xE97E, 0xF8F7]


def cc2500_crc(data):
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x8005) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def frskyx_crc(data):
    """Multiprotocol's FrSkyX_crc() (FrSkyDVX_common.ino)."""
    crc = 0
    for b in data:
        val = ((crc >> 8) ^ b) & 0xFF
        word = FRSKYX_SHORT[val & 0x0F] ^ ((0x1081 * (val >> 4)) & 0xFFFF)
        crc = ((crc << 8) ^ word) & 0xFFFF
    return crc


def decode(x, fs, baud, proto):
    """-> dict with offset (Hz), bytes after the sync word, crc_ok."""
    amp = np.abs(x)
    on = np.nonzero(amp > 0.3 * amp.max())[0]
    if len(on) < 100:
        return {"error": "no signal"}
    x = x[on[0]:on[-1]]
    f = gfsk.discriminator(x, fs)
    sps = fs / baud
    k = max(1, int(round(sps / 2)))
    f = np.convolve(f, np.ones(k) / k, mode="same")
    pre = f[int(2 * sps):int(26 * sps)]                 # inside the 1010 preamble
    offset = float(np.mean(pre))
    f -= offset
    best = None
    for o in np.linspace(0, sps, 32, endpoint=False):
        idx = np.round(o + np.arange(int((len(f) - 1 - o) / sps) + 1) * sps).astype(int)
        idx = idx[idx < len(f)]
        score = np.mean(np.abs(f[idx]))
        if best is None or score > best[0]:
            best = (score, idx)
    bits = "".join("1" if v > 0 else "0" for v in f[best[1]])
    sync = "1101001110010001" * (2 if proto == "d16" else 1)
    pos = bits.find(sync)
    if pos < 0:
        return {"error": "no sync word", "offset": offset}
    body = bits[pos + len(sync):]
    data = [int(body[i:i + 8], 2) for i in range(0, len(body) - 7, 8)]
    if not data:
        return {"error": "nothing after sync", "offset": offset}
    n = data[0] + 1                                       # length byte + payload
    if proto == "d8":
        pkt = data[:n + 2]
        ok = len(pkt) == n + 2 and cc2500_crc(pkt[:n]) == (pkt[n] << 8 | pkt[n + 1])
    else:
        pkt = data[:n]
        ln = pkt[0]
        ok = len(pkt) == n and frskyx_crc(pkt[3:3 + ln - 4]) == (pkt[ln - 1] << 8 | pkt[ln])
    return {"offset": offset, "bytes": pkt, "crc_ok": ok}


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--proto", choices=["d8", "d16"], default="d16")
    ap.add_argument("--count", type=int, default=10)
    ap.add_argument("--bind", action="store_true", help="bind mode (channel 0, -30 dBm)")
    ap.add_argument("--rx-gain", type=float, default=None, help="default 10 (bind: 40)")
    args = ap.parse_args(argv[1:])

    fs = 2e6 if args.proto == "d8" else 3e6     # 16K samples hold one whole packet
    baud = gfsk.cc2500_baud(0xAA, 0x39) if args.proto == "d8" else gfsk.cc2500_baud(0x7B, 0x61)
    rx_gain = args.rx_gain if args.rx_gain is not None else (40 if args.bind else 10)
    name = "frskyd" if args.proto == "d8" else "frskyx"

    br = Bridge()
    con = Console(br)
    for c in ("mpm stop", f"radio rate {fs}", "radio rxfollow 1",
              f"mpm start {name} 0 0 0 {'bind' if args.bind else 'run'}", f"rf gain rx {rx_gain}"):
        print(f"  {c:32s} {con.run(c, timeout=20).strip()}")
    hops = [int(v) for v in con.run("mpm hops").split()][:47]
    br.write(ADC_CAPTURE + 0x08, [N])

    good = 0
    try:
        for i in range(args.count):
            con.run("radio capture")
            # "done" is still set from the previous capture: wait for this one to
            # start (next packet) and then to finish
            for want in (1, 2):
                for _ in range(200):
                    if br.read(ADC_CAPTURE + 0x04)[0] & want:
                        break
                    time.sleep(0.001)
            raw = br.read(ADC_CAPTURE + 0x10000, N)
            st = con.run("mpm status")
            lo = float(re.search(r"capture LO ([0-9.]+)", st).group(1))
            freq = float(re.search(r"capture freq ([0-9.]+)", st).group(1))
            iq = np.array([struct.unpack("<hh", struct.pack("<I", s)) for s in raw], dtype=float)
            r = decode(iq[:, 0] + 1j * iq[:, 1], fs, baud, args.proto)
            chan = round((freq - F0) / SPACING)
            in_table = args.bind or chan in hops
            if "error" in r:
                print(f"[FAIL] packet {i}: chan {chan:3d} ({freq / 1e6:.3f} MHz): {r['error']}")
                continue
            off_err = r["offset"] - (freq - lo)
            ok = r["crc_ok"] and abs(off_err) < 10e3 and in_table
            good += ok
            print(f"[{'PASS' if ok else 'FAIL'}] packet {i}: chan {chan:3d} ({freq / 1e6:.3f} MHz, "
                  f"{'in' if in_table else 'NOT in'} hop table), offset error {off_err / 1e3:+.1f} kHz, "
                  f"CRC {'ok' if r['crc_ok'] else 'BAD'}: {' '.join(f'{b:02x}' for b in r['bytes'][:12])} ...")
    finally:
        print("\n" + con.run("mpm status"))
        con.run("mpm stop")
        con.run("radio rxfollow 0")
    print(f"{good}/{args.count} packets decoded and checked")
    return 0 if good == args.count else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
