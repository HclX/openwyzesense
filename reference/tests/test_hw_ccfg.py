# The CCFG restore command (HOST_OWS_CCFG, firmware/common/gateway.h) on real flash: the LaunchPad's ROM bootloader
# backdoor is switched off over JTAG (one CCFG byte cleared: the "bootloader pin misconfigured" case), our gateway
# firmware is asked to put its built-in CCFG back, and page 31 must then be what it was, byte for byte. Only on the
# LaunchPad, where JTAG can always reflash a chip whatever its CCFG. Opt in with OWS_HW=1 (preset fw).
import os
import pathlib
import re
import sys
import tempfile
import time

import hw
import pytest

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from host_link import HostLink  # noqa: E402

GATEWAY = ROOT / 'build' / 'fw' / 'firmware' / 'gateway-launchpad.elf'
pytestmark = pytest.mark.skipif(os.environ.get('OWS_HW') != '1', reason='needs the LaunchPad: set OWS_HW=1')


def jtag(*cmds):
    for _ in range(3):
        out = hw.openocd('init', *cmds, 'shutdown')
        if 'all zeroes' not in out and 'BYPASS' not in out:
            return out
        hw.usb_reset()
    raise RuntimeError(out[-1000:])


def words(out, addr):
    """The words OpenOCD's mdw printed from addr on (one line)."""
    return [int(w, 16) for w in re.search(rf'0x{addr:08x}: ((?:[0-9a-f]{{8}} ?)+)', out).group(1).split()]


def word(out, addr):
    return words(out, addr)[0]


def test_ccfg_restore_revives_a_disabled_bootloader_backdoor():
    assert GATEWAY.exists(), 'build it first: cmake --preset fw && cmake --build --preset fw'
    hw.flash(GATEWAY)
    tmp = pathlib.Path(tempfile.mkdtemp(prefix='ows-ccfg-'))
    try:
        out = jtag('mdw 0x500012f0 2', 'mdw 0x1ffd8 1', f'dump_image {tmp}/before.bin 0x1f000 0x1000')
        lo, hi = words(out, 0x500012F0)
        ieee = hi << 32 | lo
        bl = word(out, 0x1FFD8)
        assert bl >> 24 == 0xC5 and bl & 0xFF == 0xC5  # bootloader + backdoor on
        out = jtag('reset halt', 'flash fillb 0x1ffd8 0x00 1', 'mdw 0x1ffd8 1', 'reset run')
        assert word(out, 0x1FFD8) == bl & ~0xFF  # backdoor off
        time.sleep(1.5)
        link = HostLink(os.environ.get('OWS_HW_UART') or hw.uart())
        try:
            link.handshake()
            assert link.ccfg_restore(ieee ^ 1) == 'refused'  # another chip's address
            assert link.ccfg_restore(ieee) == 'ok'
            assert link.ccfg_restore(ieee) == 'already right'
            assert link.call(0x5316)[0]['payload'] == b'0.0.0.47 V1.8 Gateway GW3U'  # still running
        finally:
            link.close()
        out = jtag('mdw 0x1ffd8 1', f'dump_image {tmp}/after.bin 0x1f000 0x1000')
        assert word(out, 0x1FFD8) == bl
        assert (tmp / 'after.bin').read_bytes() == (tmp / 'before.bin').read_bytes()
    finally:
        hw.flash(GATEWAY)  # whatever happened: a full reflash, CCFG included
