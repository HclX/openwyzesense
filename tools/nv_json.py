#!/usr/bin/env python3
"""A device's NV page (flash page 30) to and from JSON (docs/design/nv-format.md, issue #61): how the flashing tools
carry a stock dongle's network and sensor table into our format, and validate it on the way.

    uv run python tools/nv_json.py export PAGE_OR_FLASH.bin [-o NV.json]   # a stock page or one of ours -> JSON
    uv run python tools/nv_json.py import NV.json -o PAGE.bin                # JSON -> a page in our format

PAGE_OR_FLASH.bin: the 4 KB page, or a whole 128 KB flash dump (a backup's flash.bin: page 30 is taken). Export lists
what validation dropped and why, and the records it does not know, on stderr too.
"""

import argparse
import json
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / 'reference'))
from wyzesense_ref import nvows  # noqa: E402

PAGE30 = 0x1E000


def page_of(path):
    data = pathlib.Path(path).read_bytes()
    if len(data) == 0x20000:
        return data[PAGE30 : PAGE30 + nvows.PAGE_SIZE]
    if len(data) != nvows.PAGE_SIZE:
        sys.exit(f'{path}: {len(data)} bytes: a 4 KB page or a 128 KB flash dump expected')
    return data


NV_CHOICES = ('migrate', 'wipe', 'keep')


def nv_page_for(choice, current, save_json=None):
    """What the flashing tools write to page 30 (#61): choice 'migrate' (the current page exported, validated and
    imported into our format; the JSON saved to save_json), 'wipe' (all FF: the firmware formats it), 'keep' (None:
    page 30 left alone), or a path to a JSON file (imported). Prints what is carried over and what is dropped."""
    if choice == 'keep':
        print('NV (page 30): kept as it is')
        return None
    if choice == 'wipe':
        print('NV (page 30): wiped')
        return b'\xff' * nvows.PAGE_SIZE
    if choice == 'migrate':
        doc = nvows.export(current)
        if save_json:
            pathlib.Path(save_json).write_text(json.dumps(doc, indent=2) + '\n')
    else:
        doc = json.loads(pathlib.Path(choice).read_text())
    print(
        f'NV (page 30): {doc.get("source", {}).get("layout", "json")} -> ours: network '
        f'{"yes" if doc.get("network") else "no"}, {len(doc.get("sensors", []))} sensors, '
        f'join {"yes" if doc.get("join") else "no"}'
        + (', cfg yes' if doc.get('cfg') else '')
        + (f' (JSON: {save_json})' if save_json and choice == 'migrate' else '')
    )
    for d in doc.get('dropped', []):
        print(f'  dropped {d["what"]}: {d["why"]}')
    return nvows.import_(doc)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    sub = ap.add_subparsers(dest='cmd', required=True)
    e = sub.add_parser('export')
    e.add_argument('page')
    e.add_argument('-o', '--out')
    i = sub.add_parser('import')
    i.add_argument('json')
    i.add_argument('-o', '--out', required=True)
    a = ap.parse_args()
    if a.cmd == 'export':
        doc = nvows.export(page_of(a.page))
        text = json.dumps(doc, indent=2) + '\n'
        if a.out:
            pathlib.Path(a.out).write_text(text)
        else:
            sys.stdout.write(text)
        n = len(doc['sensors'])
        print(
            f'{doc["source"]["layout"]} page: network {"yes" if doc["network"] else "no"}, {n} sensors, '
            f'join {"yes" if doc["join"] else "no"}' + (', cfg yes' if doc.get('cfg') else ''),
            file=sys.stderr,
        )
        for d in doc['dropped']:
            print(f'  dropped {d["what"]}: {d["why"]}', file=sys.stderr)
        for r in doc['unrecognised']:
            print(f'  not imported: item {r["item"]}/{r["sub"]} {r["data"]}', file=sys.stderr)
    else:
        page = nvows.import_(json.loads(pathlib.Path(a.json).read_text()))
        pathlib.Path(a.out).write_bytes(page)
        print(f'{a.out}: {len(nvows.records(page))} records', file=sys.stderr)


if __name__ == '__main__':
    main()
