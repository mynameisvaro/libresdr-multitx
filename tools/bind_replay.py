#!/usr/bin/env python3
"""Send a recorded AFHDS2A bind packet from the board and log what answers.

The packet goes out through gfsk_tx exactly as given (lead-in, preamble, ID,
FEC, CRC), one every --period seconds on one channel, while `grx log` listens
on that channel; it stops after the first packet that isn't the board's own
(bytes 5..8 not ff ff ff ff, as bind_sniff.py). With a receiver in bind mode
this tells whether it answers a given packet at all, without the protocol and
its timing.

  bind_replay.py [--packet HEX] [--chan 140] [--seconds 30] [--period 0.1]
                 [--tx-gain DB] [--rx-gain DB]

The default packet is the FS-i6X's bind packet the X6B answered
(logs/bind_real_ch140.log).
"""

import argparse
import re
import sys
import time

import a7105
import bind_sniff
from usbdbg import PROMPT, Bridge, Console

GFSK_TX = 0x44040000            # gfsk_tx: 0x00 CTRL, 0x08 NBITS, 0x2000 + 4 * i packet bytes
FSI6X_BC01 = ("bccc05b6c4ffffffff0100151d6266326a8b44542d255d957f7b76"
              "0180ffffffffffffffff00")


def air_bits(payload, lead_in=16):
    """lead-in (carrier), 4-byte preamble, ID, FEC-coded payload + CRC -> bytes"""
    crc = bind_sniff.CODING.crc(payload)
    body = bind_sniff.CODING.encode(payload + bytes([crc >> 8, crc & 0xFF]))
    bits = "1" * lead_in + "01" * 16 + "".join(f"{b:08b}" for b in a7105.FLYSKY_ID) + body
    bits += "0" * (-len(bits) % 8)
    return bytes(int(bits[i:i + 8], 2) for i in range(0, len(bits), 8))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--packet", default=FSI6X_BC01, help="38-byte payload, hex")
    ap.add_argument("--chan", type=int, default=140)
    ap.add_argument("--seconds", type=float, default=30)
    ap.add_argument("--period", type=float, default=0.1, help="seconds between packets (at least the bridge round trip)")
    ap.add_argument("--tx-gain", type=float, default=64, help="AD9361 TX gain (the bind power maps to about 64)")
    ap.add_argument("--rx-gain", type=float, default=45)
    ap.add_argument("--save")
    a = ap.parse_args()
    payload = bytes.fromhex(a.packet)
    if len(payload) != 38:
        raise SystemExit("--packet: 38 bytes")
    if a.seconds > 40:
        raise SystemExit("--seconds: at most 40")

    br = Bridge()
    con = Console(br)
    for c in ("mpm stop", "radio stop", "radio rate 61440000", "radio start", f"rf gain rx {a.rx_gain}",
              f"rf gain tx {a.tx_gain}", "gfsk cfg 500000 186000 0.7", "gfsk level -3"):
        con.run(c, timeout=30)
    lo = float(re.search(r"LO ([0-9.]+) MHz", con.run("mpm status")).group(1)) * 1e6
    freq = a7105.chan_freq(a.chan)
    con.run(f"gfsk chan {freq - lo:.0f}")
    pkt = air_bits(payload)
    br.write(GFSK_TX + 0x2000, list(pkt))
    br.write(GFSK_TX + 0x08, [8 * len(pkt)])

    print(f"sending {payload.hex()} on channel {a.chan} every {a.period} s for {a.seconds:.0f} s", flush=True)
    con.poll()
    con.send(f"grx log {freq:.0f} 500000 {a7105.FLYSKY_ID.hex()} {bind_sniff.NBYTES * 14} {a.seconds} "
             f"{5 * 14} {'f' * 14} 4\r")
    time.sleep(0.3)
    out, sent, t_end = b"", 0, time.time() + a.seconds
    # the bridge works while the CPU is busy in grx log
    while time.time() < t_end and not out.endswith(PROMPT):
        br.write(GFSK_TX + 0x00, [1])
        sent += 1
        time.sleep(a.period)
        out += con.poll()
    while not out.endswith(PROMPT):
        out += con.poll()
        time.sleep(0.01)
    line = out.decode(errors="replace").splitlines()[-2] if b"\n" in out else out.decode()
    print(f"  {sent} sent; {line.strip()}")
    n = int(re.search(r"(\d+) packets kept", line).group(1))
    entries = []
    for i in range(n):
        ts, off, lvl, se, bits = con.run(f"grx dump {i}").split()
        entries.append({"tstamp": int(ts), "offset": int(off), "level": int(lvl), "syncerr": int(se), "bits": bits})
    con.run("radio stop")
    if a.save:
        import json
        json.dump(entries, open(a.save, "w"), indent=1)
    # the board's own packets are many: show the last few before the trigger and what follows
    trig = [i for i, e in enumerate(entries) if bind_sniff.decode(e["bits"])[0][5:9] != b"\xff" * 4]
    if trig:
        bind_sniff.show(entries[max(0, trig[0] - 2):])
        return 0
    print(f"  no answer ({n} packets kept, all the board's own)")
    return 1


if __name__ == "__main__":
    sys.exit(main())
