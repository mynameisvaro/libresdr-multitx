#!/usr/bin/env python3
"""Measure a receiver's link through the ESP32 SBUS monitor, and tune the frequency offset.

Reads tools/sbus_monitor's once-a-second lines from the ESP32's serial port
(also over USB/IP), timestamps them into a log, and, with --sweep, steps
Multiprotocol's option byte (FrSky: frequency fine tune, ~1.59 kHz per step)
on the running protocol, reporting the lost-frame rate for each value.

  sbus_tune.py --port /dev/serial/by-id/usb-Silicon_Labs_CP2102_...  [--log logs/sbus.log]
  sbus_tune.py --port ... --sweep -60 -40 -20 0 20 [--dwell 15]
  sbus_tune.py --port ... --watch 60          # print the lines for 60 s, then a summary

The protocol must already be running (mpm start frskyx ...). The ESP32 isn't
reset on opening (DTR/RTS held low). Needs pyserial.
"""

import argparse
import sys
import threading
import time

import serial


class Monitor:
    """Reads sbus_monitor lines in the background: (time, frames, lost, failsafe)."""

    def __init__(self, port, log=None, echo=False):
        self.s = serial.Serial()
        self.s.port, self.s.baudrate, self.s.timeout = port, 115200, 2
        self.s.dtr = self.s.rts = False
        self.s.open()
        self.log = open(log, "a", buffering=1) if log else None
        self.echo = echo
        self.rows = []
        threading.Thread(target=self._run, daemon=True).start()

    def _run(self):
        while True:
            line = self.s.readline().decode(errors="replace").strip()
            if not line:
                continue
            t = time.time()
            if self.log:
                self.log.write(f"{t:.2f} {time.strftime('%H:%M:%S')} {line}\n")
            if self.echo:
                print(f"{time.strftime('%H:%M:%S')} {line}", flush=True)
            p = line.split()
            if len(p) > 6 and p[0] == "fps":
                self.rows.append((t, int(p[1]), int(p[3]), int(p[6])))

    def stats(self, t0, t1):
        rows = [r for r in self.rows if t0 <= r[0] <= t1]
        frames = sum(r[1] for r in rows)
        lost = sum(r[2] for r in rows)
        return len(rows), frames, lost, sum(r[3] for r in rows)


def summary(n, frames, lost, fs):
    return (f"{n} s, {frames} frames, lost {lost} ({100 * lost / max(frames, 1):.2f}%), "
            f"failsafe {fs}")


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--port", required=True, help="the ESP32's serial port")
    ap.add_argument("--log", help="append the timestamped lines to this file")
    ap.add_argument("--sweep", type=int, nargs="+", metavar="OPT", help="option values to try")
    ap.add_argument("--dwell", type=float, default=15, help="seconds measured per value")
    ap.add_argument("--settle", type=float, default=3, help="seconds ignored after a change")
    ap.add_argument("--watch", type=float, help="print the monitor for this many seconds")
    args = ap.parse_args(argv[1:])

    mon = Monitor(args.port, args.log, echo=args.watch is not None)
    if args.watch is not None:
        t0 = time.time()
        time.sleep(args.watch)
        print(summary(*mon.stats(t0, time.time())))
    if args.sweep:
        from usbdbg import Bridge, Console
        con = Console(Bridge())
        for opt in args.sweep:
            con.run(f"mpm option {opt}", timeout=20)
            t0 = time.time() + args.settle
            time.sleep(args.settle + args.dwell)
            print(f"option {opt:+4d} ({opt * 26e3 / 16384:+6.1f} kHz): "
                  f"{summary(*mon.stats(t0, time.time()))}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
