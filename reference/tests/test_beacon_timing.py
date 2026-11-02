# When stock gateways answer a beacon request (#84): from the decoded frames in ground_truth/rf, the beacons that
# followed each beacon request, start to start. Only stock coordinators: the house dongle (845B/3366), a neighbour's
# (D712/A5FB), and the spare dongle (65FE/D425) before our firmware went onto it (2026-10-23 23:22).
import json
import pathlib

ROOT = pathlib.Path(__file__).resolve().parents[2]
STOCK = {('845B', '3366'), ('D712', 'A5FB'), ('65FE', 'D425')}
OURS_FROM = '2026-10-23_2322'  # the spare dongle runs our gateway from this session on


def stock_beacon_delays():
    delays = {}
    for f in sorted((ROOT / 'ground_truth' / 'rf').glob('*/decoded_frames.jsonl')):
        session = f.parent.name
        frames = sorted((json.loads(x) for x in f.read_text().splitlines()), key=lambda x: x['t'])
        frames = [x for x in frames if x.get('crc_ok')]
        for i, req in enumerate(frames):
            if req.get('cmd') != 'beacon-req':
                continue
            for b in frames[i + 1 : i + 8]:
                ms = (b['t'] - req['t']) * 1000
                if ms > 400:
                    break
                who = (b.get('span') or b.get('src_pan'), b.get('src'))
                if b['type'] != 'beacon' or who not in STOCK:
                    continue
                if who == ('65FE', 'D425') and session >= OURS_FROM:
                    continue
                delays.setdefault(who, []).append(ms)
    return delays


def test_stock_gateways_answer_beacon_requests_after_a_random_delay():
    delays = stock_beacon_delays()
    assert set(delays) == STOCK
    every = sorted(ms for v in delays.values() for ms in v)
    assert len(every) >= 60
    assert every[0] >= 35 and every[-1] <= 160  # 38-156 ms, start to start
    for who, v in delays.items():  # each one varies: early when the air is free, later (a beacon or two) otherwise
        assert min(v) < 45 and max(v) > 75, (who, sorted(v))
