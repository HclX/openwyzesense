# Hardware-in-the-loop: the LaunchPad (receive-only build) against the RTL-SDR, both listening to the house
# traffic. Opt in with OWS_HW=1 (needs the LaunchPad on /dev/ttyACM0, the SDR, rtl_433, and a built image);
# OWS_HW_SECONDS sets the listening time (default 180). Nothing here transmits.
import os
import pathlib
import re
import subprocess
import sys
import threading
import time

import hw
import pytest
from wyzesense_ref import app, frame, iq

ROOT = pathlib.Path(__file__).resolve().parents[2]
UART = os.environ.get('OWS_HW_UART') or hw.uart()
SECONDS = int(os.environ.get('OWS_HW_SECONDS', '180'))

pytestmark = pytest.mark.skipif(os.environ.get('OWS_HW') != '1', reason='hardware tests: set OWS_HW=1')


def flash_receive_only():
    elf = ROOT / 'build' / 'fw' / 'firmware' / 'sniffer-launchpad.elf'
    assert elf.exists(), 'build the firmware first: cmake --build --preset fw'
    # never flash an image that could transmit (test_firmware.py checks the same on every run)
    r = subprocess.run(
        [sys.executable, '-m', 'pytest', '-q', str(ROOT / 'reference/tests/test_firmware.py'), '-k', 'cannot_transmit'],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    assert r.returncode == 0, r.stdout
    hw.flash(elf)


def read_uart(lines, stop):
    import termios

    fd = os.open(UART, os.O_RDONLY | os.O_NOCTTY | os.O_NONBLOCK)
    attrs = termios.tcgetattr(fd)
    attrs[4] = attrs[5] = termios.B115200
    attrs[3] &= ~(termios.ICANON | termios.ECHO)
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    buf = b''
    while not stop.is_set():
        try:
            buf += os.read(fd, 512)
        except BlockingIOError:
            time.sleep(0.01)
            continue
        while b'\n' in buf:
            line, buf = buf.split(b'\n', 1)
            lines.append((time.time(), line.decode(errors='replace').strip()))
    os.close(fd)


def test_launchpad_receives_what_the_sdr_receives(tmp_path):
    lines, stop = [], threading.Event()
    reader = threading.Thread(target=read_uart, args=(lines, stop))
    reader.start()  # before the reset, to see the banner
    flash_receive_only()
    time.sleep(2)
    stream = tmp_path / 'stream.cu8'
    subprocess.run(
        ['timeout', str(SECONDS), 'rtl_433', '-f', '906.7M', '-s', '250k', '-g', '35', '-w', str(stream)],
        capture_output=True,
    )
    stop.set()
    reader.join()

    assert any('RX on 906.8 MHz' in line for _, line in lines), lines[:5]
    assert any('AES ok' in line for _, line in lines)
    frame_line = re.compile(r'^RX (-?\d+) (\d+) ([0-9a-f]+)$')  # rssi, radio time, MAC frame
    board = {bytes.fromhex(m.group(3)) for m in (frame_line.match(line) for _, line in lines) if m}
    sdr = {f['psdu'][:-4] for f in iq.decode_file(stream, 250e3, 906.7e6) if f['crc_ok']}
    sdr |= {
        f['psdu'][:3]
        for f in iq.decode_file(stream, 250e3, 906.7e6)
        if len(f['psdu']) == 7 and frame.parse_mac(f['psdu'][:3])['type'] == 'ack'
    }
    if not sdr:
        pytest.skip(f'no Wyze traffic in {SECONDS} s')
    both = sdr & board
    # the SDR's front end is noisier near the band edge than the CC1310; allow a few misses either way
    assert len(both) >= 0.8 * len(sdr), (len(both), len(sdr), len(board))
    for f in board:  # what the board hands up decodes
        m = frame.parse_mac(f)
        if m['type'] == 'data' and m['payload']:
            pt = app.decrypt(m['payload'])
            assert pt[0] in (0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xAB, 0xB0, 0xE8), f.hex()
