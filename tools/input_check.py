#!/usr/bin/env python3
"""Check the handset input path (Multi serial frames over USB, fw/src/input.c).

Sends frames with multi_link.MultiLink and checks, through the input status in
the mailbox and `mpm status` on the console:

  1. a FrSky X frame starts the protocol (model in the status, channels exact)
  2. new channel values arrive exactly (16 distinct values, read back 1..8)
  3. setting the bind flag restarts in bind mode, clearing it ends bind
  4. no frames for longer than the timeout pauses the protocol (no callbacks),
     the next frame resumes it
  5. a frame for another protocol (AFHDS2A) switches to it
  6. `mpm stop` on the console takes control back

No receiver needed; the radio is started, so keep an antenna or a load on TX/RX.

  input_check.py [--timeout MS]     (default 300; over the USB/IP tunnel use 3000)
"""

import argparse
import re
import sys
import threading
import time

from multi_link import ST_BINDING, ST_PAUSED, ST_RUNNING, ST_SIGNAL, ST_USB, Frame, MultiLink
from usbdbg import Bridge, Console

results = []


def check(name, ok, detail=""):
    results.append(ok)
    print(f"[{'PASS' if ok else 'FAIL'}] {name}" + (f": {detail}" if detail else ""), flush=True)


def callbacks(con):
    return int(re.search(r"callbacks (\d+)", con.run("mpm status")).group(1))


def channels(con):
    return [int(v) for v in re.search(r"channels ([\d ]+)", con.run("mpm status")).group(1).split()]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--timeout", type=int, default=300, help="firmware input timeout (ms)")
    a = ap.parse_args()

    br = Bridge()
    lock = threading.Lock()                     # the handset thread and the checks share the bridge
    transact = br.transact
    def locked(cmds):
        with lock:
            return transact(cmds)
    br.transact = locked
    con, link = Console(br), MultiLink(br)
    con.run("mpm stop")
    con.run("radio rate 61440000")
    con.run(f"input timeout {a.timeout}")

    # the "handset": sends the current frame at a quarter of the timeout (or as
    # fast as the bridge allows) until told to stop
    state = {"frame": None, "run": True}
    def handset():
        while state["run"]:
            if state["frame"]:
                link.send(state["frame"])
            time.sleep(min(a.timeout / 4000, 0.02))
    thread = threading.Thread(target=handset, daemon=True)

    def use(frame, settle=0.0):
        # switch the handset to a new frame; return the status once the firmware
        # has taken at least two frames after the switch
        n0 = link.status()[4]
        state["frame"] = frame
        t = time.time()
        while link.status()[4] < n0 + 2 and time.time() - t < 20:
            time.sleep(0.01)
        time.sleep(settle)
        return link.status()

    ch = [1024] * 16
    ch[2] = 204
    state["frame"] = Frame(15, 0, 0, 0, ch)
    thread.start()
    t = time.time()
    st = link.status()
    while not st[2] & ST_RUNNING and time.time() - t < 20:   # first start: radio up (~1 s)
        time.sleep(0.1)
        st = link.status()
    check("FrSky X frame starts the protocol", st[2] & ST_RUNNING and st[3] == 15,
          f"status {st[2]:#x}, model {st[3]:#x}")

    vals = [100 + 120 * i for i in range(16)]
    use(Frame(15, 0, 0, 0, vals))
    got = channels(con)
    check("channel values exact", got == vals[:8], f"sent {vals[:8]}, read {got}")

    st = use(Frame(15, 0, 0, 0, vals, bind=True))
    check("bind flag starts bind", bool(st[2] & ST_BINDING), f"status {st[2]:#x}")
    st = use(Frame(15, 0, 0, 0, vals))
    check("clearing the bind flag ends bind", not st[2] & ST_BINDING, f"status {st[2]:#x}")

    state["frame"] = None                       # signal lost
    time.sleep(a.timeout / 1000 + 1)
    st = link.status()
    c0 = callbacks(con)
    time.sleep(1)
    c1 = callbacks(con)
    check("no frames: protocol paused", bool(st[2] & ST_PAUSED) and c1 == c0,
          f"status {st[2]:#x}, callbacks {c0} -> {c1}")
    st = use(Frame(15, 0, 0, 0, vals))
    c2 = callbacks(con)
    check("next frame resumes it", bool(st[2] & ST_SIGNAL) and c2 > c1,
          f"status {st[2]:#x}, callbacks {c1} -> {c2}")

    st = use(Frame(28, 0, 0, 0, ch), settle=0.5)
    check("AFHDS2A frame switches protocol", st[2] & ST_RUNNING and st[3] == 28,
          f"status {st[2]:#x}, model {st[3]:#x}")

    state["run"] = False
    thread.join()
    con.run("mpm stop")
    st = link.status()
    check("mpm stop takes control back", not st[2] & (ST_USB | ST_RUNNING), f"status {st[2]:#x}")
    con.run("input timeout 70")
    print(con.run("input").strip())
    print(f"{sum(results)}/{len(results)} checks passed")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
