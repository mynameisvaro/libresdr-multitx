#!/usr/bin/env python3
"""Bind an AFHDS2A receiver: catch its reply on RX2 and hand it to the virtual A7105.

Binding AFHDS2A takes two ways: the transmitter sends bind packets, the
receiver answers with its own ID (0xbc ... 0x01), and the transmitter then
puts that ID in every packet. The virtual A7105 has no receiver yet, so this
does that part: it runs the protocol in bind mode, captures the air after
each bind packet, decodes the receiver's reply like an A7105 (FEC, CRC) and
queues it in the virtual chip's RX FIFO (a7105 rx). Multiprotocol's
AFHDS2A code then finishes the bind by itself, unmodified.

  afhds2a_bind.py [--sub N] [--rx N] [--timeout S] [--rx-gain DB]

Needs an antenna on RX2 as well as on TX/RX, the receiver close by and put in
bind mode, and the coding (a7105 crc / a7105 fec) set to the real chip's
(a7105_sniff.py). The bind runs at 4 MSPS so that one capture spans a whole
packet period; afterwards the protocol is restarted normally at 61.44 MSPS.

The receiver ID lives in RAM: after reloading the firmware, set it again with
"mpm rxid ID RX" (printed at the end) instead of binding again.
"""

import argparse
import re
import sys
import time

import numpy as np

import a7105
from usbdbg import ADC_CAPTURE, Bridge, Console

N = 16384
FS = 4e6
RXPACKET = 38                     # FIFO length (FEP 0x25); the code reads 37
# Multiprotocol listens from ~1.7-2.4 ms after the start of its bind packet
# until the next one at 3.85 ms, so a reply it can receive starts before
# ~2.6 ms. Later bursts are our own next packet (also 0xbc with our TX ID).
REPLY_BEFORE = 3.2e-3


def find_reply(x, coding):
    """Capture that starts with our bind packet -> (reply bytes, note) or (None, note)"""
    b = a7105.bursts(x, int(200e-6 * FS))
    if not b:
        return None, "no signal (our own packet missing)"
    end = b[0][1] + int(50e-6 * FS)
    rest = x[end:]
    if len(rest) < int(1e-3 * FS):
        return None, "capture too short"
    noise = np.median(np.abs(rest))
    if np.max(np.abs(rest)) < 6 * max(noise, 1):
        return None, "no reply"
    for s, e in a7105.bursts(rest, int(200e-6 * FS)):
        if end + s > REPLY_BEFORE * FS or e >= len(rest):
            continue
        bits, off = a7105.demod(rest[s:e], FS)
        pos = a7105.find_id(bits, max_err=2)
        if pos < 0 or len(bits) - pos < 14 * (RXPACKET + 2):
            continue
        data, fixed = coding.decode(bits[pos:], RXPACKET + 2)
        crc_ok = coding.crc(data[:RXPACKET]) == (data[RXPACKET] << 8 | data[RXPACKET + 1])
        if not crc_ok:
            return None, f"reply with a bad CRC ({fixed} bits corrected): {data[:12].hex()}"
        return data[:RXPACKET], f"reply, offset {off / 1e3:+.1f} kHz, {fixed} bits corrected"
    return None, "a burst without the FlySky ID"


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--sub", type=int, default=0, help="sub-protocol (0 PWM,IBUS .. 3 PPM,SBUS)")
    ap.add_argument("--rx", type=int, default=0, help="RX number (model match)")
    ap.add_argument("--timeout", type=float, default=60)
    ap.add_argument("--rx-gain", type=float, default=30)
    args = ap.parse_args(argv[1:])

    br = Bridge()
    con = Console(br)
    for c in ("mpm stop", f"radio rate {FS}", "radio rxfollow 1",
              f"mpm start flsky2a {args.sub} {args.rx} 0 bind", f"rf gain rx {args.rx_gain}"):
        print(f"  {c:36s} {con.run(c, timeout=20).strip()}")
    coding = a7105.Coding.from_console(con.run("a7105 coding"))
    st = con.run("mpm status")
    txid = bytes(int(v, 16) for v in re.search(r"rx_tx_addr (\w+) (\w+) (\w+) (\w+)", st).groups())
    print(f"  coding: {coding}\n  TX ID {txid.hex()}; put the receiver in bind mode now")
    br.write(ADC_CAPTURE + 0x08, [N])

    rxid = None
    t0 = time.time()
    tries = 0
    try:
        while time.time() - t0 < args.timeout:
            tries += 1
            con.run("radio capture")
            for want in (1, 2):
                for _ in range(200):
                    if br.read(ADC_CAPTURE + 0x04)[0] & want:
                        break
                    time.sleep(0.001)
            x = a7105.iq_from_raw(br.read(ADC_CAPTURE + 0x10000, N))
            reply, note = find_reply(x, coding)
            if reply is None:
                print(f"\r  capture {tries}: {note:70s}", end="", flush=True)
                continue
            print(f"\n  capture {tries}: {note}: {reply.hex()}")
            if (reply[0] != 0xBC or reply[9] != 0x01 or reply[1:5] != txid
                    or reply[5:9] == b"\xff" * 4):
                print("  (not a bind reply for us)")
                continue
            rxid = reply[5:9]
            con.run("a7105 rx " + reply.hex())
            break
        if rxid is None:
            print(f"\nno bind reply in {args.timeout:.0f} s")
            return 1
        # the protocol sends its last bind packets (with the RX ID) and switches
        # to normal packets by itself
        for _ in range(50):
            if "bound" in con.run("mpm status"):
                break
            time.sleep(0.1)
        print(con.run("mpm status"))
    finally:
        con.run("mpm stop")
        con.run("radio rxfollow 0")
        con.run("radio rate 61.44e6")
    if rxid is None:
        return 1
    print(con.run(f"mpm start flsky2a {args.sub} {args.rx} 0", timeout=20))
    print(f"receiver ID {rxid.hex()}: bound and running (AFHDS2A, RX {args.rx}).\n"
          f"After reloading the firmware: mpm rxid {rxid.hex()} {args.rx}, then mpm start flsky2a "
          f"{args.sub} {args.rx}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
