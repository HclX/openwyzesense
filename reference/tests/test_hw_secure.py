# The secured radio link (#28, docs/design/security.md) on hardware: our sensor on the LaunchPad (preset fw,
# sensor-launchpad.elf) and our gateway on the spare dongle (77A85A36, gateway-dongle-app.bin through ows-flash), both
# built with -DOWS_PAN=0x65FE. This test plays the host's part that is not written yet: it pushes the network key
# (OWS_SUB_NET_KEY, RAM only) and sets GW_FLAG_ANNOUNCE, pairs the sensor, and checks that the link comes up (the
# sensor's "secure session"), that an event goes through it, and that after a gateway restart (its key gone until the
# host pushes it again) the sensor's events still arrive. It TRANSMITS (test PAN only): opt in with OWS_HW=1 and
# OWS_HW_TX=1. OWS_HW_SAVE=<dir> keeps the dongle's host traffic and the board's log there.
import json
import os
import pathlib
import sys
import time

import hw
import pytest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / 'tools'))
import sbl_common  # noqa: E402
from host_link import HostLink  # noqa: E402
from test_hw_sensor import DONGLE, SENSOR, SPARE_MAC, UART, Board, reset_board  # noqa: E402
from wyzesense_ref import app, host  # noqa: E402

pytestmark = pytest.mark.skipif(
    os.environ.get('OWS_HW') != '1' or os.environ.get('OWS_HW_TX') != '1',
    reason="transmits: set OWS_HW=1 and OWS_HW_TX=1 (with the user's permission)",
)

OWS_SUB_NET_KEY = 0x07  # firmware/common/host_proto.h
A6_OURS_SECURE = 1 << 1  # firmware/common/wyze_app.h
CONTACT = 0x01


def push_key(dongle, key):
    p = dongle.ows_call(OWS_SUB_NET_KEY, key)
    return bool(p) and p[-1] == 1


def open_dongle(log):
    """The dongle as a host sees it after a start: handshake, time, the network key pushed, GW_FLAG_ANNOUNCE on."""
    d = HostLink(DONGLE, log=log)
    hs = d.handshake()
    assert hs['mac'] == SPARE_MAC, 'not the spare dongle'
    d.send(host.TIME_SYNC, int(time.time() * 1000).to_bytes(8, 'big'))
    return d


def restart_gateway():
    """The gateway chip restarted, as at power-up (its key gone): update mode, then the ROM bootloader's reset (what
    ows-flash does after a flash, without writing anything)."""
    with open(DONGLE, 'r+b', buffering=0) as f:  # sbl_common works on a file object
        sbl_common.enter(f)
        sbl_common.sbl_cmd(f, sbl_common.CMD_RESET)
    time.sleep(2.5)


def info(board):
    t0 = time.time()
    board.say('info')
    return board.wait('info', 2, since=t0) or ''


def events_from(pkts, mac):
    alarms = [host.parse_alarm(m['payload'])['plaintext'] for m in pkts if m['cmd'] == host.ALARM]
    return [pt for pt in alarms if pt[1:9] == mac]


def pair(board, dongle, mac):
    board.say('leave')
    board.say(f'type {CONTACT:02X}')
    t0 = time.time()
    assert dongle.call(host.SET_SCAN, b'\x01')[0]['payload'] == b''
    try:
        board.say('join')
        scan = dongle.wait_for(host.SCAN_RESULT, 15)
        assert scan and scan[0]['payload'][1:9] == mac, 'no scan result'
        assert dongle.call(host.GET_R1, mac + app.R1_TOKEN, timeout=10), 'no R1 reply'
    finally:
        dongle.call(host.SET_SCAN, b'\x00')
    assert dongle.call(host.VERIFY_SENSOR, mac + b'\xff\x04')[0]['payload'] == b''
    for _ in range(60):
        dongle.packets(0.2)
        joined = board.wait('joined', 0, since=t0)
        if joined:
            assert joined.split()[1] == '65FE' and joined.endswith(', gateway ours'), joined
            return
    raise AssertionError('not joined')


def secure_session(board, dongle, seconds):
    """Waits (reading the dongle meanwhile, as a host does) until the sensor reports a secured session."""
    end = time.time() + seconds
    line = ''
    while time.time() < end:
        dongle.packets(0.8)
        line = info(board)
        if ' secure session' in line:
            return line
    return line


def event_arrives(board, dongle, mac, state, seconds):
    dongle.packets(0.5)
    os.write(board.fd, f'event {state}\n'.encode())
    end, pkts = time.time() + seconds, []
    while time.time() < end:
        pkts += dongle.packets(0.5)
        mine = [pt for pt in events_from(pkts, mac) if pt[0] == 0xA2]
        if mine:
            return mine
    return []


def test_secured_link_on_hardware():
    log = []
    board = Board(UART)
    assert SENSOR.exists(), 'build it first: cmake --preset fw -DOWS_PAN=0x65FE && cmake --build --preset fw'
    hw.flash(SENSOR)
    banner = board.wait('OpenWyzeSense sensor', 5) or reset_board(board)
    assert banner
    mac = banner.split('MAC ')[1][:8].encode()
    dongle = open_dongle(log)
    flags_before = dongle.flags()
    try:
        if mac in dongle.sensors():
            dongle.call(host.DELETE_SENSOR, mac)
        key = os.urandom(16)
        assert push_key(dongle, key), 'the gateway did not take the network key'
        on = dongle.flags(host.FLAG_ANNOUNCE)
        assert on and on[0] & host.FLAG_ANNOUNCE, on

        # pairing: the A6 says the gateway is ours and secures the link; the sensor gets its key, then a session
        pair(board, dongle, mac)
        line = secure_session(board, dongle, 20)
        assert ' secure session' in line, line
        features = int(line.split('gw ours ')[1][:4], 16)
        assert features & A6_OURS_SECURE, line

        # an event through the sealed link reaches the host as a normal report
        mine = event_arrives(board, dongle, mac, 1, 8)
        assert mine and mine[-1][14] == 1, 'no event through the secured link'

        # the gateway restarts (its key gone); the host pushes it again, as it will at every start; the sensor's
        # events still arrive (its session renewed: a resync, docs/design/security.md)
        dongle.close()
        dongle = None
        restart_gateway()
        dongle = open_dongle(log)
        assert push_key(dongle, key)
        dongle.flags(host.FLAG_ANNOUNCE)
        mine = []
        for state in (0, 1, 0):  # a few tries: the first may only renew the session
            mine = event_arrives(board, dongle, mac, state, 8)
            if mine:
                break
        assert mine, 'no event after the gateway restarted'
        assert ' secure session' in secure_session(board, dongle, 10)
    finally:
        try:
            if dongle is None:  # failed between the restart and reopening it
                dongle = open_dongle(log)
            dongle.call(host.SET_SCAN, b'\x00')
            dongle.call(host.DELETE_SENSOR, mac)
            if flags_before:
                dongle.flags(flags_before[0])
            dongle.close()
        finally:
            board.say('leave')
            board.close()
            save = os.environ.get('OWS_HW_SAVE')
            if save:
                d = pathlib.Path(save)
                d.mkdir(parents=True, exist_ok=True)
                (d / 'dongle_host.jsonl').write_text(''.join(json.dumps(r, default=str) + '\n' for r in log))
                (d / 'launchpad_uart.log').write_text(''.join(f'{t:.3f} {line}\n' for t, line in board.lines))
