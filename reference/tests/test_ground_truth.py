# Checks that tie docs/FACTS.md to the raw evidence in ground_truth/: checksums, IQ -> frames, frames vs the
# bridge's decoded state, and the firmware-binary facts (offsets into the dumps).
import glob
import hashlib
import json
import struct
import zlib

import gt
import pytest
from test_reference import (
    A2_MOTION,
    A3_ANNOUNCE,
    A4_TOKEN,
    A5_RESPONSE,
    A6_DONE,
    ASSOC_REQ,
    ASSOC_RESP,
    E8_CLIMATE,
    ZERO_MAC,
)
from wyzesense_ref import app, frame, host, iq

GT = gt.GT  # the repository's ground_truth/, or OWS_GROUND_TRUTH (a full copy: gt.py)
RF = GT / 'rf'
HOST = GT / 'host'
FW = GT / 'firmware'


def sums(name):
    listed = {}
    for line in (gt.PUBLIC / name).read_text().splitlines():
        digest, rel = line.split('  ', 1)
        assert rel not in listed, f'duplicate entry in {name}: {rel}'
        listed[rel] = digest
    return listed


def test_checksums():
    # SHA256SUMS: every published file, exactly; SHA256SUMS.unpublished: the files left out of the public repository
    # (ground_truth/README.md), checked too when OWS_GROUND_TRUTH points at a full copy
    public, unpublished = sums('SHA256SUMS'), sums('SHA256SUMS.unpublished')
    assert not set(public) & set(unpublished)
    meta = ('README.md', 'SHA256SUMS', 'SHA256SUMS.unpublished')
    actual = {
        p.relative_to(gt.PUBLIC).as_posix()
        for p in gt.PUBLIC.rglob('*')
        if p.is_file() and p.relative_to(gt.PUBLIC).as_posix() not in meta
    }
    assert set(public) == actual
    for rel, digest in public.items():
        assert hashlib.sha256((gt.PUBLIC / rel).read_bytes()).hexdigest() == digest, rel
    if GT != gt.PUBLIC:
        for rel, digest in unpublished.items():
            assert hashlib.sha256((GT / rel).read_bytes()).hexdigest() == digest, rel


# ---- RF: raw IQ -> frames ----


def decoded_psdus(pattern, fs, tuned_hz):
    gt.need('rf/' + pattern.split('/')[0])
    out = set()
    for p in sorted(glob.glob(str(RF / pattern))):
        out |= {f['psdu'].hex() for f in iq.decode_file(p, fs, tuned_hz) if f['crc_ok']}
    return out


def test_iq_join_clip_contains_the_join():
    got = decoded_psdus('2026-10-22_1821_906.7M_250k_g35/snip_182141_059.cu8', 250e3, 906.7e6)
    for h in (ASSOC_REQ, ASSOC_RESP, A3_ANNOUNCE, A4_TOKEN, A5_RESPONSE, A6_DONE):
        assert h in got


def test_iq_morning_clips_contain_test_vectors():
    got = decoded_psdus('2026-10-22_0730_906.7M_250k_g35/snip_*.cu8', 250e3, 906.7e6)
    for h in (ZERO_MAC, E8_CLIMATE, A2_MOTION):
        assert h in got


def test_iq_rtl433_clips_decode():
    # 2.4 MS/s rtl_433 -S captures tuned on channel: the second dongle's beacons
    got = decoded_psdus('2026-10-21_2118_906.8M_2400k_g10/g00[12]_*.cu8', 2400e3, 906.8e6)
    assert any(frame.parse_mac(bytes.fromhex(h)[:-4]).get('src') == 0xD425 for h in got)


# ---- decoded frames (derived) vs bridge state (independent ground truth) ----


def load_session(sess):
    frames = [json.loads(line) for fj in sess.glob('**/decoded_frames.jsonl') for line in fj.read_text().splitlines()]
    events = [json.loads(line) for fj in sess.glob('**/bridge_events.jsonl') for line in fj.read_text().splitlines()]
    return frames, [e for e in events if 'mac' in e and not e.get('initial')]


def sensor_messages(frames):
    seen, out = set(), []
    for f in frames:
        if not f['crc_ok'] or f['type'] != 'data' or not f.get('payload') or f['psdu'] in seen:
            continue
        seen.add(f['psdu'])
        b = bytes.fromhex(f['psdu'])
        assert zlib.crc32(b[:-4]) == int.from_bytes(b[-4:], 'little')
        p = app.parse(app.decrypt(bytes.fromhex(f['payload'])))
        out.append((f['t'], p))
    return out


def test_every_decoded_sensor_message_carries_a_mac():
    n = 0
    for sess in RF.iterdir():
        if not sess.is_dir():
            continue
        for _, p in sensor_messages(load_session(sess)[0]):
            if p['msg'] not in app.SENSOR_MSGS:
                continue
            n += 1
            assert p['mac'] == bytes(8) or all(chr(c) in '0123456789ABCDEF' for c in p['mac'])
    assert n >= (140 if GT != gt.PUBLIC else 90)  # the public recordings hold 97 of them


def test_ack_fcs():
    # every ACK, from dongles and sensors alike, carries CRC-32 over FC + seq + one zero byte
    # (decoded_frames.jsonl marks them crc_ok: false, since it checks the standard FCS)
    n = 0
    for fj in RF.glob('**/decoded_frames.jsonl'):
        for line in fj.read_text().splitlines():
            f = json.loads(line)
            if f.get('type') != 'ack':
                continue
            b = bytes.fromhex(f['psdu'])
            assert b[3:] == frame.ack_fcs(b[:3]), f['psdu']
            n += 1
    assert n >= 250


def reverse_bits(b):
    return bytes(int(f'{x:08b}'[::-1], 2) for x in b)


def test_launchpad_with_stock_settings_stores_bytes_bit_reversed():
    # The LaunchPad, with the stock radio settings, next to the SDR: every frame the SDR decoded (data frames
    # and ACKs) is in the board's log with each byte bit-reversed. And the board got the ACKs, though its radio
    # drops frames that fail its CRC check: the radio's CRC accepts the ACK FCS.
    sess = gt.need('rf/2026-10-22_2236_906.7M_250k_g35_launchpad')
    board = {
        reverse_bits(bytes.fromhex(line.split()[4])) for line in (sess / 'launchpad_uart.log').read_text().splitlines()
    }
    frames = [json.loads(line) for line in (sess / 'decoded_frames.jsonl').read_text().splitlines()]
    sdr = {bytes.fromhex(f['psdu'])[:-4] for f in frames if f['crc_ok']}
    acks = {bytes.fromhex(f['psdu'])[:3] for f in frames if f['type'] == 'ack'}
    assert len(sdr) == 3 and len(acks) == 3
    assert sdr | acks <= board


def test_launchpad_hears_every_report_the_house_dongle_reported():
    # 30 min of morning traffic: for every change the bridge reported, the LaunchPad (stock radio settings)
    # received the sensor's report, and the A2 / E8 contents agree with the bridge's state
    sess = gt.need('rf/2026-10-23_0720_launchpad_bridge')
    reports = []
    for line in (sess / 'launchpad_uart.log').read_text().splitlines():
        f = line.split()
        if len(f) != 5 or f[1] != 'RX':
            continue  # boot banner
        t, hexframe = f[0], f[4]
        m = frame.parse_mac(bytes.fromhex(hexframe))
        if m['type'] == 'data' and m.get('dst') == 0x3366 and m['payload']:
            reports.append((float(t), app.parse(app.decrypt(m['payload']))))
    events = [json.loads(line) for line in (sess / 'bridge_events.jsonl').read_text().splitlines()]
    events = [e for e in events if not e.get('initial')]
    checked = 0
    for e in events:
        near = [(t, p) for t, p in reports if p.get('mac') == e['mac'].encode() and -3 <= e['received'] - t <= 8]
        assert near, e
        t, p = min(near, key=lambda tp: abs(e['received'] - tp[0]))
        st = e['state']
        if p['msg'] == 0xA2:
            assert p['active'] == st.get('is_active', st.get('is_open'))
            checked += 1
        if p['msg'] == 0xE8:
            assert abs(p['temperature_c'] - st['temperature']) < 0.006 and p['humidity_pct'] == st['humidity']
            checked += 1
    assert len(events) >= 14 and checked >= 7


def test_our_gateway_transmits_like_the_stock_dongle():
    # Our gateway on the LaunchPad (stock radio settings), answering injected frames on the test network.
    sess = RF / '2026-10-23_0824_906.7M_250k_g0_launchpad_tx'
    got = []
    for p in sorted(sess.glob('snip_*.cu8')):
        got += iq.decode_file(p, 250e3, 906.7e6)
    beacons = [f for f in got if f['crc_ok'] and f['mac']['type'] == 'beacon']
    acks = [f for f in got if f['mac']['type'] == 'ack']
    data = [f for f in got if f['crc_ok'] and f['mac']['type'] == 'data']
    assert [f['psdu'][:-4].hex() for f in beacons] == [f'0080{k:02x}fe650f0fff4f0000' for k in (3, 4, 5)]
    # the radio itself makes the stock ACK FCS (CRC-32 over FC + seq + one zero byte)
    assert len(acks) == 6 and all(f['psdu'][3:] == frame.ack_fcs(f['psdu'][:3]) for f in acks)
    assert [f['psdu'][0] for f in acks] == [0x02, 0x12] * 3  # pending only before a B0
    for f in data:
        pt = app.decrypt(f['mac']['payload'])
        assert pt[0] == app.MSG_TIME_SYNC and pt[9] == 0x28 and f['mac']['src'] == 0x0F0F
    # B0 timing, from the full stream: 29-38 ms after the start of the pending ACK, as the stock dongle
    rows = [json.loads(line) for line in (sess / 'decoded_frames.jsonl').read_text().splitlines()]
    pend = [r['sample'] for r in rows if r['type'] == 'ack' and r['fc'] == 0x12]
    b0 = [r['sample'] for r in rows if r['type'] == 'data' and r.get('src') == '0F0F']
    assert len(pend) == len(b0) == 3
    assert all(29 <= (b - a) / 250 <= 38 for a, b in zip(pend, b0, strict=False))


def join_messages(sess, mac):
    """A sensor's join, from a session's decoded frames: the plaintexts of its A3, then the next A4, its A5, the
    next A6 (A4 and A6 carry no MAC: the first after the sensor's A3 / A5)."""
    pts = []
    for r in map(json.loads, (sess / 'decoded_frames.jsonl').read_text().splitlines()):
        if r['type'] == 'data' and r['crc_ok']:
            pts.append(app.decrypt(bytes.fromhex(r['payload'])))
    out, want = [], [(0xA3, True), (0xA4, False), (0xA5, True), (0xA6, False)]
    for pt in pts:
        if len(out) < 4 and pt[0] == want[len(out)][0] and (not want[len(out)][1] or pt[1:9] == mac):
            out.append(pt)
    return out


def host_packets(path):
    return [
        (r['dir'], host.parse(p)) for r in map(json.loads, open(path)) for p in host.scan(bytes.fromhex(r['proto_hex']))
    ]


def test_a_stock_sensor_joins_our_gateway():
    # The house's stock v2 contact 77BD514D joining our gateway (LaunchPad, test network) and reporting
    # (R1, stock sensor with our gateway); the same sensor joined the stock house dongle in 2026-10-22_1821.
    mac = b'77BD514D'
    ours = RF / '2026-10-23_1640_906.7M_250k_g10_stock_contact_launchpad_gw'
    stock = RF / '2026-10-22_1821_906.7M_250k_g35'
    a, b = join_messages(ours, mac), join_messages(stock, mac)
    assert [pt[0] for pt in a] == [pt[0] for pt in b] == [0xA3, 0xA4, 0xA5, 0xA6]
    assert a[1] == b[1]  # our A4 is the stock one, byte for byte
    assert a[2] == b[2]  # the sensor's A5 answer is the same for either gateway
    assert a[0][:14] == b[0][:14] and a[0][15:] == b[0][15:]  # A3: all but byte 14 (new on every join)
    assert a[3][:5] == b[3][:5] == bytes.fromhex('a6ff040000')  # A6: ff 04 00 00, then the clock
    # every data frame the sensor sent got our ACK (same sequence number, the stock ACK FCS) right after it
    rows = [json.loads(line) for line in (ours / 'decoded_frames.jsonl').read_text().splitlines()]
    for i, r in enumerate(rows):
        if r['crc_ok'] and r.get('src') == '0001' and r.get('dst') == '0F0F':
            psdu = bytes.fromhex(r['psdu'])
            ack = bytes.fromhex(rows[i + 1]['psdu'])
            assert rows[i + 1]['type'] == 'ack' and ack[2] == psdu[2] and ack[3:] == frame.ack_fcs(ack[:3])
            assert 0 < rows[i + 1]['t'] - r['t'] < 0.2
    # the host side: the scan result and R1 reply as from a stock dongle, then each report once, in order,
    # across a reset of the gateway at 16:42:45
    d = HOST / '2026-10-23_launchpad_gateway_stock_contact_77BD514D'
    got = [
        (r['dir'], r['ts_ms'], host.parse(p))
        for r in map(json.loads, (d / 'launchpad_host.jsonl').read_text().splitlines())
        for p in host.scan(bytes.fromhex(r['proto_hex']))
    ]

    def replies(cmd):
        return [m['payload'] for direction, _, m in got if direction == 'R' and m['cmd'] == cmd]

    assert replies(host.SCAN_RESULT) == [a[0][:11]] and replies(host.GET_R1 + 1) == [a[2][:27]]
    alarms = [host.parse_alarm(p)['plaintext'] for p in replies(host.ALARM)]
    assert all(pt[1:9] == mac and pt[9] == 0x0E for pt in alarms)
    assert [pt[0] for pt in alarms[:2]] == [0xA2, 0xA1]  # the first reports, as to the stock dongle
    counters = [int.from_bytes(pt[15:17], 'big') for pt in alarms]
    assert counters == [1, 1] + list(range(2, 0x0E))  # nothing lost, nothing twice
    states = [pt[14] for pt in alarms[2:]]
    assert states == [k % 2 for k in range(12)]  # open/closed by hand, alternating


def test_v1_motion_and_climate_join_our_gateway():
    # Two more of the house's stock sensors joining our gateway: the v1 motion 77A8C793 (no v1 join had been
    # captured before) and the climate 77C68193, whose join to a stock dongle is in 2026-10-22_1823.
    rf = RF / '2026-10-23_1650_906.7M_250k_g10_v1_motion_climate_launchpad_gw'
    d = HOST / '2026-10-23_launchpad_gateway_v1_motion_77A8C793_climate_77C68193'
    # climate: the same join as to the stock dongle, the sensor's A5 answer included
    a, b = join_messages(rf, b'77C68193'), join_messages(RF / '2026-10-22_1823_906.7M_250k_g35', b'77C68193')
    assert [pt[0] for pt in a] == [pt[0] for pt in b] == [0xA3, 0xA4, 0xA5, 0xA6]
    assert a[1] == b[1] and a[2] == b[2] and a[0][:14] == b[0][:14]
    got = host_packets(d / 'launchpad_host_climate.jsonl')
    alarms = [
        host.parse_alarm(m['payload'])['plaintext']
        for direction, m in got
        if direction == 'R' and m['cmd'] == host.ALARM
    ]
    assert [pt[0] for pt in alarms] == [0xA1] + [0xE8] * 4  # A1 then E8, as to the stock dongle
    counters = [int.from_bytes(pt[17:19] if pt[0] == 0xE8 else pt[15:17], 'big') for pt in alarms]
    assert counters == list(range(0x18FA, 0x18FF))  # continued from before the join
    assert [tuple(pt[14:17]) for pt in alarms[1:]] == [(19, 70, 58), (20, 17, 75), (21, 37, 74), (22, 40, 92)]
    # v1 motion: the host side (most of its own frames are too weak in the recording)
    got = host_packets(d / 'launchpad_host_motion_v1.jsonl')

    def replies(cmd):
        return [m['payload'] for direction, m in got if direction == 'R' and m['cmd'] == cmd]

    (scan,) = replies(host.SCAN_RESULT)
    assert scan == b'\xa3' + b'77A8C793' + bytes([0x02, 0x1E])  # v1 motion, firmware 0x1E
    (r1,) = replies(host.GET_R1 + 1)
    assert len(r1) == 27 and r1[:9] == b'\xa5' + b'77A8C793' and r1[25:] == bytes([0x02, 0x1E])
    alarms = [host.parse_alarm(p)['plaintext'] for p in replies(host.ALARM)]
    assert [(pt[0], pt[14], int.from_bytes(pt[15:17], 'big')) for pt in alarms] == [
        (0xA1, 1, 0),
        (0xA2, 1, 1),
        (0xA2, 0, 2),
        (0xA2, 1, 3),
    ]  # its counter restarted at the join
    # on the air: our A4 and A6 to it as the stock dongle's, and a B0 after the first A1 of each sensor (not v2
    # contact / motion, FACTS §4)
    rows = [json.loads(line) for line in (rf / 'decoded_frames.jsonl').read_text().splitlines()]
    ours = [
        (r['time'], app.decrypt(bytes.fromhex(r['payload'])))
        for r in rows
        if r['type'] == 'data' and r['crc_ok'] and r.get('src') == '0F0F'
    ]
    kinds = [pt[0] for _, pt in ours]
    assert kinds == [0xA4, 0xA6, 0xB0] * 2
    assert all(pt[9] == 0x28 for _, pt in ours if pt[0] == 0xB0) and ours[0][1] == b[1]


def test_a_stock_sensor_joins_our_firmware_on_the_dongle():
    # the spare dongle with our gateway firmware: the stock v2 contact 77BD514D pairs and reports, as with the stock
    # dongle (its A5 answer is the one in its stock joins), each report once and in order
    got = host_packets(HOST / '2026-10-23_dongle_ours_stock_contact_77BD514D' / 'dongle_host.jsonl')

    def replies(cmd):
        return [m['payload'] for direction, m in got if direction == 'R' and m['cmd'] == cmd]

    assert replies(0x4305) == [b'77A85A36'] and replies(0x4303)[0].hex().startswith('55ffeb67')  # the CH554's
    stock_a5 = join_messages(RF / '2026-10-22_1821_906.7M_250k_g35', b'77BD514D')[2]
    assert replies(host.GET_R1 + 1) == [stock_a5[:27]]
    alarms = [host.parse_alarm(p)['plaintext'] for p in replies(host.ALARM)]
    assert all(pt[1:9] == b'77BD514D' for pt in alarms) and [pt[0] for pt in alarms[:2]] == [0xA2, 0xA1]
    assert [int.from_bytes(pt[15:17], 'big') for pt in alarms] == [1, 1] + list(range(2, 11))
    assert [pt[14] for pt in alarms[2:]] == [k % 2 for k in range(9)]  # closed, open, ... closed


@pytest.mark.parametrize('session', ['2026-10-22_0730_906.7M_250k_g35', '2026-10-22_1823_906.7M_250k_g35'])
def test_decoded_state_matches_bridge(session):
    frames, events = load_session(gt.need('rf/' + session))
    msgs = sensor_messages(frames)
    agree = 0
    for e in events:
        if e['signal'] == 0:
            continue  # the bridge's placeholder events at a join:
        if e['state'].get('temperature') == 0.0 and e['state'].get('humidity') == 0:
            continue  # no reading yet
        cand = [p for t, p in msgs if p.get('mac') == e['mac'].encode() and -1.0 <= e['received'] - t <= 3.0]
        st = e['state']
        for p in cand:
            if p['msg'] == 0xE8 and 'temperature' in st:
                assert abs(p['temperature_c'] - st['temperature']) < 0.006 and p['humidity_pct'] == st['humidity']
                agree += 1
            if p['msg'] == 0xA2 and ('is_active' in st or 'is_open' in st):
                assert p['active'] == st.get('is_active', st.get('is_open'))
                agree += 1
    assert agree >= 20


# ---- firmware dumps ----


def rd(rel):
    return gt.read('firmware/' + rel)


def u32(b, off):
    return struct.unpack_from('<I', b, off)[0]


def test_aes_key_in_gateway_ram():
    assert rd('gateway_2025-02-08_v1.8/RAM.bin')[0x14E0:0x14F0] == app.KEY


def test_gateway_radio_setup():
    ram = rd('gateway_2025-02-08_v1.8/RAM.bin')
    cmd = ram[0x2648 : 0x2648 + 24]
    assert struct.unpack_from('<H', cmd, 0)[0] == 0x3807  # CMD_PROP_RADIO_DIV_SETUP
    mod = struct.unpack_from('<H', cmd, 14)[0]
    assert mod & 7 == 1 and mod >> 3 == 20  # GFSK, deviation 20 x 250 Hz
    assert cmd[16] == 15 and struct.unpack_from('<H', cmd, 17)[0] == 0x3333  # preScale, rateWord
    assert cmd[21] == 2 and struct.unpack_from('<H', cmd, 22)[0] == 0xE0A0  # preamConf, formatConf
    assert u32(ram, 0x2728) == 0x00001107 and u32(ram, 0x2774) == 0x0100505C  # MCE patch, DSSS=2


def test_ccfg():
    for img in ('motion_v1_777A9A46/dump.bin', 'gateway_2025-02-08_v1.8/FLASH.bin'):
        d = rd(img)
        assert u32(d, 0x1FFA8 + 12 * 4) == 0xC5FE01C5  # BL_CONFIG: backdoor on, DIO1, low
        assert u32(d, 0x1FFA8 + 15 * 4) == 0xFFC500C5  # CCFG_TAP_DAP_0: CPU DAP enabled
    assert rd('motion_v1_777A9A46/dump.bin')[0x1FFCC:0x1FFD4] == b'777A9A46'  # IEEE_MAC_1 + IEEE_BLE_0


def bl_target(d, a):
    h1, h2 = struct.unpack_from('<HH', d, a)
    s = (h1 >> 10) & 1
    i1, i2 = 1 - (((h2 >> 13) & 1) ^ s), 1 - (((h2 >> 11) & 1) ^ s)
    off = (s << 24) | (i1 << 23) | (i2 << 22) | ((h1 & 0x3FF) << 12) | ((h2 & 0x7FF) << 1)
    return a + 4 + (off - (1 << 25) if s else off)


def test_motion_firmware_join_and_mac_code():
    m = rd('motion_v1_777A9A46/dump.bin')
    assert u32(m, 0x85A0) == 0x61C88647  # XXTEA delta (subtracted)
    assert u32(m, 0x859C) == 0x20000F24  # XXTEA key pointer
    assert bl_target(m, 0xF76A) == 0x842C  # A5 builder calls XXTEA
    assert m[0xF770:0xF772] == bytes([0xA5, 0x21])  # movs r1, #0xA5 (message type)
    assert u32(m, 0xCCC0) == 0x0001FFCC  # 0xCC14: target = CCFG MAC
    assert bl_target(m, 0xCCA6) == 0x13C7C  # ... via the FlashProgram wrapper
    assert u32(rd('gateway_2025-02-08_v1.8/FLASH.bin'), 0x8C9C) == 0x61C88647


def test_gateway_dumps():
    assert b'0.0.0.47 V1.8 Gateway GW3U' in rd('gateway_2025-02-08_v1.8/RAM.bin')
    assert rd('gateway_2025-02-08_v1.8/FLASH.bin')[0x1E058:0x1E060] == b'77A88E96'


def test_gateway_host_link_is_uart0():
    d, ram = rd('gateway_2025-02-08_v1.8/FLASH.bin'), rd('gateway_2025-02-08_v1.8/RAM.bin')
    assert u32(d, 0x1731C) == 115200 and u32(d, 0x17320) == 3  # UART params: baud, 8 data bits
    assert u32(d, 0x17328) == 0x40001000 and u32(d, 0x17330) == 0x15  # UART0 base, its interrupt
    assert u32(d, 0x17340) == 0x20003E18 and u32(d, 0x17344) == 0x40  # RX ring buffer, 64 bytes
    # in that ring: host -> dongle bytes (a 0x5333 time sync after an ACK of 0x5332)
    assert ram[0x3E18:0x3E22].hex() == '5332ff0283aa55530b33'
    # dongle -> host: a complete, checksum-valid 0x532F (sensor count = 2) built in the CC1310's RAM
    assert host.parse(ram[0x18D5:0x18DD]) == {'cmd': 0x532F, 'to_host': True, 'payload': b'\x02'}


def test_stock_nv_page():
    from wyzesense_ref import nvpage

    v14 = rd('gateway_2025-02-02_v1.4/FLASH.bin')[nvpage.PAGE : nvpage.PAGE + nvpage.PAGE_SIZE]
    assert len(nvpage.records(v14)) == 191 and nvpage.records(v14)[0][0] == 4  # the whole page parses
    assert nvpage.network(v14) == {'pan': 0x2357, 'short': 0xC907, 'ext': 0x00124B001D90494A, 'extra': b'\x17\x00'}
    table = nvpage.sensors(v14)
    assert len(table) == int.from_bytes(nvpage.latest(v14)[(4, 0)], 'little') == 12
    shorts = {}
    for pan, short, mac in table.values():
        assert pan == 0x2357
        shorts.setdefault(short, []).append(mac)
    assert sorted(shorts[4]) == [b'777C1FCA', b'7788CCF6'] and sorted(shorts[5]) == [bytes(8), b'7788CD06']
    v18 = rd('gateway_2025-02-08_v1.8/FLASH.bin')[nvpage.PAGE : nvpage.PAGE + nvpage.PAGE_SIZE]
    assert nvpage.network(v18)['pan'] == 0x0A96 and nvpage.network(v18)['short'] == 0xF4B1
    assert nvpage.sensors(v18) == {0: (0x0A96, 1, b'77BD514D'), 1: (0x0A96, 2, b'77A88E96')}


def test_pin_tables():
    # TI PIN driver tables (PIN_Config words, PIN_TERMINATE 0xFE): pin in bits 0-7, flags above
    def table(img, off):
        d, out = rd(img), []
        while u32(d, off) != 0xFE:
            out.append(u32(d, off))
            off += 4
        return out

    assert table('gateway_2025-02-08_v1.8/FLASH.bin', 0x174C0) == [0x8080080A, 0x8080080D]  # DIO10, 13: out, low
    assert table('motion_v1_777A9A46/dump.bin', 0x156B0) == [
        0x8080080C,  # DIO12: out, low
        0x80002001,
        0x80002000,
        0x80002002,
    ]  # DIO1, 0, 2: in, pull-up


def test_same_radio_patch_in_every_image():
    patch = rd('gateway_2025-02-08_v1.8/FLASH.bin')[0x2EC : 0x2EC + 1024]  # 256 words
    for img in (
        'gateway_2025-01-31_v1.8/FLASH.bin',
        'gateway_2025-02-02_v1.4/FLASH.bin',
        'motion_v1_777A9A46/dump.bin',
    ):
        assert rd(img)[0x2EC : 0x2EC + 1024] == patch


def test_contact_v1_dump():
    """The v1 contact 777B42DF (ground_truth/firmware/contact_v1_777B42DF_2026-10-24): a 5x5 CC1310F128, its ROM
    bootloader disabled in the CCFG, its MAC in the CCFG intact, and no network or MAC record in its NV page."""
    import struct as _s

    from wyzesense_ref import nvpage

    d = gt.need('firmware/contact_v1_777B42DF_2026-10-24/flash.bin').parent
    fl, ccfg, fcfg1 = ((d / n).read_bytes() for n in ('flash.bin', 'ccfg.bin', 'fcfg1.bin'))
    assert len(fl) == 0x20000 and fl[0x1FFA8:] == ccfg
    assert (_s.unpack_from('<I', fcfg1, 0x294)[0] >> 16) & 7 == 1  # package 5x5 (RHB)
    assert _s.unpack_from('<I', ccfg, 0x30)[0] == 0x00FE0100  # BL_CONFIG: bootloader off
    assert _s.unpack_from('<I', ccfg, 0x3C)[0] == 0xFFC500C5  # CPU debug port on
    assert ccfg[0x24:0x2C] == b'777B42DF'  # 0x1FFCC: the MAC
    items = {r[1] for r in nvpage.records(fl[0x1E000:0x1F000])}
    assert items == {4, 6}  # no network (1), no MAC (8)


def test_contact_v1_zero_mac_join():
    """The dead v1 contact 777B42DF joins with its MAC all zero (its CCFG MAC intact: test_contact_v1_dump)."""
    import json as _j

    from wyzesense_ref import app as _app

    rows = [
        _j.loads(line)
        for line in (GT / 'rf' / '2026-10-24_1147_906.7M_250k_g10_contact_v1_zero_mac_pairs' / 'decoded_frames.jsonl')
        .read_text()
        .splitlines()
    ]
    rows = [r for r in rows if r['crc_ok'] and r.get('dpan') == '65FE']
    assoc = [r for r in rows if r['type'] == 'cmd' and r['payload'].startswith('01')]
    assert assoc and all(bytes.fromhex(r['psdu'])[9:17] == bytes(8) for r in assoc)  # extended source address 0
    pts = [
        _app.decrypt(bytes.fromhex(r['payload']))
        for r in rows
        if r['type'] == 'data' and r['payload'] and len(r['payload']) % 32 == 0
    ]
    a3 = [pt for pt in pts if pt[0] == 0xA3]
    assert a3 and a3[0][:11] == b'\xa3' + bytes(8) + b'\x01\x10'
    assert any(pt[0] == 0xA2 and pt[1:9] == bytes(8) and pt[9] == 0x01 for pt in pts)
