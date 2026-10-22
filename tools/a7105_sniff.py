#!/usr/bin/env python3
"""Learn the A7105's CRC and FEC coding from a real FlySky transmitter.

The A7105 data sheet gives the packet layout but not the CRC-16 initial value
or reflection, nor the bit layout of its (7,4) Hamming code words, which the
virtual A7105 needs to match. This listens on one channel, captures packets
from a FlySky transmitter (or anything else sending with the FlySky ID code),
and works the coding out:

  - FEC (AFHDS2A): the code words of all packets should form a 16-word linear
    code. Every linear map from it to nibbles is tried, and the ones that
    give a known packet type in byte 0 of every packet are kept (bind 0xbb /
    0xbc, sticks 0x58, settings 0xaa, failsafe 0x56; AFHDS: 0x55 / 0xaa).
  - CRC: two packets of the same length differ in their CRC by the zero-init
    CRC of their difference, whatever the initial value and final XOR, so
    every polynomial (and reflection) is tried on packet pairs. The initial
    value then follows from one packet (with final XOR 0).

  a7105_sniff.py [--proto afhds2a|afhds] [--chan N] [--packets N] [--save F]
  a7105_sniff.py --load F [F...]     # solve again from saved bursts

Put the transmitter next to the board with an antenna on RX2 (or cable +
attenuator into RX2) and switch it to bind (it then sends on fixed channels:
AFHDS2A on 13 and 140, AFHDS on 1). Packets of more than one type (bind and
normal) make the FEC solution unique, so let it run through a bind and back to
normal if it can. Move the sticks while it captures: the CRC needs packets
that differ (AFHDS bind packets are all the same otherwise). At the end it prints the firmware commands that set the
coding; nothing is changed on the board.
"""

import argparse
import itertools
import sys
import time
from collections import Counter

import numpy as np

import a7105

N = 16384
FS = 4e6
RX_OFFSET = 1e6               # RX LO below the channel, away from the DC correction
TYPES = {"afhds2a": {0xBB, 0xBC, 0x58, 0xAA, 0x56}, "afhds": {0x55, 0xAA}}
NBYTES = {"afhds2a": 38, "afhds": 21}


# ---------------------------------------------------------------- capture
def capture(args):
    from usbdbg import ADC_CAPTURE, Bridge, Console
    br = Bridge()
    con = Console(br)
    f = a7105.chan_freq(args.chan)
    cmds = ["mpm stop", f"radio rate {FS}", "radio start", f"rf freq rx {f - RX_OFFSET}",
            f"rf gain rx {args.rx_gain}"]
    if args.keep_tx:                      # self-test: a protocol is already transmitting
        cmds = [f"rf freq rx {f - RX_OFFSET}", f"rf gain rx {args.rx_gain}"]
    for c in cmds:
        print(f"  {c:32s} {con.run(c, timeout=20).strip()}")
    br.write(ADC_CAPTURE + 0x08, [N])
    found, tries = [], 0
    t0 = time.time()
    while len(found) < args.packets and time.time() - t0 < args.timeout:
        tries += 1
        br.write(ADC_CAPTURE + 0x00, [1])
        time.sleep(N / FS + 0.002)
        for _ in range(100):
            if not br.read(ADC_CAPTURE + 0x04)[0] & 1:
                break
            time.sleep(0.001)
        x = a7105.iq_from_raw(br.read(ADC_CAPTURE + 0x10000, N))
        x *= np.exp(-2j * np.pi * RX_OFFSET * np.arange(N) / FS)
        if np.max(np.abs(x)) < 64:                     # nothing there
            continue
        for s, e in a7105.bursts(x, int(100e-6 * FS)):
            if e < N - 1 and s > 0:                    # whole bursts only
                found.append(x[max(0, s - 20):e + 20].astype(np.complex64))
        print(f"\r  {tries} captures, {len(found)} bursts", end="", flush=True)
    print()
    return found


# ---------------------------------------------------------------- demodulation
def burst_bits(bursts, ident):
    """-> list of (bit string after the ID, ID used)"""
    out = []
    ids = Counter()
    for x in bursts:
        bits, off = a7105.demod(x, FS)
        pos = a7105.find_id(bits, ident) if ident else -1
        if pos < 0:
            # unknown ID: take the 32 bits after a run of 16 preamble bits
            for pre in ("01" * 8, "10" * 8):
                p = bits.find(pre)
                if p >= 0:
                    while bits[p + 16:p + 18] == pre[:2]:
                        p += 2
                    ids[bits[p + 16:p + 48]] += 1
                    pos = p + 48
                    break
        if pos > 0:
            out.append(bits[pos:])
    if ids:
        for b, n in ids.most_common(3):
            print(f"  ID code {int(b, 2):08x}: {n} packets")
    return out


# ---------------------------------------------------------------- CRC
def crc_all(data, polys, inits, reflect):
    """zero-padded CRC register for each (poly, init) pair (numpy arrays)"""
    crc = inits.astype(np.uint32).copy()
    polys = polys.astype(np.uint32)
    for b in data:
        if reflect:
            b = int(f"{b:08b}"[::-1], 2)
        for i in range(7, -1, -1):
            fb = ((crc >> 15) & 1) ^ ((b >> i) & 1)
            crc = ((crc << 1) & 0xFFFF) ^ (polys * fb)
    if reflect:
        r = np.zeros_like(crc)
        for i in range(16):
            r |= ((crc >> i) & 1) << (15 - i)
        crc = r
    return crc


def solve_crc(packets, nbytes):
    """packets: list of byte strings (payload + 2 CRC bytes) -> list of (poly, init, reflect)"""
    pk = [p for p in packets if len(p) >= nbytes + 2]
    uniq = list({bytes(p[:nbytes + 2]): None for p in pk})
    if len(uniq) < 2:
        return []
    sols = []
    polys = np.arange(1, 0x10000, 2)
    for reflect in (False, True):
        cand = polys
        for a, b in itertools.islice(itertools.combinations(uniq, 2), 6):
            diff = bytes(x ^ y for x, y in zip(a[:nbytes], b[:nbytes]))
            target = (a[nbytes] << 8 | a[nbytes + 1]) ^ (b[nbytes] << 8 | b[nbytes + 1])
            c = crc_all(diff, cand, np.zeros(len(cand)), reflect)
            cand = cand[c == target]
            if not len(cand):
                break
        for poly in cand:
            p = uniq[0]
            got = p[nbytes] << 8 | p[nbytes + 1]
            inits = np.arange(0x10000)
            c = crc_all(p[:nbytes], np.full(len(inits), poly), inits, reflect)
            for init in inits[c == got]:
                sols.append((int(poly), int(init), reflect))
    return sols


# ---------------------------------------------------------------- FEC
def solve_fec(streams, nbytes, types):
    """streams: bit strings after the ID -> list of (fec table, low_first)"""
    nwords = 2 * (nbytes + 2)
    words = [[int(s[i:i + 7], 2) for i in range(0, 7 * nwords, 7)] for s in streams
             if len(s) >= 7 * nwords]
    count = Counter(w for ws in words for w in ws)
    code = [w for w, _ in count.most_common(16)]
    print(f"  {len(words)} packets, {len(count)} distinct code words "
          f"(16 most common: {' '.join(f'{w:02x}' for w in sorted(code))})")
    # keep packets made only of the 16 code words (no bit errors)
    words = [ws for ws in words if all(w in code for w in ws)]
    cs = set(code)
    linear = len(code) == 16 and all((a ^ b) in cs for a in code for b in code)
    if not linear:
        print("  the code words don't form a linear code: can't solve the FEC this way")
        return []
    nz = [w for w in code if w]
    # a basis of the code
    basis = []
    span = {0}
    for w in nz:
        if w not in span:
            basis.append(w)
            span |= {s ^ w for s in span}
    # coordinates of each code word in that basis
    coord = {}
    for m in range(16):
        w = 0
        for i in range(4):
            if m >> i & 1:
                w ^= basis[i]
        coord[w] = m
    first = [ws[:2] for ws in words]
    sols = []
    vecs = list(range(1, 16))
    for imgs in itertools.permutations(vecs, 4):
        # the images of the basis must be independent
        sp = {0}
        ok = True
        for v in imgs:
            if v in sp:
                ok = False
                break
            sp |= {s ^ v for s in sp}
        if not ok:
            continue

        def nib(w):
            m, n = coord[w], 0
            for i in range(4):
                if m >> i & 1:
                    n ^= imgs[i]
            return n
        for low_first in (False, True):
            good = True
            for a, b in first:
                na, nb = nib(a), nib(b)
                byte0 = (nb << 4 | na) if low_first else (na << 4 | nb)
                if byte0 not in types:
                    good = False
                    break
            if good:
                table = [0] * 16
                for w in code:
                    table[nib(w)] = w
                sols.append((table, low_first))
    return sols


# ---------------------------------------------------------------- main
def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--proto", choices=["afhds2a", "afhds"], default="afhds2a")
    ap.add_argument("--chan", type=int, default=None, help="default: bind channel (140 / 1)")
    ap.add_argument("--packets", type=int, default=20)
    ap.add_argument("--timeout", type=float, default=60)
    ap.add_argument("--rx-gain", type=float, default=40)
    ap.add_argument("--save", help="save the bursts (.npz) to solve again later")
    ap.add_argument("--load", nargs="+", help="solve from saved bursts (one or more files) instead of capturing")
    ap.add_argument("--any-id", action="store_true", help="don't assume the FlySky ID code")
    ap.add_argument("--keep-tx", action="store_true",
                    help="self-test: leave the board's own protocol running")
    args = ap.parse_args(argv[1:])
    if args.chan is None:
        args.chan = 140 if args.proto == "afhds2a" else 1

    if args.load:
        bursts = [b for f in args.load for b in np.load(f, allow_pickle=True)["bursts"]]
    else:
        bursts = capture(args)
        if args.save:
            np.savez(args.save, bursts=np.array(bursts, dtype=object))
            print(f"  saved {len(bursts)} bursts to {args.save}")
    if not bursts:
        print("no packets")
        return 1
    streams = burst_bits(bursts, None if args.any_id else a7105.FLYSKY_ID)
    print(f"  {len(streams)} of {len(bursts)} bursts have the preamble and ID")
    nbytes = NBYTES[args.proto]
    fec = args.proto == "afhds2a"

    if fec:
        fsols = solve_fec(streams, nbytes, TYPES[args.proto])
        print(f"  {len(fsols)} FEC solutions fit the packet types")
    else:
        fsols = [(None, False)]
    results = []
    for table, low_first in fsols:
        coding = a7105.Coding(fec=table, low_first=low_first)
        pk = [coding.decode(s, nbytes + 2, fec)[0] for s in streams if len(s) >= (14 if fec else 8) * (nbytes + 2)]
        for poly, init, reflect in solve_crc(pk, nbytes):
            c = a7105.Coding(poly, init, reflect, 0, table, low_first)
            good = sum(c.crc(p[:nbytes]) == (p[nbytes] << 8 | p[nbytes + 1]) for p in pk)
            results.append((good, len(pk), c, pk))
    if not results:
        if fsols and len({bytes(p) for p in pk}) < 2:
            print("all packets are the same, so the CRC can't be solved: move the sticks while capturing")
        else:
            print("no coding found (more packets, or packets of more than one type, may help)")
        return 1
    results.sort(key=lambda r: -r[0])
    for good, n, c, pk in results[:5]:
        print(f"  CRC ok in {good}/{n} packets with {c}")
    good, n, c, pk = results[0]
    print("\npackets:")
    for p in pk[:6]:
        print("  " + " ".join(f"{b:02x}" for b in p))
    if len(results) > 1:
        print(f"\n{len(results)} codings fit; with more packet types there would be fewer")
    print("\nset it in the firmware with:")
    for cmd in c.console_cmds():
        print(f"  {cmd}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
