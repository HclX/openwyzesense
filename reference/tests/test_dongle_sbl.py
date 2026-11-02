# tools/dongle_sbl.py against a simulated dongle: the CH554's HID framing and TI's ROM serial bootloader over a
# 128 KiB flash (erase -> 0xFF, programming only clears bits). The point: the tool never touches pages 30-31 (the
# sensor table, and the CCFG whose BL_CONFIG keeps the bootloader reachable), and resets the chip only after
# verifying what it wrote.
import binascii
import hashlib
import json
import pathlib
import sys

import pytest

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import dongle_sbl as sbl  # noqa: E402
import nv_json  # noqa: E402
import stock_like  # noqa: E402
from wyzesense_ref import nvows  # noqa: E402

# the spare dongle's backup (ground_truth/firmware/dongle_77A85A36_2026-10-23_before_ours) is not published: a
# stand-in with its CCFG and its stock sensor table (stock_like.py)


class FakeDongle:
    """What the tool sees through /dev/hidraw: reports of a length byte + bytes; SBL behind it."""

    def __init__(self, flash):
        self.flash = bytearray(flash)
        self.out = []  # reports waiting to be read
        self.update_mode = False
        self.status = 0x40
        self.download = None  # [next address, bytes left]
        self.erased, self.resets, self.corrupt_at = [], 0, None
        self.log = []

    def _report(self, data):  # the CH554: a long response spreads over several reports
        for i in range(0, len(data), 62):
            self.out.append((bytes([len(data[i : i + 62])]) + data[i : i + 62]).ljust(0x3F, b'\0'))

    def write(self, data):
        data = bytes(data)
        if not self.update_mode:
            assert data == sbl.UPDATE_MODE_REQUEST
            self.update_mode = True
            self._report(bytes.fromhex('55aa4303130158'))
            return len(data)
        if data[0] == 0x00 and len(data) == 2:
            return 2  # the host's ACK of a response
        size, cks, cmd, payload = data[0], data[1], data[2], data[3 : data[0]]
        assert size == len(data) and cks == (cmd + sum(payload)) & 0xFF
        self.log.append(cmd)
        rsp = self.command(cmd, payload)
        if rsp is None:
            self._report(b'\x00\x33')
        elif rsp is True:
            self._report(b'\x00\xcc')
        else:
            self._report(b'\x00\xcc' + bytes([len(rsp) + 2, sum(rsp) & 0xFF]) + rsp)
        return len(data)

    def read(self, n):
        return self.out.pop(0)

    def command(self, cmd, p):
        def u32(b):
            return int.from_bytes(b, 'big')

        if cmd in (sbl.CMD_AUTO_BAUD, sbl.CMD_PING):
            return True
        if cmd == sbl.CMD_GET_CHIP_ID:
            return bytes.fromhex('20018000')
        if cmd == sbl.CMD_GET_STATUS:
            return bytes([self.status])
        if cmd == sbl.CMD_RESET:
            self.resets += 1
            return True
        if cmd == sbl.CMD_CRC32:
            a, n = u32(p[:4]), u32(p[4:8])
            return binascii.crc32(self.flash[a : a + n]).to_bytes(4, 'big')
        if cmd == sbl.CMD_MEMORY_READ:
            a, words = u32(p[:4]), p[5]
            return bytes(self.flash[a : a + 4 * words]) if a < len(self.flash) else bytes(4 * words)
        if cmd == sbl.CMD_SECTOR_ERASE:
            a = u32(p[:4])
            self.erased.append(a // 0x1000)
            self.flash[a : a + 0x1000] = b'\xff' * 0x1000
            self.status = 0x40
            return True
        if cmd == sbl.CMD_DOWNLOAD:
            self.download = [u32(p[:4]), u32(p[4:8])]
            self.status = 0x40
            return True
        if cmd == sbl.CMD_SEND_DATA:
            a, left = self.download
            for i, b in enumerate(p):
                if self.corrupt_at == a + i:
                    b ^= 0x10
                self.flash[a + i] &= b
            self.download = [a + len(p), left - len(p)]
            return True
        raise AssertionError(f'the tool sent command {cmd:#x}')


@pytest.fixture
def backup(tmp_path):
    d = stock_like.dongle_backup(tmp_path / 'backup')
    return json.loads((d / 'info.json').read_text()), (d / 'flash.bin').read_bytes()


def our_image():
    img = ROOT / 'build' / 'fw' / 'firmware' / 'gateway-dongle-app.bin'
    if img.exists():
        return img.read_bytes()
    # without a firmware build: a stand-in with a plausible vector table
    return (0x20005000).to_bytes(4, 'little') + (0x101).to_bytes(4, 'little') + bytes(range(256)) * 100


def test_flash_writes_only_pages_below_30_and_verifies(backup):
    info, flash = backup
    dev = FakeDongle(flash)
    image = sbl.check_image(our_image())
    sbl.write_pages(dev, image, info, flash)
    assert bytes(dev.flash[: len(image)]) == image
    assert bytes(dev.flash[sbl.APP_END :]) == flash[sbl.APP_END :]  # pages 30-31 untouched
    assert max(dev.erased) < 30 and dev.erased == list(range(len(dev.erased)))
    assert dev.resets == 1
    assert sbl.CMD_SECTOR_ERASE in dev.log and not {0x2B, 0x2C, 0x2D} & set(dev.log)


def test_restore_puts_stock_back(backup):
    info, flash = backup
    dev = FakeDongle(flash)
    sbl.write_pages(dev, sbl.check_image(our_image()), info, flash)
    dev.update_mode = False
    sbl.write_pages(dev, sbl.check_image(flash[: sbl.APP_END]), info, flash)
    assert bytes(dev.flash) == flash


# Page 30 in our NV format (#61): migrated from the stock table, wiped, or kept; restore puts the backup's back;
# page 31 is never erased
def nv_of(dev):
    return bytes(dev.flash[sbl.NV_PAGE : sbl.NV_PAGE + sbl.PAGE])


def test_flash_migrates_the_stock_table(backup, tmp_path):
    info, flash = backup
    dev = FakeDongle(flash)
    js = tmp_path / 'nv.json'
    sbl.write_pages(
        dev, sbl.check_image(our_image()), info, flash, nv=lambda cur: nv_json.nv_page_for('migrate', cur, js)
    )
    page = nv_of(dev)
    assert nvows.is_ows(page)
    doc = nvows.export(page)
    assert doc['network'] and len(doc['sensors']) == 2 and doc['dropped'] == []  # the shared 0x0003 dropped on the way
    assert json.loads(js.read_text())['dropped'][0]['why'].startswith('shares short address 0003 with')
    assert bytes(dev.flash[0x1F000:]) == flash[0x1F000:] and 31 not in dev.erased and 30 in dev.erased
    assert dev.resets == 1


@pytest.mark.parametrize('choice', ['wipe', 'keep'])
def test_flash_wipes_or_keeps_page_30(backup, choice):
    info, flash = backup
    dev = FakeDongle(flash)
    sbl.write_pages(dev, sbl.check_image(our_image()), info, flash, nv=lambda cur: nv_json.nv_page_for(choice, cur))
    want = b'\xff' * sbl.PAGE if choice == 'wipe' else flash[sbl.NV_PAGE : sbl.NV_PAGE + sbl.PAGE]
    assert nv_of(dev) == want and bytes(dev.flash[0x1F000:]) == flash[0x1F000:]


def test_restore_after_a_migration_is_stock_again(backup, tmp_path):
    info, flash = backup
    dev = FakeDongle(flash)
    sbl.write_pages(
        dev,
        sbl.check_image(our_image()),
        info,
        flash,
        nv=lambda cur: nv_json.nv_page_for('migrate', cur, tmp_path / 'nv.json'),
    )
    dev.update_mode = False
    sbl.write_pages(
        dev,
        sbl.check_image(flash[: sbl.APP_END]),
        info,
        flash,
        nv=lambda _: flash[sbl.NV_PAGE : sbl.NV_PAGE + sbl.PAGE],
    )
    assert bytes(dev.flash) == flash


def test_page_30_only_through_write_nv_and_page_31_never():
    with pytest.raises(ValueError):
        sbl.below_app_end(sbl.NV_PAGE, sbl.PAGE)  # outside write_nv
    sbl._nv_writing = True
    try:
        sbl.below_app_end(sbl.NV_PAGE, sbl.PAGE)
        for addr, size in ((0x1F000, 4), (sbl.NV_PAGE, sbl.PAGE + 4), (0x1FFA8, 88)):
            with pytest.raises(ValueError):
                sbl.below_app_end(addr, size)
    finally:
        sbl._nv_writing = False


def test_a_bad_write_is_caught_before_the_reset(backup):
    info, flash = backup
    dev = FakeDongle(flash)
    dev.corrupt_at = 0x123
    with pytest.raises(AssertionError, match='CRC'):
        sbl.write_pages(dev, sbl.check_image(our_image()), info, flash)
    assert dev.resets == 0 and bytes(dev.flash[sbl.APP_END :]) == flash[sbl.APP_END :]


def test_refusals(backup):
    info, flash = backup
    with pytest.raises(ValueError):
        sbl.check_image(b'\x00' * (sbl.APP_END + 4))  # too big for pages 0-29
    with pytest.raises(ValueError):
        sbl.check_image(b'\xff' * 1024)  # no vector table
    with pytest.raises(ValueError):
        sbl.below_app_end(0x1E000, 0x1000)  # page 30
    with pytest.raises(ValueError):
        sbl.below_app_end(0x1F000, 0x1000)  # page 31
    with pytest.raises(ValueError):
        sbl.below_app_end(0x1D000, 0x2000)  # runs into page 30
    dev = FakeDongle(flash)
    with pytest.raises(ValueError):
        sbl.sbl_cmd(dev, sbl.CMD_SECTOR_ERASE, (0x1F000).to_bytes(4, 'big'))  # the last guard, at the chip
    with pytest.raises(AssertionError):
        sbl.sbl_cmd(dev, 0x2C)  # bank erase: never
    other = dict(info, chip_id='20018001')
    with pytest.raises(AssertionError, match='chip ID'):
        sbl.write_pages(FakeDongle(flash), sbl.check_image(our_image()), other, flash)
    changed = bytearray(flash)
    changed[0x1FF00] ^= 1  # a page 31 we do not know
    with pytest.raises(AssertionError, match='page 31'):
        sbl.write_pages(FakeDongle(changed), sbl.check_image(our_image()), info, flash)


def test_the_full_image_is_refused():
    # <product>.bin runs to the CCFG at the end of flash: never something to write
    full = ROOT / 'build' / 'fw' / 'firmware' / 'gateway-dongle.bin'
    if not full.exists():
        pytest.skip('no firmware build')
    with pytest.raises(ValueError, match='app.bin'):
        sbl.check_image(full.read_bytes())


def test_the_backup_is_sound(tmp_path):
    info, flash = sbl.check_backup(stock_like.dongle_backup(tmp_path / 'backup'))
    assert hashlib.sha256(flash).hexdigest() == info['flash.bin']['sha256'] and info['reset_source'] == 'pin reset'


def holed_image():
    """Our image with an all-FF kilobyte in its middle, as a fast write skips."""
    img = bytearray(sbl.check_image(our_image()))
    img[0x2000:0x2400] = b'\xff' * 0x400
    return bytes(img)


def test_fast_write_is_the_same_flash_with_fewer_round_trips(backup):
    info, flash = backup
    image = holed_image()
    slow, fast = FakeDongle(flash), FakeDongle(flash)
    sbl.write_pages(slow, image, info, flash)
    phases = sbl.write_pages(fast, image, info, flash, fast=True)
    assert bytes(fast.flash) == bytes(slow.flash) and bytes(fast.flash[: len(image)]) == image
    assert bytes(fast.flash[sbl.APP_END :]) == flash[sbl.APP_END :]
    assert fast.log.count(sbl.CMD_DOWNLOAD) == 2  # the FF kilobyte skipped
    assert phases['bytes_sent'] == len(image) - 0x400
    assert fast.log.count(sbl.CMD_GET_STATUS) < slow.log.count(sbl.CMD_GET_STATUS) / 3
    assert sbl.CMD_MEMORY_READ not in fast.log and fast.resets == 1  # verified by CRC only


def test_fast_write_still_catches_a_bad_byte(backup):
    info, flash = backup
    dev = FakeDongle(flash)
    dev.corrupt_at = 0x3001
    with pytest.raises(AssertionError, match='CRC'):
        sbl.write_pages(dev, holed_image(), info, flash, fast=True)
    assert dev.resets == 0


def test_runs():
    img = b'\x01' * 8 + b'\xff' * 300 + b'\x02' * 4 + b'\xff' * 100 + b'\x03' * 4
    assert [(a, len(d)) for a, d in sbl.runs(img, True)] == [(0, 8), (308, 108)]  # gap 100 < 256: merged
    assert sbl.runs(img, False) == [(0, img)]
