#!/usr/bin/env python3
"""Check the PPM input path (hw/ip/ppm_in + firmware) without a radio.

Uses ppm_in's built-in generator in place of the CN2 pin, so nothing has to be
wired, and checks:

  1. the decoder measures the generator's channel widths
  2. `input ppm` feeds them into a running protocol, mapped as Multiprotocol maps
     PPM (988..2012 us = -100..+100%, map16b integer maths, clamped)
  3. new widths follow
  4. no PPM frames: the protocol pauses (callbacks stop); frames again: it resumes

  ppm_check.py
"""

import re
import sys
import time

from usbdbg import Bridge, Console

results = []


def check(name, ok, detail=""):
    results.append(ok)
    print(f"[{'PASS' if ok else 'FAIL'}] {name}" + (f": {detail}" if detail else ""), flush=True)


def multi_map(us, lo=988, hi=2012):
    """Multiprotocol: map16b(2*us, 2*lo, 2*hi, 204, 1844), clamped to 0..2047."""
    y = (1844 - 204) * (2 * us - 2 * lo)
    q = abs(y) // ((hi - lo) * 2) * (1 if y >= 0 else -1)   # C division truncates
    v = q + 204
    return 0 if v < 0 else min(v, 2047)


def main():
    con = Console(Bridge())

    def run(c):
        out = con.run(c, timeout=30).strip()
        if "usage" in out or "unknown" in out or "no ppm_in" in out:
            raise SystemExit(f"'{c}': {out}")
        return out

    def channels():
        return [int(v) for v in re.search(r"channels ([\d ]+)", run("mpm status")).group(1).split()]

    def callbacks():
        return int(re.search(r"callbacks (\d+)", run("mpm status")).group(1))

    run("mpm stop")
    run("radio rate 61440000")
    widths = [988, 1500, 2012, 1200, 1750, 900, 2100, 1600]
    run("ppm test " + " ".join(map(str, widths)))
    time.sleep(0.2)
    out = run("ppm")
    got = [float(v) for v in re.search(r"channels:([\d. ]+) us", out).group(1).split()]
    check("decoder measures the generator", len(got) == 8 and all(abs(g - w) <= 0.5 for g, w in zip(got, widths)),
          out)

    run("input ppm")
    run("input timeout 300")
    run("mpm start frskyx")
    time.sleep(0.5)
    want = [multi_map(w) for w in widths]
    got = channels()
    check("channels mapped like Multiprotocol", got == want, f"want {want}, got {got}")

    widths2 = [1100, 1300, 1500, 1700, 1900, 1000, 2000, 1500]
    run("ppm test " + " ".join(map(str, widths2)))
    time.sleep(0.3)
    want2 = [multi_map(w) for w in widths2]
    got = channels()
    check("new widths follow", got == want2, f"want {want2}, got {got}")

    run("ppm test 1500 frame 1000")          # one channel only: no valid frames
    time.sleep(1.0)
    c0 = callbacks()
    time.sleep(1.0)
    c1 = callbacks()
    st = run("input")
    check("no PPM frames: protocol paused", c1 == c0 and "paused" in st, f"callbacks {c0} -> {c1}; {st}")

    run("ppm test " + " ".join(map(str, widths)))
    time.sleep(0.5)
    c2 = callbacks()
    time.sleep(0.5)
    c3 = callbacks()
    check("frames again: protocol resumes", c3 > c2 and channels() == want, f"callbacks {c2} -> {c3}")

    late = re.search(r"late \(>100 us\) (\d+), worst (\d+) us", run("mpm status")).groups()
    check("no late callbacks", late[0] == "0", f"late {late[0]}, worst {late[1]} us")

    run("mpm stop")
    run("input console")
    run("input timeout 70")
    run("ppm test off")
    print(f"{sum(results)}/{len(results)} checks passed")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
