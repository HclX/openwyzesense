# Which stock sensors resend their reports (#92): 7 days of the house dongle's host log. A resend reaches the host as
# the same report (MAC, message, counter) again within seconds; A1 heartbeats repeat a counter hours apart, not counted.
import collections
import json
import statistics

import gt
from wyzesense_ref import app, host

LOG_REL = 'host/2026-05-21_house_dongle_778B5F63'  # 7 days of the house dongle: not published (gt.py)


def resends_per_sensor():
    rows = []
    for f in sorted(gt.need(LOG_REL).glob('*.jsonl')):
        for line in f.read_text().splitlines():
            x = json.loads(line)
            if x.get('dir') != 'R':
                continue
            p = host.parse(bytes.fromhex(x['proto_hex']))
            if not p or p.get('cmd') != host.ALARM:
                continue
            a = host.parse_alarm(p['payload'])
            q = app.parse(a['plaintext'] + bytes(-len(a['plaintext']) % 16))
            if 'counter' in q:
                rows.append((x['ts_ms'] / 1000, q['mac'].decode(), q['msg'], q['counter'], -a['rssi']))
    per = collections.defaultdict(lambda: {'reports': 0, 'copies': 0, 'rssi': []})
    last = {}
    for t, mac, msg, counter, rssi in sorted(rows):
        s = per[mac]
        s['rssi'].append(rssi)
        key = (mac, msg, counter)
        if key in last and t - last[key] <= 3:
            s['copies'] += 1
        else:
            s['reports'] += 1
        last[key] = t
    return {mac: (s['reports'], s['copies'], statistics.median(s['rssi'])) for mac, s in per.items()}


def test_resends_concentrate_in_two_sensors_not_the_weakest():
    per = resends_per_sensor()
    reports = sum(r for r, _, _ in per.values())
    copies = sum(c for _, c, _ in per.values())
    assert reports == 4126 and copies == 88
    heavy = {mac for mac, (r, c, _) in per.items() if c / r > 0.2}
    assert heavy == {'77C684C4', '77C6853E'}
    for mac, (r, c, _) in per.items():
        if mac not in heavy:
            assert c / r < 0.01, mac
    weakest = min(per, key=lambda m: per[m][2])  # the lowest RSSI at the dongle
    assert weakest == '778B77D9' and per[weakest][1] == 0 and per[weakest][2] <= -85
