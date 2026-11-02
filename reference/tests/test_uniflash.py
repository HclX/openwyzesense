# Safety and UX tests for tools/uniflash.py, tools/usb_dev.py, tools/dongle_sbl.py, tools/host_link.py, and udev rules
# (TASK-18): simulated 128 KB CC1310 flash behind DSLite (no hardware touched).
import pathlib
import struct
import sys

import pytest

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import dongle_sbl  # noqa: E402
import host_link  # noqa: E402
import slot_image  # noqa: E402
import stock_like  # noqa: E402
import uniflash  # noqa: E402
import usb_dev  # noqa: E402

CONTACT_FLASH, CONTACT_FCFG1 = stock_like.contact_flash(), stock_like.FCFG1  # the unit's dump is not published


class FakeDSLiteChip:
    def __init__(self, flash_bytes, fcfg1_bytes):
        self.flash = bytearray(flash_bytes)
        self.fcfg1 = bytes(fcfg1_bytes)
        self.resets = 0
        self.writes = []

    def dslite(self, *args, timeout=300):
        cmd = args[0]
        if cmd == 'memory':
            rng = args[args.index('--range') + 1]
            out = pathlib.Path(args[args.index('--output') + 1])
            addr_s, words_s = rng.split(',')
            addr, nbytes = int(addr_s, 0), int(words_s) * 4
            if addr >= 0x50001000:
                off = addr - 0x50001000
                out.write_bytes(self.fcfg1[off : off + nbytes])
            else:
                out.write_bytes(bytes(self.flash[addr : addr + nbytes]))
            return 'ok'
        if cmd == 'flash':
            if '-r' in args:
                self.resets += 1
                return 'reset ok'
            spec = args[-1]
            img_path, addr_s = spec.rsplit(',', 1)
            addr = int(addr_s, 0)
            data = pathlib.Path(img_path).read_bytes()
            # Necessary Sectors Only erase + program
            first_page = (addr // 0x1000) * 0x1000
            last_page = ((addr + len(data) - 1) // 0x1000) * 0x1000
            for p in range(first_page, last_page + 0x1000, 0x1000):
                self.flash[p : p + 0x1000] = b'\xff' * 0x1000
            self.flash[addr : addr + len(data)] = data
            self.writes.append((addr, len(data)))
            return 'flash ok'
        raise AssertionError(f'unexpected dslite call: {args}')


def _make_build_dir(tmp_path):
    bdir = tmp_path / 'build'
    bdir.mkdir()
    boot = struct.pack('<II', 0x20004000, 0x101) + b'\x12' * 512
    raw_a = b'\xff' * 0x100 + struct.pack('<II', 0x20004000, 0x01101) + b'\x34' * 512
    slot_a = slot_image.make(raw_a, 'WYZE-CONTACT-V1', 99, slot='a')
    gw_app = struct.pack('<II', 0x20004000, 0x101) + b'\x56' * 1024
    boot_p = bdir / 'boot-wyze_contact_v1.bin'
    slot_a_p = bdir / 'sensor-contact-v1-slot-a.img'
    gw_app_p = bdir / 'gateway-contact-v1-app.bin'
    boot_p.write_bytes(boot)
    slot_a_p.write_bytes(slot_a)
    gw_app_p.write_bytes(gw_app)
    return bdir, boot_p, slot_a_p, gw_app_p


def test_cli_requires_backup_or_explicit_override(tmp_path, monkeypatch):
    bdir, boot, slot_a, gw_app = _make_build_dir(tmp_path)
    chip = FakeDSLiteChip(CONTACT_FLASH, CONTACT_FCFG1)
    monkeypatch.setattr(uniflash, 'dslite', chip.dslite)

    with pytest.raises(SystemExit):
        uniflash.main(['flash', str(gw_app)])
    with pytest.raises(SystemExit):
        uniflash.main(['slots', str(boot), str(slot_a)])
    assert chip.writes == []

    # Wrong MAC in backup is rejected before any flash write
    wrong_bak = tmp_path / 'wrong_bak'
    wrong_bak.mkdir()
    bad_fl = bytearray(CONTACT_FLASH)
    bad_fl[0x1FFCC:0x1FFD4] = b'77000001'
    (wrong_bak / 'flash.bin').write_bytes(bad_fl)
    with pytest.raises(ValueError, match='does not match connected chip MAC 777B42DF'):
        uniflash.main(['flash', str(gw_app), '--backup', str(wrong_bak)])
    assert chip.writes == []

    # Matching backup succeeds, and still succeeds after enable-bootloader changes BL_CONFIG
    good_bak = tmp_path / 'good_bak'
    uniflash.main(['backup', str(good_bak)])
    uniflash.main(['enable-bootloader', 'w', '--yes'])
    uniflash.main(['slots', str(boot), str(slot_a), '--backup', str(good_bak)])
    assert bytes(chip.flash[:0x10000]) == uniflash.slots_image(boot, slot_a)
    assert chip.flash[0x1FFCC:0x1FFD4] == b'777B42DF'


def test_provision_sensor_and_gateway_preserve_original_backup(tmp_path, monkeypatch):
    bdir, boot, slot_a, gw_app = _make_build_dir(tmp_path)
    orig_flash = CONTACT_FLASH
    chip = FakeDSLiteChip(orig_flash, CONTACT_FCFG1)
    monkeypatch.setattr(uniflash, 'dslite', chip.dslite)

    backups = tmp_path / 'backups'
    info = uniflash.provision('sensor', backup_root=backups, build_dir=bdir)
    assert info['mac'] == '777B42DF' and info['backed_up_now'] is True and info['bootloader'] == 'w'
    saved = (backups / '777B42DF' / 'flash.bin').read_bytes()
    assert saved == orig_flash  # full 128 KB stock image backed up before flashing
    assert int.from_bytes(chip.flash[0x1FFA8 + 0x30 : 0x1FFA8 + 0x34], 'little') == 0xC5FE0AC5  # DIO10 W pad enabled
    assert chip.flash[0x1FFCC:0x1FFD4] == b'777B42DF'

    # Re-provisioning the same unit as a gateway verifies and preserves the original stock backup without clobbering it
    info2 = uniflash.provision('gateway', backup_root=backups, build_dir=bdir)
    assert info2['backed_up_now'] is False
    assert (backups / '777B42DF' / 'flash.bin').read_bytes() == orig_flash
    assert bytes(chip.flash[: len(gw_app.read_bytes())]) == gw_app.read_bytes()

    # Zero-MAC chip is refused before touching flash
    zero_fl = bytearray(orig_flash)
    zero_fl[0x1FFCC:0x1FFD4] = b'00000000'
    bad_chip = FakeDSLiteChip(zero_fl, CONTACT_FCFG1)
    monkeypatch.setattr(uniflash, 'dslite', bad_chip.dslite)
    with pytest.raises(RuntimeError, match='no valid 8-hex-digit MAC'):
        uniflash.provision('sensor', backup_root=tmp_path / 'b2', build_dir=bdir)
    assert bad_chip.writes == []


def test_vid_pid_sysfs_filtering_and_udev_rule(tmp_path, monkeypatch):
    hid_sys = tmp_path / 'sys_hidraw'
    tty_sys = tmp_path / 'sys_tty'
    dev_root = tmp_path / 'dev'
    for d in (hid_sys, tty_sys, dev_root):
        d.mkdir()

    # hidraw0 = USB keyboard (046d:c52b), hidraw2 = Wyze USB dongle (1a86:e024)
    (hid_sys / 'hidraw0' / 'device').mkdir(parents=True)
    (hid_sys / 'hidraw0' / 'device' / 'uevent').write_text('HID_ID=0003:0000046D:0000C52B\n')
    (hid_sys / 'hidraw2' / 'device').mkdir(parents=True)
    (hid_sys / 'hidraw2' / 'device' / 'uevent').write_text('HID_ID=0003:00001A86:0000E024\n')

    # ttyACM0 = unrelated modem (2341:0043), ttyACM1 = XDS110 aux (if03), ttyACM2 = XDS110 user UART (0451:bef3 if00)
    for name, vid, pid, ifnum in (
        ('ttyACM0', '2341', '0043', '00'),
        ('ttyACM1', '0451', 'bef3', '03'),
        ('ttyACM2', '0451', 'bef3', '00'),
    ):
        usb_parent = tmp_path / f'usb_{name}'
        iface = usb_parent / 'iface'
        iface.mkdir(parents=True)
        (usb_parent / 'idVendor').write_text(vid + '\n')
        (usb_parent / 'idProduct').write_text(pid + '\n')
        (iface / 'bInterfaceNumber').write_text(ifnum + '\n')
        (tty_sys / name).mkdir()
        (tty_sys / name / 'device').symlink_to(iface)

    assert usb_dev.find_hidraw(sysfs=hid_sys, dev_root=dev_root) == str(dev_root / 'hidraw2')
    assert dongle_sbl.find_dongle(sysfs=hid_sys, dev_root=dev_root) == str(dev_root / 'hidraw2')
    assert usb_dev.find_tty(sysfs=tty_sys, dev_root=dev_root) == str(dev_root / 'ttyACM2')
    assert host_link.resolve_port('dongle', hidraw_sysfs=hid_sys, tty_sysfs=tty_sys, dev_root=dev_root) == str(
        dev_root / 'hidraw2'
    )
    assert host_link.resolve_port('launchpad', hidraw_sysfs=hid_sys, tty_sysfs=tty_sys, dev_root=dev_root) == str(
        dev_root / 'ttyACM2'
    )
    assert host_link.resolve_port(None, hidraw_sysfs=hid_sys, tty_sysfs=tty_sys, dev_root=dev_root) == str(
        dev_root / 'ttyACM2'
    )
    # Without LaunchPad, auto port falls back to Wyze dongle (never hidraw0 keyboard!)
    assert host_link.resolve_port(None, hidraw_sysfs=hid_sys, tty_sysfs=tmp_path / 'empty', dev_root=dev_root) == str(
        dev_root / 'hidraw2'
    )

    # Verify dongle_sbl.finish() prints immediate replug instructions instead of blocking 2 minutes
    monkeypatch.setattr(dongle_sbl, 'back_to_normal', lambda *a, **kw: False)
    dongle_sbl.finish('/dev/null', wait=False)

    udev = (ROOT / 'tools' / 'udev' / '99-wyze-dongle.rules').read_text()
    assert '1a86' in udev and 'e024' in udev and 'uaccess' in udev
