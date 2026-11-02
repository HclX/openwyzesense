# Codec tests on frames received over the air (ground_truth/rf, 2026-10-21/22).
# PSDUs are given as hex (MAC frame + CRC-32); test_ground_truth.py shows they come out of the raw IQ.
import pathlib
import zlib

from wyzesense_ref import app, frame, host, phy

VEC = pathlib.Path(__file__).parent / 'vectors'

FRAMES = {
    n: h
    for n, h in (
        line.split() for line in (VEC / 'frames.txt').read_text().splitlines() if line and not line.startswith('#')
    )
}
# 2026-10-22 morning stream, PAN 0x845B, dongle = 0x3366; join of v2 contact sensor 77BD514D at 18:21:41-44
E8_CLIMATE, A2_MOTION, ZERO_MAC, DATA_REQ, B0_SYNC = (
    FRAMES[n] for n in ('E8_CLIMATE', 'A2_MOTION', 'ZERO_MAC', 'DATA_REQ', 'B0_SYNC')
)
ASSOC_REQ, ASSOC_RESP, A3_ANNOUNCE, A4_TOKEN, A5_RESPONSE, A6_DONE = (
    FRAMES[n] for n in ('ASSOC_REQ', 'ASSOC_RESP', 'A3_ANNOUNCE', 'A4_TOKEN', 'A5_RESPONSE', 'A6_DONE')
)


def chip_vectors():
    # 10 kchip/s chip strings sliced from ground_truth/rf/2026-10-21_2118_906.8M_2400k_g10 (g001..g010)
    lines = (VEC / 'beacon_chips.txt').read_text().split('\n')
    return [c for c in lines[1::2] if c]


def payload(h):
    return frame.parse_mac(bytes.fromhex(h)[:-4])['payload']


def test_phy_decode_real_bursts():
    for chips in chip_vectors():
        errs, bits = phy.decode(chips)
        assert errs == 0
        f = frame.parse(bits)
        assert f['crc_ok']
        m = frame.parse_mac(f['mac_frame'])
        assert (m['type'], m['src_pan'], m['src']) == ('beacon', 0x65FE, 0xD425)


def test_phy_encode_matches_air_bit_exact():
    for chips in chip_vectors():
        _, bits = phy.decode(chips)
        mac_frame = frame.parse(bits)['mac_frame']
        tx = phy.encode(frame.build(mac_frame))
        start = chips.find(phy.HEADER)
        if start < 0:  # other FM polarity
            chips = chips.translate(str.maketrans('01', '10'))
            start = chips.find(phy.HEADER)
        rx = chips[start:]
        assert tx[: len(rx)] == rx


def test_crc32_on_captured_frames():
    for h in (
        E8_CLIMATE,
        A2_MOTION,
        ZERO_MAC,
        DATA_REQ,
        B0_SYNC,
        ASSOC_REQ,
        ASSOC_RESP,
        A3_ANNOUNCE,
        A4_TOKEN,
        A5_RESPONSE,
        A6_DONE,
    ):
        b = bytes.fromhex(h)
        assert zlib.crc32(b[:-4]) == int.from_bytes(b[-4:], 'little')


def test_mac_parse_and_build():
    b = bytes.fromhex(E8_CLIMATE)[:-4]
    m = frame.parse_mac(b)
    assert (m['type'], m['ack_req'], m['dst_pan'], m['dst'], m['src'], m['seq']) == (
        'data',
        1,
        0x845B,
        0x3366,
        0x000D,
        0xEE,
    )
    assert frame.build_data(0xEE, 0x845B, 0x3366, 0x000D, m['payload']) == b
    assert frame.parse_mac(bytes.fromhex(DATA_REQ)[:-4])['cmd'] == 'data-req'


def test_climate_report():
    p = app.parse(app.decrypt(payload(E8_CLIMATE)))
    assert p['mac'] == b'77C68217' and p['sensor_type'] == 0x07
    assert (p['temperature_c'], p['humidity_pct']) == (19.96, 50)  # bridge: 19.96 C / 50 %
    assert p['counter'] == 0x1316


def test_motion_event():
    p = app.parse(app.decrypt(payload(A2_MOTION)))
    assert p['mac'] == b'778B77D9' and p['active'] is True and p['counter'] == 0x713B


def test_low_battery_flag_is_byte_12_not_a_length():
    # FACTS.md: byte 12 is the low-battery latch (0x01 once a stock sensor set it), byte 13 alone the data length.
    # Read as a 16-bit length, a sensor with a low battery had its reports dropped (#75)
    pt = bytearray(app.decrypt(payload(A2_MOTION)))
    ok = app.parse(bytes(pt))
    assert ok['low_battery'] is False
    pt[12] = 0x01
    low = app.parse(bytes(pt))
    assert low['low_battery'] is True and low['active'] is True and low['counter'] == ok['counter']
    assert len(host.alarm_payload(bytes(pt), 0, 50)) == len(host.alarm_payload(app.decrypt(payload(A2_MOTION)), 0, 50))


def test_zero_mac_v1_contact():
    p = app.parse(app.decrypt(payload(ZERO_MAC)))
    assert p['msg'] == 0xA1 and p['mac'] == bytes(8) and p['sensor_type'] == 0x01
    assert p['counter'] == 0x000D


def test_time_sync():
    p = app.parse(app.decrypt(payload(B0_SYNC)))
    assert p['msg'] == app.MSG_TIME_SYNC and p['time_ms'] == 1792680838506  # 2026-10-22 07:53:58.506 PDT


def test_build_reproduces_v2_ciphertext():
    p = app.parse(app.decrypt(payload(E8_CLIMATE)))
    pt = app.build_sensor_msg(
        0xE8, p['mac'], p['sensor_type'], p['temp_raw'], p['battery_raw'], p['data'], p['counter']
    )
    assert app.encrypt(pt) == payload(E8_CLIMATE)


def test_association():
    req = frame.parse_mac(bytes.fromhex(ASSOC_REQ)[:-4])
    assert (req['cmd'], req['dst'], req['src_pan']) == ('assoc-req', 0x3366, 0xFFFF)
    assert req['src'].to_bytes(8, 'little') == b'77BD514D'  # extended address = ASCII MAC
    assert req['payload'] == bytes([0x01, 0x80])  # capability: allocate address
    resp = frame.parse_mac(bytes.fromhex(ASSOC_RESP)[:-4])
    assert resp['cmd'] == 'assoc-resp' and resp['src'] == 0x00124B001E9FF933
    assert resp['payload'] == bytes([0x02, 0x02, 0x00, 0x00])  # short address 0x0002, status success


def test_join_handshake():
    a3 = app.parse(app.decrypt(payload(A3_ANNOUNCE)))
    assert (a3['mac'], a3['sensor_type'], a3['fw_version']) == (b'77BD514D', 0x0E, 0x21)
    a4 = app.parse(app.decrypt(payload(A4_TOKEN)))
    assert a4['token'] == app.R1_TOKEN
    a5 = app.parse(app.decrypt(payload(A5_RESPONSE)))
    assert a5['mac'] == b'77BD514D' and (a5['sensor_type'], a5['fw_version']) == (0x0E, 0x21)
    assert a5['response'].hex() == 'd2df6d684801efb54788284d2e096bbc'  # same again at the 18:26 re-join
    a6 = app.parse(app.decrypt(payload(A6_DONE)))
    assert a6['status'] == bytes([0xFF, 0x04, 0, 0]) and a6['time_ms'] == 1792718501753


def test_parse_mac_rejects_truncated_and_reserved_modes():
    import pytest

    for bad in (
        b'\x02\x00',  # < 3 bytes
        b'\x01\x04\x00\x34\x12',  # dst_mode == 1 (reserved)
        b'\x01\x40\x00\x34\x12',  # src_mode == 1 (reserved)
        b'\x61\x88\x01\x5b',  # truncated dst_pan
        b'\x63\xcc\x01\x5b\x84\x01\x02\x03',  # truncated ext dst address
    ):
        with pytest.raises(ValueError):
            frame.parse_mac(bad)


def test_iq_chunked_decode_preserves_boundary_bursts():
    from wyzesense_ref import iq

    clip = (
        pathlib.Path(__file__).resolve().parents[2]
        / 'ground_truth'
        / 'rf'
        / '2026-10-22_1821_906.7M_250k_g35'
        / 'snip_182141_059.cu8'
    )
    whole = [f['psdu'].hex() for f in iq.decode_file(clip, 250e3, 906.7e6, chunk_seconds=60.0) if f['crc_ok']]
    chunked = [f['psdu'].hex() for f in iq.decode_file(clip, 250e3, 906.7e6, chunk_seconds=0.9) if f['crc_ok']]
    assert chunked == whole
    for h in (ASSOC_REQ, ASSOC_RESP, A3_ANNOUNCE, A4_TOKEN, A5_RESPONSE, A6_DONE):
        assert h in chunked
