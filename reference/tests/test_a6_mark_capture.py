# Stock sensors receiving our gateway's A6 mark (GW_FLAG_ANNOUNCE, firmware/common/wyze_app.h) and carrying on as
# usual: ground_truth/rf/2026-10-24_1042_906.7M_250k_g10_stock_sensors_a6_mark (docs/stock-unknowns.md J6).
import json
import pathlib

from wyzesense_ref import app

D = (
    pathlib.Path(__file__).resolve().parents[2]
    / 'ground_truth'
    / 'rf'
    / '2026-10-24_1042_906.7M_250k_g10_stock_sensors_a6_mark'
)
SENSORS = {b'77BD514D', b'77A8C793', b'77C68193'}


def frames():
    for line in (D / 'decoded_frames.jsonl').read_text().splitlines():
        r = json.loads(line)
        if (
            r['crc_ok']
            and r['type'] == 'data'
            and r.get('dpan') == '65FE'
            and r['payload']
            and len(r['payload']) % 32 == 0
        ):
            yield r, app.decrypt(bytes.fromhex(r['payload']))


def test_stock_sensors_take_the_marked_a6_and_report_after_it():
    a6 = [(r['t'], r['dst']) for r, pt in frames() if pt[0] == 0xA6]
    marks = {pt[11:16] for r, pt in frames() if pt[0] == 0xA6}
    assert len(a6) == 3 and marks == {b'OW\x01\x00\x01'}
    # every sensor: its handshake (A5, answered by a marked A6), then reports as usual
    for mac in SENSORS:
        a5 = [r['t'] for r, pt in frames() if pt[0] == 0xA5 and pt[1:9] == mac]
        assert len(a5) == 1, mac
        assert any(pt[0] in (0xA1, 0xA2, 0xE8) and pt[1:9] == mac and r['t'] > a5[0] for r, pt in frames()), mac
