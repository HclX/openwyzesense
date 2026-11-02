# The two-slot boot (docs/ota.md) on the LaunchPad: the boot selector in page 0, the sensor app in slot A (version N)
# and slot B (N + 1), unjoined images (preset fw: an unjoined sensor never transmits). The newer slot runs on probation
# and uses up an attempt per boot; after three without confirming, the selector goes back to A; an image that runs 60 s
# confirms itself; a slot with a bad byte is never chosen. Header words read over JTAG. Opt in with OWS_HW=1.
import contextlib
import os
import pathlib
import re
import struct
import subprocess
import sys
import tempfile
import termios
import time

import hw
import pytest

ROOT = pathlib.Path(__file__).resolve().parents[2]
B = ROOT / 'build' / 'fw' / 'firmware'
SLOT_A, SLOT_B = 0x01000, 0x0F000
pytestmark = pytest.mark.skipif(os.environ.get('OWS_HW') != '1', reason='needs the LaunchPad: set OWS_HW=1')


def jtag(*cmds):
    for _ in range(3):
        out = hw.openocd('init', *cmds, 'shutdown')
        if 'all zeroes' not in out and 'BYPASS' not in out:
            return out
        hw.usb_reset()
    raise RuntimeError(out[-1000:])


def header_words(base):
    """confirmed, attempts[3] of a slot (header offset 64)"""
    out = jtag(f'mdw {base + 64:#x} 4')
    return [int(w, 16) for w in re.search(rf'0x{base + 64:08x}: ((?:[0-9a-f]{{8}} ?)+)', out).group(1).split()]


MARK = 0x20000C08  # a .bss word of the sensor app (uart.c rx_tail): zeroed by every start-up


def marker():
    """The marker word, read until a JTAG session really returns it (a failed one must not look like "gone")."""
    for _ in range(5):
        m = re.search(rf'0x{MARK:08x}: ([0-9a-f]{{8}})', jtag(f'mdw {MARK:#x} 1'))
        if m:
            return int(m.group(1), 16)
        time.sleep(1)
    raise RuntimeError('cannot read the marker over JTAG')


def open_uart():
    fd = os.open(hw.uart(), os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    a = termios.tcgetattr(fd)
    a[0] = a[1] = 0
    a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    a[3] = 0
    a[4] = a[5] = termios.B115200
    termios.tcsetattr(fd, termios.TCSANOW, a)
    termios.tcflush(fd, termios.TCIOFLUSH)
    return fd


def running_slot():
    """The slot the app says it runs from (its "info" reply), asked until a reply gets through; after three tries
    the XDS110's serial side is taken as hung and the probe reset on USB (no target reset). '' if never."""
    for i in range(6):
        if i == 3:
            hw.usb_reset()
        fd = open_uart()
        try:
            os.write(fd, b'info\n')
            out, end = b'', time.time() + 1
            while time.time() < end:
                try:
                    out += os.read(fd, 512)
                except BlockingIOError:
                    time.sleep(0.05)
        finally:
            os.close(fd)
        for line in out.decode(errors='replace').splitlines():
            if line.startswith('info ') and ' slot ' in line:
                return 'slot ' + line.split(' slot ')[1][:1]
    return ''


def boot_banner():
    """Resets the board; returns the sensor app's banner line. The harness is flaky in two ways: OpenOCD's reset now
    and then does not take (CLAUDE.md), and the XDS110's serial side now and then loses the banner. A RAM marker
    tells them apart: still there, the reset did not happen (reset again: no boot was counted; a banner then is a late
    copy of an earlier one); gone but no banner, the boot happened (no second one: it would use another attempt), and
    the slot is asked of the app instead ("info")."""
    fd = open_uart()
    try:
        for _ in range(4):
            jtag('halt', f'mww {MARK:#x} 0x5a5a5a5a', 'reset run')
            out, end = b'', time.time() + 3
            while time.time() < end:
                try:
                    out += os.read(fd, 512)
                except BlockingIOError:
                    time.sleep(0.05)
            if marker() == 0x5A5A5A5A:
                continue  # no reset (a banner now would be a late one)
            lines = [line for line in out.decode(errors='replace').splitlines() if 'OpenWyzeSense sensor' in line]
            return lines[-1] if lines else f'(banner lost) {running_slot()}'
        return ''
    finally:
        with contextlib.suppress(OSError):  # the probe was reset on USB meanwhile (running_slot)
            os.close(fd)


def program_b(img):
    for _ in range(3):
        if 'Programming Finished' in jtag('reset halt', f'program {img} {SLOT_B:#x}'):
            return
        time.sleep(2)
    raise RuntimeError(f'programming {img} failed')


def test_two_slot_boot():
    for f in ('boot-launchxl_cc1310.bin', 'sensor-launchpad-slot-a.img', 'sensor-launchpad-slot-b.raw'):
        assert (B / f).exists(), f'build it first: cmake --build --preset fw ({f})'
    tmp = pathlib.Path(tempfile.mkdtemp(prefix='ows-boot-'))
    version = struct.unpack_from('<I', (B / 'sensor-launchpad-slot-a.img').read_bytes(), 8)[0]
    newer = tmp / 'slot-b-newer.img'
    subprocess.run(
        [
            sys.executable,
            str(ROOT / 'tools' / 'slot_image.py'),
            str(B / 'sensor-launchpad-slot-b.raw'),
            str(newer),
            '--board-name',
            'LAUNCHXL-CC1310',
            '--version',
            str(version + 1),
        ],
        check=True,
    )
    for _ in range(3):  # OpenOCD now and then fails a session (CLAUDE.md)
        out = jtag(
            'reset halt',
            f'program {B / "boot-launchxl_cc1310.bin"} 0x0',
            f'program {B / "sensor-launchpad-slot-a.img"} {SLOT_A:#x}',
            f'program {newer} {SLOT_B:#x}',
        )
        if out.count('Programming Finished') == 3:
            break
        time.sleep(2)
    assert out.count('Programming Finished') == 3, out[-800:]

    # B, the newer, on probation: each boot uses an attempt, then A
    for i in range(3):
        assert 'slot B' in boot_banner(), f'boot {i + 1}'
        assert header_words(SLOT_B) == [0xFFFFFFFF] + [0] * (i + 1) + [0xFFFFFFFF] * (2 - i)
    assert 'slot A' in boot_banner()
    assert 'slot A' in boot_banner()  # B stays dropped

    # A (also unconfirmed: a wired install) confirms itself after 60 s
    assert header_words(SLOT_A)[0] == 0xFFFFFFFF
    time.sleep(65)
    assert header_words(SLOT_A)[0] == 0
    assert 'slot A' in boot_banner()

    # a fresh, newer B, with one byte wrong: never chosen
    bad = bytearray(newer.read_bytes())
    bad[0x2000] ^= 0x01
    (tmp / 'slot-b-bad.img').write_bytes(bad)
    program_b(tmp / 'slot-b-bad.img')
    assert 'slot A' in boot_banner()
    # and the good one again: B
    program_b(newer)
    assert 'slot B' in boot_banner()
