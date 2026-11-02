#!/usr/bin/env python3
"""The gateway image a relay carries for its contact board (#76), written to OUT padded to whole words.

    gateway_image.py OUT

It is build/fw/firmware/gateway-contact-v1-app.bin, built first (cmake preset fw: the ARM toolchain from
tools/toolchain.sh), or the file named in OWS_GATEWAY_IMAGE (CI hands over the one its firmware job built). Both relay
builds use it: esp8266/embed.py, idf/main/CMakeLists.txt.
"""

import os
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]


def image() -> bytes:
    given = os.environ.get('OWS_GATEWAY_IMAGE')
    if given:
        return pathlib.Path(given).read_bytes()
    if not (ROOT / 'build' / 'fw' / 'CMakeCache.txt').exists():
        subprocess.run(['cmake', '--preset', 'fw'], cwd=ROOT, check=True, stdout=subprocess.DEVNULL)
    subprocess.run(
        ['cmake', '--build', '--preset', 'fw', '--target', 'gateway-contact-v1.elf'],
        cwd=ROOT,
        check=True,
        stdout=subprocess.DEVNULL,
    )
    return (ROOT / 'build' / 'fw' / 'firmware' / 'gateway-contact-v1-app.bin').read_bytes()


def main():
    out = pathlib.Path(sys.argv[1])
    data = image()
    data += b'\xff' * (-len(data) % 4)  # the bootloader writes words
    if not out.exists() or out.read_bytes() != data:
        out.write_bytes(data)
    return 0


if __name__ == '__main__':
    sys.exit(main())
