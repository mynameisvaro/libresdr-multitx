#!/usr/bin/env python3
"""Compares the firmware's ExpressLRS transmitter (fw/src/elrs, with ExpressLRS's
own OTA/FHSS/CRC code from the submodule, built for the PC) with tools/elrs.py,
an independent model written from the ExpressLRS source.

Every slot of every case must match: silent (telemetry) slots, frequencies and
the packet bytes (sync packets, channel packing, switches, CRC with the nonce).
Also checks the firmware's MD5 (binding phrase -> UID) against hashlib.

  python3 fw/test/test_elrs.py      (needs g++)
"""

import os
import random
import subprocess
import sys
import tempfile

here = os.path.dirname(os.path.abspath(__file__))
fw = os.path.join(here, "..")
sys.path.insert(0, os.path.join(fw, "../tools"))
import elrs  # noqa: E402

ELRS = os.path.join(fw, "third_party/expresslrs/src")
UPSTREAM = ["lib/OTA/OTA.cpp", "lib/FHSS/FHSS.cpp", "lib/FHSS/random.cpp", "lib/CRC/crc.cpp"]
INCLUDES = ["src/elrs/shim", "src/elrs"]
UP_INCLUDES = ["include", "lib/OTA", "lib/FHSS", "lib/CRC", "lib/FIFO"]


def build(tmp):
    exe = os.path.join(tmp, "elrs_host")
    flags = ["-O1", "-std=gnu++17", "-DRADIO_SX127X", "-DTARGET_TX", "-DRegulatory_Domain_FCC_915", "-w"]
    flags += ["-I" + os.path.join(fw, d) for d in INCLUDES] + ["-I" + os.path.join(ELRS, d) for d in UP_INCLUDES]
    srcs = [os.path.join(ELRS, s) for s in UPSTREAM] + [os.path.join(fw, "src/elrs/elrs_tx.cpp"),
                                                         os.path.join(here, "elrs_host.cpp")]
    md5o = os.path.join(tmp, "md5.o")
    subprocess.run(["gcc", "-O1", "-c", "-Wall", "-Wextra", os.path.join(fw, "src/elrs/md5.c"), "-o", md5o], check=True)
    subprocess.run(["g++"] + flags + srcs + [md5o, "-o", exe], check=True)
    return exe


def run_case(exe, phrase, rate, domain, mode, tlm, mm, rx, n, seed, t0_ms):
    out = subprocess.run([exe, "run", phrase, str(rate), domain, str(mode), str(tlm), str(mm), str(rx),
                          str(n), str(seed), str(t0_ms)], capture_output=True, text=True, check=True).stdout
    uid = elrs.uid_from_phrase(phrase)
    r = elrs.RATES[rate]
    tlm_enum = elrs.TLM_STD
    if tlm:
        tlm_enum = [e for e in range(elrs.TLM_NO_TLM, elrs.TLM_1_2 + 1) if elrs.tlm_denom(e) == tlm][0]
    tx = elrs.Tx(uid, r, domain, mode, tlm_enum, rx & 0x3F if mm else None, t0_us=t0_ms * 1000)
    bad = 0
    kinds = {"rc": 0, "sync": 0, "tlm": 0}
    for i, line in enumerate(out.splitlines()):
        f = line.split()
        chans = [int(v) for v in f[-1].split(",")]
        o = tx.tick([elrs.multi_to_crsf(v) for v in chans])
        kinds[o["type"]] += 1
        if f[0] == "-":
            ok = o["packet"] is None
            got = "silent"
        else:
            ok = (o["packet"] is not None and abs(float(f[0]) - o["freq"]) < 0.01
                  and bytes.fromhex(f[1]) == o["packet"])
            got = f"{f[0]} {f[1]}"
        if not ok:
            bad += 1
            if bad <= 3:
                want = "silent" if o["packet"] is None else f"{o['freq']:.3f} {o['packet'].hex()}"
                print(f"    slot {i} nonce {o['nonce']}: firmware {got}, model {want}")
    return bad, kinds


def main():
    with tempfile.TemporaryDirectory() as tmp:
        exe = build(tmp)
        random.seed(2)
        fails = 0
        phrases = ["multitx", "my binding phrase", "", "ä unicode phrase", "x" * 70, "a" * 55, "b" * 56]
        for p in phrases + ["".join(random.choice("abcdefgh 123") for _ in range(random.randrange(1, 40)))
                            for _ in range(20)]:
            got = subprocess.run([exe, "uid", p], capture_output=True, text=True, check=True).stdout.strip()
            if got != elrs.uid_from_phrase(p).hex():
                fails += 1
                print(f"FAIL uid {p!r}: {got} vs {elrs.uid_from_phrase(p).hex()}")
        print(f"[{'PASS' if not fails else 'FAIL'}] binding phrase -> UID (MD5), {len(phrases) + 20} phrases")

        cases = 0
        for rate in range(len(elrs.RATES)):
            for domain in ("FCC915", "EU868", "AU915", "IN866"):
                for mode in (0, 1, 2):
                    for tlm, mm in ((0, 0), (0, 1), (1, 0), (8, 0)):
                        n = 2000 if rate in (0, 5) else 800
                        phrase = random.choice(phrases)
                        bad, kinds = run_case(exe, phrase, rate, domain, mode, tlm, mm, random.randrange(64), n,
                                              random.randrange(1, 1 << 30), random.randrange(0, 100000))
                        cases += 1
                        name = (f"rate {elrs.RATES[rate].name:10s} {domain:6s} switch {mode} "
                                f"tlm {'std' if not tlm else '1:' + str(tlm):5s} mm {mm}")
                        if bad:
                            fails += 1
                            print(f"[FAIL] {name}: {bad} slots differ")
                        elif cases <= 6 or rate == 0 and mode == 0 and tlm == 0 and mm == 0:
                            print(f"[PASS] {name}: {n} slots ({kinds['rc']} rc, {kinds['sync']} sync, "
                                  f"{kinds['tlm']} silent)")
        print(f"{cases} transmitter cases, {fails} failed" if fails else f"all {cases} transmitter cases equal")
        return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
