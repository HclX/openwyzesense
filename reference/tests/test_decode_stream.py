# Tests for tools/decode_stream.py (IQ session decoding and burst snipping) and tools/slot_image.py CLI / validation.
import json
import pathlib
import shutil
import struct
import sys

import pytest
from test_reference import A3_ANNOUNCE, ASSOC_REQ, ASSOC_RESP

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import decode_stream  # noqa: E402
import slot_image  # noqa: E402

JOIN_SNIP = ROOT / 'ground_truth' / 'rf' / '2026-10-22_1821_906.7M_250k_g35' / 'snip_182141_059.cu8'


def test_decode_stream_session_and_snips(tmp_path, monkeypatch):
    shutil.copyfile(JOIN_SNIP, tmp_path / 'stream.cu8')
    (tmp_path / 'start_time.txt').write_text('1792693300.0\n')

    monkeypatch.setattr(sys, 'argv', ['decode_stream.py', str(tmp_path)])
    decode_stream.main()
    jsonl = tmp_path / 'decoded_frames.jsonl'
    rows = [json.loads(line) for line in jsonl.read_text().splitlines()]
    psdus = {r['psdu'] for r in rows if r['crc_ok']}
    assert ASSOC_REQ in psdus and ASSOC_RESP in psdus and A3_ANNOUNCE in psdus
    assert any(len(r.get('src', '')) == 16 for r in rows)  # extended IEEE src in ASSOC_REQ
    assert any(len(r.get('dst', '')) == 4 for r in rows)  # short dst in data / command frames
    assert not list(tmp_path.glob('snip_*.cu8'))

    monkeypatch.setattr(sys, 'argv', ['decode_stream.py', str(tmp_path), '--snips'])
    decode_stream.main()
    snips = list(tmp_path.glob('snip_*.cu8'))
    assert len(snips) >= 1 and all(s.stat().st_size > 0 for s in snips)


def _raw_slot(slot_idx, payload_size=64, sp=0x20005000, thumb=1):
    entry = (slot_image.SLOT_BASES[slot_idx] + slot_image.OFFSET + 16) | thumb
    vec = struct.pack('<II', sp, entry)
    return b'\xff' * slot_image.OFFSET + vec + b'\x5a' * (payload_size - len(vec))


def test_slot_image_cli_and_validation_guards(tmp_path, monkeypatch):
    raw_a = tmp_path / 'a.raw'
    out_a = tmp_path / 'a.img'
    raw_b = tmp_path / 'b.raw'
    out_b = tmp_path / 'b.img'
    raw_a.write_bytes(_raw_slot(0))
    raw_b.write_bytes(_raw_slot(1))

    monkeypatch.setattr(
        sys,
        'argv',
        ['slot_image.py', str(raw_a), str(out_a), '--board-name', 'LAUNCHXL-CC1310', '--version', '7', '--slot', 'a'],
    )
    slot_image.main()
    monkeypatch.setattr(
        sys,
        'argv',
        ['slot_image.py', str(raw_b), str(out_b), '--board-name', 'LAUNCHXL-CC1310', '--version', '7', '--slot', 'b'],
    )
    slot_image.main()

    img_a = out_a.read_bytes()
    img_b = out_b.read_bytes()
    slot_image.validate_builds([img_a, img_b])

    with pytest.raises(ValueError, match='lacks Thumb bit'):
        slot_image.make(_raw_slot(0, thumb=0), 'LAUNCHXL-CC1310', 7)
    with pytest.raises(ValueError, match='outside slot A and slot B'):
        bad = b'\xff' * slot_image.OFFSET + struct.pack('<II', 0x20005000, 0x00101) + b'\x00' * 56
        slot_image.make(bad, 'LAUNCHXL-CC1310', 7)
    with pytest.raises(ValueError, match='invalid slot image size'):
        slot_image.make(b'\xff' * slot_image.OFFSET + b'\x00' * 4, 'LAUNCHXL-CC1310', 7)
    with pytest.raises(ValueError, match='linked for slot A, not slot B'):
        slot_image.make(_raw_slot(0), 'LAUNCHXL-CC1310', 7, slot='b')

    with pytest.raises(ValueError, match='exactly 2'):
        slot_image.validate_builds([img_a])
    with pytest.raises(ValueError, match='too short'):
        slot_image.validate_builds([b'short', img_b])
    with pytest.raises(ValueError, match='magic'):
        slot_image.validate_builds([b'\x00' * len(img_a), img_b])
    with pytest.raises(ValueError, match='size mismatch'):
        slot_image.validate_builds([img_a + b'\x00', img_b])
    with pytest.raises(ValueError, match='CRC-32'):
        slot_image.validate_builds([img_a[:-1] + bytes([img_a[-1] ^ 1]), img_b])
    with pytest.raises(ValueError, match='stamped for slot'):
        slot_image.validate_builds([img_b, img_b])
    img_b_v8 = slot_image.make(_raw_slot(1), 'LAUNCHXL-CC1310', 8)
    with pytest.raises(ValueError, match='disagree'):
        slot_image.validate_builds([img_a, img_b_v8])
