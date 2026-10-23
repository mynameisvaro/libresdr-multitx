#!/usr/bin/env python3
"""ExpressLRS 4.x over-the-air model, 900 MHz (SX127x LoRa rates).

An independent implementation, written from the ExpressLRS 4.1.0 source
(fw/third_party/expresslrs: src/lib/OTA, src/lib/FHSS, src/lib/CRC,
src/src/common.cpp, src/src/tx_main.cpp, src/src/rx_main.cpp), used to check the
firmware (which builds ExpressLRS's own OTA/FHSS/CRC code) and to decode what the
board sends:

  - the UID from a binding phrase, the CRC seeds and the OTA version
  - the FHSS sequence and channel frequencies per regulatory domain
  - the packet layouts: SYNC, RC (Wide / Hybrid switches, 8/12/16-channel full res)
  - the TX's slot rules: nonce, hop, telemetry and sync slots (Tx)
  - parse(): CRC check (finding the nonce of RC packets) and the fields

  elrs.py uid PHRASE                        the 6-byte UID ELRS derives from it
  elrs.py fhss PHRASE [--domain FCC915]     the hop sequence and frequencies
  elrs.py sim PHRASE [--rate 0] [--n 40]    the first packets a TX sends

ExpressLRS 3.x is not compatible with 4.x over the air (different CRC seed,
channel bits, rate numbers), and isn't modelled.
"""

import argparse
import hashlib
import sys

OTA_VERSION_ID = 4

# ---------------------------------------------------------------- rates
# src/src/common.cpp, the RADIO_SX127X tables (ExpressLRS_AirRateConfig and
# ExpressLRS_AirRateRFperf). cr is the LoRa coding rate 4/(4+cr).

# expresslrs_RFrates_e (src/include/common.h)
RATE_ENUM = dict(LORA_900_25HZ=0, LORA_900_50HZ=1, LORA_900_100HZ=2, LORA_900_100HZ_8CH=3,
                 LORA_900_150HZ=4, LORA_900_200HZ=5, LORA_900_200HZ_8CH=6, LORA_900_250HZ=7,
                 LORA_900_333HZ_8CH=8, LORA_900_500HZ=9, LORA_900_50HZ_DVDA=10)

# expresslrs_tlm_ratio_e
TLM_STD, TLM_NO_TLM, TLM_1_128, TLM_1_64, TLM_1_32, TLM_1_16, TLM_1_8, TLM_1_4, TLM_1_2 = range(9)


def tlm_denom(e):
    """TLMratioEnumToValue"""
    return 1 if e == TLM_NO_TLM else 1 << (8 + TLM_NO_TLM - e)


class Rate:
    def __init__(self, index, name, enum, sf, cr, preamble, tlm, hop, interval, size, sends,
                 toa, sync_disc_ms, sync_conn_ms):
        self.__dict__.update(locals())
        del self.__dict__["self"]
        self.bw = 500e3


RATES = [
    Rate(0, "200Hz", RATE_ENUM["LORA_900_200HZ"], 6, 3, 8, TLM_1_64, 4, 5000, 8, 1, 4380, 600, 5000),
    Rate(1, "100Hz Full", RATE_ENUM["LORA_900_100HZ_8CH"], 6, 4, 8, TLM_1_32, 4, 10000, 13, 1, 6690, 600, 5000),
    Rate(2, "100Hz", RATE_ENUM["LORA_900_100HZ"], 7, 3, 8, TLM_1_32, 4, 10000, 8, 1, 8770, 600, 5000),
    Rate(3, "50Hz", RATE_ENUM["LORA_900_50HZ"], 8, 3, 10, TLM_1_16, 4, 20000, 8, 1, 18560, 600, 5000),
    Rate(4, "25Hz", RATE_ENUM["LORA_900_25HZ"], 9, 3, 10, TLM_1_8, 2, 40000, 8, 1, 29950, 600, 5000),
    Rate(5, "D50", RATE_ENUM["LORA_900_50HZ_DVDA"], 6, 3, 8, TLM_1_64, 2, 5000, 8, 4, 4380, 600, 5000),
]

# LoRa framing for every 900 MHz rate (src/lib/SX127xDriver): sync word 0x12,
# implicit header, LoRa CRC off, IQ standard (900 MHz never inverts IQ)
LORA_SYNC = 0x12

# ---------------------------------------------------------------- domains
# src/lib/FHSS/FHSS.cpp; frequencies in SX127x register units (FREQ_STEP)
FREQ_STEP = 61.03515625          # 32 MHz / 2^19
FREQ_SPREAD_SCALE = 256
FHSS_SEQUENCE_LEN = 256


def freq_reg(hz):
    return int(hz / FREQ_STEP)


DOMAINS = {
    "AU915": (915500000, 926900000, 20, 921000000),
    "FCC915": (903500000, 926900000, 40, 915000000),
    "EU868": (863275000, 869575000, 13, 868000000),
    "IN866": (865375000, 866950000, 4, 866000000),
    "AU433": (433420000, 434420000, 3, 434000000),
    "EU433": (433100000, 434450000, 3, 434000000),
    "US433": (433250000, 438000000, 8, 434000000),
    "US433W": (423500000, 438000000, 20, 434000000),
}


class Domain:
    def __init__(self, name):
        start, stop, count, center = DOMAINS[name]
        self.name = name
        self.start = freq_reg(start)
        self.stop = freq_reg(stop)
        self.count = count
        self.center = center
        self.sync_channel = count // 2
        self.spread = (self.stop - self.start) * FREQ_SPREAD_SCALE // (count - 1)
        self.seq_count = (FHSS_SEQUENCE_LEN // count) * count

    def channel_hz(self, ch):
        """frequency of channel ch (FHSSgetNextFreq, FreqCorrection 0), in Hz"""
        return (self.start + self.spread * ch // FREQ_SPREAD_SCALE) * FREQ_STEP


# ---------------------------------------------------------------- UID, seeds


def uid_from_phrase(phrase):
    """python/build_flags.py: MD5 of the -D flag, first 6 bytes"""
    return bytes(hashlib.md5(f'-DMY_BINDING_PHRASE="{phrase}"'.encode()).digest()[:6])


def crc_init(uid):
    """OtaUpdateCrcInitFromUid"""
    return (((uid[4] << 8) | uid[5]) ^ (OTA_VERSION_ID << 8)) & 0xFFFF


def uid_seed(uid):
    """OtaGetUidSeed"""
    return ((uid[2] << 24) + (uid[3] << 16) + (uid[4] << 8) + (uid[5] ^ OTA_VERSION_ID)) & 0xFFFFFFFF


# ---------------------------------------------------------------- CRC (Crc2Byte)
CRC14_POLY = 0x2E57
CRC16_POLY = 0x3D65


def crc_calc(data, bits, poly, crc):
    """Crc2Byte::calc, bitwise: MSB-first, `bits` wide, crc is the initial value"""
    mask = (1 << bits) - 1
    top = 1 << (bits - 1)
    for b in data:
        # the table version: crc = (crc << 8) ^ tab[((crc >> (bits - 8)) ^ b) & 0xFF]
        idx = ((crc >> (bits - 8)) ^ b) & 0xFF
        t = idx << (bits - 8)
        for _ in range(8):
            t = (t << 1) ^ (poly if t & top else 0)
        crc = ((crc << 8) ^ t) & 0xFFFF   # uint16_t arithmetic
    return crc & mask


# ---------------------------------------------------------------- FHSS


class Rng:
    """src/lib/FHSS/random.cpp"""

    def __init__(self, seed):
        self.seed = seed & 0xFFFFFFFF

    def rng(self):
        self.seed = (214013 * self.seed + 2531011) % 2147483648
        return self.seed >> 16

    def n(self, mx):
        return self.rng() % mx


def fhss_sequence(seed, dom):
    """FHSSrandomiseFHSSsequenceBuild"""
    n, sync = dom.count, dom.sync_channel
    seq = []
    for i in range(dom.seq_count):
        if i % n == 0:
            seq.append(sync)
        elif i % n == sync:
            seq.append(0)
        else:
            seq.append(i % n)
    r = Rng(seed)
    for i in range(dom.seq_count):
        if i % n != 0:
            offset = (i // n) * n
            k = r.n(n - 1) + 1
            seq[i], seq[offset + k] = seq[offset + k], seq[i]
    return seq


# ---------------------------------------------------------------- channels (crsf_protocol.h)
CRSF_MIN, CRSF_1000, CRSF_MID, CRSF_2000, CRSF_MAX = 172, 191, 992, 1792, 1811


def fmap(x, in_min, in_max, out_min, out_max):
    num = (x - in_min) * (out_max - out_min) * 2
    q = abs(num) // (in_max - in_min) * (1 if num >= 0 else -1)   # C division truncates
    r = (q + out_min * 2 + 1)
    r = abs(r) // 2 * (1 if r >= 0 else -1)
    return max(0, min(65535, r))


def crsf_to_uint10(v):
    return fmap(v, CRSF_MIN, CRSF_MAX, 0, 1023)


def uint10_to_crsf(v):
    return fmap(v, 0, 1023, CRSF_MIN, CRSF_MAX)


def crsf_to_n(v, cnt):
    if v <= CRSF_1000:
        return 0
    if v >= CRSF_2000:
        return cnt - 1
    return (v - CRSF_1000) * cnt // (CRSF_2000 - CRSF_1000 + 1)


def n_to_crsf(v, mx):
    return v * (CRSF_2000 - CRSF_1000) // mx + CRSF_1000


def crsf_to_switch3b(v):
    size = (CRSF_MAX - CRSF_MIN) // 6
    if v < CRSF_MID - size // 4 or v > CRSF_MID + size // 4:
        return crsf_to_n(v, 6)
    return 7


def switch3b_to_crsf(v):
    return {0: CRSF_1000, 5: CRSF_2000, 6: CRSF_MID, 7: CRSF_MID}.get(v, v * 240 + 391)


def crsf_to_bit(v):
    return 1 if v > CRSF_MID else 0


def multi_to_crsf(m):
    """Multiprotocol channel value (204..1844 = 988..2012 us) -> CRSF (172..1811), as
    ExpressLRS maps its PPM input (fmap over the same microsecond end points)"""
    return fmap(m, 204, 1844, CRSF_MIN, CRSF_MAX)


def pack_4x10(vals):
    """PackUInt11ToChannels4x10 after decimation: 4 x 10 bits, little-endian bit order"""
    acc = 0
    for i, v in enumerate(vals):
        acc |= (v & 0x3FF) << (10 * i)
    return acc.to_bytes(5, "little")


def unpack_4x10(b):
    acc = int.from_bytes(b[:5], "little")
    return [(acc >> (10 * i)) & 0x3FF for i in range(4)]


# ---------------------------------------------------------------- packets
TYPE_RCDATA, TYPE_DATA, TYPE_SYNC = 0b00, 0b01, 0b10
SM_WIDE_OR_8CH, SM_HYBRID_OR_16CH, SM_12CH = 0, 1, 2
MODELMATCH_MASK = 0x3F


def wide_switch_index(nonce):
    """HybridWideNonceToSwitchIndex"""
    return ((nonce & 0b111) + ((nonce >> 3) & 0b1)) % 8


def sync_bytes(fhss_index, nonce, rate, switch_mode, tlm_enum, uid, model_id=None, gemini=0, ota_protocol=0):
    """OTA_Sync_s (6 bytes); model_id None = model match off"""
    uid5 = uid[5]
    if model_id is not None:
        uid5 ^= (~model_id) & MODELMATCH_MASK
    flags = (switch_mode & 1) | ((tlm_enum - TLM_NO_TLM) & 7) << 1 | (gemini & 1) << 4 | (ota_protocol & 3) << 5
    return bytes([fhss_index & 0xFF, nonce & 0xFF, rate.enum, flags, uid[4], uid5 & 0xFF])


def finish(body, nonce, uid, full):
    """add the CRC: GeneratePacketCrcStd / GeneratePacketCrcFull"""
    body = bytearray(body)
    v = 0 if (body[0] & 3) == TYPE_SYNC else nonce
    init = crc_init(uid) ^ v
    if full:
        c = crc_calc(body[:11], 16, CRC16_POLY, init)
        return bytes(body[:11]) + bytes([c & 0xFF, c >> 8])
    body[0] &= 3                        # crcHigh zero while calculating
    c = crc_calc(body[:7], 14, CRC14_POLY, init)
    body[0] |= (c >> 8) << 2
    return bytes(body[:7]) + bytes([c & 0xFF])


class RcPacker:
    """OtaPackChannelData for the TX: keeps the round-robin state of Hybrid8 and
    the full-res high/low aux alternation"""

    def __init__(self, full, switch_mode):
        self.full, self.mode = full, switch_mode
        self.hybrid_next = 0
        self.high_aux = False

    def pack(self, ch, nonce, tx_power=1, stubborn_ack=0):
        """ch: 16 CRSF values. Returns the packet without CRC (7 or 11 bytes)."""
        armed = crsf_to_bit(ch[4])
        if not self.full:
            chans = pack_4x10([crsf_to_uint10(max(CRSF_MIN, min(CRSF_MAX, v))) for v in ch[:4]])
            if self.mode == SM_WIDE_OR_8CH:
                idx = wide_switch_index(nonce)
                val = stubborn_ack << 6
                val |= tx_power if idx == 7 else crsf_to_n(ch[idx + 1 + 4], 64) & 0x3F
            else:
                idx = self.hybrid_next
                value = crsf_to_n(ch[6 + 1 + 4], 16) if idx == 6 else crsf_to_switch3b(ch[idx + 1 + 4])
                val = stubborn_ack << 6 | idx << 3 | value
                self.hybrid_next = (idx + 1) % 7
            return bytes([TYPE_RCDATA]) + chans + bytes([(val & 0x7F) | armed << 7])
        if self.mode == SM_WIDE_OR_8CH:
            high = False
        else:
            high = self.high_aux
            self.high_aux = not self.high_aux
        if self.mode == SM_HYBRID_OR_16CH:
            lo, hi = (8, 12) if high else (0, 4)
        else:
            lo, hi = 0, (8 if high else 4)
        b0 = TYPE_RCDATA | (stubborn_ack & 1) << 2 | (max(1, min(8, tx_power)) - 1) << 3 | int(high) << 6 | armed << 7
        return (bytes([b0]) + pack_4x10([v >> 1 for v in ch[lo:lo + 4]])
                + pack_4x10([v >> 1 for v in ch[hi:hi + 4]]))


def sync_packet(sync6, full):
    if full:
        return bytes([TYPE_SYNC]) + sync6 + bytes(4)
    return bytes([TYPE_SYNC]) + sync6


# ---------------------------------------------------------------- TX slot rules


class Tx:
    """The transmitter's timing rules (tx_main.cpp: timerCallback, SendRCdataToRF,
    TXdoneISR, GenerateSyncPacketData) for a TX that never hears telemetry (so it
    stays "disconnected" and sends a sync every SyncPktIntervalDisconnected), no
    sync spam, no MSP data, not binding. tick() is one timer period."""

    def __init__(self, uid, rate, domain="FCC915", switch_mode=SM_WIDE_OR_8CH, tlm=TLM_STD,
                 model_id=None, tx_power=1, t0_us=0):
        self.uid, self.rate = uid, rate
        self.dom = Domain(domain)
        self.seq = fhss_sequence(uid_seed(uid), self.dom)
        self.full = rate.size == 13
        if not self.full and switch_mode > SM_HYBRID_OR_16CH:
            switch_mode = SM_WIDE_OR_8CH    # adjustSwitchModeForAirRate
        self.mode = switch_mode
        self.tlm_enum = rate.tlm if tlm == TLM_STD else tlm
        self.denom = tlm_denom(self.tlm_enum)
        self.model_id, self.tx_power = model_id, tx_power
        self.packer = RcPacker(self.full, switch_mode)
        self.nonce = 0
        self.ptr = 0
        self.ch = self.dom.sync_channel        # FHSSgetInitialFreq
        self.sync_slot = 0
        # tx_main.cpp starts with SyncPacketLastSent = 0 and millis() well past it,
        # so the first sync goes out on the first chance; the firmware does the same
        self.sync_last_ms = t0_us // 1000 - rate.sync_disc_ms - 1
        self.tlm_gap = False
        self.t_us = t0_us

    def freq_hz(self):
        return self.dom.channel_hz(self.ch)

    def on_sync_channel(self):
        return self.seq[self.ptr] == self.dom.sync_channel

    def tick(self, channels):
        """Returns dict(nonce, ptr, freq, packet or None (telemetry slot), type)."""
        now_ms = self.t_us // 1000
        self.t_us += self.rate.interval
        self.nonce = (self.nonce + 1) & 0xFF
        if self.tlm_gap:                    # TelemetryRcvPhase == ttrpPreReceiveGap
            self.tlm_gap = False
            return dict(nonce=self.nonce, ptr=self.ptr, freq=self.freq_hz(), packet=None, type="tlm")
        r = self.rate
        hop_res = self.nonce % r.hop
        if ((self.sync_slot // 2) <= hop_res and now_ms - self.sync_last_ms > r.sync_disc_ms
                and self.on_sync_channel()):
            self.sync_last_ms = now_ms
            s6 = sync_bytes(self.ptr, self.nonce, r, self.mode, self.tlm_enum, self.uid, self.model_id)
            body = sync_packet(s6, self.full)
            self.sync_slot = (self.sync_slot + 1) % (r.hop * 2)
            kind = "sync"
        else:
            body = self.packer.pack(channels, self.nonce, self.tx_power)
            kind = "rc"
        pkt = finish(body, self.nonce, self.uid, self.full)
        out = dict(nonce=self.nonce, ptr=self.ptr, freq=self.freq_hz(), packet=pkt, type=kind)
        # TXdoneISR
        if (self.nonce + 1) % r.hop == 0:
            self.ptr = (self.ptr + 1) % self.dom.seq_count
            self.ch = self.seq[self.ptr]
        if self.denom != 1 and (self.nonce + 1) % self.denom == 0:
            self.tlm_gap = True
        return out


# ---------------------------------------------------------------- receive side


def parse(pkt, uid, nonce=None):
    """Check a received packet's CRC and decode it. For RC packets the CRC seed
    includes the nonce: with nonce None every value is tried. Returns a dict, or
    None when no CRC matches."""
    full = len(pkt) == 13
    ptype = pkt[0] & 3
    if ptype == TYPE_SYNC:
        cands = [0]
    else:
        cands = range(256) if nonce is None else [nonce]
    ok_nonces = []
    for n in cands:
        if finish(pkt[:11] if full else pkt[:7], n, uid, full) == bytes(pkt):
            ok_nonces.append(n)
    if not ok_nonces:
        return None
    d = dict(type={0: "rc", 1: "data", 2: "sync", 3: "?"}[ptype], full=full)
    if ptype != TYPE_SYNC:
        d["nonces"] = ok_nonces
    if ptype == TYPE_SYNC:
        s = pkt[1:7]
        d.update(fhss_index=s[0], nonce=s[1], rate_enum=s[2], switch_mode=s[3] & 1,
                 tlm_enum=((s[3] >> 1) & 7) + TLM_NO_TLM, gemini=(s[3] >> 4) & 1,
                 ota_protocol=(s[3] >> 5) & 3, uid4=s[4], uid5=s[5])
    elif ptype == TYPE_RCDATA and not full:
        d["ch10"] = unpack_4x10(pkt[1:6])
        d["switches"] = pkt[6] & 0x7F
        d["armed"] = pkt[6] >> 7
        d["ch"] = [uint10_to_crsf(v) for v in d["ch10"]]
    elif ptype == TYPE_RCDATA:
        d["armed"] = pkt[0] >> 7
        d["high_aux"] = (pkt[0] >> 6) & 1
        d["uplink_power"] = ((pkt[0] >> 3) & 7) + 1
        d["low11"] = [v << 1 for v in unpack_4x10(pkt[1:6])]
        d["high11"] = [v << 1 for v in unpack_4x10(pkt[6:11])]
    return d


def toa_us(rate, payload_len=None):
    """SX127x time on air (implicit header, no CRC, LDRO off), in microseconds"""
    sf, n = rate.sf, payload_len or rate.size
    tsym = (1 << sf) / rate.bw
    import math
    pl = 8 + max(math.ceil((8 * n - 4 * sf + 28 - 20) / (4 * sf)) * (rate.cr + 4), 0)
    return (rate.preamble + 4.25 + pl) * tsym * 1e6


# ---------------------------------------------------------------- command line


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("uid")
    p.add_argument("phrase")
    p = sub.add_parser("fhss")
    p.add_argument("phrase")
    p.add_argument("--domain", default="FCC915", choices=sorted(DOMAINS))
    p = sub.add_parser("sim")
    p.add_argument("phrase")
    p.add_argument("--rate", type=int, default=0, choices=range(len(RATES)))
    p.add_argument("--domain", default="FCC915", choices=sorted(DOMAINS))
    p.add_argument("--n", type=int, default=40)
    a = ap.parse_args()

    uid = uid_from_phrase(a.phrase)
    if a.cmd == "uid":
        print(",".join(str(b) for b in uid), f"({uid.hex()})")
    elif a.cmd == "fhss":
        dom = Domain(a.domain)
        seq = fhss_sequence(uid_seed(uid), dom)
        print(f"{dom.name}: {dom.count} channels, sync channel {dom.sync_channel}, {dom.seq_count} hops")
        for i in range(0, dom.seq_count, 20):
            print(" ".join(f"{c:2d}" for c in seq[i:i + 20]))
        print("channel MHz:", " ".join(f"{dom.channel_hz(c) / 1e6:.3f}" for c in range(dom.count)))
    else:
        r = RATES[a.rate]
        tx = Tx(uid, r, a.domain, t0_us=600_000)
        ch = [CRSF_MID] * 16
        for _ in range(a.n):
            o = tx.tick(ch)
            pkt = o["packet"].hex() if o["packet"] else "-- telemetry slot --"
            print(f"nonce {o['nonce']:3d} ptr {o['ptr']:3d} {o['freq'] / 1e6:9.4f} MHz {o['type']:4s} {pkt}")


if __name__ == "__main__":
    sys.exit(main())
