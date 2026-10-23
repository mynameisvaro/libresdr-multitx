#!/usr/bin/env python3
"""ExpressLRS from the board, received by a real SX1278/SX1276 (tools/sx127x_listen
on an ESP32): the first check of MultiTX's LoRa frames and ExpressLRS packets by
Semtech silicon instead of our own decoder.

Starts ExpressLRS on the board (unless --no-board), sets the SX127x to one channel
of the domain with ExpressLRS's LoRa settings and checks every packet it hears:

  - the chip's own LoRa demodulation worked (SF6, implicit header, sync 0x12)
  - the ExpressLRS CRC matches, for some nonce (tools/elrs.py)
  - RC packets carry the channels set on the board; sync packets the UID, rate,
    and an FHSS index that points at this channel
  - timing: between two packets, the nonce difference times the packet period
    equals the arrival time difference (the board's slots keep time and the
    nonce counts them), and the spread of those residuals
  - the frequency error the chip measures (FEI): the board's carrier offset as
    the SX127x sees it

  sx127x_listen.py [--port /dev/ttyUSB0] [--domain EU433] [--channel N] [--rate 0]
                   [--seconds 20] [--no-board] [--raw]

Needs pyserial; the board side needs the firmware (usbdbg.py).
"""

import argparse
import glob
import re
import statistics
import sys
import time

import serial

import elrs

PHRASE = "multitx sx127x check"
CHANNELS = [204, 700, 1024, 1844, 1700, 300, 600, 900, 1200, 1500, 1800, 1024]
LINE = re.compile(r"pkt t=(\d+) len=(\d+) rssi=(-?\d+) snr=(-?[\d.]+) fei=(-?\d+) crcerr=(\d) ([0-9a-f]*)")


def find_port():
    ports = sorted(glob.glob("/dev/serial/by-id/usb-Silicon_Labs_CP210*_UART*")) + \
        sorted(glob.glob("/dev/serial/by-id/usb-Silicon_Labs_CP2102*"))
    ports = [p for p in ports if "CP2108" not in p]     # the coworker's board
    if not ports:
        raise SystemExit("no ESP32 (CP2102) found; pass --port")
    return ports[0]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port")
    ap.add_argument("--domain", default="EU433", choices=sorted(elrs.DOMAINS))
    ap.add_argument("--channel", type=int, default=None, help="channel to listen on (default: the sync channel)")
    ap.add_argument("--rate", type=int, default=0, choices=range(len(elrs.RATES)))
    ap.add_argument("--seconds", type=float, default=20)
    ap.add_argument("--power", type=float, default=-10, help="board output, dBm")
    ap.add_argument("--no-board", action="store_true", help="don't start the board (it's already running)")
    ap.add_argument("--raw", action="store_true", help="print every packet")
    a = ap.parse_args()

    uid = elrs.uid_from_phrase(PHRASE)
    dom = elrs.Domain(a.domain)
    seq = elrs.fhss_sequence(elrs.uid_seed(uid), dom)
    r = elrs.RATES[a.rate]
    chan = dom.sync_channel if a.channel is None else a.channel
    freq = dom.channel_hz(chan)
    ch_crsf = [elrs.multi_to_crsf(v) for v in CHANNELS] + [elrs.CRSF_MID] * 4

    con = None
    if not a.no_board:
        from usbdbg import Bridge, Console
        con = Console(Bridge())
        cmds = ["mpm stop", "radio stop", "radio rate 61440000", "radio rxfollow 0", "input console",
                f"elrs phrase {PHRASE}", f"elrs domain {a.domain}", "elrs switch 0", "elrs tlm std",
                "elrs modelmatch off", f"elrs power {a.power}"]
        cmds += [f"mpm ch {i + 1} {v}" for i, v in enumerate(CHANNELS)]
        cmds += [f"mpm start elrs {a.rate}"]
        for c in cmds:
            out = con.run(c, timeout=30).strip()
            if "usage" in out or "unknown" in out or "failed" in out:
                raise SystemExit(f"'{c}': {out}")

    port = a.port or find_port()
    s = serial.Serial(port, 115200, timeout=0.2)
    time.sleep(0.1)
    s.reset_input_buffer()
    cr = 4 + r.cr
    s.write(f"lora {r.sf} {int(r.bw)} {cr} {r.size} sync 12 pre {r.preamble}\nf {round(freq)}\n".encode())
    print(f"listening on {a.domain} channel {chan} ({freq / 1e6:.4f} MHz), {r.name}: SF{r.sf} 4/{cr} "
          f"{r.size} bytes, {a.seconds:.0f} s, port {port}")

    pkts = []
    garbled = 0
    t_end = time.time() + a.seconds
    while time.time() < t_end:
        line = s.readline().decode(errors="replace").strip()
        if not line:
            continue
        if line.startswith("cfg ") or line.startswith("sx127x"):
            print("  " + line)
        m = LINE.match(line)
        if not m:
            continue
        t, n, rssi, snr, fei, crcerr, hexs = m.groups()
        if len(hexs) != 2 * int(n):     # (a line cut short or run into the next)
            garbled += 1
            continue
        pkts.append(dict(t=int(t), rssi=int(rssi), snr=float(snr), fei=int(fei), crcerr=int(crcerr),
                         pkt=bytes.fromhex(hexs)))
        if a.raw:
            print("  " + line)
    if con:
        print("  board: " + con.run("elrs").strip().splitlines()[-1])
        con.run("mpm stop")
        con.run("radio stop")
    s.close()

    if not pkts:
        print("[FAIL] no packets received (wiring? antenna? the board's power and distance?)")
        return 1

    # bad: a valid CRC but wrong contents (a protocol error); crc_bad: no CRC
    # matches (a radio error: ExpressLRS turns the LoRa CRC off, so the chip
    # passes corrupted packets on)
    good, rc, sync, bad, crc_bad = [], 0, 0, 0, 0
    for p in pkts:
        d = elrs.parse(p["pkt"], uid)
        if d is None:
            crc_bad += 1
            continue
        p["d"] = d
        if d["type"] == "sync":
            sync += 1
            ok = seq[d["fhss_index"]] == chan and d["uid4"] == uid[4] and d["uid5"] == uid[5] and d["rate_enum"] == r.enum
            p["nonce"] = d["nonce"]
            if not ok:
                print(f"[FAIL] sync packet fields: {d}")
                bad += 1
                continue
        elif d["type"] == "rc":
            rc += 1
            if r.size == 8:
                want = [elrs.crsf_to_uint10(max(elrs.CRSF_MIN, min(elrs.CRSF_MAX, v))) for v in ch_crsf[:4]]
                if d["ch10"] != want:
                    print(f"[FAIL] channels {d['ch10']} != {want}")
                    bad += 1
                    continue
            p["nonces"] = d["nonces"]
        good.append(p)
    n = len(pkts)
    ok_pkts = bad == 0 and len(good) >= 0.95 * n and sync > 0
    print(f"[{'PASS' if ok_pkts else 'FAIL'}] {n} packets from the SX127x: {len(good)} with a valid ExpressLRS CRC "
          f"({rc} rc, {sync} sync), {crc_bad} failed the CRC (radio errors), {bad} with wrong contents"
          + (f", {garbled} serial lines garbled" if garbled else ""))

    # timing: pick each RC packet's nonce so consecutive packets agree with their
    # arrival times (the CRC alone can match more than one nonce)
    period = r.interval
    resid = []
    prev = None
    for p in good:
        cands = [p["nonce"]] if "nonce" in p else p["nonces"]
        if prev is None:
            p["n"] = cands[0]
        else:
            dt = p["t"] - prev["t"]
            best = min(cands, key=lambda c: abs(dt - ((c - prev["n"]) % 256) * period))
            p["n"] = best
            k = (best - prev["n"]) % 256
            if k and dt < 200 * period:
                resid.append(dt - k * period)
        prev = p
    if resid:
        spread = max(resid) - min(resid)
        mean = statistics.mean(resid)
        ok = abs(mean) < 50 and spread < 200
        print(f"[{'PASS' if ok else 'FAIL'}] timing over {len(resid)} packet pairs: arrival vs nonce x {period} us, "
              f"mean {mean:+.1f} us, spread {spread} us (min {min(resid)}, max {max(resid)})")
    rssi = statistics.median(p["rssi"] for p in pkts)
    snr = statistics.median(p["snr"] for p in pkts)
    fei = statistics.median(p["fei"] for p in pkts)
    print(f"  RSSI {rssi:.0f} dBm, SNR {snr:.1f} dB, frequency error (chip FEI) {fei:+.0f} Hz "
          f"({fei / freq * 1e6:+.1f} ppm)")
    return 0 if ok_pkts else 1


if __name__ == "__main__":
    sys.exit(main())
