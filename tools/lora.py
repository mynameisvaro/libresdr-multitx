#!/usr/bin/env python3
"""LoRa physical layer model (SX127x-style): encoder, modulator, demodulator, decoder.

The reference for the firmware's LoRa encoder and the lora_tx modulator, and the
decoder their tests use. The coding chain (after the published reverse engineering:
M. Knight, "Decoding LoRa", GRCon 2016; P. Robyns et al., gr-lora; J. Tapparel et
al., gr-lora_sdr):

  payload -> whitening -> nibbles (low first) -> [explicit header: length, CR,
  CRC flag, checksum] -> [CRC-16 nibbles] -> Hamming (4/5..4/8) -> diagonal
  interleaver (first block: SF-2 rows at 4/8, then SF rows at the CR) -> Gray
  (inverse) mapping, +1 -> chirps after a preamble, sync word and 2.25 down-chirps

  lora.py decode FILE --fs HZ --offset HZ --sf 7 --bw 125e3 [--cr 4] [--implicit LEN]
             [--dtype cf32|cs16] [--check]     decode a capture; --check re-encodes
                                               every payload and compares the symbols
  lora.py selftest                             encode -> modulate -> decode, all SF/CR

Symbols here are chirp start positions 0..2^SF-1 at one sample per chip.
"""

import argparse
import sys

import numpy as np

# ---------------------------------------------------------------- coding


def whitening_seq(n):
    """LoRa payload whitening bytes: LFSR, next bit = b7^b5^b4^b3, from 0xFF."""
    out, w = [], 0xFF
    for _ in range(n):
        out.append(w)
        bit = (w >> 7 ^ w >> 5 ^ w >> 4 ^ w >> 3) & 1
        w = (w << 1 | bit) & 0xFF
    return out


def crc16(data):
    """CRC-16/CCITT (poly 0x1021, init 0) over all but the last two bytes, then XOR
    with those two bytes: what an SX127x appends (low byte first)."""
    crc = 0
    for b in data[:-2] if len(data) >= 2 else []:
        crc ^= b << 8
        for _ in range(8):
            crc = (crc << 1 ^ 0x1021) & 0xFFFF if crc & 0x8000 else crc << 1 & 0xFFFF
    if len(data) >= 2:
        crc ^= data[-1] ^ data[-2] << 8
    elif len(data) == 1:
        crc = data[0]
    return crc


def header_nibbles(length, cr, has_crc):
    n = [length >> 4, length & 0xF, cr << 1 | int(has_crc)]
    b = lambda v, i: v >> i & 1
    c4 = b(n[0], 3) ^ b(n[0], 2) ^ b(n[0], 1) ^ b(n[0], 0)
    c3 = b(n[0], 3) ^ b(n[1], 3) ^ b(n[1], 2) ^ b(n[1], 1) ^ b(n[2], 0)
    c2 = b(n[0], 2) ^ b(n[1], 3) ^ b(n[1], 0) ^ b(n[2], 3) ^ b(n[2], 1)
    c1 = b(n[0], 1) ^ b(n[1], 2) ^ b(n[1], 0) ^ b(n[2], 2) ^ b(n[2], 1) ^ b(n[2], 0)
    c0 = b(n[0], 0) ^ b(n[1], 1) ^ b(n[2], 3) ^ b(n[2], 2) ^ b(n[2], 1) ^ b(n[2], 0)
    return n + [c4, c3 << 3 | c2 << 2 | c1 << 1 | c0]


def hamming(nib, cr):
    """Codeword bits MSB first: d0 d1 d2 d3 then the parity bits for 4/(4+cr)."""
    d0, d1, d2, d3 = (nib >> i & 1 for i in range(4))
    if cr == 1:
        par = [d0 ^ d1 ^ d2 ^ d3]
    else:
        par = [d0 ^ d1 ^ d2, d1 ^ d2 ^ d3, d0 ^ d1 ^ d3, d0 ^ d2 ^ d3][:cr]
    return [d0, d1, d2, d3] + par


def gray_inv(g):
    v = g
    s = g >> 1
    while s:
        v ^= s
        s >>= 1
    return v


def encode(payload, sf, cr, has_crc=True, implicit=False, ldro=False, pad=None):
    """Payload bytes -> data symbols (after the preamble, sync word and SFD).
    pad: nibbles for the unused rows of the last block (default zeros; a real
    SX127x puts other values there, which receivers ignore)."""
    payload = bytes(payload)
    w = whitening_seq(len(payload))
    nibbles = []
    for b, k in zip(payload, w):
        x = b ^ k
        nibbles += [x & 0xF, x >> 4]
    if has_crc:
        c = crc16(payload)
        nibbles += [c & 0xF, c >> 4 & 0xF, c >> 8 & 0xF, c >> 12]
    if not implicit:
        nibbles = header_nibbles(len(payload), cr, has_crc) + nibbles
    symbols, pos, first = [], 0, True
    while pos < len(nibbles) or first:
        sf_app = sf - 2 if first or ldro else sf
        cw_cr = 4 if first else cr
        cw_len = 4 + cw_cr
        block = nibbles[pos:pos + sf_app]
        fill = list(pad or [])[:sf_app - len(block)]
        block += fill + [0] * (sf_app - len(block) - len(fill))
        pos += sf_app
        cws = [hamming(n, cw_cr) for n in block]
        for i in range(cw_len):
            bits = [cws[(i - j - 1) % sf_app][i] for j in range(sf_app)]
            v = 0
            for bit in bits:
                v = v << 1 | bit
            if first or ldro:                      # reduced rate: parity bit and a zero
                v = v << 2 | (sum(bits) & 1) << 1
            symbols.append((gray_inv(v) + 1) % (1 << sf))
        first = False
    return symbols


def decode_symbols(symbols, sf, cr=None, has_crc=None, implicit_len=None, ldro=False,
                   raw=False):
    """Data symbols -> (payload, header dict, crc_ok). Uses the data bits of each
    codeword only (no error correction), so it doesn't depend on the parity rules.
    raw=True also returns every nibble and codeword decoded (padding included)."""
    codewords = []
    n = 1 << sf
    nibbles, pos, first = [], 0, True
    hdr = {}
    total_nibbles = None
    while pos < len(symbols):
        sf_app = sf - 2 if first or ldro else sf
        cw_cr = 4 if first else (cr if cr else 4)
        cw_len = 4 + cw_cr
        blk = symbols[pos:pos + cw_len]
        if len(blk) < cw_len:
            break
        pos += cw_len
        rows = [[0] * cw_len for _ in range(sf_app)]
        for i, s in enumerate(blk):
            v = (s - 1) % n
            if first or ldro:
                v >>= 2
            g = v ^ v >> 1
            bits = [g >> (sf_app - 1 - j) & 1 for j in range(sf_app)]
            for j in range(sf_app):
                rows[(i - j - 1) % sf_app][i] = bits[j]
        for r in rows:
            nibbles.append(r[0] | r[1] << 1 | r[2] << 2 | r[3] << 3)
            codewords.append(r)
        if implicit_len is None and not hdr and len(nibbles) >= 3:
            cr = nibbles[2] >> 1                   # (SF5/6: the header spans two blocks)
        if implicit_len is None and not hdr and len(nibbles) >= 5:
            h = nibbles[:5]
            hdr = {"length": h[0] << 4 | h[1], "cr": h[2] >> 1, "crc": bool(h[2] & 1),
                   "checksum_ok": header_nibbles(h[0] << 4 | h[1], h[2] >> 1, h[2] & 1) == h}
            cr, has_crc = hdr["cr"], hdr["crc"]
            nibbles = nibbles[5:]
            total_nibbles = 2 * hdr["length"] + (4 if has_crc else 0)
        elif first and implicit_len is not None:
            total_nibbles = 2 * implicit_len + (4 if has_crc else 0)
        first = False
        if total_nibbles is not None and len(nibbles) >= total_nibbles:
            break
    if total_nibbles is None:
        return (b"", hdr, None, [], codewords) if raw else (b"", hdr, None)
    length = (total_nibbles - (4 if has_crc else 0)) // 2
    w = whitening_seq(length)
    payload = bytes(((nibbles[2 * i] | nibbles[2 * i + 1] << 4) ^ w[i]) & 0xFF
                    for i in range(min(length, len(nibbles) // 2)))
    crc_ok = None
    if has_crc and len(nibbles) >= total_nibbles:
        c = nibbles[2 * length:2 * length + 4]
        crc_ok = (c[0] | c[1] << 4 | c[2] << 8 | c[3] << 12) == crc16(payload)
    if raw:
        return payload, hdr, crc_ok, nibbles[total_nibbles:], codewords
    return payload, hdr, crc_ok


# ---------------------------------------------------------------- chirps


def chirp(sf, symbol=0, os=1, down=False):
    """One LoRa chirp at os samples per chip, baseband -BW/2..+BW/2 (unit power):
    the continuous-phase chirp sampled at t = k / os chips. Up-chirp for symbol s:
    f(t) = ((t + s) mod N) / N - 1/2 (in BW), wrapping at t = N - s."""
    n = 1 << sf
    t = np.arange(n * os) / os                     # in chips
    if down:
        cyc = t / 2 - t * t / (2 * n)
    else:
        s = symbol % n
        cyc = ((t + s) ** 2 - s * s) / (2 * n) - t / 2 - np.maximum(0, t - (n - s))
    return np.exp(2j * np.pi * cyc)


T_UP, T_DOWN, T_QDOWN = 0, 1, 2      # chirp types of hw/ip/lora_tx


def frame_entries(symbols, preamble=8, sync=0x12):
    """A whole frame as lora_tx entries (type, start value): preamble up-chirps,
    the two sync-word chirps, 2.25 down-chirps, the data symbols."""
    e = [(T_UP, 0)] * preamble + [(T_UP, (sync >> 4) * 8), (T_UP, (sync & 0xF) * 8)]
    e += [(T_DOWN, 0), (T_DOWN, 0), (T_QDOWN, 0)]
    return e + [(T_UP, s) for s in symbols]


def modulate(symbols, sf, os=1, preamble=8, sync=0x12, invert_iq=False):
    """Preamble up-chirps, the two sync-word chirps, 2.25 down-chirps, the data."""
    n = (1 << sf) * os
    parts = [chirp(sf, 0, os) for _ in range(preamble)]
    parts += [chirp(sf, (sync >> 4) * 8, os), chirp(sf, (sync & 0xF) * 8, os)]
    dc = chirp(sf, 0, os, down=True)
    parts += [dc, dc, dc[:n // 4]]
    parts += [chirp(sf, s, os) for s in symbols]
    x = np.concatenate(parts)
    return np.conj(x) if invert_iq else x


def _spectrum(x, sf, start, down_ref=False, pad=1):
    n = 1 << sf
    ref = np.conj(chirp(sf, 0, 1, down=down_ref))
    seg = x[start:start + n]
    if len(seg) < n:
        return None
    return np.abs(np.fft.fft(seg * ref, n * pad))


def demod_windows(x, sf, start, count, down_ref=False):
    """Peak bin of each of `count` windows from `start` (one sample per chip)."""
    out = []
    for k in range(count):
        sp = _spectrum(x, sf, start + k * (1 << sf), down_ref)
        if sp is None:
            break
        out.append(int(np.argmax(sp)))
    return out


def _peak(x, sf, start, down_ref=False, pad=16):
    """Fractional peak bin of one window (zero-padded FFT)."""
    sp = _spectrum(x, sf, start, down_ref, pad)
    return np.argmax(sp) / pad


def _wrap(v, n):
    return (v + n / 2) % n - n / 2


def _sync_after(x, sf, pos, sync):
    """From an up-chirp-aligned preamble at pos: (data_start, cfo) or None."""
    n = 1 << sf
    k_up = np.mean([_peak(x, sf, pos + i * n) for i in range(2)])
    for p in range(pos, pos + 40 * n, n):
        w = demod_windows(x, sf, p, 3)
        if len(w) < 3:
            return None
        s1, s2 = (_wrap(v - k_up, n) for v in w[:2])
        if abs(_wrap(s1 - (sync >> 4) * 8, n)) <= 1 and abs(_wrap(s2 - (sync & 0xF) * 8, n)) <= 1:
            k_down = _peak(x, sf, p + 2 * n, down_ref=True)
            sto = _wrap(k_down - k_up, n) / 2
            cfo = _wrap(k_up + sto, n)
            start = p + 2 * n + int(round(sto)) + n * 9 // 4
            return start, cfo, sto - round(sto)
    return None


def find_frames(x, sf, sync=0x12, preamble_min=5):
    """Frames in x (one sample per chip): yields (data_start, cfo_bins, frac):
    frac is the timing left after data_start (a fraction of a chip).

    A run of equal up-chirp peaks is a preamble. Its peak bin is k_up = cfo - sto
    (sto: how far the chirp boundary lies after the window start), a down-chirp's
    is k_down = cfo + sto, so the sync word's down-chirps separate the two."""
    n = 1 << sf
    pos = 0
    while pos + (preamble_min + 8) * n < len(x):
        bins = demod_windows(x, sf, pos, preamble_min)
        steady = all(abs(_wrap(b - bins[0], n)) <= 1 for b in bins)   # (a peak can straddle two bins)
        if not steady or not np.abs(x[pos:pos + n]).any():
            pos += n // 4
            continue
        pos += (-bins[0]) % n                      # roughly on the chirp boundaries
        found = _sync_after(x, sf, pos, sync)
        if found:
            yield found
            pos = found[0] + 8 * n
        else:
            pos += n


def demod_frame(x, sf, data_start, cfo_bins, nsym, frac=None):
    """Symbols of a frame, from fractional peaks. The offset left after the CFO
    correction (timing between two samples, CFO error) is measured on the
    preamble chirps before the sync word, which must peak at 0, and taken off."""
    n = 1 << sf
    t = np.arange(len(x))
    y = x * np.exp(-2j * np.pi * cfo_bins * t / n)
    pre = [data_start - n * (17 + 4 * k) // 4 for k in range(1, 5)]   # 4.25 + k symbols back
    pre = [p for p in pre if p >= 0]
    resid = 0.0
    if pre:
        pk = [_wrap(np.argmax(_spectrum(y, sf, p, pad=16)) / 16, n) for p in pre]
        resid = float(np.median(pk))
    out = []
    for k in range(nsym):
        sp = _spectrum(y, sf, data_start + k * n, pad=16)
        if sp is None:
            break
        out.append(int(round(np.argmax(sp) / 16 - resid)) % n)
    return out


# ---------------------------------------------------------------- captures


def resample(x, fs, offset, bw, os=8):
    """Shift by -offset and resample from fs to os * bw (any ratio)."""
    from fractions import Fraction
    from scipy import signal
    r = Fraction(bw * os / fs).limit_denominator(4096)
    t = np.arange(len(x))
    y = np.asarray(x, np.complex64) * np.exp(-2j * np.pi * offset * t / fs)
    return signal.resample_poly(y, r.numerator, r.denominator).astype(np.complex64)


def load(path, dtype, fs, offset, bw, os=1):
    raw = np.fromfile(path, dtype=np.complex64 if dtype == "cf32" else np.int16)
    if dtype == "cs16":
        raw = (raw[0::2] + 1j * raw[1::2]).astype(np.complex64)
    return resample(raw, fs, offset, bw, os)


def decode_iq(x, fs, bw, sf, offset=0.0, sync=0x12, cr=None, crc=None, implicit=None,
              invert_iq=False, os=8):
    """Every frame in a baseband capture: [(payload, header, crc_ok, symbols)]."""
    xo = resample(x, fs, offset, bw, os)          # off the channel first: IQ inversion
    if invert_iq:                                  # is relative to the carrier
        xo = np.conj(xo)
    out = []
    for syms in frames_os(xo, sf, os, sync):
        payload, hdr, crc_ok = decode_symbols(syms, sf, cr, crc, implicit)
        out.append((payload, hdr, crc_ok, syms))
    return out


def frames_os(xo, sf, os, sync=0x12, nsym=8 + 255 * 4, drift_per_bin=0.0):
    """Frames in an os-times oversampled capture. The sync runs at one sample per
    chip; the symbols are then taken at the full rate, from the sample nearest
    the chip boundary, with the frequency offset (refined on the preamble)
    removed. Dechirping at os samples per chip keeps the part of a symbol after
    its frequency wrap apart (bin s - n instead of s), so the two parts are added
    by magnitude and their phase step doesn't matter. drift_per_bin: symbol
    timing drift per bin of carrier offset (BW / N / carrier: one crystal sets
    both, so a long frame slips by cfo * that per symbol). Yields symbol lists."""
    n = 1 << sf
    m = n * os
    ref = np.conj(chirp(sf, 0, os))
    back = 8 * m                                    # room for two preamble chirps
    for start, cfo, frac in find_frames(xo[::os], sf, sync):
        at = int(round((start + frac) * os))       # data start, full rate
        if at < back:
            continue
        y = xo[at - back:]
        y = y * np.exp(-2j * np.pi * cfo * np.arange(len(y)) / m)
        # what is left of the offset, on preamble chirps 5.25 and 6.25 symbols back
        pk = []
        for k in (1, 2):
            p0 = back - m * (17 + 4 * k) // 4
            sp = np.abs(np.fft.fft(y[p0:p0 + m] * ref, m * 8))
            f = np.argmax(sp) / 8
            pk.append(f if f < m / 2 else f - m)
        y = y * np.exp(-2j * np.pi * float(np.mean(pk)) * np.arange(len(y)) / m)
        syms = []
        slip = -cfo * drift_per_bin               # fraction of a symbol per symbol
        for k in range(nsym):
            at_k = back + int(round(k * m * (1 + slip)))
            w = y[at_k:at_k + m]
            if len(w) < m:
                break
            sp = np.abs(np.fft.fft(w * ref))
            syms.append(int(np.argmax(sp[:n] + sp[m - n:])))   # bin s plus its wrapped part
        yield syms


def cmd_decode(a):
    os = 8
    xo = load(a.file, a.dtype, a.fs, a.offset, a.bw, os)
    sf = a.sf
    frames = 0
    bad = 0
    drift = a.bw / (1 << sf) / a.freq if a.freq else 0.0
    for syms in frames_os(xo, sf, os, a.sync, drift_per_bin=drift):
        payload, hdr, crc_ok, pad, _ = decode_symbols(syms, sf, a.cr, a.crc, a.implicit, raw=True)
        frames += 1
        line = f"frame {frames}: {payload.hex()}  {hdr}  crc {crc_ok}  padding {pad}"
        if a.check and payload:
            cr = hdr.get("cr", a.cr)
            has_crc = hdr.get("crc", a.crc)
            ref = encode(payload, sf, cr, has_crc, implicit=a.implicit is not None, pad=pad)
            got = syms[:len(ref)]
            diff = [i for i, (p, q) in enumerate(zip(ref, got)) if p != q]
            line += f"  re-encoded {len(ref)} symbols: " + ("all equal" if not diff else
                                                             f"{len(diff)} differ at {diff[:8]}")
            bad += bool(diff)
        print(line)
    print(f"{frames} frames" + (f", {bad} with symbol differences" if a.check else ""))
    return 0 if frames and not bad else 1


def cmd_selftest(_):
    rng = np.random.default_rng(1)
    fails = 0
    for sf in range(5, 13):
        for cr in range(1, 5):
            for implicit in (False, True):
                for crc in (False, True):
                    p = bytes(rng.integers(0, 256, int(rng.integers(1, 40))).astype(np.uint8))
                    syms = encode(p, sf, cr, crc, implicit)
                    x = modulate(syms, sf, preamble=8)
                    x = np.concatenate([np.zeros(300, complex), x, np.zeros(3000, complex)])
                    x = x * np.exp(2j * np.pi * 3.3 * np.arange(len(x)) / (1 << sf))   # 3.3 bins CFO
                    got = None
                    for start, cfo, frac in find_frames(x, sf):
                        rx = demod_frame(x, sf, start, cfo, len(syms), frac)
                        got = decode_symbols(rx, sf, cr if implicit else None, crc if implicit else None,
                                             len(p) if implicit else None)
                        break
                    ok = got is not None and got[0] == p and got[2] in (None, True)
                    if not ok:
                        fails += 1
                        print(f"FAIL sf{sf} cr{cr} implicit {implicit} crc {crc}: {got}")
    print("selftest", "PASS" if not fails else f"FAIL ({fails})")
    return 1 if fails else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    d = sub.add_parser("decode")
    d.add_argument("file")
    d.add_argument("--fs", type=float, required=True)
    d.add_argument("--offset", type=float, default=0, help="signal frequency minus capture centre")
    d.add_argument("--dtype", default="cf32", choices=["cf32", "cs16"])
    d.add_argument("--sf", type=int, required=True)
    d.add_argument("--bw", type=float, default=125e3)
    d.add_argument("--cr", type=int, default=None, help="1..4 (4/5..4/8); implicit header only")
    d.add_argument("--crc", type=int, default=None, help="0/1; implicit header only")
    d.add_argument("--implicit", type=int, default=None, metavar="LEN")
    d.add_argument("--sync", type=lambda v: int(v, 0), default=0x12)
    d.add_argument("--check", action="store_true", help="re-encode and compare symbols")
    d.add_argument("--freq", type=float, default=None,
                   help="carrier (Hz): corrects the symbol timing drift of long frames")
    sub.add_parser("selftest")
    a = ap.parse_args()
    return cmd_decode(a) if a.cmd == "decode" else cmd_selftest(a)


if __name__ == "__main__":
    sys.exit(main())
