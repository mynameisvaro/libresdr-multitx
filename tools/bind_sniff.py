#!/usr/bin/env python3
"""Record an AFHDS2A bind between a real handset and receiver on one channel.

The firmware's `grx log` re-arms gfsk_rx a few microseconds after each packet,
so it keeps the handset's bind packet and the receiver's reply that follows it.
It keeps the last 96 packets and stops 16 packets after the first one that
isn't a plain handset bind packet (bytes 5..8 not ff ff ff ff: the receiver's
reply, or the handset's packets once the bind is done).
Each packet is decoded with the learned A7105 coding (FEC, then the CRC tried
over 38 and 37 bytes: Multiprotocol sends 38 and reads 37 back) and printed
with its time after the previous packet, carrier offset (gfsk_rx's -25.6 kHz
bias removed, tools/freq_cal.py) and level.

  bind_sniff.py [--chan 140] [--seconds 20] [--rx-gain DB] [--save F.json]
  bind_sniff.py --load F.json

Put the receiver in bind mode (power it with the bind button held), start the
tool, then start the handset's bind within the first seconds.
"""

import argparse
import json
import sys

import a7105

FEC = [int(v, 16) for v in "00 0b 16 1d 27 2c 31 3a 45 4e 53 58 62 69 74 7f".split()]
CODING = a7105.Coding(0x1021, 0x1D0F, False, 0, FEC, False)
FS = 61.44e6
NBYTES = 40                    # up to 38 payload bytes + CRC
GRX_BIAS = -25.6e3             # freq_cal.py, FlySky ID and settings


def decode(hexbits):
    bits = "".join(f"{b:08b}" for b in bytes.fromhex(hexbits))
    data, _ = CODING.decode(bits, NBYTES)
    for n in (38, 37):
        if CODING.crc(data[:n]) == (data[n] << 8 | data[n + 1]):
            _, fixed = CODING.decode(bits, n + 2)
            return data[:n], n, fixed
    return data[:38], None, None


def kind(p):
    if p[0] == 0xBB:
        return "handset bind 1"
    if p[0] == 0xBC and p[5:9] == b"\xff" * 4:
        return f"handset bind {p[9] + 1}"
    if p[0] == 0xBC:
        return f"bc, ID at 5..8 {p[5:9].hex()}, byte 9 = {p[9]:02x}"
    if p[0] == 0x58:
        return "sticks"
    return f"type {p[0]:02x}"


def show(entries):
    prev = None
    for e in entries:
        p, n, fixed = decode(e["bits"])
        dt = "" if prev is None else f"{((e['tstamp'] - prev) % 2**32) / FS * 1e6:9.0f} us"
        prev = e["tstamp"]
        crc = f"CRC ok over {n}" if n else "CRC bad"
        print(f"{dt:>12s}  {(e['offset'] - GRX_BIAS) / 1e3:+7.1f} kHz  level {e['level']:5d}  "
              f"{crc}{f', {fixed} bits fixed' if n and fixed else ''}  {kind(p)}")
        print(f"              {' '.join(f'{b:02x}' for b in p)}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--chan", type=int, default=140)
    ap.add_argument("--seconds", type=float, default=20)
    ap.add_argument("--rx-gain", type=float, default=30)
    ap.add_argument("--save")
    ap.add_argument("--load")
    a = ap.parse_args()

    if a.load:
        show(json.load(open(a.load)))
        return 0
    if a.seconds > 40:
        raise SystemExit("--seconds: at most 40 (the timer wraps every 43 s with interrupts off)")

    from usbdbg import Bridge, Console
    con = Console(Bridge())
    for c in ("mpm stop", "radio stop", "radio rate 61440000", "radio start", f"rf gain rx {a.rx_gain}"):
        con.run(c, timeout=30)
    freq = a7105.chan_freq(a.chan)
    print(f"logging channel {a.chan} ({freq / 1e6:.3f} MHz) for {a.seconds:.0f} s: start the bind now")
    # trigger: the code words of bytes 5..8 (bit 70 on) aren't FEC(ff ff ff ff), all ones
    out = con.run(f"grx log {freq:.0f} 500000 {a7105.FLYSKY_ID.hex()} {NBYTES * 14} {a.seconds} "
                  f"{5 * 14} {'f' * 14} 16", timeout=a.seconds + 30).strip()
    print(f"  {out}")
    n = int(out.split()[0])
    entries = []
    for i in range(n):
        ts, off, lvl, se, bits = con.run(f"grx dump {i}").split()
        entries.append({"tstamp": int(ts), "offset": int(off), "level": int(lvl), "syncerr": int(se), "bits": bits})
    con.run("radio stop")
    if a.save:
        json.dump(entries, open(a.save, "w"), indent=1)
        print(f"  saved to {a.save}")
    show(entries)
    return 0


if __name__ == "__main__":
    sys.exit(main())
