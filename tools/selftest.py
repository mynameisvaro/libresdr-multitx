#!/usr/bin/env python3
"""Hardware self-test for the MultiTX phase 1 bitstream.

Checks, in order: USB bridge, code_ram write/readback, GPIO reset values,
axi_ad9361 core registers, timer, and the AD9361 product ID over SPI.

  selftest.py           quick memory test (16 KB)
  selftest.py --full    whole code_ram (512 KB, ~1 min over USB/IP)

Note: the memory test overwrites code_ram (holds the CPU in reset while it runs),
and the AD9361 test releases the AD9361 from reset.
"""

import random
import sys
import time

from ad9361_spi import AD9361
from usbdbg import Bridge

GPIO_FE, GPIO_CTRL, GPIO_DAC = 0x4000_0000, 0x4001_0000, 0x4002_0000
TIMER, AD9361_CORE, ADC_CAPTURE = 0x41C0_0000, 0x7902_0000, 0x4400_0000

results = []


def check(name, ok, detail):
    results.append(ok)
    print(f"[{'PASS' if ok else 'FAIL'}] {name:<22} {detail}")


def main(argv):
    full = "--full" in argv
    br = Bridge()
    rd = lambda a: br.read(a)[0]

    version, status = br.info()
    check("usb bridge", version == 0x0001_0000, f"version {version:#010x}, status {status:#010x}")
    adcc = rd(ADC_CAPTURE + 0x0C)
    check("adc_capture id", adcc == 0x4144_4343, f"{adcc:#010x}")

    nwords = 131072 if full else 4096
    words = [random.getrandbits(32) for _ in range(nwords)]
    br.ctrl(1)
    t = time.time()
    br.write(0x0, words)
    back = br.read(0x0, nwords)
    bad = sum(a != b for a, b in zip(words, back))
    check("code_ram", bad == 0, f"{nwords * 4 // 1024} KB, {bad} mismatches, {time.time() - t:.1f} s")
    br.write(0x0, [0] * nwords)             # leave zeros (NOPs) behind, CPU stays halted

    fe = (rd(GPIO_FE), rd(GPIO_FE + 8))
    check("gpio_fe reset", fe == (0x48, 0x48), f"{fe[0]:#04x} {fe[1]:#04x}")
    dac = rd(GPIO_DAC)
    check("gpio_dac reset", dac == 0x7FFF, f"{dac:#06x}")

    v_adc, v_dac = rd(AD9361_CORE), rd(AD9361_CORE + 0x4000)
    br.write(AD9361_CORE + 0x08, [0xA5A5_5A5A])
    scratch = rd(AD9361_CORE + 0x08)
    check("axi_ad9361", scratch == 0xA5A5_5A5A and v_adc >> 16 == 10,
          f"ADC core {v_adc >> 16}.{(v_adc >> 8) & 0xFF:02d}, DAC core {v_dac >> 16}.{(v_dac >> 8) & 0xFF:02d}, scratch {scratch:#010x}")

    br.write(TIMER, [0x80])
    a, b = rd(TIMER + 8), rd(TIMER + 8)
    br.write(TIMER, [0])
    check("axi_timer", a != b, f"{a:#x} -> {b:#x}")

    ad = AD9361(br)
    ad.reset()
    pid = ad.read(0x037)[0]
    check("AD9361 SPI", (pid & 0xF8) == 0x08, f"product ID {pid:#04x} (rev {pid & 7})")

    print(f"\n{sum(results)}/{len(results)} passed")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
