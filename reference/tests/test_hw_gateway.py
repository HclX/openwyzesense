# Hardware-in-the-loop for the gateway app: the receive-only passive mirror build
# (`cmake --preset fw -B build/fw-mirror -DOWS_PASSIVE=ON -DOWS_PAN=0x845B -DOWS_SHORT=0x3366`: the house
# dongle's PAN and short address, no TX path) on the LaunchPad, driven over its UART by a host that does what
# wyzesense2mqtt does, while the house bridge's state is read alongside (GET /api/dongles, read-only).
# Opt in with OWS_HW=1; OWS_HW_SECONDS sets the listening time (default 600). Nothing here transmits.
import json
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
from wyzesense_ref import app, host  # noqa: E402

UART = os.environ.get('OWS_HW_UART') or hw.uart()
SECONDS = int(os.environ.get('OWS_HW_SECONDS', '600'))
BRIDGE = os.environ.get('OWS_BRIDGE', '').rstrip('/') + '/api/dongles'  # the house bridge: its URL, not in the repo
MIRROR = ROOT / 'build' / 'fw-mirror' / 'firmware'

pytestmark = [
    pytest.mark.skipif(os.environ.get('OWS_HW') != '1', reason='hardware tests: set OWS_HW=1'),
    pytest.mark.skipif(not os.environ.get('OWS_BRIDGE'), reason="needs the house bridge's URL: OWS_BRIDGE=<base URL>"),
]


def flash_mirror():
    elf = MIRROR / 'gateway-launchpad.elf'
    assert elf.exists(), (
        'build it first: cmake --preset fw -B build/fw-mirror -DOWS_PASSIVE=ON -DOWS_PAN=0x845B -DOWS_SHORT=0x3366 '
        '&& cmake --build build/fw-mirror'
    )
    e = Elf(elf)
    assert 'radio_tx' not in e.symbols and 'phy_cmd_tx' not in e.symbols  # house identity: never TX
    hw.flash(elf, erase_nv=True)  # start with an empty sensor table


def watch_bridge(changes, stop):
    """Sensor state changes on the house bridge, as (time, sensor) — polled, read-only."""
    last = {}
    while not stop.is_set():
        r = subprocess.run(['curl', '-s', '--max-time', '10', BRIDGE], capture_output=True, text=True)
        try:
            snap = {s['mac']: s for d in json.loads(r.stdout)['dongles'] for s in d['sensors']}
        except (ValueError, KeyError):
            snap = None
        if snap:
            for mac, s in snap.items():
                if last and (
                    mac not in last or (last[mac]['last_seen'], last[mac]['state']) != (s['last_seen'], s['state'])
                ):
                    changes.append((time.time(), s))
            last = snap
        stop.wait(2)


def test_gateway_speaks_the_stock_host_protocol():
    flash_mirror()
    time.sleep(1.5)
    listen_like_wyzesense2mqtt(HostLink(UART))


def listen_like_wyzesense2mqtt(link, mac=None):
    """The stock handshake (MAC: mac if given), an empty table, a time sync; then SECONDS of alarms, checked against
    the house bridge. Shared with test_hw_contact_gateway.py."""
    hs = link.handshake()
    assert hs['inquiry'] == b'\x01' and len(hs['enr']) == 16 and hs['auth'] == b''
    assert len(hs['mac']) == 8 and all(chr(c) in '0123456789ABCDEF' for c in hs['mac'])
    if mac:
        assert hs['mac'] == mac, hs['mac']
    assert hs['version'] == b'0.0.0.47 V1.8 Gateway GW3U'
    assert link.sensors() == []  # nothing paired (page 30 erased)
    link.send(host.TIME_SYNC, int(time.time() * 1000).to_bytes(8, 'big'))

    changes, stop = [], threading.Event()
    watcher = threading.Thread(target=watch_bridge, args=(changes, stop))
    watcher.start()
    pkts = link.packets(SECONDS)
    stop.set()
    watcher.join()
    link.close()

    alarms = [(m['t'], host.parse_alarm(m['payload'])) for m in pkts if m['cmd'] == host.ALARM]
    if not alarms:
        pytest.skip(f'no house traffic in {SECONDS} s')
    agree = 0
    for t, a in alarms:
        pt = a['plaintext']
        assert pt[0] in (0xA1, 0xA2, 0xE8, 0xAB) and all(chr(c) in '0123456789ABCDEF' for c in pt[1:9])
        assert abs(a['time_ms'] / 1000 - t) < 3  # the gateway's clock runs from our time sync
        assert 30 <= a['rssi'] <= 110  # -dBm
        if pt[0] == 0xAB:
            continue
        p = app.parse(pt + bytes(-len(pt) % 16))
        for tc, s in changes:  # the same report, as the house dongle saw it
            if s['mac'].encode() != p['mac'] or not 0 <= tc - t <= 25:
                continue
            st = s['state']
            if p['msg'] == 0xA2 and ('is_active' in st or 'is_open' in st):
                assert p['active'] == st.get('is_active', st.get('is_open'))
                agree += 1
            if p['msg'] == 0xE8 and 'temperature' in st:
                assert abs(p['temperature_c'] - st['temperature']) < 0.006 and p['humidity_pct'] == st['humidity']
                agree += 1
    print(f'{len(alarms)} alarms, {len(changes)} bridge changes, {agree} states checked against the bridge')
