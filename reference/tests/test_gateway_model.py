# Replays the captured house traffic (PAN 0x845B) through the gateway model: every frame a sensor sent goes into
# Gateway.rx(), and what the model sends must equal, byte for byte, what the stock dongle sent next.
# For the joins, a scripted host does what wyzesense2mqtt does when pairing (FACTS.md §9).
# The same replay, written out as vectors/gateway_replay.txt, drives the C gateway's native test.
import json
import os
import pathlib

import pytest
from wyzesense_ref import app, frame, gateway, host

RF = pathlib.Path(__file__).resolve().parents[2] / 'ground_truth' / 'rf'
REPLAY_VECTORS = pathlib.Path(__file__).parent / 'vectors' / 'gateway_replay.txt'
PAN, SHORT, EXT, MAC = 0x845B, 0x3366, 0x00124B001E9FF933, b'778B5F63'
# only the two short re-pair sessions are published; the longer recordings of everyday house traffic are not
# (ground_truth/README.md)
STREAMS = [
    '2026-10-22_1821_906.7M_250k_g35',
    '2026-10-22_1823_906.7M_250k_g35',
]
JOINS = {'2026-10-22_1821_906.7M_250k_g35', '2026-10-22_1823_906.7M_250k_g35'}


def frames(session):
    lines = (RF / session / 'decoded_frames.jsonl').read_text().splitlines()
    rows = sorted((json.loads(line) for line in lines), key=lambda r: r['t'])
    out = []
    for r in rows:
        b = bytes.fromhex(r['psdu'])
        if len(b) < 7:
            continue
        m = frame.parse_mac(b[:-4])
        if m['type'] != 'ack' and not r['crc_ok']:
            continue
        out.append((r['t'], b, m))
    return out


def from_dongle(m, prev):
    if m['type'] == 'ack':  # the dongle's ACK follows a sensor frame with the same seq
        return prev is not None and not prev[1] and prev[0]['seq'] == m['seq'] and prev[0].get('ack_req')
    if m['type'] == 'beacon':
        return m.get('src') == SHORT and m.get('src_pan') == PAN
    return m.get('src') in (SHORT, EXT) and m.get('dst_pan') == PAN


def to_dongle(m):
    return (m['type'] == 'cmd' and m.get('cmd') == 'beacon-req') or (m.get('dst_pan') == PAN and m.get('dst') == SHORT)


def known_sensors():
    """MAC -> short address of the house sensors, from their reports (the dongle's table before the joins)."""
    t = {}
    for s in STREAMS:
        for _, _b, m in frames(s):
            if m['type'] == 'data' and to_dongle(m) and m['src'] != SHORT:
                pt = app.decrypt(m['payload'])
                if pt[0] in app.SENSOR_MSGS and pt[1:9] != bytes(8):
                    t.setdefault(pt[1:9].decode(), m['src'])
    del t['77C68193']  # moved here from the user's second dongle at 18:28:15: new to this dongle
    return t


class PairingHost:
    """What wyzesense2mqtt does on the host side while pairing (scan on, R1 token, verify)."""

    def __init__(self, gw):
        self.gw = gw

    def handle(self, packets):
        tx = []
        for p in packets:
            m = host.parse(p)
            if m['cmd'] == host.SCAN_RESULT:
                tx += self.gw.host(host.build(host.GET_R1, m['payload'][1:9] + app.R1_TOKEN))[0]
            elif m['cmd'] == host.GET_R1 + 1:
                tx += self.gw.host(host.build(host.VERIFY_SENSOR, m['payload'][1:9] + b'\xff\x04'))[0]
        return tx


def set_clock_from(gw, expected):
    # the dongle clock is the one thing the model cannot know: take it from the B0 / A6 the dongle sent
    for b in expected:
        m = frame.parse_mac(b[:-4])
        if m['type'] != 'data':
            continue
        pt = app.decrypt(m['payload'])
        if pt[0] == app.MSG_TIME_SYNC:
            gw.clock_ms = int.from_bytes(pt[3:9], 'big')
        if pt[0] == app.MSG_JOIN_DONE:
            gw.clock_ms = int.from_bytes(pt[5:11], 'big')


def replay(session, log=None):
    """Replays a session through the model. With log (a list), appends the replay as vector lines:
    SESSION name permit dsn bsn / SENSOR mac short / CLOCK ms / DSN n / RX psdu / TX psdu / MAYBE_BEACON / END"""
    log = [] if log is None else log
    fr = frames(session)

    def first(kind):
        return next((m['seq'] for _, _, m in fr if kind(m)), 0)

    gw = gateway.Gateway(
        PAN,
        SHORT,
        EXT,
        MAC,
        known_sensors(),
        dsn=first(lambda m: m['type'] in ('data', 'cmd') and m.get('src') in (SHORT, EXT)),
        bsn=first(lambda m: m['type'] == 'beacon' and m.get('src') == SHORT),
    )
    gw.permit = session in JOINS
    log.append(f'SESSION {session} {int(gw.permit)} {gw.dsn} {gw.bsn}')
    log += [f'SENSOR {mac} {short:04x}' for mac, short in gw.sensors.items()]
    clock = None
    hst = PairingHost(gw)
    stats = {'windows': 0, 'exact': 0, 'mismatch': [], 'dsn_gaps': 0, 'uncaptured': 0}
    prev = None
    for i, (t, b, m) in enumerate(fr):
        dongle = from_dongle(m, prev)
        prev = (m, dongle)
        if dongle or not to_dongle(m):
            continue
        expected, last = [], (m, False)
        for _, b2, m2 in fr[i + 1 :]:
            if m2['type'] == 'beacon' and m2.get('src_pan') != PAN:
                continue  # other coordinators answering too
            if not from_dongle(m2, last):
                break
            expected.append(b2)
            last = (m2, True)
        set_clock_from(gw, expected)
        if gw.clock_ms != clock:
            clock = gw.clock_ms
            log.append(f'CLOCK {clock}')
        dsn = next((frame.parse_mac(x[:-4])['seq'] for x in expected if x[0] & 7 in (1, 3)), None)
        if dsn is not None and dsn != gw.dsn:  # the dongle sent something we did not capture
            stats['dsn_gaps'] += 1
            gw.dsn = dsn
            log.append(f'DSN {dsn}')
        log.append(f'RX {b.hex()}')
        log += [f'TX {x.hex()}' for x in expected]
        tx, out = gw.rx(b)
        hst.handle(out)
        stats['windows'] += 1
        if tx[: len(expected)] == expected and len(expected) >= min(1, len(tx)):
            stats['exact'] += 1
        elif not expected and all(x[0] & 7 == 0 for x in tx):
            stats['uncaptured'] += 1  # a beacon lost under another coordinator's; the next one's seq still checks
            log.append('MAYBE_BEACON')
        else:
            stats['mismatch'].append((round(t, 3), [x.hex() for x in expected], [x.hex() for x in tx]))
    log.append('END')
    return gw, stats


def replay_vectors():
    log = [
        '# Generated by test_gateway_model.py from ground_truth/rf: the captured house traffic as the stock',
        '# dongle saw it (RX) and answered it (TX). Regenerate: OWS_WRITE_VECTORS=1 uv run pytest -k replay_vectors',
    ]
    for s in STREAMS:
        replay(s, log)
    return '\n'.join(log) + '\n'


@pytest.mark.parametrize('session', STREAMS)
def test_replay_matches_stock_dongle(session):
    gw, s = replay(session)
    assert s['windows'] >= 2
    assert not s['mismatch'], s['mismatch'][:5]
    assert s['dsn_gaps'] <= 1 and s['uncaptured'] <= 2


def test_replay_vectors_are_current():
    text = replay_vectors()
    if os.environ.get('OWS_WRITE_VECTORS') == '1':
        REPLAY_VECTORS.write_text(text)
    assert REPLAY_VECTORS.read_text() == text


def test_join_assigns_addresses_like_the_stock_dongle():
    gw, _ = replay('2026-10-22_1823_906.7M_250k_g35')
    assert gw.sensors['77BD514D'] == 0x0002  # known sensor: its old address
    assert gw.sensors['77C68193'] == 0x0012  # new sensor: next after the highest in use (0x0011)


def test_host_handshake_and_lists():
    gw = gateway.Gateway(PAN, SHORT, EXT, MAC, {'77BD514D': 2, '77C68193': 0x12})
    _, out = gw.host(host.build(host.INQUIRY))
    assert out == [host.build(0x4328, b'\x01', to_host=True)]
    _, out = gw.host(host.build(host.GET_VERSION))
    assert out == [host.build_ack(0x5316, to_host=True), host.build(0x5317, gateway.VERSION, to_host=True)]
    _, out = gw.host(host.build(host.FINISH_AUTH, b'\xff'))
    assert [p.hex() for p in out] == ['55aa5314ff0265', '55aa530315016a']  # as captured
    _, out = gw.host(host.build(host.GET_SENSOR_LIST, b'\x02'))
    assert [host.parse(p)['payload'] for p in out[1:]] == [b'77BD514D', b'77C68193']
    gw.host(host.build(host.TIME_SYNC, (1792718501753).to_bytes(8, 'big')))
    assert gw.clock_ms == 1792718501753


def test_alarm_forwarding():
    gw = gateway.Gateway(PAN, SHORT, EXT, MAC, clock_ms=0x019E4E7BF23B)
    pt = app.build_sensor_msg(0xA1, b'77C681D1', 0x07, 0x13, 0x5E, b'\x00', 0x0A75)
    data = frame.build_data(7, PAN, SHORT, 0x000C, app.encrypt(pt))
    tx, out = gw.rx(gateway.psdu(data), rssi=0x4C)
    assert tx == [frame.build_ack(7) + frame.ack_fcs(frame.build_ack(7))]
    # 77C681D1's heartbeat as the house dongle sent it to the host (ground_truth/host/2026-05-21_*, 2026-05-22)
    assert out[0].hex() == '55aa531d190000019e4e7bf23ba1373743363831443107135e0001000a754c07c7'
