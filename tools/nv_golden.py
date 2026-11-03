#!/usr/bin/env python3
"""The golden NV vectors (#42): pages (stock, ours, blank, and made-up ones with every validation case) with what
nvows.export() makes of them (<name>.json, as tools/nv_json.py writes it) and what nvows.import_() makes of that
(<name>.ows.bin, or <name>.error when import refuses). test_nv_json.py and the Rust crate (crates/ows-host) are both
held to these files, byte for byte.

    uv run tools/nv_golden.py          # rewrite reference/tests/vectors/nv/golden
    uv run tools/nv_golden.py --check  # differ: exit 1
"""

import argparse
import json
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'reference'))
from wyzesense_ref import nvows  # noqa: E402

VECTORS = ROOT / 'reference' / 'tests' / 'vectors' / 'nv'
GOLDEN = VECTORS / 'golden'


def stock_page(records):
    """A stock (TI NV) page: 4 bytes of its own, then each record's data and its 8-byte header."""
    page = bytearray(b'\x00\x00\x10\x00')
    for item, sub, data, valid in records:
        page += data + bytes([0x06, 0x40 if valid else 0x00]) + item.to_bytes(2, 'little')
        page += sub.to_bytes(2, 'little') + len(data).to_bytes(2, 'little')
    return bytes(page) + b'\xff' * (4096 - len(page))


def sensor(pan, short, mac):
    return nvows.sensor_data(pan, short, mac)


PAN, COORD, EXT = 0x65FE, 0xD425, 0x00124B0012345678


def made_up():
    net = nvows.network_data(PAN, COORD, EXT)
    yield (
        'stock_validation',
        stock_page(
            [
                (1, 0, net, True),
                (4, 0, bytes([9, 0, 0, 0]), True),
                (5, 1, sensor(PAN, 0x0003, b'77A8C793'), True),
                (5, 2, sensor(PAN, 0x0004, b'77a8c794'), True),  # lower case: not a MAC
                (5, 3, sensor(PAN, 0xFFFE, b'77A8C795'), True),  # no address
                (5, 4, sensor(0x1234, 0x0006, b'77A8C796'), True),  # another PAN
                (5, 5, sensor(PAN, 0x0007, b'77A8C793'), True),  # MAC again
                (5, 6, sensor(PAN, 0x0003, b'77A8C797'), True),  # address again: re-pair
                (5, 7, sensor(PAN, 0x0008, b'00000000'), True),  # the v1 zero MAC
                (5, 8, bytes(16), True),  # not 24 bytes
                (5, 9, sensor(PAN, 0x0009, b'77A8C798'), True),
                (5, 9, sensor(PAN, 0x0009, b'77A8C798'), False),  # deleted
                (5, 10, sensor(PAN, 0x000A, b'77A8C799'), True),
                (6, 0, b'\x01\x02\x03\x04', True),  # unknown
            ]
        ),
    )
    yield 'stock_bad_network', stock_page([(1, 0, nvows.network_data(0xFFFF, COORD, EXT), True)])
    yield (
        'ows_sensor_join_cfg',
        nvows.encode(
            {
                (0x20, 0): bytes([0x0E, 1])
                + (0x65FD).to_bytes(2, 'little')
                + (0x7B1F).to_bytes(2, 'little')
                + (0x0005).to_bytes(2, 'little')
                + b'\x00\x00'
                + (0x0003).to_bytes(2, 'little'),
                (0x21, 0): bytes([0x0E, 1, 2, 0]) + (250).to_bytes(2, 'little') + b'\x00\x00',
                (0x30, 1): b'\xaa\xbb\xcc\xdd',  # unknown
            }
        ),
    )
    yield (
        'ows_join_pan_845b',
        nvows.encode({(0x20, 0): bytes([0x01, 0]) + (0x845B).to_bytes(2, 'little') + b'\x66\x33\x05\x00' + bytes(4)}),
    )
    yield 'ows_bad_cfg', nvows.encode({(0x21, 0): bytes([0, 0, 7, 0]) + bytes(4)})
    # ours, with a superseded record, a deleted one and a bad CRC
    page = bytearray(nvows.encode({(1, 0): net, (4, 0): bytes([1, 0, 0, 0])}).rstrip(b'\xff'))
    page += nvows.record(5, 1, sensor(PAN, 0x0003, b'77A8C793'))
    page += nvows.record(5, 1, sensor(PAN, 0x0004, b'77A8C793'))
    page += nvows.record(5, 2, sensor(PAN, 0x0005, b'77A8C794'))
    page += nvows.record(5, 2, sensor(PAN, 0x0005, b'77A8C794'), live=False)
    bad = bytearray(nvows.record(5, 3, sensor(PAN, 0x0006, b'77A8C795')))
    bad[-1] ^= 1
    page += bad
    yield 'ows_superseded', bytes(page) + b'\xff' * (4096 - len(page))
    yield 'blank', b'\xff' * 4096


def cases():
    for p in sorted(VECTORS.glob('*.page30.bin')):
        yield p.name.removesuffix('.page30.bin'), p.read_bytes(), False
    for name, page in made_up():
        yield name, page, True


def outputs(page):
    doc = nvows.export(page)
    out = {'.json': (json.dumps(doc, indent=2) + '\n').encode()}
    try:
        out['.ows.bin'] = nvows.import_(doc)
    except ValueError as e:
        out['.error'] = (str(e) + '\n').encode()
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--check', action='store_true')
    a = ap.parse_args(argv)
    GOLDEN.mkdir(exist_ok=True)
    want = {}
    for name, page, made in cases():
        if made:
            want[f'{name}.page.bin'] = page
        for ext, data in outputs(page).items():
            want[name + ext] = data
    have = {p.name for p in GOLDEN.iterdir()}
    differ = sorted(n for n, d in want.items() if n not in have or (GOLDEN / n).read_bytes() != d)
    extra = sorted(have - set(want))
    if a.check:
        for n in differ + extra:
            print(f'golden NV vector out of date: {n}', file=sys.stderr)
        return 1 if differ or extra else 0
    for n in extra:
        (GOLDEN / n).unlink()
    for n in differ:
        (GOLDEN / n).write_bytes(want[n])
    print(f'{len(want)} files, {len(differ)} written, {len(extra)} removed', file=sys.stderr)
    return 0


if __name__ == '__main__':
    sys.exit(main())
