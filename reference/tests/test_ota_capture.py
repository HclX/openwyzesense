# The OTA update captured on the host side (ground_truth/host/2026-10-24_ota_launchpad_over_spare_dongle): our
# gateway's 0x53F4-0x53F8 traffic as firmware/common/ota.h describes it, and the bytes served making up exactly the
# slot-B image the sensor wrote.
import binascii
import json
import pathlib
import struct

from wyzesense_ref import host

D = pathlib.Path(__file__).resolve().parents[2] / 'ground_truth' / 'host' / '2026-10-24_ota_launchpad_over_spare_dongle'
MAC = b'77FED830'


def packets():
    for line in (D / 'dongle_host.jsonl').read_text().splitlines():
        r = json.loads(line)
        m = host.parse(bytes.fromhex(r['proto_hex']))
        m['dir'] = r['dir']
        yield m


def test_the_offer_carries_both_builds_headers():
    imgs = [(D / f'slot-{s}-v53.img').read_bytes() for s in 'ab']
    offers = [m['payload'] for m in packets() if m['cmd'] == host.OWS_OTA_OFFER and m['dir'] == 'W']
    assert len(offers) == 1
    o = offers[0]
    assert o[:8] == MAC
    magic, board, version, _, _ = struct.unpack_from('<5I', imgs[0])
    assert magic == 0x4953574F and struct.unpack('>II', o[8:16]) == (board, version) == (board, 53)
    for s, img in enumerate(imgs):
        size, crc = struct.unpack_from('<II', img, 12)
        assert struct.unpack('>II', o[16 + 8 * s : 24 + 8 * s]) == (size, crc)
        assert size == len(img) - 0x100 and binascii.crc32(img[0x100:]) == crc


def test_the_served_bytes_are_the_slot_b_build_and_it_took():
    img = (D / 'slot-b-v53.img').read_bytes()
    got = bytearray(len(img))
    needs, seen = 0, set()
    for m in packets():
        p = m['payload']
        if m['cmd'] == host.OWS_OTA_NEED and m['dir'] == 'R':
            assert p[:8] == MAC and p[8] == 1  # the sensor ran from A: it asked for the B build
            needs += 1
        elif m['cmd'] == host.OWS_OTA_DATA and m['dir'] == 'W':
            off = int.from_bytes(p[:4], 'big')
            assert len(p) - 4 <= 32  # pieces small enough for the dongle's USB link
            got[off : off + len(p) - 4] = p[4:]
            seen.update(range(off, off + len(p) - 4))
    assert seen == set(range(len(img))) and bytes(got) == img
    assert needs >= (len(img) + 95) // 96
    done = [m['payload'] for m in packets() if m['cmd'] == host.OWS_OTA_DONE and m['dir'] == 'R']
    assert done and done[0][:8] == MAC and done[0][8] == 0 and int.from_bytes(done[0][9:13], 'big') == 53
    log = (D / 'launchpad_uart.log').read_text()
    assert 'ota offer 53, slot B, 36132 bytes: taken' in log and 'slot B v53' in log and 'image confirmed' in log
