#!/usr/bin/env python3
"""A CC1310 on 2-pin cJTAG (TMS, TCK and RESET_N from a LaunchPad's XDS110: a Wyze v1 contact or motion sensor,
docs/v1-contact.md) through TI UniFlash's DSLite, which OpenOCD cannot replace there (its XDS110 driver does 4-pin
JTAG only).

    uv run python tools/uniflash.py provision --sensor [--board wyze_contact_v1|wyze_motion_v1] [--nv CHOICE]
    uv run python tools/uniflash.py provision --gateway [--nv CHOICE]   # backup -> gateway-contact-v1-app.bin -> SBL w
    uv run python tools/uniflash.py backup DIR          # flash (read twice, compared), CCFG, FCFG1: read-only
    uv run python tools/uniflash.py flash APP.bin --backup DIR [--nv CHOICE]       # pages 0-29 (+ page 30 if --nv)
    uv run python tools/uniflash.py slots BOOT.bin SLOT_A.img --backup DIR [--nv CHOICE]   # boot selector + slot A
    uv run python tools/uniflash.py reset               # a board reset (RESET_N), the debug power domain off
    uv run python tools/uniflash.py enable-bootloader w|button --yes   # the one CCFG write we allow (issue #14)
    uv run python tools/uniflash.py read ADDR WORDS OUT # memory, in 32-bit words

Every operation ends with a **board reset with the debug power domain released** (DSLite -r 2,
KeepDebugPoweredAtDisconnect=false): left powered (the default), our firmware printed its banner after a flash and then
stopped answering until a real power-off; this reset brings it back from that too, without touching the board
(2026-10-24). Page 30 (NV) is untouched by default (`--nv keep`), or updated with `--nv wipe|migrate|NV.json`;
page 31 (CCFG, with the unit's MAC) is never written except `BL_CONFIG` in `enable-bootloader` / `provision`
(`--bootloader w`, default on `wyze_contact_v1`, `keep` on `wyze_motion_v1`).
"""

import argparse
import hashlib
import json
import pathlib
import subprocess
import sys
import tempfile

import nv_json  # tools/: the NV page choices (#61)

ROOT = pathlib.Path(__file__).resolve().parents[1]
DSLITE = ROOT / '.toolchain/uniflash/deskdb/content/TICloudAgent/linux/ccs_base/DebugServer/bin/DSLite'
CONFIG = ROOT / 'firmware/boards/wyze_contact_v1/xds110_2pin.ccxml'
APP_END = 0x1E000  # page 30: the NV page; page 31: code + the CCFG (the MAC)
CLEAN_RESET = ['-s', 'KeepDebugPoweredAtDisconnect=false', '-r', '2']


def dslite(*args, timeout=300):
    if not DSLITE.exists():
        sys.exit(f'{DSLITE} missing: install TI UniFlash into .toolchain/uniflash (docs/v1-contact.md)')
    r = subprocess.run([str(DSLITE), *args], capture_output=True, text=True, timeout=timeout)
    out = r.stdout + r.stderr
    if r.returncode or 'Failed' in out or 'fatal' in out:
        raise RuntimeError(f'DSLite {args[0]} failed:\n{out[-1500:]}')
    return out


def read(addr, words, out, config=CONFIG):
    dslite('memory', '--config', str(config), '--range', f'{addr:#x},{words}', '--output', str(out), '--timeout', '200')
    return pathlib.Path(out).read_bytes()


def reset(config=CONFIG):
    """A board reset through RESET_N, the debug domain released: the chip starts as after a power-up."""
    return dslite('flash', '--config', str(config), *CLEAN_RESET, timeout=120)


NV_PAGE = 0x1E000  # page 30: written only by write_nv() (#61)


def write_nv(page, config=CONFIG):
    """Page 30 <- page (4096 bytes: our NV format, or all FF), read back and compared."""
    if len(page) != 0x1000:
        raise ValueError(f'{len(page)} bytes: page 30 takes exactly 4096')
    with tempfile.TemporaryDirectory() as t:
        p = pathlib.Path(t) / 'nv.bin'
        p.write_bytes(page)
        dslite(
            'flash',
            '--config',
            str(config),
            '-s',
            'FlashEraseSetting=Necessary Sectors Only',
            '-s',
            'VerifyAfterProgramLoad=Full verification',
            f'{p},{NV_PAGE:#x}',
        )
        if read(NV_PAGE, 1024, pathlib.Path(t) / 'back.bin', config) != page:
            raise RuntimeError('page 30 read-back differs')


def nv_step(choice, config=CONFIG, save_json=None):
    """Page 30 by choice (nv_json.nv_page_for: migrate / wipe / keep / an NV.json)."""
    if choice == 'keep':
        return
    with tempfile.TemporaryDirectory() as t:
        current = read(NV_PAGE, 1024, pathlib.Path(t) / 'nv.bin', config)
    page = nv_json.nv_page_for(choice, current, save_json)
    if page is not None:
        write_nv(page, config)


def _valid_hex_mac(mac):
    return len(mac) == 8 and all(c in b'0123456789ABCDEF' for c in mac) and mac != b'00000000'


def read_chip_ccfg(config=CONFIG):
    with tempfile.TemporaryDirectory() as t:
        ccfg = read(CCFG_ADDR, 22, pathlib.Path(t) / 'ccfg.bin', config)
    mac = ccfg[0x24:0x2C]
    if not _valid_hex_mac(mac):
        raise RuntimeError(f'chip CCFG has no valid 8-hex-digit MAC ({mac.hex()}): refusing to touch flash')
    return ccfg, mac.decode('ascii')


def verify_backup(backup_dir, config=CONFIG):
    """Verify backup_dir/flash.bin is a complete 128 KB dump matching the connected chip's CCFG MAC."""
    bdir = pathlib.Path(backup_dir)
    fl_path = bdir / 'flash.bin'
    if not fl_path.is_file():
        raise ValueError(f'{fl_path} missing: run `uniflash.py backup {bdir}` first')
    fl = fl_path.read_bytes()
    if len(fl) != 0x20000:
        raise ValueError(f'{fl_path}: {len(fl)} bytes, expected 131072 (128 KB)')
    meta_path = bdir / 'backup.json'
    if meta_path.is_file():
        meta = json.loads(meta_path.read_text())
        if hashlib.sha256(fl).hexdigest() != meta.get('flash_sha256'):
            raise ValueError(f'{fl_path} SHA-256 does not match {meta_path}')
    ccfg, mac = read_chip_ccfg(config)
    bak_ccfg = fl[CCFG_ADDR:]
    if bak_ccfg[0x24:0x2C].decode(errors='replace') != mac:
        raise ValueError(f'backup MAC {bak_ccfg[0x24:0x2C]!r} does not match connected chip MAC {mac}')
    if bak_ccfg[:BL_CONFIG] + bak_ccfg[BL_CONFIG + 4 :] != ccfg[:BL_CONFIG] + ccfg[BL_CONFIG + 4 :]:
        raise ValueError('connected chip CCFG differs from backup outside BL_CONFIG')
    return mac


def flash(app, config=CONFIG, nv='keep', backup_dir=None):
    data = pathlib.Path(app).read_bytes()
    if not data or len(data) > APP_END:
        raise ValueError(f'{app}: {len(data)} bytes: an app image for pages 0-29 only (never the NV page or the CCFG)')
    if backup_dir is not None:
        verify_backup(backup_dir, config)
    dslite(
        'flash',
        '--config',
        str(config),
        '-s',
        'FlashEraseSetting=Necessary Sectors Only',
        '-s',
        'VerifyAfterProgramLoad=Full verification',
        f'{app},0x0',
    )
    with tempfile.TemporaryDirectory() as t:
        back = read(0, (len(data) + 3) // 4, pathlib.Path(t) / 'back.bin', config)[: len(data)]
    if back != data:
        raise RuntimeError('read-back differs from the image')
    nv_step(nv, config)
    reset(config)


SLOT_A, SLOT_B = 0x01000, 0x0F000


def slots_image(boot, slot_a):
    """The two-slot layout as one image at 0x0: the boot selector (page 0), slot A, and slot B's first page blank (its
    header: whatever an earlier image left there must not look like a slot)."""
    b, a = pathlib.Path(boot).read_bytes(), pathlib.Path(slot_a).read_bytes()
    if len(b) > SLOT_A or len(a) > SLOT_B - SLOT_A or a[:4] != b'OWSI':
        raise ValueError('a boot selector of at most one page and a slot image (tools/slot_image.py) expected')
    img = bytearray(b'\xff' * (SLOT_B + 0x1000))
    img[: len(b)] = b
    img[SLOT_A : SLOT_A + len(a)] = a
    return bytes(img)


def slots(boot, slot_a, config=CONFIG, nv='keep', backup_dir=None):
    """slots_image() flashed and verified."""
    img = slots_image(boot, slot_a)
    if backup_dir is not None:
        verify_backup(backup_dir, config)
    with tempfile.TemporaryDirectory() as t:
        p = pathlib.Path(t) / 'slots.bin'
        p.write_bytes(img)
        flash(p, config, nv)


CCFG_ADDR, PAGE31 = 0x1FFA8, 0x1F000
BL_CONFIG = 0x30  # offset in the CCFG
BACKDOORS = {'w': 10, 'button': 11}  # the ROM bootloader's backdoor: the W pad or the button (both pulled up on board)
MAX_BL_PIN = 14  # 5x5 package: DIO0-14 (TI errata SWRZ062F advisory 13)


def bootloader_ccfg(ccfg, pin):
    """The unit's CCFG with only BL_CONFIG changed: ROM bootloader on, backdoor on DIO<pin>, active low."""
    if len(ccfg) != 88 or not 0 <= pin <= MAX_BL_PIN:
        raise ValueError('an 88-byte CCFG and a backdoor pin 0-14 expected')
    new = bytearray(ccfg)
    new[BL_CONFIG : BL_CONFIG + 4] = (0xC5FE00C5 | pin << 8).to_bytes(4, 'little')

    def w(c, o):
        return int.from_bytes(c[o : o + 4], 'little')

    assert new[:BL_CONFIG] + new[BL_CONFIG + 4 :] == ccfg[:BL_CONFIG] + ccfg[BL_CONFIG + 4 :]  # nothing else
    assert w(new, 0x3C) == 0xFFC500C5, 'the CPU debug port must stay enabled (TAP_DAP_0)'
    assert w(new, 0x44) == 0, 'IMAGE_VALID_CONF must stay 0 (the flash image runs)'
    return bytes(new)


def enable_bootloader(pin, config=CONFIG):
    """The one CCFG write we allow (the user, 2026-10-24, #14): the ROM bootloader enabled, its backdoor DIO<pin>
    active low. Page 31 is read whole (twice, compared) and written back with only BL_CONFIG changed: the stock code
    before the CCFG (3,950 bytes on a v1 contact, used by the stock firmware) and the MAC stay; the whole page is
    read back and compared (#26)."""
    words = (0x20000 - PAGE31) // 4
    with tempfile.TemporaryDirectory() as t:
        t = pathlib.Path(t)
        page = read(PAGE31, words, t / 'page31_a.bin', config)
        if read(PAGE31, words, t / 'page31_b.bin', config) != page:
            raise RuntimeError('two reads of page 31 differ: not writing')
        off = CCFG_ADDR - PAGE31
        new = page[:off] + bootloader_ccfg(page[off:], pin)
        if new == page:
            print('already enabled')
            return
        assert new[:off] == page[:off] and len(new) == len(page)
        (t / 'page31.bin').write_bytes(new)
        dslite(
            'flash',
            '--config',
            str(config),
            '-s',
            'FlashEraseSetting=Necessary Sectors Only',
            '-s',
            'VerifyAfterProgramLoad=Full verification',
            f'{t / "page31.bin"},{PAGE31:#x}',
        )
        back = read(PAGE31, words, t / 'back.bin', config)
        if back != new:
            raise RuntimeError(
                f'page 31 read-back differs at {next(k for k in range(len(new)) if back[k] != new[k]):#x}'
            )
    reset(config)


def backup(out, config=CONFIG):
    out = pathlib.Path(out)
    out.mkdir(parents=True, exist_ok=True)
    a = read(0, 32768, out / 'flash.bin', config)
    b = read(0, 32768, out / 'flash_2.bin', config)
    if a != b:
        raise RuntimeError('two reads of the flash differ')
    (out / 'flash_2.bin').unlink()
    ccfg = read(0x1FFA8, 22, out / 'ccfg.bin', config)
    fcfg1 = read(0x50001000, 256, out / 'fcfg1.bin', config)
    if ccfg != a[0x1FFA8:]:
        raise RuntimeError('the CCFG read differs from the end of the flash read')
    info = {
        'mac_ccfg': ccfg[0x24:0x2C].decode(errors='replace'),
        'bl_config': f'{int.from_bytes(ccfg[0x30:0x34], "little"):08x}',
        'fcfg1_user_id': f'{int.from_bytes(fcfg1[0x294:0x298], "little"):08x}',
        'flash_sha256': hashlib.sha256(a).hexdigest(),
        'config': str(config.relative_to(ROOT)),
    }
    (out / 'backup.json').write_text(json.dumps(info, indent=2) + '\n')
    reset(config)
    return info


def provision(
    mode,
    config=CONFIG,
    backup_root=None,
    build_dir=None,
    board='wyze_contact_v1',
    bootloader=None,
    nv='keep',
    boot=None,
    slot_a=None,
    app=None,
):
    """Single-command provisioning: read & check CCFG MAC -> backup (or verify existing stock backup) -> slots/flash
    -> enable-bootloader -> clean reset."""
    if mode not in ('sensor', 'gateway'):
        raise ValueError(f'unknown provision mode {mode!r}')
    bdir_build = pathlib.Path(build_dir) if build_dir else ROOT / 'build' / 'fw' / 'firmware'
    if bootloader is None:
        bootloader = 'keep' if board == 'wyze_motion_v1' else 'w'
    if bootloader != 'keep' and bootloader not in BACKDOORS:
        raise ValueError(f'unknown bootloader backdoor {bootloader!r}')
    if mode == 'sensor':
        boot = pathlib.Path(boot) if boot else bdir_build / f'boot-{board}.bin'
        default_slot = 'sensor-motion-v1-slot-a.img' if board == 'wyze_motion_v1' else 'sensor-contact-v1-slot-a.img'
        slot_a = pathlib.Path(slot_a) if slot_a else bdir_build / default_slot
        slots_image(boot, slot_a)  # validate build artifacts up front before touching the chip
    else:
        app = pathlib.Path(app) if app else bdir_build / 'gateway-contact-v1-app.bin'
        data = app.read_bytes()
        if not data or len(data) > APP_END:
            raise ValueError(f'{app}: invalid gateway app image')

    _, mac = read_chip_ccfg(config)
    root_dir = (
        pathlib.Path(backup_root)
        if backup_root
        else ROOT / 'backups' / ('motion_v1' if board == 'wyze_motion_v1' else 'contact_v1')
    )
    bdir = root_dir if (root_dir / 'flash.bin').is_file() else root_dir / mac
    if (bdir / 'flash.bin').is_file():
        verify_backup(bdir, config)
        backed_up = False
    else:
        backup(bdir, config)
        backed_up = True

    if mode == 'sensor':
        slots(boot, slot_a, config, nv)
    else:
        flash(app, config, nv)

    if bootloader != 'keep':
        enable_bootloader(BACKDOORS[bootloader], config)
    reset(config)
    return {
        'mac': mac,
        'mode': mode,
        'board': board,
        'backup_dir': str(bdir),
        'backed_up_now': backed_up,
        'bootloader': bootloader,
    }


def _add_backup_guard(parser):
    grp = parser.add_mutually_exclusive_group(required=True)
    grp.add_argument('--backup', type=pathlib.Path, help="directory holding this chip's flash.bin backup")
    grp.add_argument(
        '--no-backup-i-know',
        action='store_true',
        help='skip the backup check (dangerous: only when already backed up elsewhere)',
    )


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--config', type=pathlib.Path, default=CONFIG)
    sub = ap.add_subparsers(dest='cmd', required=True)
    sub.add_parser('backup').add_argument('dir')
    fl = sub.add_parser('flash')
    fl.add_argument('app')
    _add_backup_guard(fl)
    fl.add_argument(
        '--nv', default='keep', help='page 30 (#61): keep (default), wipe, migrate, or an NV.json to import'
    )
    sl = sub.add_parser('slots')
    sl.add_argument('boot')
    sl.add_argument('slot_a')
    _add_backup_guard(sl)
    sl.add_argument(
        '--nv', default='keep', help='page 30 (#61): keep (default), wipe, migrate, or an NV.json to import'
    )
    pr = sub.add_parser('provision')
    pm = pr.add_mutually_exclusive_group(required=True)
    pm.add_argument('--sensor', action='store_true', help='provision boot selector + sensor slot A image')
    pm.add_argument('--gateway', action='store_true', help='provision gateway-contact-v1-app.bin')
    pr.add_argument('--board', choices=['wyze_contact_v1', 'wyze_motion_v1'], default='wyze_contact_v1')
    pr.add_argument('--backup-dir', type=pathlib.Path, help='backup root directory (default: backups/<board>/<MAC>)')
    pr.add_argument('--build-dir', type=pathlib.Path, help='firmware build directory (default: build/fw/firmware)')
    pr.add_argument(
        '--bootloader',
        choices=[*BACKDOORS, 'keep'],
        default=None,
        help='ROM bootloader backdoor (default: w on wyze_contact_v1, keep on wyze_motion_v1)',
    )
    pr.add_argument('--nv', default='keep', help='page 30 (#61): keep (default), wipe, migrate, or an NV.json')
    pr.add_argument('--boot', type=pathlib.Path)
    pr.add_argument('--slot-a', type=pathlib.Path)
    pr.add_argument('--app', type=pathlib.Path)
    sub.add_parser('reset')
    eb = sub.add_parser('enable-bootloader')
    eb.add_argument('backdoor', choices=BACKDOORS, help='w: the W pad (DIO10); button: the button (DIO11)')
    eb.add_argument('--yes', action='store_true', help='really write the CCFG')
    r = sub.add_parser('read')
    r.add_argument('addr', type=lambda s: int(s, 0))
    r.add_argument('words', type=int)
    r.add_argument('out')
    a = ap.parse_args(argv)
    if a.cmd == 'backup':
        print(json.dumps(backup(a.dir, a.config), indent=2))
    elif a.cmd == 'flash':
        flash(a.app, a.config, a.nv, backup_dir=a.backup)
        print('flashed, verified, reset')
    elif a.cmd == 'slots':
        slots(a.boot, a.slot_a, a.config, a.nv, backup_dir=a.backup)
        print('selector + slot A installed, verified, reset')
    elif a.cmd == 'provision':
        info = provision(
            'sensor' if a.sensor else 'gateway',
            config=a.config,
            backup_root=a.backup_dir,
            build_dir=a.build_dir,
            board=a.board,
            bootloader=a.bootloader,
            nv=a.nv,
            boot=a.boot,
            slot_a=a.slot_a,
            app=a.app,
        )
        print(json.dumps(info, indent=2))
    elif a.cmd == 'reset':
        reset(a.config)
        print('reset')
    elif a.cmd == 'enable-bootloader':
        if not a.yes:
            sys.exit('writes the CCFG (page 31): add --yes')
        enable_bootloader(BACKDOORS[a.backdoor], a.config)
        print(f'bootloader enabled, backdoor {a.backdoor} (DIO{BACKDOORS[a.backdoor]}) active low; reset')
    elif a.cmd == 'read':
        read(a.addr, a.words, a.out, a.config)
        print(a.out)


if __name__ == '__main__':
    main()
