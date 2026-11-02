# Transmit bring-up: the gateway (built with -DOWS_DEBUG=ON -DOWS_PAN=0x65FE -DOWS_SHORT=0x0F0F in build/fw-tx-test)
# is made
# to answer injected frames (a beacon request, a sensor report, a data request), and the RTL-SDR checks what
# it sent: beacons, ACKs (with and without frame-pending) and a B0 time sync. It TRANSMITS: opt in with
# OWS_HW=1 and OWS_HW_TX=1, only with the user's permission, and never on the house network.
import os
import pathlib
import subprocess
import sys
import threading
import time

import hw
import pytest

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from host_link import HostLink  # noqa: E402
from test_firmware import Elf  # noqa: E402
from wyzesense_ref import app, frame, host, iq  # noqa: E402

UART = os.environ.get('OWS_HW_UART') or hw.uart()
PAN, SHORT, SENSOR = 0x65FE, 0x0F0F, 0x0001
TEST = ROOT / 'build' / 'fw-tx-test' / 'firmware'
INJECT = 0x43F0

pytestmark = pytest.mark.skipif(
    os.environ.get('OWS_HW') != '1' or os.environ.get('OWS_HW_TX') != '1',
    reason="transmits: set OWS_HW=1 and OWS_HW_TX=1 (with the user's permission)",
)


def flash_test_build():
    elf = TEST / 'gateway-launchpad.elf'
    assert elf.exists(), (
        'build it first: cmake --preset fw -B build/fw-tx-test -DOWS_DEBUG=ON -DOWS_PAN=0x65FE -DOWS_SHORT=0x0F0F '
        '&& cmake --build build/fw-tx-test'
    )
    e = Elf(elf)
    assert 'phy_cmd_tx' in e.symbols
    # the identity compiled in (gw_init arguments are constants in main): make sure it is the test network
    assert subprocess.run(['grep', '-q', 'OWS_PAN:STRING=0x65FE', str(TEST.parent / 'CMakeCache.txt')]).returncode == 0
    hw.flash(elf)


def inject(link, f, rssi=-60):
    link.send(INJECT, bytes([rssi & 0xFF]) + f)


def test_gateway_transmits_like_the_stock_dongle(tmp_path):
    flash_test_build()
    time.sleep(1.5)
    link = HostLink(UART)
    link.handshake()
    now_ms = int(time.time() * 1000)
    link.send(host.TIME_SYNC, now_ms.to_bytes(8, 'big'))

    stream = tmp_path / 'stream.cu8'
    sdr = threading.Thread(
        target=subprocess.run,
        kwargs=dict(
            args=[
                'timeout',
                '25',
                'rtl_433',
                '-f',
                '906.7M',
                '-s',
                '250k',
                '-g',
                '0',
                '-w',
                str(stream),
            ],  # the board is near
            capture_output=True,
        ),
    )
    sdr.start()
    time.sleep(4)
    report = app.build_sensor_msg(0xA1, b'77FFFF01', 0x07, 0x14, 0x5E, b'\x00', 0x0001)  # a climate sensor
    for k in range(3):
        inject(link, bytes([0x03, 0x08, 0x40 + k, 0xFF, 0xFF, 0xFF, 0xFF, 0x07]))  # beacon request
        time.sleep(1.0)
        inject(link, frame.build_data(0x50 + k, PAN, SHORT, SENSOR, app.encrypt(report)))  # -> ACK
        time.sleep(1.0)
        req = (
            (0x8863).to_bytes(2, 'little')
            + bytes([0x60 + k])
            + PAN.to_bytes(2, 'little')
            + SHORT.to_bytes(2, 'little')
            + SENSOR.to_bytes(2, 'little')
            + b'\x04'
        )
        inject(link, req)  # -> ACK + B0
        time.sleep(1.5)
    sdr.join()
    link.close()

    got = [f for f in iq.decode_file(stream, 250e3, 906.7e6)]
    beacons = [f for f in got if f['crc_ok'] and f['mac']['type'] == 'beacon' and f['mac'].get('src') == SHORT]
    acks = [f for f in got if f['mac']['type'] == 'ack' and f['mac']['seq'] in (0x50, 0x51, 0x52, 0x60, 0x61, 0x62)]
    b0 = [f for f in got if f['crc_ok'] and f['mac']['type'] == 'data' and f['mac'].get('src') == SHORT]
    print(f'beacons {len(beacons)}, acks {len(acks)}, B0 {len(b0)}')
    for f in acks:
        print(
            'ack',
            f['psdu'].hex(),
            'zlib' if f['crc_ok'] else '',
            'stock-ack-fcs' if f['psdu'][3:] == frame.ack_fcs(f['psdu'][:3]) else '',
        )

    assert len(beacons) == 3
    for k, f in enumerate(beacons):  # the stock beacon, superframe 0x4FFF (pairing off)
        assert f['psdu'][:-4] == bytes([0x00, 0x80, k]) + PAN.to_bytes(2, 'little') + SHORT.to_bytes(
            2, 'little'
        ) + bytes([0xFF, 0x4F, 0, 0])
    assert len(acks) == 6
    for f in acks:  # pending on the data requests (a B0 follows), and the stock ACK FCS
        seq = f['mac']['seq']
        assert f['psdu'][:3] == frame.build_ack(seq, pending=seq >= 0x60)
        assert f['psdu'][3:] == frame.ack_fcs(f['psdu'][:3]), 'the radio did not produce the stock ACK FCS'
    assert len(b0) == 3
    for f in b0:  # starts as the stock dongle's do: 29-38 ms after the pending ACK
        ack = max((a for a in acks if a['sample'] < f['sample']), key=lambda a: a['sample'])
        gap_ms = (f['sample'] - ack['sample']) / 250
        print(f'pending ACK -> B0: {gap_ms:.1f} ms')
        assert ack['psdu'][0] == 0x12 and 29 <= gap_ms <= 38
        pt = app.parse(app.decrypt(f['mac']['payload']))
        assert pt['msg'] == app.MSG_TIME_SYNC and abs(pt['time_ms'] - time.time() * 1000) < 60_000
        assert app.decrypt(f['mac']['payload'])[9] == 0x28


def test_gateway_keeps_its_table_in_flash():
    # a sensor associates (injected frames, pairing on), the board is reset, and the sensor is still there
    flash_test_build()
    time.sleep(1.5)
    link = HostLink(UART)
    link.handshake()
    mac = b'77FFEE01'
    if mac in link.sensors():
        link.call(host.DELETE_SENSOR, mac)
    link.call(host.SET_SCAN, b'\x01')
    assoc = (
        bytes([0x23, 0xC8, 0x70])
        + PAN.to_bytes(2, 'little')
        + SHORT.to_bytes(2, 'little')
        + b'\xff\xff'
        + mac
        + b'\x01\x80'
    )
    inject(link, assoc)
    time.sleep(1.2)
    poll = bytes([0x63, 0xC8, 0x71]) + PAN.to_bytes(2, 'little') + SHORT.to_bytes(2, 'little') + mac + b'\x04'
    inject(link, poll)  # -> ACK pending + association response
    time.sleep(0.5)
    link.call(host.SET_SCAN, b'\x00')
    assert mac in link.sensors()
    link.close()
    hw.reset()
    time.sleep(1.5)
    link = HostLink(UART)
    link.handshake()
    try:
        assert mac in link.sensors()  # loaded from flash page 30 after the reset
        assert link.call(host.DELETE_SENSOR, mac)[0]['payload'] == mac + b'\xff'
    finally:
        link.close()
