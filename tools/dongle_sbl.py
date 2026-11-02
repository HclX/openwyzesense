#!/usr/bin/env python3
"""The Wyze dongle's CC1310 through TI's ROM serial bootloader (SBL), over the dongle's USB HID, the way the user's
WyzeSenseUpdater (https://github.com/HclX/WyzeSenseUpdater, ws_updater.py) does it: host command 0x4312 puts the
dongle in its update mode; from then on each HID report is one SBL packet (size, checksum, command, data), the
dongle's CH554 relaying it to the CC1310's UART. The framing below is that tool's, unchanged.

    uv run python tools/dongle_sbl.py backup OUTDIR [--device /dev/hidrawN]
    uv run python tools/dongle_sbl.py flash IMAGE.bin --backup DIR --yes
    uv run python tools/dongle_sbl.py restore --backup DIR --yes        # the backup's pages 0-29: stock again
    uv run python tools/dongle_sbl.py bench IMAGE.bin --backup DIR --yes  # time --fast against the default

`backup` reads, each twice and CRC-checked by the chip where it can: flash (128 KiB), CCFG, FCFG1, and the
reset-source register (how the chip came to be in the bootloader); then resets the CC1310 as ws_updater.py does.

`flash` writes an image to flash pages 0-29, then page 30 (the NV page) as --nv says (#61): migrate (the default: the
current page exported to JSON, saved as nv.json in the backup directory, validated and imported into our format),
wipe, keep, or an NV.json; `restore` writes the backup's page 30 back. Page 31 (code and the CCFG, whose BL_CONFIG
keeps the bootloader reachable) is never erased or written: every erase and download is checked against APP_END
twice (page 30 only inside write_nv()), and bank erase, set-CCFG and memory-write are not in this file. Unlike
ws_updater.py `flash`, which erases all 32 pages first and writes the CCFG back last, nothing here leaves the chip
without its CCFG. It needs the backup of this chip (same chip ID, same page 31), checks the image (size, vector
table), verifies what it wrote (the chip's CRC32 and a read-back) and that pages 30-31 did not change, and only
then resets the chip. The dongle stays in update mode until it is unplugged (the user: "you will have to
physically unplug the dongle to re-enable it").
"""

import argparse
import binascii
import hashlib
import json
import logging
import pathlib
import signal
import sys
import time

import nv_json  # tools/: the NV page choices (#61)
import usb_dev  # tools/: sysfs VID:PID discovery (1a86:e024)

# --- WyzeSenseUpdater's framing (ws_updater.py, unchanged but for names) ---

SBL_MAX_MEMREAD_BYTES = 63 * 4
RSP_CODE_ACK = 0x00CC
CMD_PING, CMD_DOWNLOAD, CMD_GET_STATUS, CMD_SEND_DATA, CMD_RESET = 0x20, 0x21, 0x23, 0x24, 0x25
CMD_SECTOR_ERASE, CMD_CRC32 = 0x26, 0x27
CMD_GET_CHIP_ID, CMD_MEMORY_READ, CMD_AUTO_BAUD = 0x28, 0x2A, 0x55
UPDATE_MODE_REQUEST = b'\x07\xaa\x55\x43\x03\x12\x01\x57'  # host command 0x4312, HID length byte first
MAX_PACK_SIZE = 0x3A
PAGE = 0x1000
APP_END = 0x1E000  # flash pages 0-29: the only ones `flash` writes
READ_ONLY = (CMD_PING, CMD_GET_STATUS, CMD_RESET, CMD_CRC32, CMD_GET_CHIP_ID, CMD_MEMORY_READ, CMD_AUTO_BAUD)


def dongle_read(fd, size):
    def handler(signum, frame):
        raise TimeoutError('read timeout')

    old = signal.signal(signal.SIGALRM, handler)
    signal.alarm(5)
    try:
        return fd.read(size)
    finally:
        signal.alarm(0)
        signal.signal(signal.SIGALRM, old)


def rawhid_read(fd):
    pkt = dongle_read(fd, 0x3F)
    logging.debug(f'[R] {pkt.hex()}')
    return pkt[1 : 1 + pkt[0]]


def rawhid_write(fd, payload):
    logging.debug(f'[W] {payload.hex()}')
    fd.write(payload)


def checksum(cmd, data=b''):
    return (cmd + sum(data)) & 0xFF


def ack(fd, success):
    rawhid_write(fd, bytes([0x00, 0xCC if success else 0x33]))


NV_PAGE = 0x1E000  # page 30: written only by write_nv() (#61)
_nv_writing = False


def below_app_end(addr, size):
    """The only flash this tool erases or programs: pages 0-29, and page 30 inside write_nv(); never page 31."""
    if addr >= 0 and size > 0 and addr + size <= APP_END:
        return
    if _nv_writing and addr >= NV_PAGE and size > 0 and addr + size <= NV_PAGE + PAGE:
        return
    raise ValueError(
        f'refusing {addr:#x}+{size:#x}: only flash below {APP_END:#x} (pages 0-29) is ever written, '
        'and page 30 by write_nv()'
    )


def sbl_cmd(fd, cmd, payload=b''):
    # the only writing commands, each with its target checked again here, at the last step before the chip
    if cmd == CMD_SECTOR_ERASE:
        below_app_end(int.from_bytes(payload[:4], 'big'), PAGE)
    elif cmd == CMD_DOWNLOAD:
        below_app_end(int.from_bytes(payload[:4], 'big'), int.from_bytes(payload[4:8], 'big'))
    elif cmd != CMD_SEND_DATA:
        assert cmd in READ_ONLY, f'command {cmd:#x} is not used by this tool'
        # (bank erase 0x2C, memory write 0x2B, set CCFG 0x2D: never)
    rawhid_write(fd, bytes([len(payload) + 3, checksum(cmd, payload), cmd]) + payload)
    rsp = rawhid_read(fd)
    assert len(rsp) >= 2, 'invalid response'
    if (rsp[0] << 8 | rsp[1]) != RSP_CODE_ACK:
        return False
    if len(rsp) == 2:
        return True
    assert len(rsp) >= 4, 'invalid payload'
    n = rsp[2] - 2
    want = rsp[3]
    data = rsp[4:]
    assert n >= len(data), 'invalid payload length'
    n -= len(data)
    while n > 0:
        more = rawhid_read(fd)[:n]
        data += more
        n -= len(more)
    if checksum(0, data) != want:
        ack(fd, False)
        return None
    ack(fd, True)
    return data


def read_mem(fd, addr, size):
    assert addr % 4 == 0 and size % 4 == 0 and size > 0
    data = b''
    while size > 0:
        n = min(size, SBL_MAX_MEMREAD_BYTES)
        rsp = sbl_cmd(fd, CMD_MEMORY_READ, addr.to_bytes(4, 'big') + bytes([1, n // 4]))
        if not rsp or len(rsp) != n:
            raise RuntimeError(f'read failed at {addr:#010x}')
        data += rsp
        addr += n
        size -= n
        print(f'\r  {addr:#010x}', end='', flush=True)
    print()
    return data


def chip_crc32(fd, addr, size):
    rsp = sbl_cmd(fd, CMD_CRC32, addr.to_bytes(4, 'big') + size.to_bytes(4, 'big') + b'\0\0\0\0')
    return int.from_bytes(rsp, 'big') if rsp and len(rsp) == 4 else None


def status_ok(fd):
    rsp = sbl_cmd(fd, CMD_GET_STATUS)
    return bool(rsp) and len(rsp) == 1 and rsp[0] == 0x40  # COMMAND_RET_SUCCESS


def enter(fd):
    """Update mode, then the bootloader: returns (reply to 0x4312, chip ID)."""
    rawhid_write(fd, UPDATE_MODE_REQUEST)
    rsp = rawhid_read(fd)
    assert sbl_cmd(fd, CMD_AUTO_BAUD), 'no bootloader (auto baud)'
    assert sbl_cmd(fd, CMD_PING), 'no bootloader (ping)'
    return rsp, sbl_cmd(fd, CMD_GET_CHIP_ID)


# --- backup ---

REGIONS = [('flash.bin', 0x00000000, 0x20000), ('ccfg.bin', 0x50003000, 0x1000), ('fcfg1.bin', 0x50001000, 0x3F0)]
RESETCTL = 0x40090004
RESET_SRC = {
    0: 'power on',
    1: 'pin reset',
    2: 'VDDS loss',
    3: 'VDD loss',
    4: 'VDDR loss',
    5: 'clock loss',
    6: 'system reset (software)',
    7: 'warm reset',
}


def backup(fd, out):
    out.mkdir(parents=True, exist_ok=True)
    info = {'time': time.strftime('%Y-%m-%d %H:%M:%S %z')}
    rsp, chip_id = enter(fd)
    info['update_mode_reply'] = rsp.hex()
    print('update mode reply:', rsp.hex())
    info['chip_id'] = chip_id.hex()
    print('chip id:', chip_id.hex())
    try:  # a peripheral register: may not be readable
        rc = int.from_bytes(read_mem(fd, RESETCTL, 4), 'little')
        info['resetctl'] = f'{rc:#010x}'
        info['reset_source'] = RESET_SRC[(rc >> 1) & 7]
        print(f'RESETCTL {rc:#010x}: last reset = {info["reset_source"]}')
    except (RuntimeError, AssertionError, TimeoutError) as e:
        info['resetctl'] = f'unreadable: {e}'
        print('RESETCTL unreadable:', e)
    for name, addr, size in REGIONS:
        print(f'{name}: {addr:#010x} + {size:#x}')
        a = read_mem(fd, addr, size)
        b = read_mem(fd, addr, size)
        assert a == b, f'{name}: two reads differ'
        crc = chip_crc32(fd, addr, size)
        if crc is not None:
            assert crc == binascii.crc32(a), f'{name}: chip CRC {crc:#x} != {binascii.crc32(a):#x}'
        (out / name).write_bytes(a)
        info[name] = {
            'address': f'{addr:#010x}',
            'size': size,
            'sha256': hashlib.sha256(a).hexdigest(),
            'chip_crc32': None if crc is None else f'{crc:#010x}',
            'reads': 2,
        }
    (out / 'info.json').write_text(json.dumps(info, indent=2) + '\n')
    sbl_cmd(fd, CMD_RESET)
    return info


# --- flash ---


def check_image(image):
    """Our image: fits below APP_END, and starts with a vector table (initial SP in RAM, reset handler in it)."""
    if not image or len(image) > APP_END:
        raise ValueError(
            f"image is {len(image)} bytes: must fit below {APP_END:#x} (the build's <product>-app.bin, "
            'without the CCFG; not <product>.bin)'
        )
    sp, reset = int.from_bytes(image[0:4], 'little'), int.from_bytes(image[4:8], 'little')
    if not (0x20000000 < sp <= 0x20005000) or not (reset & 1) or (reset & ~1) >= len(image):
        raise ValueError(f'no vector table: SP {sp:#x}, reset {reset:#x}')
    return image + b'\xff' * (-len(image) % 4)


def check_backup(backup):
    info = json.loads((backup / 'info.json').read_text())
    flash = (backup / 'flash.bin').read_bytes()
    assert len(flash) == 0x20000 and hashlib.sha256(flash).hexdigest() == info['flash.bin']['sha256'], 'bad backup'
    assert flash[0x1FFD8:0x1FFDC] == bytes.fromhex('c501fec5'), "the backup's BL_CONFIG is not the stock one"
    return info, flash


def runs(image, skip_ff):
    """(offset, bytes) stretches to send: the whole image, or (fast) only its stretches that are not all FF (erased
    flash reads FF already), in 4-byte words, with gaps under 256 bytes merged (a new download costs a round trip)."""
    if not skip_ff:
        return [(0, image)]
    words = [k for k in range(0, len(image), 4) if image[k : k + 4] != b'\xff\xff\xff\xff']
    out = []
    for k in words:
        if out and k - (out[-1][0] + out[-1][1]) < 256:
            out[-1][1] = k + 4 - out[-1][0]
        else:
            out.append([k, 4])
    return [(a, image[a : a + n]) for a, n in out]


def write_nv(fd, page):
    """Page 30 <- page (4096 bytes, our NV format or all FF), verified by the chip's CRC32."""
    global _nv_writing
    assert len(page) == PAGE
    _nv_writing = True
    try:
        sbl_cmd(fd, CMD_SECTOR_ERASE, NV_PAGE.to_bytes(4, 'big'))
        assert status_ok(fd), 'erasing page 30 failed'
        for start, data in runs(page, True):
            a = NV_PAGE + start
            assert sbl_cmd(fd, CMD_DOWNLOAD, a.to_bytes(4, 'big') + len(data).to_bytes(4, 'big')) and status_ok(fd)
            for off in range(0, len(data), MAX_PACK_SIZE):
                assert sbl_cmd(fd, CMD_SEND_DATA, data[off : off + MAX_PACK_SIZE]), (
                    f'sending page 30 failed at {a + off:#x}'
                )
            assert status_ok(fd), f'programming page 30 at {a:#x} failed'
    finally:
        _nv_writing = False
    assert chip_crc32(fd, NV_PAGE, PAGE) == binascii.crc32(page), 'page 30 CRC differs from what was written'


def write_pages(fd, image, info, flash_backup, fast=False, entered=False, reset=True, nv=None):
    """Pages 0..n-1 <- image, with every check; raises before reset if anything is off. fast: GET_STATUS once per
    stretch instead of after every packet, all-FF stretches skipped, verified by the chip's CRC32 alone (no read-back;
    pages 30-31 still checked). Returns the seconds each phase took."""
    t = {'start': time.time()}
    if not entered:
        _, chip_id = enter(fd)
        assert chip_id.hex() == info['chip_id'], f"chip ID {chip_id.hex()} is not the backup's {info['chip_id']}"
    assert chip_crc32(fd, 0x1F000, PAGE) == binascii.crc32(flash_backup[0x1F000:]), "page 31 is not the backup's"
    keep = chip_crc32(fd, APP_END, 0x20000 - APP_END)  # pages 30-31 now (30 may have changed since)
    print(f'pages 30-31 CRC {keep:#010x} (page 31 as in the backup)')
    t['checked'] = time.time()
    pages = (len(image) + PAGE - 1) // PAGE
    for p in range(pages):
        sbl_cmd(fd, CMD_SECTOR_ERASE, (p * PAGE).to_bytes(4, 'big'))
        assert status_ok(fd), f'erasing page {p} failed'
        print(f'\r  erased page {p}', end='', flush=True)
    print()
    t['erased'] = time.time()
    sent = 0
    for start, data in runs(image, fast):
        assert sbl_cmd(fd, CMD_DOWNLOAD, start.to_bytes(4, 'big') + len(data).to_bytes(4, 'big')) and status_ok(fd), (
            f'download at {start:#x} refused'
        )
        off, tries = 0, 0
        while off < len(data):
            chunk = data[off : off + MAX_PACK_SIZE]
            if sbl_cmd(fd, CMD_SEND_DATA, chunk) and (fast or status_ok(fd)):
                off, tries, sent = off + len(chunk), 0, sent + len(chunk)
                print(f'\r  written {start + off:#07x}/{len(image):#07x}', end='', flush=True)
            else:
                tries += 1
                assert tries <= 3, f'sending data failed at {start + off:#x}'
        if fast:
            assert status_ok(fd), f'programming the stretch at {start:#x} failed'
    print()
    t['written'] = time.time()
    crc = chip_crc32(fd, 0, len(image))
    assert crc == binascii.crc32(image), f'chip CRC {crc:#x} != image CRC {binascii.crc32(image):#x}'
    if not fast:
        assert read_mem(fd, 0, len(image)) == image, 'read-back differs'
    assert chip_crc32(fd, APP_END, 0x20000 - APP_END) == keep, 'pages 30-31 changed!'
    if nv is not None:  # nv(current page 30) -> what to write there (#61), after the image is verified; None: nothing
        page = nv(read_mem(fd, NV_PAGE, PAGE))
        if page is not None:
            write_nv(fd, page)
        assert chip_crc32(fd, 0x1F000, PAGE) == binascii.crc32(flash_backup[0x1F000:]), 'page 31 changed!'
    t['verified'] = time.time()
    print(f'verified: chip CRC{"" if fast else " and read-back"} match, page 31 unchanged')
    if reset:
        sbl_cmd(fd, CMD_RESET)
    phases = {
        'checks': t['checked'] - t['start'],
        'erase': t['erased'] - t['checked'],
        'write': t['written'] - t['erased'],
        'verify': t['verified'] - t['written'],
        'total': t['verified'] - t['start'],
        'bytes_sent': sent,
    }
    print('  ' + ', '.join(f'{k} {v:.2f} s' if k != 'bytes_sent' else f'{v} bytes sent' for k, v in phases.items()))
    return phases


INQUIRY = b'\x07\xaa\x55\x43\x03\x27\x01\x6c'  # host command 0x4327, HID length byte first


def find_dongle(sysfs=pathlib.Path('/sys/class/hidraw'), dev_root=pathlib.Path('/dev')):
    return usb_dev.find_hidraw(sysfs=sysfs, dev_root=dev_root)


def back_to_normal(device, seconds=1.0, interval=0.3):
    """After the bootloader's reset: whether the dongle answers the normal host protocol again (0x4327 -> 0x4328)."""
    end = time.time() + seconds
    while True:
        dev = device or find_dongle()
        if dev:
            try:
                with open(dev, 'wb+') as fd:
                    rawhid_write(fd, INQUIRY)
                    for _ in range(3):
                        if b'\x55\xaa\x43\x04\x28' in rawhid_read(fd):
                            return True
            except (OSError, TimeoutError, IndexError):
                pass
        if time.time() >= end:
            return False
        time.sleep(interval)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('action', choices=['backup', 'flash', 'restore', 'bench'])
    ap.add_argument('path', type=pathlib.Path, nargs='?', help='backup: the output directory; flash: the image')
    ap.add_argument('--backup', type=pathlib.Path, help='flash / restore: the backup of this chip')
    ap.add_argument('--yes', action='store_true', help='flash / restore / bench: really write')
    ap.add_argument('--fast', action='store_true', help='flash / restore: status per stretch, skip FF, CRC-only verify')
    ap.add_argument(
        '--nv',
        default='migrate',
        help='flash: page 30 (#61): migrate (default: the page carried into our format, its JSON saved in '
        "the backup directory), wipe, keep, or an NV.json to import; restore always writes the backup's",
    )
    ap.add_argument('--device', default=None, help='dongle /dev/hidrawN (default: auto-detect USB 1a86:e024)')
    ap.add_argument('--wait', action='store_true', help='wait up to 60 s for replug after reset')
    ap.add_argument('-v', '--verbose', action='store_true')
    args = ap.parse_args(argv)
    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.INFO)
    device = args.device or find_dongle()
    if not device:
        sys.exit(
            'no Wyze USB dongle (1a86:e024) found on /dev/hidraw*: plug it in, install '
            'tools/udev/99-wyze-dongle.rules, or pass --device /dev/hidrawN'
        )
    if args.action == 'backup':
        if not args.path:
            ap.error('backup requires an output directory PATH')
        with open(device, 'wb+') as fd:  # as ws_updater.py opens it
            print(json.dumps(backup(fd, args.path), indent=2))
        finish(device, wait=args.wait)
        return 0
    if not args.backup:
        ap.error('--backup DIR is required')
    info, flash = check_backup(args.backup)
    if args.action != 'restore' and not args.path:
        ap.error(f'{args.action} requires an IMAGE.bin path')
    image = check_image(flash[:APP_END] if args.action == 'restore' else args.path.read_bytes())
    print(f'{args.action}: {len(image)} bytes to pages 0-{(len(image) - 1) // PAGE}, CRC {binascii.crc32(image):#010x}')
    if not args.yes:
        print('not written: add --yes')
        return 1
    with open(device, 'wb+') as fd:
        if args.action == 'bench':  # both ways in one session: one replug
            _, chip_id = enter(fd)
            assert chip_id.hex() == info['chip_id'], "not the backup's chip"
            print('== as before (status after every packet, read-back)')
            slow = write_pages(fd, image, info, flash, fast=False, entered=True, reset=False)
            print('== --fast')
            quick = write_pages(fd, image, info, flash, fast=True, entered=True, reset=True)
            print(
                f'total: {slow["total"]:.2f} s -> {quick["total"]:.2f} s; '
                f'write {slow["write"]:.2f} s -> {quick["write"]:.2f} s'
            )
        elif args.action == 'restore':  # the backup's page 30 too: stock again, as backed up (pairings since are lost)
            write_pages(fd, image, info, flash, fast=args.fast, nv=lambda _: flash[NV_PAGE : NV_PAGE + PAGE])
        else:
            nv_json_path = args.backup / 'nv.json'
            write_pages(
                fd,
                image,
                info,
                flash,
                fast=args.fast,
                nv=lambda current: nv_json.nv_page_for(args.nv, current, nv_json_path),
            )
    finish(device, wait=args.wait)
    return 0


def finish(device, wait=False):
    if back_to_normal(device, seconds=1.0, interval=0.3):
        print('done: the dongle is back in normal mode (no unplug needed)', flush=True)
        return
    print('done: unplug and replug the dongle now to return it to normal mode', flush=True)
    if wait:
        print('waiting up to 60 s for replug...', flush=True)
        if back_to_normal(None, seconds=60.0, interval=1.0):
            print('replugged: the dongle is back in normal mode', flush=True)
        else:
            print('still not answering Inquiry after 60 s: check USB connection', flush=True)


if __name__ == '__main__':
    sys.exit(main())
