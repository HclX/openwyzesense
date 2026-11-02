#!/usr/bin/env python3
"""TI's CC13x0 ROM serial bootloader on a plain UART: a v1 contact's RX/TX pads (DIO1/DIO0 on the 5x5 package), once
its CCFG enables the bootloader (issue #14: tools/uniflash.py enable-bootloader w|button).

    uv run python tools/uart_sbl.py [--port PORT] check
    uv run python tools/uart_sbl.py [--port PORT] flash BOOT.bin SLOT_A.img --yes [--mac 777B42DF]
    uv run python tools/uart_sbl.py [--port PORT] flash APP.bin --yes [--mac ...]

PORT: default the LaunchPad's UART (reference/tests/hw.uart()). Enter the bootloader first: hold the backdoor (the W
pad to GND, or the button) while the chip resets (power, S, or tools/uniflash.py reset), then release it; the ROM only
looks at it at the reset. Both commands leave the chip in the bootloader: then a pin reset (S: tools/uniflash.py reset,
or an ESP32's line) or a power cycle starts the new image. The bootloader's own reset (CMD_RESET, a system reset) left
the chip in the ROM, running nothing, on 777B42DF (2026-10-24), as a software system reset did on the LaunchPad
(docs/FACTS.md §5). Connecting again to a bootloader already synced works (the sync's first byte is then taken as a
packet size: zeros fill that packet up, then a ping).

`flash` writes pages 0-29 only, through tools/sbl_common.py's write path (as ows-flash on the dongle: every erase and
download checked against page 30 twice, the chip's CRC32 of what was written, pages 30-31 checked unchanged), over the
UART instead of the dongle's HID: the packets are the same, the dongle's CH554 only wraps them in HID reports. Before
writing it reads page 31 and checks that the CCFG there holds a MAC (the --mac one, if given) and the bootloader
enabled; then resets the chip into the new image. Two files: the boot selector and a slot image, laid out as
tools/uniflash.py slots does (the OTA layout); one file: an app image at 0x0.
"""

import argparse
import binascii
import os
import pathlib
import sys
import termios
import time

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import nv_json  # noqa: E402
import sbl_common as sbl  # noqa: E402
import uniflash  # noqa: E402

GAP = float(os.environ.get('OWS_UART_GAP', 0.02))  # a reply is complete once the line has been quiet this long
# (tests on a pty, where nothing is slow, set it lower)


class UartAsHid:
    """The file object sbl_common expects (read(size) -> one HID report: a length byte, then what the chip sent;
    write(packet)), over a UART. A read waits up to 5 s for the first byte (sbl_common's own timeout), then takes what
    follows until the line is quiet."""

    def __init__(self, port):
        self.fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        a = termios.tcgetattr(self.fd)
        a[0] = a[1] = 0
        a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
        a[3] = 0
        a[4] = a[5] = termios.B115200
        termios.tcsetattr(self.fd, termios.TCSANOW, a)
        termios.tcflush(self.fd, termios.TCIOFLUSH)
        self.pending = b''
        self.timeout = 5  # for the first byte of a reply (an erase can take a while)

    def _take(self):
        try:
            self.pending += os.read(self.fd, 4096)
            return True
        except BlockingIOError:
            return False

    def read(self, size):
        end = time.time() + self.timeout
        while not self.pending:
            if time.time() > end:
                raise TimeoutError('read timeout')
            if not self._take():
                time.sleep(0.002)
        quiet = time.time()
        while time.time() - quiet < GAP:
            if self._take():
                quiet = time.time()
            else:
                time.sleep(0.002)
        n = min(len(self.pending), size - 1)
        out, self.pending = self.pending[:n], self.pending[n:]
        return bytes([n]) + out

    def write(self, data):
        os.write(self.fd, data)
        termios.tcdrain(self.fd)

    def close(self):
        os.close(self.fd)


SOFT_SBL = bytes.fromhex('aa555308f0054f53424c037f') + b'\r\nsbl\r\n'


def _try_sync(u, timeout):
    u.timeout = timeout
    os.write(u.fd, b'\x55\x55')
    try:
        r = u.read(0x3F)[1:].lstrip(b'\0')
    except TimeoutError:
        r = b''
    if r[:1] == b'\xcc':
        assert sbl.sbl_cmd(u, sbl.CMD_PING), 'no answer to ping'
        return 'fresh'
    # no ACK: either no bootloader yet, or one already synced (an earlier run of this tool): then the first 0x55 was
    # taken as the size of an 85-byte packet; zeros fill it up (its checksum fails: a NAK) and, between packets,
    # are skipped; then a ping tells
    os.write(u.fd, b'\0' * 100)
    time.sleep(0.1)
    u.pending = b''
    termios.tcflush(u.fd, termios.TCIFLUSH)
    try:
        if sbl.sbl_cmd(u, sbl.CMD_PING):
            return 'synced'
    except (TimeoutError, AssertionError):
        pass
    return None


def enter(u):
    """Auto-baud, ping, chip ID; falls back to software SBL entry (OWS_SUB_ENTER_SBL + sensor 'sbl') when running
    OWS firmware is on the UART."""
    state = _try_sync(u, 0.3)
    if state is None:
        u.write(SOFT_SBL)
        time.sleep(0.05)
        u.pending = b''
        termios.tcflush(u.fd, termios.TCIFLUSH)
        state = _try_sync(u, 0.3)
    if state is None:
        raise SystemExit(
            'no bootloader: no ACK to the auto-baud sync or a ping (backdoor held at reset? bootloader enabled?)'
        )
    if state == 'synced':
        print('bootloader already synced')
    chip_id = sbl.sbl_cmd(u, sbl.CMD_GET_CHIP_ID)
    u.timeout = 5
    return chip_id


def check_page31(page31, mac):
    ccfg = page31[uniflash.CCFG_ADDR - uniflash.PAGE31 :]
    have = ccfg[0x24:0x2C]
    if len(have) != 8 or not set(have) <= set(b'0123456789ABCDEF') or have == b'00000000':
        raise SystemExit(f'no MAC in the CCFG ({have.hex()}): not writing')
    if mac and have != mac.encode():
        raise SystemExit(f'the CCFG MAC is {have.decode()}, not {mac}: not writing')
    bl = ccfg[uniflash.BL_CONFIG : uniflash.BL_CONFIG + 4]
    bl_word = int.from_bytes(bl, 'little')
    assert len(bl) == 4 and bl[0] == 0xC5 and bl[3] == 0xC5 and bl[1] < 32 and (bl[2] & 1) == 0, (
        f'BL_CONFIG {bl_word:#010x}: bootloader not enabled?'
    )
    return have.decode(), bl[1]


def main():
    sys.path.insert(0, str(ROOT / 'reference' / 'tests'))
    import hw

    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--port')
    sub = ap.add_subparsers(dest='cmd', required=True)
    sub.add_parser('check')
    f = sub.add_parser('flash')
    f.add_argument('files', nargs='+', type=pathlib.Path, help='BOOT.bin SLOT_A.img, or APP.bin')
    f.add_argument('--yes', action='store_true')
    f.add_argument('--mac', help='write only if the CCFG holds this MAC')
    f.add_argument(
        '--slow', action='store_true', help='status after every packet and a read-back (as ows-flash does by default)'
    )
    f.add_argument('--nv', default='keep', help='page 30 (#61): keep (default), wipe, migrate, or an NV.json to import')
    a = ap.parse_args()
    u = UartAsHid(a.port or hw.uart())
    chip = enter(u)
    print('bootloader: ACK; chip ID', chip.hex() if chip else '?')
    page31 = sbl.read_mem(u, uniflash.PAGE31, sbl.PAGE)
    mac, pin = check_page31(page31, getattr(a, 'mac', None))
    print(f'CCFG: MAC {mac}, backdoor DIO{pin}')
    if a.cmd == 'check':
        # left in the bootloader: flash can follow. (Its own reset, CMD_RESET, with the backdoor still held came back to
        # a bootloader that answered nothing, not even a sync, until a pin reset: 777B42DF, 2026-10-24.)
        return
    if len(a.files) == 2:
        image = uniflash.slots_image(*a.files)
    elif len(a.files) == 1:
        image = a.files[0].read_bytes()
    else:
        ap.error('BOOT.bin SLOT_A.img, or APP.bin')
    image = sbl.check_image(image)
    print(f'flash: {len(image)} bytes to pages 0-{(len(image) - 1) // sbl.PAGE}, CRC {binascii.crc32(image):#010x}')
    if not a.yes:
        print('not written: add --yes')
        return
    # write_pages checks page 31 against a backup; here the one just read (its CCFG checked above)
    sbl.write_pages(
        u,
        image,
        {},
        b'\xff' * uniflash.PAGE31 + page31,
        fast=not a.slow,
        entered=True,
        reset=False,
        nv=lambda current: nv_json.nv_page_for(a.nv, current),
    )
    print("written; now reset it with S (tools/uniflash.py reset) or a power cycle (not the bootloader's own reset)")


if __name__ == '__main__':
    main()
