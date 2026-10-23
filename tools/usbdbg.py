#!/usr/bin/env python3
"""Host side of the MultiTX USB debug bridge (see docs/architecture.md).

Talks to hw/ip/usb_debug_bridge over the FX3 data endpoints of the B220
(Ettus B200 firmware, so the FPGA must already be loaded by vm_load_fpga.sh).

  usbdbg.py id                       bridge version/status + adc_capture ID
  usbdbg.py read ADDR [COUNT]        read 32-bit words
  usbdbg.py write ADDR VALUE...      write 32-bit words
  usbdbg.py halt | run               hold / release the MicroBlaze
  usbdbg.py load FILE.bin [ADDR]     halt, load a binary into code_ram, verify, run
  usbdbg.py capture N OUT.csv [rx2]  capture N RX samples to CSV (i,q)
  usbdbg.py console                  firmware console (Ctrl-C to leave)
  usbdbg.py console CMD...           run firmware shell commands, print the output
  usbdbg.py gpif-reset               ask the FX3 to reset its GPIF side (and the bridge)
"""

import struct
import sys
import time

import usb.core
import usb.util

VID, PID = 0x2500, 0x0020
EP_OUT, EP_IN = 0x02, 0x86              # FX3 data endpoints
IF_OUT, IF_IN = 1, 2
VREQ_GPIF_RESET = 0x72

MAGIC_CMD, MAGIC_RSP, MAGIC_END = 0x55534244, 0x55534252, 0x454E4421
OP_WRITE, OP_READ, OP_CTRL_WR, OP_CTRL_RD = 0x01, 0x02, 0x03, 0x04
CHUNK = 1024                            # words per command: keeps packets < 16 KB

CODE_RAM = 0x0000_0000
ADC_CAPTURE = 0x4400_0000
MAILBOX = 0x0007_F000                   # firmware console, see fw/src/console.h
MAILBOX_MAGIC = 0x434F4E53
PROMPT = b"mtx> "


class BridgeError(Exception):
    pass


def _w64(hi, lo=0):
    # a 64-bit word on the wire: high 32 bits first, each little-endian
    return struct.pack("<II", hi & 0xFFFFFFFF, lo & 0xFFFFFFFF)


def encode(cmds, seq):
    """Command packet bytes for a list of (op, flags, count, addr, data_words)."""
    body = b""
    for op, flags, count, addr, data in cmds:
        body += _w64((op << 24) | (flags << 16) | count, addr)
        if op == OP_WRITE:
            d = list(data) + [0] * (len(data) & 1)
            for i in range(0, len(d), 2):
                body += _w64(d[i], d[i + 1])
    return _w64((seq << 16) | (len(body) + 8), MAGIC_CMD) + body


def decode(cmds, seq, raw):
    """Parse a response packet into (op, err, count, addr, data_words) per command."""
    words = [struct.unpack_from("<II", raw, i) for i in range(0, len(raw) - 7, 8)]
    if not words or words[0][0] != MAGIC_RSP or (words[0][1] & 0xFFF) != seq:
        raise BridgeError(f"bad response header {words[:1]}")
    pos, out = 1, []
    for op, flags, count, addr, _ in cmds:
        data = []
        nwords = {OP_READ: (count + 1) // 2, OP_CTRL_RD: 1}.get(op, 0)
        for hi, lo in words[pos:pos + nwords]:
            data += [hi, lo]
        pos += nwords
        hi, lo = words[pos]
        pos += 1
        if (hi >> 24) != (op | 0x80):
            raise BridgeError(f"unexpected status {hi:08x} for op {op:#x}")
        out.append((op, (hi >> 16) & 0xFF, hi & 0xFFFF, lo, data[:count] if op == OP_READ else data))
    if words[pos][0] != MAGIC_END:
        raise BridgeError("missing END word")
    return out


class Bridge:
    def __init__(self, timeout_ms=5000):
        self.dev = usb.core.find(idVendor=VID, idProduct=PID)
        if self.dev is None:
            raise BridgeError("no B220 (2500:0020) found; is the USB/IP tunnel attached?")
        for intf in (IF_OUT, IF_IN):
            usb.util.claim_interface(self.dev, intf)
        self.timeout = timeout_ms
        self.seq = 0

    def gpif_reset(self):
        self.dev.ctrl_transfer(0x40, VREQ_GPIF_RESET, 0, 0, b"\0\0\0\0", self.timeout)

    # --- packet layer -------------------------------------------------------
    def transact(self, cmds):
        """cmds: list of (op, flags, count, addr, data_words). Returns a list of
        (op, err, count, addr, data_words) in the same order."""
        self.seq = (self.seq + 1) & 0xFFF
        self.dev.write(EP_OUT, encode(cmds, self.seq), self.timeout)
        return decode(cmds, self.seq, bytes(self.dev.read(EP_IN, 16384, self.timeout)))
    def _check(self, results):
        for op, err, count, addr, _ in results:
            if err:
                raise BridgeError(f"op {op:#x} at {addr:#010x}: error flags {err:#04x}")
        return results

    # --- convenience --------------------------------------------------------
    def read(self, addr, count=1, fixed=False):
        out = []
        while count:
            n = min(count, CHUNK)
            (_, _, _, _, data), = self._check(self.transact([(OP_READ, int(fixed), n, addr, None)]))
            out += data
            count -= n
            addr += 0 if fixed else 4 * n
        return out

    def write(self, addr, words, fixed=False):
        words = list(words)
        for i in range(0, len(words), CHUNK):
            part = words[i:i + CHUNK]
            self._check(self.transact([(OP_WRITE, int(fixed), len(part), addr, part)]))
            addr += 0 if fixed else 4 * len(part)

    def ctrl(self, value):
        self._check(self.transact([(OP_CTRL_WR, 0, 0, value, None)]))

    def info(self):
        (_, _, _, _, (version, status)), = self.transact([(OP_CTRL_RD, 0, 0, 0, None)])
        return version, status


def load(br, path, addr=CODE_RAM):
    blob = open(path, "rb").read()
    blob += b"\0" * (-len(blob) % 4)
    words = list(struct.unpack(f"<{len(blob) // 4}I", blob))
    br.ctrl(1)                              # hold the CPU
    t = time.time()
    br.write(addr, words)
    back = br.read(addr, len(words))
    if back != words:
        bad = next(i for i, (a, b) in enumerate(zip(words, back)) if a != b)
        raise BridgeError(f"verify failed at {addr + 4 * bad:#010x}")
    br.ctrl(0)                              # release it
    print(f"loaded {len(blob)} bytes at {addr:#010x} in {time.time() - t:.1f} s, CPU running")


def capture(br, n, path, rx2=False):
    br.write(ADC_CAPTURE + 0x08, [n])
    br.write(ADC_CAPTURE + 0x00, [1 | (int(rx2) << 1)])
    for _ in range(100):
        if br.read(ADC_CAPTURE + 0x04)[0] & 2:
            break
        time.sleep(0.05)
    else:
        raise BridgeError("capture did not finish (is the AD9361 clocking the FPGA?)")
    samples = br.read(ADC_CAPTURE + 0x10000, n)
    with open(path, "w") as f:
        f.write("i,q\n")
        for s in samples:
            i, q = struct.unpack("<hh", struct.pack("<I", s))
            f.write(f"{i},{q}\n")
    print(f"{n} samples -> {path}")


class Console:
    """Host end of the firmware console mailbox (fw/src/console.h)."""
    TX_OFF, RX_OFF = 0x40, 0x840

    def __init__(self, br):
        self.br = br
        self.session = None

    def _header(self):
        (magic, session, tx_head, tx_tail, rx_head, rx_tail,
         tx_size, rx_size, dropped, uptime) = self.br.read(MAILBOX, 10)
        if magic != MAILBOX_MAGIC:
            return None
        if session != self.session:     # firmware (re)started: take its counters
            self.session = session
            self.tx_tail, self.rx_head = tx_tail, rx_head
            self.tx_size, self.rx_size = tx_size, rx_size
        return tx_head, rx_tail

    def poll(self):
        """Bytes the firmware has written since the last call (b"" if none)."""
        hdr = self._header()
        if hdr is None:
            return b""
        head, out = hdr[0], b""
        while self.tx_tail != head:
            start = self.tx_tail % self.tx_size
            n = min((head - self.tx_tail) & 0xFFFFFFFF, self.tx_size - start)
            first = start & ~3
            nwords = (start + n - first + 3) // 4
            raw = struct.pack(f"<{nwords}I", *self.br.read(MAILBOX + self.TX_OFF + first, nwords))
            out += raw[start - first:start - first + n]
            self.tx_tail = (self.tx_tail + n) & 0xFFFFFFFF
        if out:
            self.br.write(MAILBOX + 12, [self.tx_tail])
        return out

    def send(self, text):
        # as many characters per round trip as the input ring has room for
        data = text.encode()
        while data:
            hdr = self._header()
            if hdr is None:
                raise BridgeError("firmware console not running (no mailbox magic)")
            room = self.rx_size - ((self.rx_head - hdr[1]) & 0xFFFFFFFF)
            if room <= 0:
                time.sleep(0.01)
                continue
            cmds = []
            for c in data[:room]:
                slot = MAILBOX + self.RX_OFF + 4 * (self.rx_head % self.rx_size)
                self.rx_head = (self.rx_head + 1) & 0xFFFFFFFF
                cmds.append((OP_WRITE, 0, 1, slot, [c]))
            cmds.append((OP_WRITE, 0, 1, MAILBOX + 16, [self.rx_head]))
            self.br.transact(cmds)
            data = data[room:]

    def run(self, command, timeout=10.0):
        """Send one shell command; return its output (up to the next prompt)."""
        self.poll()                     # drop the backlog
        self.send(command + "\r")
        out, t = b"", time.time()
        while not out.endswith(PROMPT):
            if time.time() - t > timeout:
                raise BridgeError(f"no prompt after {command!r}")
            data = self.poll()
            out += data
            if not data:
                time.sleep(0.005)
        text = out[:-len(PROMPT)].decode(errors="replace").replace("\r\n", "\n")
        # drop an echo of the command line (UART-style), not output that merely
        # starts with the command's name ("input console, ...")
        return text.split("\n", 1)[1] if text.startswith(command + "\n") else text


def console(br, commands):
    con = Console(br)
    if con._header() is None:
        raise BridgeError("firmware console not running (no mailbox magic)")
    if commands:
        for c in commands:
            sys.stdout.write(con.run(c))
        return
    import select
    sys.stdout.write("[console: type a command and Enter, Ctrl-C to leave]\n")
    try:
        while True:
            data = con.poll()
            if data:
                sys.stdout.write(data.decode(errors="replace").replace("\r\n", "\n"))
                sys.stdout.flush()
            if select.select([sys.stdin], [], [], 0 if data else 0.01)[0]:
                line = sys.stdin.readline()
                if not line:
                    break
                con.send(line.rstrip("\n") + "\r")
    except KeyboardInterrupt:
        print()


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 1
    num = lambda s: int(s, 0)
    br = Bridge()
    cmd = argv[1]
    if cmd == "id":
        version, status = br.info()
        print(f"bridge version {version:#010x} status {status:#010x}")
        print(f"adc_capture ID {br.read(ADC_CAPTURE + 0x0C)[0]:#010x} (expect 0x41444343)")
    elif cmd == "read":
        addr, count = num(argv[2]), num(argv[3]) if len(argv) > 3 else 1
        for i, w in enumerate(br.read(addr, count)):
            print(f"{addr + 4 * i:#010x}: {w:#010x}")
    elif cmd == "write":
        br.write(num(argv[2]), [num(v) for v in argv[3:]])
    elif cmd == "halt":
        br.ctrl(1)
    elif cmd == "run":
        br.ctrl(0)
    elif cmd == "load":
        load(br, argv[2], num(argv[3]) if len(argv) > 3 else CODE_RAM)
    elif cmd == "gpif-reset":
        br.gpif_reset()
    elif cmd == "console":
        console(br, argv[2:])
    elif cmd == "capture":
        capture(br, num(argv[2]), argv[3], len(argv) > 4 and argv[4] == "rx2")
    else:
        print(__doc__)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
