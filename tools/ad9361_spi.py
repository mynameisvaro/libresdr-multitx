#!/usr/bin/env python3
"""AD9361 register access from the host, through the USB bridge and the
AXI Quad SPI (ad9361_spi @ 0x44A0_0000). For bring-up only; the firmware
will own the AD9361 later.

  ad9361_spi.py reset            pulse RESETB (gpio_ctrl bit 0)
  ad9361_spi.py read REG [N]     read N consecutive registers
  ad9361_spi.py write REG VALUE
  ad9361_spi.py id               reset if needed, read the product ID (0x037)
"""

import sys
import time

from usbdbg import Bridge

SPI = 0x44A0_0000
SRR, SPICR, SPISR, DTR, DRR, SSR = 0x40, 0x60, 0x64, 0x68, 0x6C, 0x70
GPIO_CTRL = 0x4001_0000

# master, SPI enabled, CPHA = 1 (AD9361 samples on the falling edge), manual SS
CR_RUN = 0x02 | 0x04 | 0x10 | 0x80
CR_INHIBIT = 0x100
CR_FIFO_RESET = 0x20 | 0x40


class AD9361:
    def __init__(self, br):
        self.br = br
        br.write(SPI + SRR, [0x0A])
        br.write(SPI + SPICR, [CR_RUN | CR_INHIBIT | CR_FIFO_RESET])
        br.write(SPI + SSR, [0xFFFFFFFF])

    def reset(self):
        ctrl = self.br.read(GPIO_CTRL)[0]
        self.br.write(GPIO_CTRL, [ctrl & ~1])
        time.sleep(0.01)
        self.br.write(GPIO_CTRL, [ctrl | 1])
        time.sleep(0.01)

    def _xfer(self, tx):
        br = self.br
        br.write(SPI + SPICR, [CR_RUN | CR_INHIBIT | CR_FIFO_RESET])
        br.write(SPI + DTR, tx, fixed=True)
        br.write(SPI + SSR, [0xFFFFFFFE])
        br.write(SPI + SPICR, [CR_RUN])                  # start shifting
        while not br.read(SPI + SPISR)[0] & 0x04:        # TX FIFO empty
            pass
        br.write(SPI + SSR, [0xFFFFFFFF])
        br.write(SPI + SPICR, [CR_RUN | CR_INHIBIT])
        return [b & 0xFF for b in br.read(SPI + DRR, len(tx), fixed=True)]

    # 16-bit instruction: [15] write, [14:12] byte count - 1, [9:0] address
    def read(self, reg, n=1):
        out = []
        for r in range(reg, reg + n):
            out.append(self._xfer([(r >> 8) & 0x03, r & 0xFF, 0x00])[2])
        return out

    def write(self, reg, value):
        self._xfer([0x80 | ((reg >> 8) & 0x03), reg & 0xFF, value & 0xFF])


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 1
    num = lambda s: int(s, 0)
    ad = AD9361(Bridge())
    cmd = argv[1]
    if cmd == "reset":
        ad.reset()
    elif cmd == "read":
        reg, n = num(argv[2]), num(argv[3]) if len(argv) > 3 else 1
        for i, v in enumerate(ad.read(reg, n)):
            print(f"0x{reg + i:03x}: 0x{v:02x}")
    elif cmd == "write":
        ad.write(num(argv[2]), num(argv[3]))
    elif cmd == "id":
        if not ad.br.read(GPIO_CTRL)[0] & 1:
            ad.reset()
        pid = ad.read(0x037)[0]
        ok = (pid & 0xF8) == 0x08
        print(f"product ID 0x{pid:02x}: {'AD9361 (rev %d)' % (pid & 7) if ok else 'NOT an AD9361 response'}")
        return 0 if ok else 2
    else:
        print(__doc__)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
