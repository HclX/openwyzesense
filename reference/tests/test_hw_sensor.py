# Our sensor firmware (LaunchPad, preset fw: sensor-launchpad.elf) pairing with a stock dongle (the user's spare,
# 77A85A36 on /dev/hidraw6) as every sensor type, with this test playing wyzesense2mqtt on the dongle's host
# side, and the dongle forwarding the sensor's reports. It TRANSMITS (the sensor joins only PAN 0x65FE): opt in
# with OWS_HW=1 and OWS_HW_TX=1. Each pairing window is a few seconds; the sensor is deleted from the dongle
# afterwards. OWS_HW_SAVE=<dir> keeps the dongle's host traffic and the board's log there.
import contextlib
import json
import os
import pathlib
import sys
import termios
import threading
import time

import hw
import pytest

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from host_link import HostLink  # noqa: E402
from wyzesense_ref import app, host  # noqa: E402

UART = os.environ.get('OWS_HW_UART') or hw.uart()
DONGLE = os.environ.get('OWS_HW_DONGLE', '/dev/hidraw6')
SPARE_MAC = b'77A85A36'
TYPES = [0x0E, 0x07, 0x02, 0x0F, 0x03, 0x01]  # v2 contact, climate, v1 motion, v2 motion, leak, v1 contact
SENSOR = ROOT / 'build' / 'fw' / 'firmware' / 'sensor-launchpad.elf'

pytestmark = pytest.mark.skipif(
    os.environ.get('OWS_HW') != '1' or os.environ.get('OWS_HW_TX') != '1',
    reason="transmits: set OWS_HW=1 and OWS_HW_TX=1 (with the user's permission)",
)


class Board:
    """The sensor app's text console. A USB reset of the XDS110 (hw.usb_reset, after a JTAG hang) re-creates the
    serial device under an open file: reads and writes then fail, and the console is reopened by its stable name."""

    def __init__(self, port):
        self.port = port
        self.fd = None
        self._open()
        self.lines, self.stop = [], threading.Event()
        self.thread = threading.Thread(target=self._read, daemon=True)
        self.thread.start()

    def _open(self):
        if self.fd is not None:
            with contextlib.suppress(OSError):
                os.close(self.fd)
        for _ in range(50):  # the device comes back a few seconds after a USB reset
            try:
                self.fd = os.open(self.port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
                break
            except OSError:
                time.sleep(0.2)
        a = termios.tcgetattr(self.fd)
        a[0] = a[1] = 0
        a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
        a[3] = 0
        a[4] = a[5] = termios.B115200
        termios.tcsetattr(self.fd, termios.TCSANOW, a)

    def _read(self):
        buf = b''
        while not self.stop.is_set():
            try:
                buf += os.read(self.fd, 512)
            except BlockingIOError:
                time.sleep(0.01)
                continue
            except OSError:  # the device went away (USB reset): reopen it
                time.sleep(0.5)
                self._open()
                continue
            while b'\n' in buf:
                line, buf = buf.split(b'\n', 1)
                self.lines.append((time.time(), line.decode(errors='replace').strip()))

    def say(self, cmd):
        for _ in range(2):
            try:
                os.write(self.fd, (cmd + '\n').encode())
                break
            except OSError:
                time.sleep(0.5)
                self._open()
        time.sleep(0.2)

    def wait(self, prefix, timeout, since=0.0):
        end = time.time() + timeout
        while True:
            hit = [line for t, line in self.lines if t >= since and line.startswith(prefix)]
            if hit:
                return hit[-1]
            if time.time() >= end:
                return None
            time.sleep(0.05)

    def close(self):
        self.stop.set()
        self.thread.join()
        os.close(self.fd)


def reset_board(board):
    """Resets the board until its banner shows (the XDS110 now and then fails a JTAG scan); the banner line."""
    for _ in range(3):
        t0 = time.time()
        hw.reset()
        banner = board.wait('OpenWyzeSense sensor', 5, since=t0)
        if banner:
            return banner
    return None


def flash_sensor():
    assert SENSOR.exists(), 'build it first: cmake --preset fw && cmake --build --preset fw'
    hw.flash(SENSOR)


def test_our_sensor_pairs_with_a_stock_dongle():
    log = []
    board = Board(UART)
    flash_sensor()
    banner = board.wait('OpenWyzeSense sensor', 5) or reset_board(board)  # the reset after flashing may not take
    assert banner and 'TX path built in' in banner
    mac = banner.split('MAC ')[1][:8].encode()
    dongle = HostLink(DONGLE, log=log)
    try:
        hs = dongle.handshake()
        assert hs['mac'] == SPARE_MAC, 'not the spare dongle'
        dongle.send(host.TIME_SYNC, int(time.time() * 1000).to_bytes(8, 'big'))
        if mac in dongle.sensors():
            dongle.call(host.DELETE_SENSOR, mac)
        for typ in TYPES:  # the last with a token of our own (the dongle relays it, A4),
            last = typ == TYPES[-1]  # and joining by the pairing button
            pair_and_report(board, dongle, mac, typ, b'OpenWyzeSense-01' if last else app.R1_TOKEN, by_button=last)
        survives_a_reset(board, dongle, mac)
        the_switch_reports(board, dongle, mac)
    finally:
        dongle.call(host.SET_SCAN, b'\x00')
        dongle.call(host.DELETE_SENSOR, mac)
        board.say('leave')
        board.close()
        save = os.environ.get('OWS_HW_SAVE')
        if save:
            d = pathlib.Path(save)
            d.mkdir(parents=True, exist_ok=True)
            (d / 'dongle_host.jsonl').write_text(''.join(json.dumps(r) + '\n' for r in log))
            (d / 'launchpad_uart.log').write_text(''.join(f'{t:.3f} {line}\n' for t, line in board.lines))


BTN_EVENT, BTN_PAIR = 13, 14  # LaunchPad BTN-1 (the reed switch / PIR), BTN-2 (pairing)


def pair_and_report(board, dongle, mac, typ, token, by_button=False):
    board.say('leave')
    board.say(f'type {typ:02X}')
    t0 = time.time()
    assert dongle.call(host.SET_SCAN, b'\x01')[0]['payload'] == b''  # stock: empty reply
    try:
        if by_button:  # the pairing button, pressed from the debugger
            hw.press(BTN_PAIR)  # held: pairing starts at 3.04 s while held
            long_press = board.wait('button long', 8, since=t0)
            hw.release(BTN_PAIR)
            assert long_press
        else:
            board.say('join')
        scan = dongle.wait_for(host.SCAN_RESULT, 10)
        assert scan, f'type {typ:02x}: no scan result'
        a3 = scan[0]['payload']
        assert a3[:10] == b'\xa3' + mac + bytes([typ]) and len(a3) == 11  # A3 bytes 0-10
        r1 = dongle.call(host.GET_R1, mac + token, timeout=10)
        assert r1, f'type {typ:02x}: no R1 reply'
        a5 = r1[0]['payload']
        assert a5[:9] == b'\xa5' + mac and a5[25] == typ and len(a5) == 27  # A5 bytes 0-26
        assert a5[9:25] == token  # our sensor echoes the A4 token: the dongle sent the host's
    finally:
        assert dongle.call(host.SET_SCAN, b'\x00')[0]['payload'] == b''
    assert dongle.call(host.VERIFY_SENSOR, mac + b'\xff\x04')[0]['payload'] == b''
    # keep reading the dongle all the time, as a host does: it resends what is not ACKed at once
    pkts = []
    for _ in range(50):
        pkts += dongle.packets(0.2)
        joined = board.wait('joined', 0, since=t0)
        if joined:
            break
    assert joined and joined.split()[1] == '65FE'

    # the first reports, then an event (or a climate reading): all forwarded as 0x5319, once each
    pkts += dongle.packets(1)
    board.lines.append((time.time(), 'test: report'))
    os.write(board.fd, (('climate 2146 48' if typ == 0x07 else 'event 1') + '\n').encode())
    pkts += dongle.packets(12)
    alarms = [host.parse_alarm(m['payload']) for m in pkts if m['cmd'] == host.ALARM]
    mine = [a['plaintext'] for a in alarms if a['plaintext'][1:9] == mac]
    kinds = [pt[0] for pt in mine]
    # first reports as the stock sensors send them (v2 contact/motion: state then A1; climate: A1 then E8),
    # then the one we asked for
    want = {0x0E: [0xA2, 0xA1, 0xA2], 0x0F: [0xA2, 0xA1, 0xA2], 0x07: [0xA1, 0xE8, 0xE8]}.get(typ, [0xA1, 0xA2])
    assert kinds == want, (hex(typ), [hex(k) for k in kinds])
    for pt in mine:
        assert pt[9] == typ
    last = mine[-1]
    if typ == 0x07:
        assert last[14:17] == bytes([21, 46, 48])  # 21.46 degC, 48 %
    else:
        assert last[14] == 1  # open / active
    counters = [int.from_bytes(pt[-2:], 'big') for pt in mine]
    assert counters == sorted(counters) and counters[-1] == counters[-2] + 1  # the event advanced it


def survives_a_reset(board, dongle, mac):
    """The sensor keeps its join in flash (R2, #38: the counter is in RAM only and restarts at 0): after a reset it
    reports without pairing again."""
    t0 = time.time()
    assert reset_board(board)
    time.sleep(0.5)
    board.say('info')
    after = board.wait('info', 2, since=t0)
    assert after and ' state 4 ' in after and after.split('pan ')[1][:4] == '65FE', after
    assert int(after.split('counter ')[1]) == 0
    dongle.packets(1)  # what the dongle resent while we were not reading
    os.write(board.fd, b'event 1\n')
    pkts = dongle.packets(5)
    mine = [host.parse_alarm(m['payload'])['plaintext'] for m in pkts if m['cmd'] == host.ALARM]
    mine = [pt for pt in mine if pt[1:9] == mac]
    assert len(mine) == 1 and mine[0][0] == 0xA2 and int.from_bytes(mine[0][-2:], 'big') == 1


def the_switch_reports(board, dongle, mac):
    """BTN-1 as the reed switch: pressed (open) and released (closed), each one A2 at the dongle."""
    for pressed in (True, False):
        t0 = time.time()
        dongle.packets(0.5)
        hw.press(BTN_EVENT, pressed)
        assert board.wait(f'input event {int(pressed)}', 2, since=t0)
        pkts = dongle.packets(5)
        mine = [host.parse_alarm(m['payload'])['plaintext'] for m in pkts if m['cmd'] == host.ALARM]
        mine = [pt for pt in mine if pt[1:9] == mac]
        assert len(mine) == 1 and mine[0][0] == 0xA2 and mine[0][14] == int(pressed), [pt.hex() for pt in mine]
