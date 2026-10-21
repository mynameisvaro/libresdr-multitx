#!/usr/bin/env python3
"""Checks that usbdbg.py and the usb_cmd_proc RTL agree on the wire format.

Encodes command packets with usbdbg.encode(), replays them through the RTL in
xsim (hw/ip/usb_debug_bridge/tb/tb_cosim.v), and decodes the responses with
usbdbg.decode(). Needs Vivado 2022.2.
"""

import os
import random
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from usbdbg import OP_CTRL_RD, OP_READ, OP_WRITE, decode, encode  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
TB = os.path.join(HERE, "..", "hw", "ip", "usb_debug_bridge", "tb")
WORK = os.path.join(TB, "work_cosim")
VIVADO = "/opt/AMD/2022.2/Vivado/2022.2/settings64.sh"

rnd = random.Random(1)
data7 = [rnd.getrandbits(32) for _ in range(7)]
data256 = [rnd.getrandbits(32) for _ in range(256)]

# (commands, check(results)) per packet
tests = [
    ([(OP_WRITE, 0, 7, 0x100, data7),
      (OP_READ, 0, 7, 0x100, None),
      (OP_READ, 0, 4, 0x104, None),
      (OP_CTRL_RD, 0, 0, 0, None)],
     lambda r: r[1][4] == data7 and r[2][4] == data7[1:5] and r[3][4] == [0x0001_0000, 0]
               and all(x[1] == 0 for x in r)),
    ([(OP_WRITE, 0, 256, 0x400, data256)],
     lambda r: r[0][1] == 0),
    ([(OP_READ, 0, 256, 0x400, None),
      (OP_READ, 0, 1, 0x8000_0000, None)],
     lambda r: r[0][4] == data256 and r[1][1] == 0x02),
]


def to_lines(pkt):
    words = [struct.unpack_from("<II", pkt, i) for i in range(0, len(pkt), 8)]
    return [f"{int(n == len(words) - 1)} {hi:08x}{lo:08x}" for n, (hi, lo) in enumerate(words)]


def main():
    os.makedirs(WORK, exist_ok=True)
    lines = []
    for seq, (cmds, _) in enumerate(tests, start=1):
        lines += to_lines(encode(cmds, seq))
    with open(os.path.join(WORK, "cmd.txt"), "w") as f:
        f.write("\n".join(lines) + "\n")

    sim = (f"source {VIVADO} && xvlog -sv ../../usb_cmd_proc.v ../tb_cosim.v >/dev/null && "
           "xelab tb_cosim -s cosim >/dev/null && xsim cosim -R >/dev/null")
    subprocess.run(["bash", "-c", sim], cwd=WORK, check=True)

    packets, cur = [], b""
    for line in open(os.path.join(WORK, "rsp.txt")):
        last, word = line.split()
        cur += struct.pack("<II", int(word[:8], 16), int(word[8:], 16))
        if last == "1":
            packets.append(cur)
            cur = b""

    ok = len(packets) == len(tests)
    for seq, ((cmds, check), raw) in enumerate(zip(tests, packets), start=1):
        good = check(decode(cmds, seq, raw))
        print(f"packet {seq}: {'ok' if good else 'MISMATCH'}")
        ok &= good
    print("PASS: usbdbg.py <-> usb_cmd_proc" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
