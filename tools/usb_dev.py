"""Linux sysfs USB VID:PID auto-detection for /dev/hidraw* (Wyze dongle 1a86:e024) and /dev/ttyACM* (TI XDS110
0451:bef3), without external dependencies."""

import pathlib
import re

WYZE_DONGLE = ('1a86', 'e024')
TI_XDS110 = ('0451', 'bef3')


def _nat_key(name):
    return [int(t) if t.isdigit() else t for t in re.split(r'(\d+)', name)]


def _read_trimmed(path):
    try:
        return path.read_text(errors='replace').strip()
    except OSError:
        return None


def _ancestors(dev_dir):
    try:
        resolved = dev_dir.resolve()
    except OSError:
        resolved = dev_dir
    out = [dev_dir]
    if resolved != dev_dir:
        out.append(resolved)
    out.extend(resolved.parents)
    return out


def _matches_vid_pid(dev_dir, vid, pid):
    vid, pid = vid.lower(), pid.lower()
    uevent = _read_trimmed(dev_dir / 'uevent') or ''
    for line in uevent.splitlines():
        if line.startswith('HID_ID='):
            parts = line.split('=', 1)[1].split(':')
            if len(parts) == 3 and int(parts[1], 16) == int(vid, 16) and int(parts[2], 16) == int(pid, 16):
                return True
        if line.startswith('PRODUCT='):
            parts = line.split('=', 1)[1].split('/')
            if len(parts) >= 2 and int(parts[0], 16) == int(vid, 16) and int(parts[1], 16) == int(pid, 16):
                return True
    for anc in _ancestors(dev_dir):
        v, p = _read_trimmed(anc / 'idVendor'), _read_trimmed(anc / 'idProduct')
        if v is not None and p is not None:
            return v.lower() == vid and p.lower() == pid
    return False


def find_hidraw(
    vid=WYZE_DONGLE[0], pid=WYZE_DONGLE[1], sysfs=pathlib.Path('/sys/class/hidraw'), dev_root=pathlib.Path('/dev')
):
    """Return the first /dev/hidrawN matching vid:pid, or None."""
    if not sysfs.is_dir():
        return None
    for entry in sorted(sysfs.iterdir(), key=lambda p: _nat_key(p.name)):
        if not entry.name.startswith('hidraw'):
            continue
        if _matches_vid_pid(entry / 'device', vid, pid):
            return str(dev_root / entry.name)
    return None


def find_tty(
    vid=TI_XDS110[0],
    pid=TI_XDS110[1],
    interface='00',
    sysfs=pathlib.Path('/sys/class/tty'),
    dev_root=pathlib.Path('/dev'),
):
    """Return the first /dev/ttyACMN or /dev/ttyUSBN matching vid:pid (and USB bInterfaceNumber if present), or None."""
    if not sysfs.is_dir():
        return None
    for entry in sorted(sysfs.iterdir(), key=lambda p: _nat_key(p.name)):
        if not entry.name.startswith(('ttyACM', 'ttyUSB')):
            continue
        dev_dir = entry / 'device'
        if not _matches_vid_pid(dev_dir, vid, pid):
            continue
        if interface is not None:
            ifnum = None
            for anc in _ancestors(dev_dir):
                ifnum = _read_trimmed(anc / 'bInterfaceNumber')
                if ifnum is not None:
                    break
            if ifnum is not None and ifnum.lower() != interface.lower():
                continue
        return str(dev_root / entry.name)
    return None


def auto_host_port(
    hidraw_sysfs=pathlib.Path('/sys/class/hidraw'),
    tty_sysfs=pathlib.Path('/sys/class/tty'),
    dev_root=pathlib.Path('/dev'),
):
    """Auto-detect a connected TI LaunchPad UART (0451:bef3 if00) or Wyze USB dongle (1a86:e024)."""
    return find_tty(sysfs=tty_sysfs, dev_root=dev_root) or find_hidraw(sysfs=hidraw_sysfs, dev_root=dev_root)
