# Host <-> dongle protocol (FACTS.md §9): codec vectors, and checks against host-side captures of stock dongles
# (ground_truth/host/: what wyzesense2mqtt read from / wrote to /dev/hidraw, May 2026).
import json
import pathlib

import gt
from wyzesense_ref import app, host

GT = pathlib.Path(__file__).resolve().parents[2] / 'ground_truth'
HOST = GT / 'host'
HOUSE_REL = 'host/2026-05-21_house_dongle_778B5F63'  # 7 days of the house dongle: not published (gt.py)
HOUSE = gt.GT / HOUSE_REL
TRACE = HOST / '2026-05-27_dongle_77A85A36'


def test_build_matches_captured_packets():
    # the stock handshake, as captured on dongle 77A85A36 (TRACE/trace_hidraw0.jsonl)
    assert host.build(host.INQUIRY).hex() == 'aa55430327016c'
    assert host.build(host.GET_ENR, b'0' * 16).hex() == 'aa55431302303030303030303030303030303030300457'
    assert host.build(host.GET_MAC).hex() == 'aa554303040149'
    assert host.build(host.GET_VERSION).hex() == 'aa55530316016b'
    assert host.build(host.FINISH_AUTH, b'\xff').hex() == 'aa55530414ff0269'
    assert host.build_ack(0x5319).hex() == 'aa555319ff026a'
    assert host.build(0x4328, b'\x01', to_host=True).hex() == '55aa43042801016f'
    assert host.build_ack(0x5316, to_host=True).hex() == '55aa5316ff0267'
    assert host.build(0x4305, b'77A85A36', to_host=True).hex() == '55aa430b0537374138354133360318'
    assert host.build(0x5315, to_host=True).hex() == '55aa530315016a'


def test_parse_and_scan():
    pkts = [host.build(0x4328, b'\x01', to_host=True), host.build_ack(0x5316, to_host=True)]
    assert host.scan(b'\x00\x55' + pkts[0] + b'\xaa\x55\x53' + pkts[1] + b'\xff' * 5) == pkts
    assert host.parse(pkts[1]) == {'cmd': 0x53FF, 'to_host': True, 'acked': 0x5316, 'payload': b''}
    assert host.parse(pkts[0][:-1] + b'\x00') is None
    assert host.hid_unwrap(bytes([7]) + pkts[1] + b'\x12' * 56) == pkts[1]


def records(*dirs):
    for d in dirs:
        for f in sorted(d.glob('*.jsonl')):
            for line in f.read_text().splitlines():
                r = json.loads(line)
                for p in host.scan(bytes.fromhex(r['proto_hex'])):
                    yield r, host.parse(p)


def test_every_capture_record_holds_valid_packets():
    for d in (HOUSE, TRACE):  # HOUSE only with a full copy (gt.py)
        for f in d.glob('*.jsonl'):
            for line in f.read_text().splitlines():
                r = json.loads(line)
                assert host.scan(bytes.fromhex(r['proto_hex'])), (f.name, r['ts_ms'])
                if 'raw_hex' in r:  # HID report: length byte, then the protocol bytes
                    assert host.hid_unwrap(bytes.fromhex(r['raw_hex'])).hex() == r['proto_hex']


def test_alarm_is_rf_plaintext_plus_clock_and_rssi():
    gt.need(HOUSE_REL)
    seen, n = set(), {0xA1: 0, 0xA2: 0, 0xE8: 0, 0xAB: 0}
    for _r, m in records(HOUSE, TRACE):
        if m['cmd'] != host.ALARM or not m['to_host'] or m['payload'] in seen:
            continue
        seen.add(m['payload'])
        a = host.parse_alarm(m['payload'])
        pt = a['plaintext']
        assert all(chr(c) in '0123456789ABCDEF' for c in pt[1:9])
        if pt[0] == 0xAB:
            assert len(pt) == 23 and pt[22] == 0x01 and pt[9] == 0x02  # v1 motion only
        else:
            p = app.parse(pt + bytes(-len(pt) % 16))
            assert len(pt) == 16 + len(p['data'])  # ends right after the counter
            assert p['sensor_type'] in app.SENSOR_TYPES
        assert host.alarm_payload(pt + bytes(-len(pt) % 16), a['time_ms'], a['rssi']) == m['payload']
        n[pt[0]] += 1
    assert min(n.values()) >= 50, n


def test_house_dongle_identity_and_sensor_list():
    gt.need(HOUSE_REL)
    devices = json.loads((GT / 'rf' / 'devices.json').read_text())
    macs = {r['mac'] for r in devices}
    got = {m['cmd']: m['payload'] for r, m in records(HOUSE) if m['to_host'] and m['cmd'] in (0x4305, 0x532F)}
    assert got[0x4305] == b'778B5F63' and '778B5F63' in macs
    listed = {m['payload'].decode() for r, m in records(HOUSE) if m['to_host'] and m['cmd'] == host.SENSOR_LIST_ITEM}
    assert len(listed) == got[0x532F][0]
    assert len(listed & macs) >= 10


def test_time_sync():
    # the dongle asks for the time (0x5332) about every 11.5 min; the host answers 0x5333 with 8-byte Unix ms
    writes = [(r, m) for r, m in records(TRACE) if not m['to_host'] and m['cmd'] == host.TIME_SYNC]
    assert writes
    for r, m in writes:
        assert len(m['payload']) == 8 and abs(int.from_bytes(m['payload'], 'big') - r['ts_ms']) < 1000
    reqs = [r['ts_ms'] for r, m in records(TRACE) if m['to_host'] and m['cmd'] == host.TIME_SYNC_REQ]
    assert len(reqs) >= 2


def test_stock_dongle_pairing_replies():
    # our sensor pairing with the spare stock dongle as six sensor types; we played the host
    d = HOST / '2026-10-23_spare_dongle_77A85A36_pairing'
    got = [
        (r['dir'], host.parse(p))
        for r in map(json.loads, (d / 'dongle_host.jsonl').read_text().splitlines())
        for p in host.scan(bytes.fromhex(r['proto_hex']))
    ]

    def replies(cmd):
        return [m['payload'] for direction, m in got if direction == 'R' and m['cmd'] == cmd]

    assert all(p == b'' for p in replies(0x531D))  # Set Scan: empty reply
    assert all(p == b'' for p in replies(0x5324))  # Verify Sensor: empty reply
    scans, r1s = replies(host.SCAN_RESULT), replies(host.GET_R1 + 1)
    types = [0x0E, 0x07, 0x02, 0x0F, 0x03, 0x01]
    assert [p[9] for p in scans] == types and [p[25] for p in r1s] == types
    for a3, a5 in zip(scans, r1s, strict=False):
        assert len(a3) == 11 and a3[:9] == b'\xa3' + b'77FED830'  # A3 bytes 0-10
        assert len(a5) == 27 and a5[:9] == b'\xa5' + b'77FED830'  # A5 bytes 0-26, the a5 included
    # our sensor echoes the A4 token in A5, and the dongle accepted it: so A4 carries the host's token (the
    # last pairing used one of our own)
    assert [a5[9:25] for a5 in r1s] == [app.R1_TOKEN] * 5 + [b'OpenWyzeSense-01']
    alarms = [host.parse_alarm(p)['plaintext'] for p in replies(host.ALARM)]
    assert len(alarms) == len({a for a in alarms}) == 15  # each once (we ACKed at once)


def test_host_link_ota_batch_and_flag_restore(tmp_path, monkeypatch):
    import struct
    import sys

    sys.path.insert(0, str(GT.parent / 'tools'))
    import host_link
    import slot_image

    raw_a = b'\xff' * 0x100 + struct.pack('<II', 0x20004000, 0x01101) + bytes(range(120))
    raw_b = b'\xff' * 0x100 + struct.pack('<II', 0x20004000, 0x0F101) + bytes(range(120))
    builds = [
        slot_image.make(raw_a, 'WYZE-CONTACT-V1', 77, slot='a'),
        slot_image.make(raw_b, 'WYZE-CONTACT-V1', 77, slot='b'),
    ]

    class FakeLink(host_link.HostLink):
        def __init__(self, events, known=()):
            self.backlog = list(events)
            self.known = list(known)
            self.offers = []
            self.data_chunks = []
            self.flag_sets = []
            self.cur_flags = host.FLAG_DEDUP | host.FLAG_ANNOUNCE

        def close(self):
            pass

        def handshake(self):
            return {'mac': b'77000001'}

        def send(self, cmd, payload=b''):
            pass

        def sensors(self):
            return list(self.known)

        def flags(self, mask=None, timeout=1.5):
            if mask is not None:
                self.cur_flags = mask
                self.flag_sets.append(mask)
            return bytes([self.cur_flags, 0x3F])

        def ows_call(self, sub_id, payload=b'', timeout=1.5):
            if sub_id == host.OWS_SUB_OTA_OFFER:
                self.offers.append(payload)
                return b'\x00'
            return b''

        def send_ows(self, sub_id, payload=b''):
            if sub_id == host.OWS_SUB_OTA_DATA:
                self.data_chunks.append(payload)

        def _read(self, timeout):
            return self.backlog.pop(0) if self.backlog else None

    # 1. Explicit 10-MAC batch (> 8 gateway queue slots): stages 8 up front, refills 9 & 10 as DONE arrives.
    macs = [f'770000{i:02X}'.encode() for i in range(10)]
    events = [
        {
            'cmd': host.OWS_RSP,
            'payload': bytes([host.OWS_SUB_OTA_NEED]) + macs[0] + bytes([1]) + (0).to_bytes(4, 'big') + bytes([96]),
        },
        *[
            {'cmd': host.OWS_RSP, 'payload': bytes([host.OWS_SUB_OTA_DONE]) + m + bytes([0]) + b'\x00\x00\x00M'}
            for m in macs
        ],
    ]
    fl = FakeLink(events)
    res = fl.ota_batch(macs, builds, timeout=5.0)
    assert res == {m: 0 for m in macs}
    assert [o[:8] for o in fl.offers] == macs  # 8 initial + 2 refilled
    assert len(fl.data_chunks) == 3  # 96 B split into 3 x 32 B chunks
    assert b''.join(c[4:] for c in fl.data_chunks) == builds[1][:96]

    # 2. CLI --ota-all-matching-board snapshots flags (0x18), turns on FLAG_OTA (0x1A), cancels & restores 0x18 on exit.
    img_a, img_b = tmp_path / 'a.img', tmp_path / 'b.img'
    img_a.write_bytes(builds[0])
    img_b.write_bytes(builds[1])
    cli_events = [
        {'cmd': host.OWS_RSP, 'payload': bytes([host.OWS_SUB_OTA_DONE]) + b'770000AA' + bytes([0]) + b'\x00\x00\x00M'}
    ]
    cli_link = FakeLink(cli_events, known=[b'770000AA'])
    monkeypatch.setattr(host_link, 'HostLink', lambda *a, **kw: cli_link)
    monkeypatch.setattr(sys, 'argv', ['host_link.py', '/dev/null', '--ota-all-matching-board', str(img_a), str(img_b)])
    host_link.main()
    assert cli_link.offers[0][:8] == b'********' and cli_link.offers[0][8:] != bytes(24)
    assert cli_link.offers[-1] == b'********' + bytes(24)  # ota_cancel() in finally
    assert cli_link.flag_sets == [
        host.FLAG_DEDUP | host.FLAG_ANNOUNCE | host.FLAG_OTA,
        host.FLAG_DEDUP | host.FLAG_ANNOUNCE,
    ]


def test_parse_build_info_pages_and_host_slot_cli(tmp_path):
    import os
    import struct
    import subprocess
    import sys

    sys.path.insert(0, str(GT.parent / 'tools'))
    import slot_image

    img_id = bytes.fromhex('0102030405060708')
    ver39 = b'v0.9.12-45-gdeadbeef01-dirty-longbuildX'
    assert len(ver39) == 39
    p0 = (
        bytes([host.OWS_SUB_BUILD_INFO])
        + struct.pack('<IBBH', 0x4253574F, 1, 0x03, 0)
        + img_id
        + b'gateway-contact-v1'.ljust(24, b'\x00')
        + b'gateway'.ljust(12, b'\x00')
    )
    p1 = bytes([host.OWS_SUB_BUILD_INFO]) + img_id + b'WYZE-CONTACT-V1'.ljust(24, b'\x00') + ver39[:22]
    p2 = bytes([host.OWS_SUB_BUILD_INFO]) + img_id + ver39[22:].ljust(18, b'\x00')

    info2 = host.parse_build_info(p0, p1)
    assert info2['version'] == ver39[:22].decode() and info2['id'] == img_id.hex() and info2['flags'] == 3
    info3 = host.parse_build_info(p0, p1, p2)
    assert info3['version'] == ver39.decode() and info3['board'] == 'WYZE-CONTACT-V1'
    assert host.parse_build_info(b'', p1) is None
    assert host.parse_build_info(p0[:4] + b'XXXX' + p0[8:], p1) is None
    assert host.parse_build_info(p0, p1, bytes([host.OWS_SUB_BUILD_INFO]) + b'\xff' * 8 + bytes(18)) is None

    host_build = pathlib.Path(os.environ.get('OWS_HOST_BUILD', GT.parent / 'build' / 'host'))
    sensor_bin = host_build / 'firmware' / 'ows-host-sensor'
    if sensor_bin.exists():
        raw_a = b'\xff' * 0x100 + struct.pack('<II', 0x20004000, 0x01101) + bytes(range(64))
        raw_b = b'\xff' * 0x100 + struct.pack('<II', 0x20004000, 0x0F101) + bytes(range(64))
        sa, sb = tmp_path / 'slot_a.img', tmp_path / 'slot_b.img'
        sa.write_bytes(slot_image.make(raw_a, 'HOST-BOARD', 10, slot='a'))
        sb.write_bytes(slot_image.make(raw_b, 'HOST-BOARD', 11, slot='b'))
        proc = subprocess.run(
            [str(sensor_bin), '--slot-a', str(sa), '--slot-b', str(sb)],
            input=b'status\n',
            capture_output=True,
            timeout=5,
            check=True,
        )
        assert b'sensor: air port' in proc.stderr
        bad = tmp_path / 'empty.img'
        bad.write_bytes(b'')
        assert subprocess.run([str(sensor_bin), '--slot-a', str(bad)], capture_output=True, timeout=5).returncode != 0
