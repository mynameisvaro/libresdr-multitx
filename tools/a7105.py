"""A7105 packets on the air: frequencies, CRC-16, (7,4) FEC and a demodulator.

Shared by flysky_check.py, a7105_sniff.py and afhds2a_bind.py. The packet is
preamble (0101.. or 1010..), ID code, payload, CRC-16, all sent MSB first; with
FEC on, payload and CRC go out as one 7-bit code word per nibble. The CRC
parameters and the code words aren't in the data sheet, so they are a Coding
that matches the firmware's (``a7105 coding`` on the console) or one learned
by a7105_sniff.py.
"""

import re

import numpy as np

import gfsk

FREQ0 = 32e6 * (0x4B + 2 / 65536)       # LO base, BIP 0x4B / BFP 0x0002: 2400.000977 MHz
STEP = 500e3                             # channel step (CHR 15, 32 MHz PFD)
BAUD = 500e3
FLYSKY_ID = bytes([0x54, 0x75, 0xC5, 0x2A])  # A7105_Init's ID for FlySky / AFHDS2A


def chan_freq(ch):
    return FREQ0 + ch * STEP


def freq_chan(f):
    return int(round((f - FREQ0) / STEP))


def crc16(data, poly, init, reflect=False, xorout=0):
    def rev(v, n):
        return int(f"{v:0{n}b}"[::-1], 2)
    crc = init
    for b in data:
        crc ^= (rev(b, 8) if reflect else b) << 8
        for _ in range(8):
            crc = ((crc << 1) ^ poly) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    if reflect:
        crc = rev(crc, 16)
    return crc ^ xorout


class Coding:
    def __init__(self, crc_poly=0x1021, crc_init=0xFFFF, crc_reflect=False, crc_xorout=0,
                 fec=None, low_first=False):
        self.crc_poly, self.crc_init = crc_poly, crc_init
        self.crc_reflect, self.crc_xorout = crc_reflect, crc_xorout
        self.fec = list(fec) if fec else default_fec()
        self.low_first = low_first

    @classmethod
    def from_console(cls, text):
        """Parse the output of the firmware's 'a7105 coding'."""
        m = re.search(r"crc poly (\w+) init (\w+) reflect (\d) xorout (\w+)\s+fec (\w{32}) (\w+)", text)
        if not m:
            raise ValueError(f"can't parse coding: {text!r}")
        fec = bytes.fromhex(m.group(5))
        return cls(int(m.group(1), 16), int(m.group(2), 16), m.group(3) == "1",
                   int(m.group(4), 16), fec, m.group(6) == "lowfirst")

    def console_cmds(self):
        """Firmware commands that set this coding."""
        return [f"a7105 crc {self.crc_poly:04x} {self.crc_init:04x} {int(self.crc_reflect)} "
                f"{self.crc_xorout:04x}",
                f"a7105 fec {bytes(self.fec).hex()} {'lowfirst' if self.low_first else 'highfirst'}"]

    def __str__(self):
        return (f"crc poly {self.crc_poly:04x} init {self.crc_init:04x} reflect "
                f"{int(self.crc_reflect)} xorout {self.crc_xorout:04x}, fec "
                f"{bytes(self.fec).hex()} {'lowfirst' if self.low_first else 'highfirst'}")

    def crc(self, data):
        return crc16(data, self.crc_poly, self.crc_init, self.crc_reflect, self.crc_xorout)

    def encode(self, data, fec=True):
        """bytes -> bit string (what follows the ID code)."""
        if not fec:
            return "".join(f"{b:08b}" for b in data)
        out = []
        for b in data:
            hi, lo = b >> 4, b & 15
            for n in ((lo, hi) if self.low_first else (hi, lo)):
                out.append(f"{self.fec[n]:07b}")
        return "".join(out)

    def decode(self, bits, nbytes, fec=True):
        """bit string -> (bytes, corrected bits); nearest code word per nibble."""
        if not fec:
            return bytes(int(bits[i:i + 8], 2) for i in range(0, 8 * nbytes, 8)), 0
        words = [int(bits[i:i + 7], 2) for i in range(0, 14 * nbytes, 7)]
        nibbles, fixed = [], 0
        for w in words:
            d = [bin(w ^ c).count("1") for c in self.fec]
            n = int(np.argmin(d))
            nibbles.append(n)
            fixed += d[n]
        out = []
        for i in range(0, len(nibbles), 2):
            a, b = nibbles[i], nibbles[i + 1]
            out.append((b << 4 | a) if self.low_first else (a << 4 | b))
        return bytes(out), fixed


def default_fec():
    """The firmware's default guess: textbook Hamming(7,4), p1 p2 d1 p3 d2 d3 d4."""
    words = []
    for n in range(16):
        d1, d2, d3, d4 = (n >> 3) & 1, (n >> 2) & 1, (n >> 1) & 1, n & 1
        p1, p2, p3 = d1 ^ d2 ^ d4, d1 ^ d3 ^ d4, d2 ^ d3 ^ d4
        words.append(p1 << 6 | p2 << 5 | d1 << 4 | p3 << 3 | d2 << 2 | d3 << 1 | d4)
    return words


def bursts(x, min_len, thresh=0.3):
    """(start, end) sample ranges where the amplitude is above thresh * max."""
    amp = np.abs(x)
    if amp.max() < 1e-9:
        return []
    on = amp > thresh * amp.max()
    edges = np.flatnonzero(np.diff(on.astype(int)))
    starts = list(edges[::2] + 1) if not on[0] else [0] + list(edges[1::2] + 1)
    ends = list(edges[1::2] + 1) if not on[0] else list(edges[::2] + 1)
    if len(ends) < len(starts):
        ends.append(len(x))
    return [(s, e) for s, e in zip(starts, ends) if e - s >= min_len]


def demod(x, fs, baud=BAUD):
    """One burst -> (bit string, carrier offset Hz). The offset comes from the
    preamble, which averages to zero deviation."""
    f = gfsk.discriminator(x, fs)
    sps = fs / baud
    k = max(1, int(round(sps / 2)))
    f = np.convolve(f, np.ones(k) / k, mode="same")
    pre = f[int(8 * sps):int(28 * sps)]       # inside the preamble, after the ramp
    offset = float(np.mean(pre))
    f -= offset
    best = None
    for o in np.linspace(0, sps, 32, endpoint=False):
        idx = np.round(o + np.arange(int((len(f) - 1 - o) / sps) + 1) * sps).astype(int)
        idx = idx[idx < len(f)]
        score = np.mean(np.abs(f[idx]))
        if best is None or score > best[0]:
            best = (score, idx)
    return "".join("1" if v > 0 else "0" for v in f[best[1]]), offset


def find_id(bits, ident=FLYSKY_ID, max_err=0):
    """Index of the first bit after the preamble + ID code, or -1."""
    pre = "01" * 8 if not ident[0] & 0x80 else "10" * 8
    pat = pre + "".join(f"{b:08b}" for b in ident)
    if max_err == 0:
        p = bits.find(pat)
        return p + len(pat) if p >= 0 else -1
    a = np.frombuffer(bits.encode(), np.uint8) - 48
    b = np.frombuffer(pat.encode(), np.uint8) - 48
    for p in range(len(a) - len(b)):
        if np.count_nonzero(a[p:p + len(b)] != b) <= max_err:
            return p + len(b)
    return -1


def iq_from_raw(raw):
    """adc_capture words {Q, I} -> complex samples."""
    v = np.array(raw, dtype=np.uint32)
    i = (v & 0xFFFF).astype(np.int16).astype(float)
    q = (v >> 16).astype(np.int16).astype(float)
    return i + 1j * q
