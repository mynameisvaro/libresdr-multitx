#!/usr/bin/env python3
"""Check Multiprotocol's FlySky (AFHDS) and AFHDS2A output end to end, without a receiver.

Runs the protocol on the board, captures single packets on RX2 (TX/RX -> cable
+ 30 dB attenuator -> RX2, as for rf_tone.py) and decodes them like an A7105
would: carrier offset, bits, preamble and ID code, FEC, payload, CRC. Each
packet's frequency is mapped back to an A7105 channel and checked against the
protocol's hop table, and the payload against what the protocol should send
(packet type, TX ID, channel values).

  flysky_check.py [--proto afhds|afhds2a] [--count N] [--bind] [--rx-gain DB]

The CRC and FEC coding are read from the firmware (a7105 coding), so this
checks the packets against the coding the virtual A7105 uses; whether that is
the real chip's coding is a7105_sniff.py's job. Needs the firmware running
(make -C fw load) and numpy.
"""

import argparse
import re
import sys
import time

import a7105
from usbdbg import ADC_CAPTURE, Bridge, Console

N = 16384
FS = 4e6                                 # 8 samples per bit, a 16K capture = 4.1 ms


def check_payload(proto, bind, p, txid, chan_vals):
    """-> error string or None"""
    if proto == "afhds":
        if p[0] != (0xAA if bind else 0x55):
            return f"type {p[0]:02x}"
        if bytes(p[1:5]) != bytes(reversed(txid)):
            return "TX ID"
        vals = [p[5 + 2 * i] | p[6 + 2 * i] << 8 for i in range(8)]
        chan_vals.append(vals)
        if not all(900 <= v <= 2100 for v in vals):
            return f"channel values {vals}"
        return None
    if bytes(p[1:5]) != txid:
        return "TX ID"
    if bind:
        return None if p[0] in (0xBB, 0xBC) else f"type {p[0]:02x}"
    if p[0] == 0x58:
        vals = [p[9 + 2 * i] | (p[10 + 2 * i] & 0x0F) << 8 for i in range(14)]
        chan_vals.append(vals)
        if not all(900 <= v <= 2100 for v in vals):
            return f"channel values {vals}"
        return None
    return None if p[0] in (0xAA, 0x56) else f"type {p[0]:02x}"


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--proto", choices=["afhds", "afhds2a"], default="afhds2a")
    ap.add_argument("--count", type=int, default=10)
    ap.add_argument("--bind", action="store_true", help="bind mode (bind channels, -23 dBm)")
    ap.add_argument("--rx-gain", type=float, default=None, help="default 10 (bind: 40)")
    args = ap.parse_args(argv[1:])

    rx_gain = args.rx_gain if args.rx_gain is not None else (40 if args.bind else 10)
    name, nbytes, fec = ("flysky", 21, False) if args.proto == "afhds" else ("flsky2a", 38, True)

    br = Bridge()
    con = Console(br)
    for c in ("mpm stop", f"radio rate {FS}", "radio rxfollow 1",
              f"mpm start {name} 0 0 0 {'bind' if args.bind else 'run'}", f"rf gain rx {rx_gain}"):
        print(f"  {c:32s} {con.run(c, timeout=20).strip()}")
    coding = a7105.Coding.from_console(con.run("a7105 coding"))
    print(f"  coding: {coding}")
    st = con.run("mpm status")
    txid = bytes(int(v, 16) for v in re.search(r"rx_tx_addr (\w+) (\w+) (\w+) (\w+)", st).groups())
    hops = [int(v) for v in con.run("mpm hops").split()][:16]
    if args.bind:
        hops = [1] if args.proto == "afhds" else [0x0D, 0x8C]
    print(f"  TX ID {txid.hex()}, channels {hops}")
    br.write(ADC_CAPTURE + 0x08, [N])

    good = 0
    chan_vals = []
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
            x = a7105.iq_from_raw(br.read(ADC_CAPTURE + 0x10000, N))
            st = con.run("mpm status")
            lo = float(re.search(r"capture LO ([0-9.]+)", st).group(1))
            freq = float(re.search(r"capture freq ([0-9.]+)", st).group(1))
            chan = a7105.freq_chan(freq)
            b = a7105.bursts(x, 100)
            if not b:
                print(f"[FAIL] packet {i}: chan {chan:3d}: no signal")
                continue
            s, e = b[0]
            bits, offset = a7105.demod(x[s:e], FS)
            pos = a7105.find_id(bits)
            if pos < 0:
                print(f"[FAIL] packet {i}: chan {chan:3d}: no preamble + ID")
                continue
            data, fixed = coding.decode(bits[pos:], nbytes + 2, fec)
            p, crc = data[:nbytes], data[nbytes] << 8 | data[nbytes + 1]
            crc_ok = coding.crc(p) == crc
            off_err = offset - (freq - lo)
            err = check_payload(args.proto, args.bind, p, txid, chan_vals)
            ok = crc_ok and abs(off_err) < 10e3 and chan in hops and err is None and fixed == 0
            good += ok
            print(f"[{'PASS' if ok else 'FAIL'}] packet {i}: chan {chan:3d} ({freq / 1e6:.3f} MHz, "
                  f"{'in' if chan in hops else 'NOT in'} hop table), offset error "
                  f"{off_err / 1e3:+.1f} kHz, CRC {'ok' if crc_ok else 'BAD'}"
                  f"{f', {fixed} bits corrected' if fixed else ''}{f', {err}' if err else ''}: "
                  f"{' '.join(f'{v:02x}' for v in p[:12])} ...")
    finally:
        print("\n" + con.run("mpm status"))
        con.run("mpm stop")
        con.run("radio rxfollow 0")
    if chan_vals:
        print(f"channel values (us) in the last packet: {chan_vals[-1]}")
    print(f"{good}/{args.count} packets decoded and checked")
    return 0 if good == args.count else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
