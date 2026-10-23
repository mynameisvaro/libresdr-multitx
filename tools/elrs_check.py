#!/usr/bin/env python3
"""Check the board's ExpressLRS transmitter end to end, without a receiver.

1. Timing (61.44 MSPS, as in use): for each rate, the packet slots keep the
   rate's period, no packet is dropped (radio busy), no callback runs late and
   the LO never retunes (the whole domain fits one LO slice; hops are NCO steps).
2. Off the air (TX/RX -> cable + 30 dB attenuator -> RX2, as for lora_check.py):
   single packets are captured, demodulated with tools/lora.py (the decoder that
   reproduces real SX127x recordings) and checked with tools/elrs.py, the
   independent ExpressLRS model: the CRC (which includes the nonce), the channel
   values and the switch carried in that nonce's slot, the frequency against the
   hop table, every LoRa symbol, and one sync packet per rate (FHSS index, nonce,
   rate, telemetry ratio, UID).

  elrs_check.py [--count N] [--rx-gain DB] [--domain FCC915] [--skip-timing]

Needs the lora_tx bitstream, the firmware and numpy/scipy.
"""

import argparse
import re
import sys
import time

import elrs
import lora
from a7105 import iq_from_raw
from usbdbg import ADC_CAPTURE, Bridge, Console

N = 16384
PHRASE = "multitx elrs check"
# rate index -> capture sample rate (16K samples must hold one whole packet)
CAPTURE_FS = {0: 2e6, 1: 2e6, 2: 1.5e6, 5: 2e6}
TIMING_S = 3.0

# channels (Multiprotocol scale, CH1..CH12): sticks, CH5 armed, distinct switches
CHANNELS = [204, 700, 1024, 1844, 1700, 300, 600, 900, 1200, 1500, 1800, 1024]


def expected_rc(d, rate, nonce, ch_crsf):
    """compare a parsed RC packet with what the TX must send for that nonce"""
    errs = []
    if rate.size == 8:
        want10 = [elrs.crsf_to_uint10(max(elrs.CRSF_MIN, min(elrs.CRSF_MAX, v))) for v in ch_crsf[:4]]
        if d["ch10"] != want10:
            errs.append(f"channels {d['ch10']} != {want10}")
        idx = elrs.wide_switch_index(nonce)
        want_sw = 1 if idx == 7 else elrs.crsf_to_n(ch_crsf[idx + 5], 64)   # TX power slot: 10 mW
        if d["switches"] & 0x3F != want_sw:
            errs.append(f"switch slot {idx}: {d['switches'] & 0x3F} != {want_sw}")
        if d["armed"] != elrs.crsf_to_bit(ch_crsf[4]):
            errs.append("armed flag")
    else:
        if d["low11"] != [(v >> 1) << 1 for v in ch_crsf[0:4]] or d["high11"] != [(v >> 1) << 1 for v in ch_crsf[4:8]]:
            errs.append(f"channels {d['low11']} {d['high11']}")
        if d["armed"] != elrs.crsf_to_bit(ch_crsf[4]):
            errs.append("armed flag")
    return errs


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--count", type=int, default=6, help="RC packets captured per rate")
    ap.add_argument("--rx-gain", type=float, default=30)
    ap.add_argument("--domain", default="FCC915", choices=sorted(elrs.DOMAINS))
    ap.add_argument("--skip-timing", action="store_true")
    a = ap.parse_args()

    br = Bridge()
    con = Console(br)
    uid = elrs.uid_from_phrase(PHRASE)
    dom = elrs.Domain(a.domain)
    seq = elrs.fhss_sequence(elrs.uid_seed(uid), dom)
    chan_hz = [dom.channel_hz(c) for c in range(dom.count)]
    ch_crsf = [elrs.multi_to_crsf(v) for v in CHANNELS] + [elrs.CRSF_MID] * 4
    results = []

    def check(name, ok, detail=""):
        results.append(ok)
        print(f"[{'PASS' if ok else 'FAIL'}] {name}" + (f": {detail}" if detail else ""), flush=True)

    def run(c, timeout=30):
        out = con.run(c, timeout=timeout).strip()
        if "usage" in out or "unknown" in out or "failed" in out:
            raise SystemExit(f"'{c}': {out}")
        return out

    def status():
        st = run("mpm status")
        m = re.search(r"elrs packets: (\d+) rc, (\d+) sync, (\d+) telemetry slots, (\d+) not sent "
                      r"\(radio busy\), in (\d+) ms", st)
        late = re.search(r"late \(>100 us\) (\d+), worst (\d+) us", st)
        slot = re.search(r"elrs slot time: packet (\d+) us, encode \+ start (\d+) us", st)
        retunes = re.search(r"(\d+) retunes", st)
        return st, [int(v) for v in m.groups()], int(late.group(1)), int(late.group(2)), \
            int(slot.group(1)) + int(slot.group(2)), int(retunes.group(1)) if retunes else None

    try:
        for c in ("mpm stop", "radio stop", "input console", f"elrs phrase {PHRASE}",
                  f"elrs domain {a.domain}", "elrs switch 0", "elrs tlm std", "elrs modelmatch off",
                  "elrs power -10"):
            run(c)
        for i, v in enumerate(CHANNELS):
            run(f"mpm ch {i + 1} {v}")
        got_uid = re.search(r"elrs UID ([\d,]+\d)", run("elrs")).group(1)
        check("binding phrase -> UID", got_uid == ",".join(map(str, uid)), got_uid)

        # ---- 1. timing at the working sample rate
        if not a.skip_timing:
            run("radio rate 61440000")
            for ri, r in enumerate(elrs.RATES):
                run(f"mpm start elrs {ri}")
                _, c0, *_ = status()
                time.sleep(TIMING_S)
                _, c1, late, worst, busy_us, retunes = status()
                # slots counted against the firmware's own clock (ms since the start)
                rate_hz = (sum(c1[:3]) - sum(c0[:3])) / ((c1[4] - c0[4]) / 1e3)
                want = 1e6 / r.interval
                # the frame has to end before the next slot: building and starting it,
                # plus its time on air
                margin = r.interval - busy_us - elrs.toa_us(r)
                ok = (abs(rate_hz - want) < 0.01 * want and c1[3] == 0 and late == 0 and retunes == 0
                      and margin > 0)
                check(f"timing {r.name:10s}", ok,
                      f"{rate_hz:6.1f} slots/s (want {want:.0f}), {c1[1]} sync, {c1[2]} telemetry slots, "
                      f"{c1[3]} not sent, late {late} (worst {worst} us), build + start {busy_us} us "
                      f"(margin {margin:.0f} us after the frame), LO retunes {retunes}")
                run("mpm stop")
            run("radio stop")

        # ---- 2. off the air
        br.write(ADC_CAPTURE + 0x08, [N])
        for ri, fs in CAPTURE_FS.items():
            r = elrs.RATES[ri]
            for c in ("mpm stop", "radio stop", f"radio rate {fs}", "radio rxfollow 1",
                      f"mpm start elrs {ri}", f"rf gain rx {a.rx_gain}"):
                run(c)

            def captures():
                return int(re.search(r"captures (\d+)", run("mpm status")).group(1))

            def capture(cmd):
                n0 = captures()
                run(cmd)
                # wait for the packet that starts the capture (the counter), then for
                # the capture to finish
                t = time.time()
                while captures() == n0 and time.time() - t < 10:
                    time.sleep(0.05)
                while not br.read(ADC_CAPTURE + 0x04)[0] & 2 and time.time() - t < 10:
                    time.sleep(0.002)
                iq = iq_from_raw(br.read(ADC_CAPTURE + 0x10000, N))
                st = run("mpm status")
                lo = float(re.search(r"capture LO ([0-9.]+)", st).group(1))
                freq = float(re.search(r"capture freq ([0-9.]+)", st).group(1))
                frames = lora.decode_iq(iq, fs, r.bw, r.sf, offset=freq - lo, sync=elrs.LORA_SYNC,
                                        cr=r.cr, crc=False, implicit=r.size)
                return freq, frames[0] if frames else None

            for i in range(a.count + 1):
                is_sync = i == a.count
                freq, fr = capture("elrs capsync" if is_sync else "radio capture")
                name = f"{r.name:10s} {'sync' if is_sync else 'rc  '} #{i if not is_sync else 0} {freq / 1e6:9.4f} MHz"
                if fr is None:
                    check(name, False, "no frame found")
                    continue
                pkt = fr[0]
                sym_ok = fr[3][:len(lora.encode(pkt, r.sf, r.cr, False, True))] == \
                    lora.encode(pkt, r.sf, r.cr, False, True)
                d = elrs.parse(pkt, uid)
                in_table = any(abs(freq - f) < 1 for f in chan_hz)
                if d is None:
                    check(name, False, f"CRC matches no nonce: {pkt.hex()}")
                    continue
                errs = [] if sym_ok else ["symbols differ"]
                if not in_table:
                    errs.append("frequency not a domain channel")
                if is_sync:
                    if d["type"] != "sync":
                        errs.append(f"type {d['type']}")
                    else:
                        if abs(chan_hz[seq[d["fhss_index"]]] - freq) > 1:
                            errs.append(f"fhss index {d['fhss_index']} is not this channel")
                        if seq[d["fhss_index"]] != dom.sync_channel:
                            errs.append("not on the sync channel")
                        if d["rate_enum"] != r.enum or d["tlm_enum"] != r.tlm or d["uid4"] != uid[4] \
                                or d["uid5"] != uid[5] or d["switch_mode"] != 0:
                            errs.append(f"fields {d}")
                    detail = (f"fhss {d.get('fhss_index')} nonce {d.get('nonce')} rate {d.get('rate_enum')} "
                              f"tlm 1:{elrs.tlm_denom(d.get('tlm_enum', 1))}")
                else:
                    if d["type"] != "rc":
                        errs.append(f"type {d['type']}")
                        detail = ""
                    else:
                        # the CRC can match more than one nonce only by chance; the
                        # switch slot then tells them apart
                        fits = [n for n in d["nonces"] if not expected_rc(d, r, n, ch_crsf)]
                        if not fits:
                            errs += expected_rc(d, r, d["nonces"][0], ch_crsf)
                        detail = f"nonce {fits[0] if fits else d['nonces']}"
                check(name, not errs, (", ".join(errs) + "; " if errs else "") + detail + f", {pkt.hex()}")
            print("  " + run("elrs").splitlines()[-1])
    finally:
        con.run("mpm stop")
        con.run("radio stop")
        con.run("radio rxfollow 0")
        con.run("radio rate 61440000")
    print(f"\n{sum(results)}/{len(results)} checks passed")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
