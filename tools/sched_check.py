#!/usr/bin/env python3
"""Check that console traffic doesn't delay the protocol callbacks.

Starts a Multiprotocol protocol, then loads the console in three ways while it
runs, and reads the scheduler's counters from `mpm status` after each phase:

  idle    nothing but the status reads
  busy    back-to-back commands with long output (mpm status, cc2500, help)
  stall   commands sent without reading the output, so the firmware's 2 KB
          output ring fills and console output waits for the host (up to 50 ms)

A callback counts as late when it starts more than 100 us after its deadline.
No RF hardware is needed beyond the board itself (the radio is started, so
keep an antenna or a load on TX/RX).

  sched_check.py [--proto frskyx] [--seconds 10]
"""

import argparse
import re
import sys
import time

from usbdbg import Bridge, Console

STATUS = re.compile(r"callbacks (\d+), late \(>100 us\) (\d+), worst (\d+) us")


def counters(con):
    m = STATUS.search(con.run("mpm status"))
    if not m:
        raise SystemExit("no scheduler counters in 'mpm status'")
    return tuple(int(v) for v in m.groups())


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--proto", default="frskyx", help="protocol and arguments for mpm start")
    ap.add_argument("--seconds", type=float, default=10, help="length of each phase")
    a = ap.parse_args()

    con = Console(Bridge())
    if con._header() is None:
        raise SystemExit("firmware console not running")
    print(con.run(f"mpm start {a.proto}", timeout=60).strip())
    time.sleep(1)

    def phase(name, load):
        c0, l0, _ = counters(con)
        t0 = time.time()
        rounds = 0
        while time.time() - t0 < a.seconds:
            load()
            rounds += 1
        c1, l1, worst = counters(con)
        dt = time.time() - t0
        print(f"{name:6s} {dt:5.1f} s, {rounds:4d} rounds, {c1 - c0:6d} callbacks "
              f"({(c1 - c0) / dt:.0f}/s), late {l1 - l0}, worst so far {worst} us")
        return l1 - l0

    def busy():
        for c in ("mpm status", "cc2500", "help"):
            con.run(c)

    def stall():
        con.poll()
        con.send("help\rhelp\rcc2500\rhelp\r")    # ~5 KB of output into a 2 KB ring
        time.sleep(0.5)                           # don't read: the ring fills
        t = time.time()
        while time.time() - t < 5 and not con.poll().endswith(b"mtx> "):
            pass

    late = phase("idle", lambda: time.sleep(0.2))
    late += phase("busy", busy)
    late += phase("stall", stall)
    con.run("mpm stop")
    print("console dropped bytes:", re.search(r"(\d+) bytes dropped", con.run("info")).group(1))
    print("PASS" if late == 0 else "FAIL: late callbacks")
    return 0 if late == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
