# The sensor app's inputs on the LaunchPad (unjoined image, preset fw: holding BTN-2 transmits a beacon request on
# 906.8 MHz): BTN-1 stands in for the reed switch / PIR, BTN-2 is the pairing button (a short press: the OTA window;
# held 3.04 s: pairing). The test presses them from the debugger (hw.press: the pin driven low), so nobody has to be
# there; the app, asleep between events, must wake on each edge and report each settled level once on its console.
# Opt in with OWS_HW=1 and OWS_HW_TX=1.
import os
import pathlib
import termios
import time

import hw
import pytest

ROOT = pathlib.Path(__file__).resolve().parents[2]
SENSOR = ROOT / 'build' / 'fw' / 'firmware' / 'sensor-launchpad.elf'

pytestmark = pytest.mark.skipif(
    os.environ.get('OWS_HW') != '1' or os.environ.get('OWS_HW_TX') != '1',
    reason="transmits on long-press join: set OWS_HW=1 and OWS_HW_TX=1 (with the user's permission)",
)


def open_uart():
    fd = os.open(os.environ.get('OWS_HW_UART') or hw.uart(), os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    a = termios.tcgetattr(fd)
    a[0] = a[1] = 0
    a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    a[3] = 0
    a[4] = a[5] = termios.B115200
    termios.tcsetattr(fd, termios.TCSANOW, a)
    return fd


def read(fd, seconds):
    out, end = b'', time.time() + seconds
    while time.time() < end:
        try:
            out += os.read(fd, 4096)
        except BlockingIOError:
            time.sleep(0.05)
    return out.decode(errors='replace')


def test_buttons_wake_the_sensor_and_report_once():
    assert SENSOR.exists(), 'build it first: cmake --preset fw && cmake --build --preset fw'
    hw.flash(SENSOR, erase_nv=True)
    fd = open_uart()  # after flashing: a USB reset of the probe renumbers the port
    try:
        for _ in range(3):
            hw.reset()
            banner = read(fd, 2)
            if 'OpenWyzeSense sensor' in banner:
                break
        assert 'TX path built in' in banner
        assert 'input' not in banner  # released buttons: no event at boot
        for dio, name, after in ((13, 'event', []), (14, 'button', ['button', 'short'])):
            hw.press(dio)
            assert read(fd, 1).split() == ['input', name, '1']
            hw.release(dio)
            assert read(fd, 1).split() == ['input', name, '0'] + after  # a short press of the button: OTA window
        # the button held 3.04 s starts a join while still down (and fails if no test gateway has pairing on)
        hw.press(14)
        out = read(fd, 6)
        hw.release(14)
        assert 'button long' in out and 'button short' not in out + read(fd, 1)
        assert 'join failed' in out + read(fd, 8)
    finally:
        hw.release(13)
        hw.release(14)
        os.close(fd)
