#!/usr/bin/env python3
"""Decode an rtl_433 -w stream into decoded_frames.jsonl, and optionally cut snips around the bursts.

    uv run python tools/decode_stream.py SESSION_DIR [--fs 250e3] [--tuned 906.7e6] [--snips]

SESSION_DIR holds stream.cu8 and start_time.txt (Unix time written just before rtl_433 started). Writes
SESSION_DIR/decoded_frames.jsonl in the format of ground_truth/rf/*/decoded_frames.jsonl (minus the fields our
IQ decoder does not measure: dev_khz, level_db). With --snips, also writes snip_HHMMSS_mmm.cu8: one slice per
group of frames, starting 0.15 s before the first.
"""

import argparse
import datetime
import json
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / 'reference'))
from wyzesense_ref import iq  # noqa: E402


def row(f, t):
    m = f['mac']
    r = {
        't': t,
        'time': datetime.datetime.fromtimestamp(t).strftime('%H:%M:%S.%f')[:-3],
        'sample': f['sample'],
        'carrier_khz': round(f['carrier_hz'] / 1e3, 1),
        'crc_ok': f['crc_ok'],
        'viterbi_errors': f['path_errors'],
        'psdu': f['psdu'].hex(),
        'payload': m['payload'].hex(),
        'fc': m['fc'],
        'type': m['type'],
        'sec': m['security'],
        'pend': m['pending'],
        'ar': m['ack_req'],
        'panc': m['pan_comp'],
        'dam': m['dst_mode'],
        'ver': m['version'],
        'sam': m['src_mode'],
        'seq': m['seq'],
    }
    width = {2: 4, 3: 16}
    if m['dst_mode'] in width and m.get('dst') is not None:
        r['dpan'], r['dst'] = f'{m["dst_pan"]:04X}', f'{m["dst"]:0{width[m["dst_mode"]]}X}'
    if m['src_mode'] in width and m.get('src') is not None:
        r['src'] = f'{m["src"]:0{width[m["src_mode"]]}X}'
    return r


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('session', type=pathlib.Path)
    ap.add_argument('--fs', type=float, default=250e3)
    ap.add_argument('--tuned', type=float, default=906.7e6)
    ap.add_argument('--snips', action='store_true')
    a = ap.parse_args()
    stream = a.session / 'stream.cu8'
    t0 = float((a.session / 'start_time.txt').read_text())
    frames = iq.decode_file(stream, a.fs, a.tuned)
    with open(a.session / 'decoded_frames.jsonl', 'w') as out:
        for f in frames:
            out.write(json.dumps(row(f, t0 + f['sample'] / a.fs)) + '\n')
    print(f'{len(frames)} frames')
    if not a.snips:
        return
    groups, last = [], None
    for f in frames:  # frames within 2 s of each other share a snip
        if last is None or f['sample'] - last > 2 * a.fs:
            groups.append([f['sample'], f['sample']])
        groups[-1][1] = last = f['sample']
    with open(stream, 'rb') as fp:
        for first, end in groups:
            lo, hi = max(0, int(first - 0.15 * a.fs)), int(end + 0.6 * a.fs)
            fp.seek(2 * lo)
            name = datetime.datetime.fromtimestamp(t0 + first / a.fs).strftime('snip_%H%M%S_%f')[:-3] + '.cu8'
            (a.session / name).write_bytes(fp.read(2 * (hi - lo)))
            print(name)


if __name__ == '__main__':
    main()
