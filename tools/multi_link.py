#!/usr/bin/env python3
"""Drive the board like a handset drives a Multi module: from the PC, over USB.

Sends Multiprotocol serial frames (protocol, sub-protocol, RX number, option,
bind / range / power flags, 16 channels; Multiprotocol.h "MULTI protocol serial
definition") into the firmware's input ring in the mailbox (fw/src/input.c), at
a steady rate, and shows the firmware's input status once a second.

  multi_link.py frskyx [--sub 0] [--rx 0] [--option -40] [--rate 50]
                [--ch 1=1500us 3=-100% 5=1844 ...] [--bind] [--seconds N]
  multi_link.py flsky2a --bind --seconds 5
  multi_link.py frskyx --gamepad [--map "1=rightx 2=-righty ..."] [--expo 0.3]

Channel values: raw 0..2047 (1024 centre), N% (-125..125), or Nus (988..2012 us
is -100..+100%, as the receivers output). The firmware pauses the protocol when no
frame arrives for its timeout (70 ms, like Multiprotocol); over the USB/IP tunnel
frames come every few hundred ms, so pass --timeout 2000 there.

--gamepad takes the sticks and switches from a gamepad on the PC (gamepad.py has
the mapping; run it alone to see your pad's input names). Like a radio's throttle
warning, nothing is sent until the throttle stick is low; holding the bind button
("back") sets the bind flag; if the pad disconnects, sending stops, so the
receiver goes to failsafe after the firmware's timeout.

Also importable: MultiLink(Bridge()).send(Frame(...)).
"""

import argparse
import struct
import sys
import time

from usbdbg import MAILBOX, OP_READ, OP_WRITE, Bridge, BridgeError

IN_BASE = MAILBOX + 0xC40               # fw/src/console.h: struct mailbox
IN_SLOTS = 8
SLOT_WORDS = 10                         # len + 36 data bytes
ST_USB, ST_SIGNAL, ST_PAUSED, ST_RUNNING, ST_BINDING, ST_BAD_PROTO = (1 << i for i in range(6))

# protocols built into the firmware (mpm list); numbers also work
PROTOCOLS = {"flysky": 1, "frskyd": 3, "frskyx": 15, "flsky2a": 28, "afhds2a": 28,
             "elrs": 200}            # ExpressLRS (fw/src/elrs): sub = rate, rx = model ID


class Frame:
    """One Multi serial frame (protocol V2, 27 bytes)."""

    def __init__(self, proto, sub=0, rx=0, option=0, channels=None, bind=False,
                 autobind=False, range_check=False, low_power=False):
        self.proto, self.sub, self.rx, self.option = proto, sub, rx, option
        self.channels = list(channels) if channels else [1024] * 16
        self.bind, self.autobind = bind, autobind
        self.range_check, self.low_power = range_check, low_power

    def encode(self):
        p, rx = self.proto, self.rx
        header = 0x54 | (0 if p & 0x20 else 1)          # bit 0: protocol 0..31 (else +32)
        b1 = (p & 0x1F) | (0x80 if self.bind else 0) | (0x40 if self.autobind else 0) \
            | (0x20 if self.range_check else 0)
        b2 = (rx & 0x0F) | (self.sub & 7) << 4 | (0x80 if self.low_power else 0)
        bits = 0
        for i, v in enumerate(self.channels[:16]):
            bits |= (max(0, min(2047, int(v))) & 0x7FF) << (11 * i)
        b26 = (p & 0xC0) | (rx & 0x30)
        return bytes([header, b1, b2, self.option & 0xFF]) + bits.to_bytes(22, "little") + bytes([b26])


class MultiLink:
    """Host end of the firmware's input ring (one bridge transaction per frame)."""

    def __init__(self, br):
        self.br = br
        self.session = None
        self.head = 0

    def _sync(self):
        session = self.br.read(MAILBOX + 4)[0]
        if session != self.session:     # firmware (re)started: it skipped to our head
            self.session = session
            self.head = self.br.read(IN_BASE)[0]

    def send(self, frame):
        """Queue one frame; returns the status words (head, tail, status, model,
        frames, errors) as read right after it."""
        self._sync()
        data = frame.encode() if isinstance(frame, Frame) else bytes(frame)
        words = [len(data)] + list(struct.unpack(f"<{(len(data) + 3) // 4}I",
                                                 data + bytes(-len(data) % 4)))
        words += [0] * (SLOT_WORDS - len(words))
        while True:
            st = self.status()
            if (self.head - st[1]) & 0xFFFFFFFF < IN_SLOTS:
                break
            time.sleep(0.002)           # ring full: the firmware is busy (protocol start)
        slot = IN_BASE + 0x20 + 4 * SLOT_WORDS * (self.head % IN_SLOTS)
        self.head = (self.head + 1) & 0xFFFFFFFF
        res = self.br.transact([(OP_WRITE, 0, SLOT_WORDS, slot, words),
                                (OP_WRITE, 0, 1, IN_BASE, [self.head]),
                                (OP_READ, 0, 6, IN_BASE, None)])
        return res[2][4]

    def status(self):
        return self.br.read(IN_BASE, 6)


def describe(st):
    head, tail, status, model, frames, errors = st
    flags = [n for b, n in ((ST_USB, "usb"), (ST_SIGNAL, "signal"), (ST_PAUSED, "paused"),
                            (ST_RUNNING, "running"), (ST_BINDING, "binding"),
                            (ST_BAD_PROTO, "bad protocol")) if status & b]
    m = "none" if model == 0xFFFFFFFF else \
        f"proto {model & 0xFF} sub {model >> 8 & 0xFF} rx {model >> 16 & 0xFF}"
    return f"{','.join(flags) or 'idle'}; {m}; {frames} frames, {errors} errors"


def channel_value(text):
    t = text.strip().lower()
    if t.endswith("us"):
        return round(1024 + (float(t[:-2]) - 1500) * 820 / 512)
    if t.endswith("%"):
        return round(1024 + float(t[:-1]) * 8.2)
    return int(t, 0)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("proto", help="protocol name (" + ", ".join(PROTOCOLS) + ") or number")
    ap.add_argument("--sub", type=int, default=0)
    ap.add_argument("--rx", type=int, default=0, help="RX number (model match)")
    ap.add_argument("--option", type=int, default=0, help="option byte, e.g. FrSky fine tune")
    ap.add_argument("--ch", nargs="*", default=[], metavar="N=VALUE", help="channel values (1..16)")
    ap.add_argument("--bind", action="store_true")
    ap.add_argument("--range", action="store_true", help="range check (low power)")
    ap.add_argument("--low-power", action="store_true")
    ap.add_argument("--rate", type=float, default=50, help="frames per second")
    ap.add_argument("--seconds", type=float, default=None, help="stop after this long (default: Ctrl-C)")
    ap.add_argument("--timeout", type=int, default=None, help="set the firmware's input timeout (ms)")
    g = ap.add_argument_group("gamepad")
    g.add_argument("--gamepad", type=int, nargs="?", const=0, default=None, metavar="INDEX",
                   help="sticks and switches from a gamepad (default the first)")
    g.add_argument("--map", default=None, help="channel mapping (see gamepad.py; default mode 2)")
    g.add_argument("--deadband", type=float, default=0.04, help="stick deadband (fraction)")
    g.add_argument("--expo", type=float, default=0.0, help="expo 0..1 on the sticks")
    g.add_argument("--endpoint", type=float, default=100, help="stick end points in %%")
    g.add_argument("--bind-button", default="back", help="button that sets the bind flag while held")
    g.add_argument("--no-throttle-check", action="store_true", help="send even with the throttle up")
    a = ap.parse_args()

    proto = PROTOCOLS.get(a.proto.lower().replace(" ", ""))
    proto = proto if proto is not None else int(a.proto, 0)
    ch = [1024] * 16
    ch[2] = 204                                       # throttle low (AETR: channel 3)
    for c in a.ch:
        n, v = c.split("=")
        ch[int(n) - 1] = channel_value(v)

    br = Bridge()
    if a.timeout is not None:
        from usbdbg import Console
        Console(br).run(f"input timeout {a.timeout}")
    link = MultiLink(br)
    frame = Frame(proto, a.sub, a.rx, a.option, ch, a.bind, False, a.range, a.low_power)
    pad = mapping = None
    if a.gamepad is not None:
        import gamepad
        pad = gamepad.Gamepad(a.gamepad)
        mapping = gamepad.Mapping(a.map or gamepad.DEFAULT_MAP, a.deadband, a.expo, a.endpoint)
        print(f"gamepad: {pad.name} ({'standard names' if pad.standard else 'raw names'}); "
              f"inputs {', '.join(mapping.inputs())}")
        missing = [i for i in mapping.inputs() + [a.bind_button] if i not in pad.state()]
        if missing:
            raise SystemExit(f"this gamepad has no {', '.join(missing)} (run gamepad.py to see its inputs)")

    def update():
        # channels and bind flag from the gamepad; False if it's gone
        if not pad.connected():
            return False
        state = pad.state()
        values = mapping.channels(state)
        for c in mapping.entries:
            frame.channels[c - 1] = values[c - 1]
        frame.bind = a.bind or bool(state.get(a.bind_button))
        return True

    if pad and not a.no_throttle_check:
        update()
        if frame.channels[2] > 245:                 # channel 3 (throttle) above -95%
            print("throttle up: move the throttle stick to low to start", flush=True)
            while update() and frame.channels[2] > 245:
                time.sleep(0.05)
    if not pad:
        print("frame", frame.encode().hex(" "))
    t0 = last = time.time()
    n = 0
    try:
        while a.seconds is None or time.time() - t0 < a.seconds:
            if pad and not update():
                print("gamepad disconnected: stopped sending (the receiver goes to failsafe)")
                break
            st = link.send(frame)
            n += 1
            now = time.time()
            if now - last >= 1:
                line = f"{now - t0:6.1f} s  {n / (now - t0):5.1f} frames/s  {describe(st)}"
                if pad:
                    line += "  ch " + " ".join(f"{(v - 1024) / 8.2:+4.0f}" for v in frame.channels[:8]) \
                            + (" BIND" if frame.bind else "")
                print(line, flush=True)
                last = now
            time.sleep(max(0, t0 + n / a.rate - time.time()))
    except KeyboardInterrupt:
        print()
    print("final:", describe(link.status()))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except BridgeError as e:
        sys.exit(f"error: {e}")
