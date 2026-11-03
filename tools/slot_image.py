#!/usr/bin/env python3
"""Fill in the header of an OTA slot image (docs/design/ota.md, firmware/common/boot_slot.h): the raw binary of a slot
build (starting at the slot base: 256 bytes of 0xFF, then the vector table) -> the image a slot holds.

    slot_image.py RAW.bin OUT.img --board-name "LAUNCHXL-CC1310" --version 123
"""

import argparse
import binascii
import pathlib
import struct

MAGIC, OFFSET = 0x4953574F, 0x100
SLOT_BASES = (0x01000, 0x0F000)
SLOT_SIZE = 0xE000


def infer_slot(reset, image_len):
    if not (reset & 1):
        raise ValueError(f'reset vector {reset:#010x} lacks Thumb bit')
    entry = reset & ~1
    for idx, base in enumerate(SLOT_BASES):
        start = base + OFFSET
        if start <= entry < min(base + SLOT_SIZE, start + image_len):
            return idx
    raise ValueError(f'reset vector {reset:#010x} is outside slot A and slot B image ranges')


def make(raw, board_name, version, slot=None):
    assert raw[:OFFSET] == b'\xff' * OFFSET, 'no blank header in front: not a slot build'
    image = raw[OFFSET:]
    if len(image) < 8 or len(image) > SLOT_SIZE - OFFSET:
        raise ValueError(f'invalid slot image size: {len(image)} bytes')
    sp, reset = struct.unpack_from('<II', image)
    assert 0x20000000 < sp <= 0x20005000 and (sp & 3) == 0, 'no valid initial SP at the image start'
    inferred = infer_slot(reset, len(image))
    if slot is not None:
        want = {'a': 0, '0': 0, 0: 0, 'b': 1, '1': 1, 1: 1}[slot]
        if want != inferred:
            raise ValueError(f'image is linked for slot {"AB"[inferred]}, not slot {"AB"[want]}')
    header = struct.pack(
        '<6I',
        MAGIC,
        binascii.crc32(board_name.encode()),
        version,
        len(image),
        binascii.crc32(image),
        inferred,
    )
    return header + raw[len(header) :]


def validate_builds(builds):
    """Verify [slot_a_img, slot_b_img] headers and ARM vector tables match their target slot ranges."""
    if len(builds) != 2:
        raise ValueError('expected exactly 2 slot images (slot A and slot B)')
    meta = []
    for idx, img in enumerate(builds):
        label = 'AB'[idx]
        if len(img) < OFFSET + 8:
            raise ValueError(f'slot {label} image is too short ({len(img)} bytes)')
        magic, board_id, version, size, crc, hdr_slot = struct.unpack_from('<6I', img, 0)
        if magic != MAGIC:
            raise ValueError(f'slot {label} image lacks OWSI header magic')
        if size < 8 or size > SLOT_SIZE - OFFSET or len(img) != OFFSET + size:
            raise ValueError(f'slot {label} image size mismatch (header={size}, actual={len(img) - OFFSET})')
        if binascii.crc32(img[OFFSET:]) != crc:
            raise ValueError(f'slot {label} image CRC-32 mismatch')
        if hdr_slot not in (idx, 0xFFFFFFFF):
            stamped = 'AB'[hdr_slot] if hdr_slot < 2 else hdr_slot
            raise ValueError(f'slot {label} argument contains an image stamped for slot {stamped}')
        sp, reset = struct.unpack_from('<II', img, OFFSET)
        if not (0x20000000 < sp <= 0x20005000 and (sp & 3) == 0):
            raise ValueError(f'slot {label} image has invalid initial SP {sp:#010x}')
        if infer_slot(reset, size) != idx:
            raise ValueError(f'slot {label} image reset vector {reset:#010x} is not linked for slot {label}')
        meta.append((board_id, version))
    if meta[0] != meta[1]:
        raise ValueError(f'slot A and slot B images disagree on (board_id, version): {meta[0]} vs {meta[1]}')


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('raw')
    ap.add_argument('out')
    ap.add_argument('--board-name', required=True)
    ap.add_argument('--version', type=int, required=True)
    ap.add_argument('--slot', choices=['a', 'b', '0', '1'])
    a = ap.parse_args()
    out = make(pathlib.Path(a.raw).read_bytes(), a.board_name, a.version, slot=a.slot)
    pathlib.Path(a.out).write_bytes(out)


if __name__ == '__main__':
    main()
