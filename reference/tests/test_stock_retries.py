# How many copies a stock sensor sends of a report nobody ACKs (stock-unknowns R10, FACTS.md): the roaming hardware
# check of 2026-11-01 (ground_truth/host/2026-11-01_roaming_stock_contact_retries), where the owner of the stock v2
# contact 77BD514D was unplugged and the other gateway overheard every copy it could.
import pathlib
import re

ROOT = pathlib.Path(__file__).resolve().parents[2]
GT = ROOT / 'ground_truth' / 'host' / '2026-11-01_roaming_stock_contact_retries'
LINE = re.compile(r'^(\S+)Z .*Cmd=(53F1|5319), Payload=\[([0-9a-f,]+)\]')


def reports(name):
    """{counter: [(seconds of day, 'own' | 'overheard')]} for 77BD514D's alarms, and the time the owner went away."""
    out, gone = {}, None
    for line in (GT / name).read_text().splitlines():
        if 'Local USB dongle 77A85A36 disconnected' in line:
            gone = secs(line.split('Z ')[0])
        m = LINE.match(line)
        if not m:
            continue
        t, cmd, hexes = m.groups()
        b = bytes.fromhex(hexes.replace(',', ''))
        if cmd == '53F1':
            if b[0] != 0x20:  # OWS_SUB_OVERHEARD only
                continue
            b = b[3:]
        if b[9:17] != b'77BD514D':
            continue
        out.setdefault(int.from_bytes(b[-3:-1], 'big'), []).append((secs(t), 'own' if cmd == '5319' else 'overheard'))
    return out, gone


def secs(t):
    h, m, s = t.split('T')[1].split(':')
    return int(h) * 3600 + int(m) * 60 + float(s)


def test_acked_reports_are_sent_once():
    r, gone = reports('w2m-run1.log')
    acked = {c: v for c, v in r.items() if v[0][0] < gone}
    assert sorted(acked) == list(range(29, 39))
    for v in acked.values():  # one copy: the owner's, and the other gateway hearing that same copy
        assert sorted(k for _, k in v) == ['overheard', 'own']


def test_strong_signal_six_unless_the_next_event_comes():
    # w2m-run4: the owner (9A78) powered off, the dongle overhearing close by (-43..-46 dBm): a report left alone is
    # sent 6 times; an "open" followed by its "close" within a second, twice
    r, _ = reports('w2m-run4.log')
    assert sorted(r) == list(range(61, 67))
    assert all(k == 'overheard' for v in r.values() for _, k in v)
    assert {c: len(v) for c, v in r.items()} == {61: 2, 62: 6, 63: 2, 64: 6, 65: 2, 66: 6}
    for c in (61, 63, 65):
        assert r[c + 1][0][0] - r[c][0][0] < 1.0


def test_unacked_reports_six_copies_over_about_two_seconds():
    bursts = {}
    for name in ('w2m-run1.log', 'w2m.log'):
        r, gone = reports(name)
        bursts.update({c: [t for t, k in v] for c, v in r.items() if v[0][0] > gone})
        assert all(k == 'overheard' for c, v in r.items() if v[0][0] > gone for _, k in v)  # the owner is gone
    assert sorted(bursts) == list(range(39, 51))
    assert max(len(v) for v in bursts.values()) == 6  # 1 + 5 retries, never more
    full = [v for v in bursts.values() if len(v) == 6]
    assert len(full) == 4  # ctr 39, 40, 46, 50
    # host arrival times (through the relay and its WebSocket), not air times: single gaps jitter (0.17-0.99 s), the
    # span of a burst does not: 1.4-2.4 s for the 5 retries, ~0.3-0.45 s each on average
    spans = [v[-1] - v[0] for v in full]
    assert all(1.3 < s < 2.5 for s in spans)
    assert all(b - a > 0.15 for v in full for a, b in zip(v, v[1:], strict=False))
    # a new event cuts the last one's retries short: most short bursts had the next report start within 2 s of their
    # first copy, before their own retries would have ended; the rest (42, 44, 48) the listener missed copies of
    order = sorted(bursts)
    cut = {
        c for c, n in zip(order, order[1:], strict=False) if len(bursts[c]) < 6 and bursts[n][0] - bursts[c][0] < 2.0
    }
    assert cut == {41, 43, 45, 47, 49}
