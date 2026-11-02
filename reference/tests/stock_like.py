# Stand-ins for a v1 contact's stock dump (not published, ground_truth/README.md) in the tool tests: its CCFG
# (vectors/ccfg/contact_v1_777B42DF_2026-10-24.ccfg.bin: the 88 configuration bytes of page 31 as the unit came,
# bootloader off, MAC 777B42DF) at the end of a 128 KB image, and an FCFG1 page. The tools only read the CCFG and
# FCFG1's user ID.
import pathlib
import random

CCFG = (pathlib.Path(__file__).parent / 'vectors' / 'ccfg' / 'contact_v1_777B42DF_2026-10-24.ccfg.bin').read_bytes()


def contact_flash():
    rnd = random.Random(0x777B42DF)
    vectors = (0x20005000).to_bytes(4, 'little') + (0x101).to_bytes(4, 'little')  # initial SP, reset handler
    code = vectors + bytes(rnd.getrandbits(8) for _ in range(30 * 4096 - len(vectors)))  # pages 0-29: stand-in code
    return code + b'\xff' * 4096 + b'\xff' * (4096 - len(CCFG)) + CCFG  # page 30 (NV) empty, page 31 ends in the CCFG


FCFG1 = bytes(1024)


# The spare dongle 77A85A36 (the test network, PAN 0x65FE): its CCFG (vectors/ccfg/, as it came) and its
# stock sensor table, page 30 (vectors/nv/: 3 test sensors and 777B42DF).
DONGLE_CCFG = (
    pathlib.Path(__file__).parent / 'vectors' / 'ccfg' / 'dongle_77A85A36_2026-10-23_before_ours.ccfg.bin'
).read_bytes()
DONGLE_PAGE30 = (
    pathlib.Path(__file__).parent / 'vectors' / 'nv' / 'dongle_77A85A36_2026-10-23_before_ours.page30.bin'
).read_bytes()


def dongle_flash():
    rnd = random.Random(0x77A85A36)
    vectors = (0x20005000).to_bytes(4, 'little') + (0x101).to_bytes(4, 'little')  # initial SP, reset handler
    code = vectors + bytes(rnd.getrandbits(8) for _ in range(30 * 4096 + 4096 - len(DONGLE_CCFG) - len(vectors)))
    return code[: 30 * 4096] + DONGLE_PAGE30 + code[30 * 4096 :] + DONGLE_CCFG


def dongle_backup(d):
    """A backup folder as ows-flash backup writes it (info.json + flash.bin), of dongle_flash()."""
    import hashlib
    import json

    d = pathlib.Path(d)
    d.mkdir(parents=True, exist_ok=True)
    flash = dongle_flash()
    (d / 'flash.bin').write_bytes(flash)
    info = {
        'reset_source': 'pin reset',
        'update_mode_reply': '55aa4303130158',
        'chip_id': '20018000',
        'flash.bin': {'address': '0x00000000', 'size': len(flash), 'sha256': hashlib.sha256(flash).hexdigest()},
    }
    (d / 'info.json').write_text(json.dumps(info, indent=2))
    return d
