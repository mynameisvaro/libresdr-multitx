#!/usr/bin/env python3
"""Compares the firmware's LoRa encoder (fw/src/lora_code.c, built for the PC)
with tools/lora.py, which was checked against real SX127x recordings.

  python3 fw/test/test_lora_code.py      (needs gcc)
"""

import ctypes
import os
import random
import subprocess
import sys
import tempfile

here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(here, "../../tools"))
import lora  # noqa: E402


class Code(ctypes.Structure):
    _fields_ = [(n, ctypes.c_uint8) for n in ("sf", "cr", "crc", "implicit", "ldro")]


def main():
    tmp = tempfile.mkdtemp()
    so = os.path.join(tmp, "lora_code.so")
    subprocess.run(["gcc", "-O2", "-shared", "-fPIC", "-Wall", "-Wextra", "-o", so,
                    os.path.join(here, "../src/lora_code.c")], check=True)
    lib = ctypes.CDLL(so)
    lib.lora_encode.restype = ctypes.c_int
    random.seed(1)
    cases = fails = 0
    out = (ctypes.c_uint16 * 4096)()
    for sf in range(5, 13):
        for cr in range(1, 5):
            for implicit in (0, 1):
                for crc in (0, 1):
                    for ldro in (0, 1):
                        for _ in range(4):
                            p = bytes(random.randrange(256) for _ in range(random.randrange(0, 256)))
                            ref = lora.encode(p, sf, cr, bool(crc), bool(implicit), bool(ldro))
                            c = Code(sf, cr, crc, implicit, ldro)
                            buf = (ctypes.c_uint8 * max(1, len(p))).from_buffer_copy(p or b"\0")
                            n = lib.lora_encode(buf, len(p), ctypes.byref(c), out, 4096)
                            cases += 1
                            if list(out[:n]) != ref:
                                fails += 1
                                if fails < 5:
                                    print(f"FAIL sf{sf} cr{cr} implicit {implicit} crc {crc} ldro {ldro} "
                                          f"len {len(p)}: {n} vs {len(ref)} symbols")
    print(f"{cases - fails}/{cases} cases equal")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
