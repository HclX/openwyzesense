# Our sensor firmware on a v1 contact sensor wired to the LaunchPad (docs/v1-contact.md: M, C, S = RESET_N, RX, TX, +, -
# to the XDS110 side, those jumpers pulled), all from here, no hands: flash preset fw (console input during the 10 s
# boot window keeps it awake for 5 minutes) via tools/uniflash.py, check that it starts with its CCFG MAC (never
# written), answers on its console, sees no input changing by itself, and comes back from the state a debugger's run
# leaves it in by a board reset alone. It may transmit (a joined unit reports; test PAN only): opt in with OWS_HW=1,
# OWS_HW_TX=1 and OWS_HW_CONTACT=1 (the LaunchPad's own CC1310 is then off its XDS110: the other test_hw_* need it).
import os
import pathlib
import re
import sys
import tempfile
import termios
import time

import hw
import pytest

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import uniflash  # noqa: E402

APP = ROOT / 'build' / 'fw' / 'firmware' / 'sensor-contact-v1-app.bin'

pytestmark = pytest.mark.skipif(
    not (
        os.environ.get('OWS_HW') == '1'
        and os.environ.get('OWS_HW_TX') == '1'
        and os.environ.get('OWS_HW_CONTACT') == '1'
    ),
    reason='needs a v1 contact on the LaunchPad: OWS_HW=1 OWS_HW_TX=1 OWS_HW_CONTACT=1',
)


class Console:
    def __init__(self):
        self.fd = os.open(hw.uart(), os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        a = termios.tcgetattr(self.fd)
        a[0] = a[1] = 0
        a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
        a[3] = 0
        a[4] = a[5] = termios.B115200
        termios.tcsetattr(self.fd, termios.TCSANOW, a)
        termios.tcflush(self.fd, termios.TCIOFLUSH)

    def read(self, seconds):
        out, end = b'', time.time() + seconds
        while time.time() < end:
            try:
                out += os.read(self.fd, 4096)
            except BlockingIOError:
                time.sleep(0.05)
        return out.decode(errors='replace')

    def ask(self, cmd, seconds=1.5):
        os.write(self.fd, b'\n')  # the first line after a start is garbled: a clean line first
        self.read(0.3)
        os.write(self.fd, cmd.encode() + b'\n')
        return self.read(seconds)

    def close(self):
        os.close(self.fd)


def test_our_firmware_on_a_v1_contact():
    assert APP.exists(), 'build it first: cmake --preset fw && cmake --build --preset fw'
    with tempfile.TemporaryDirectory() as t:
        ccfg = uniflash.read(0x1FFA8, 22, pathlib.Path(t) / 'ccfg.bin')
    mac = ccfg[0x24:0x2C].decode(errors='replace')
    assert re.fullmatch('[0-9A-F]{8}', mac), f'no MAC in the CCFG: {ccfg[0x24:0x2C].hex()}'
    con = Console()
    try:
        uniflash.flash(APP)  # verified by a read-back, then the clean reset
        banner = con.read(4)
        assert f'on Wyze Sense v1 contact, MAC {mac}' in banner, banner
        info = con.ask('info')
        assert f'info {mac} type 01' in info, info
        assert 'input' not in con.read(3)  # nothing moves by itself (the reed switch, the button)

        # the state a debugger leaves it in (DSLite's run, the debug domain kept powered): no answer; a board reset
        # with the domain released brings it back
        uniflash.dslite(
            'flash',
            '--config',
            str(uniflash.CONFIG),
            '-s',
            'FlashEraseSetting=Necessary Sectors Only',
            '-u',
            f'{APP},0x0',
        )
        con.read(3)
        stuck = con.ask('info', 2)
        uniflash.reset()
        assert 'OpenWyzeSense sensor' in con.read(4)
        assert f'info {mac}' in con.ask('info'), 'no answer after the board reset'
        print(
            f'after a debugger run: {"stuck" if f"info {mac}" not in stuck else "still answering"}; '
            'after the board reset: answering'
        )
    finally:
        con.close()
