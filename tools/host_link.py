#!/usr/bin/env python3
"""A host for the dongle protocol (docs/FACTS.md §9): our gateway on the LaunchPad's UART (a tty), or a stock
dongle through its USB HID interface (a /dev/hidraw device).

    uv run python tools/host_link.py [PORT] [--seconds N] [--pair N] [--log FILE.jsonl]
    uv run python tools/host_link.py PORT --ota MAC SLOT_A.img SLOT_B.img [--pair N] [--seconds N]

PORT: auto-detected by USB VID:PID (`0451:bef3` TI LaunchPad UART or `1a86:e024` Wyze USB dongle `/dev/hidrawN`), an
explicit `/dev/ttyACM*` / `/dev/hidraw*` / pty path, or `'dongle'` / `'launchpad'`. Does the stock handshake and a
time sync, prints the sensor list, then every packet the gateway sends, as it comes, for --seconds. --pair N: scanning
on for up to N seconds, pairing the first sensor that shows up as wyzesense2mqtt does (R1 with its token, scanning off,
verify).
--log: every byte both ways, as {ts_ms, dir, proto_hex} lines (the ground_truth/host/ format).
--ota: an OTA update for our sensor MAC (docs/design/ota.md) through our gateway: turns GW_FLAG_OTA on, offers the two
builds (tools/slot_image.py images, linked for slot A and slot B), serves the blocks the sensor asks for, prints its
result, for up to --seconds (default 360, extended while blocks advance). The sensor must be paired already (a 3 s
press; or --pair N first), and takes the update within 5 minutes of a short press of its button: press it once this
says so.
As a library: HostLink(port).handshake(), .call(cmd, payload), .packets(seconds).
"""

import os
import pathlib
import select
import struct
import sys
import termios
import time

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'reference'))
sys.path.insert(0, str(ROOT / 'tools'))
import slot_image  # noqa: E402
import usb_dev  # noqa: E402
from wyzesense_ref import app, host, nvows  # noqa: E402


def resolve_port(
    port=None,
    hidraw_sysfs=pathlib.Path('/sys/class/hidraw'),
    tty_sysfs=pathlib.Path('/sys/class/tty'),
    dev_root=pathlib.Path('/dev'),
):
    """Resolve PORT (None/'auto'/'dongle'/'launchpad' or an explicit device path) via USB VID:PID."""
    if port in (None, 'auto'):
        found = usb_dev.auto_host_port(hidraw_sysfs=hidraw_sysfs, tty_sysfs=tty_sysfs, dev_root=dev_root)
        if not found:
            raise FileNotFoundError(
                'no TI LaunchPad (0451:bef3) or Wyze USB dongle (1a86:e024) found; pass PORT explicitly'
            )
        return found
    if port in ('dongle', 'hidraw'):
        found = usb_dev.find_hidraw(sysfs=hidraw_sysfs, dev_root=dev_root)
        if not found:
            raise FileNotFoundError('no Wyze USB dongle (1a86:e024) found on /dev/hidraw*')
        return found
    if port == 'launchpad':
        found = usb_dev.find_tty(sysfs=tty_sysfs, dev_root=dev_root)
        if not found:
            raise FileNotFoundError('no TI LaunchPad (0451:bef3) found on /dev/ttyACM*')
        return found
    return str(port)


class HostLink:
    def __init__(self, port=None, log=None):
        """log: a list to append every packet to, as {ts_ms, dir (W host -> dongle, R), proto_hex}"""
        port = resolve_port(port)
        self.port = port
        self.log = log
        self.fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        self.hid = 'hidraw' in port  # each read: 1 length byte + protocol bytes
        self.auto_ack = True  # ACK async packets as they come (as a host must)
        self.buf = b''
        self.backlog = []  # packets read while waiting for something else
        if self.hid:
            return
        a = termios.tcgetattr(self.fd)
        a[0] = a[1] = 0  # raw: no input/output processing
        a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
        a[3] = 0
        a[4] = a[5] = termios.B115200
        termios.tcsetattr(self.fd, termios.TCSANOW, a)
        termios.tcflush(self.fd, termios.TCIOFLUSH)

    def close(self):
        os.close(self.fd)

    def _write(self, pkt):
        sent = pkt
        while pkt:  # the fd is non-blocking: wait while the port is full
            try:
                pkt = pkt[os.write(self.fd, pkt) :]
            except BlockingIOError:
                select.select([], [self.fd], [], 0.1)
        if self.log is not None:
            self.log.append({'ts_ms': int(time.time() * 1000), 'dir': 'W', 'proto_hex': sent.hex()})

    def send(self, cmd, payload=b''):
        self._write(host.build(cmd, payload))

    def _read(self, timeout):
        """Next packet from the gateway (ACKing async ones, answering time requests), or None on timeout."""
        end = time.time() + timeout
        while True:
            pkts = host.scan(self.buf)
            if pkts:
                p = pkts[0]
                self.buf = self.buf[self.buf.index(p) + len(p) :]
                if self.log is not None:
                    self.log.append({'ts_ms': int(time.time() * 1000), 'dir': 'R', 'proto_hex': p.hex()})
                m = host.parse(p)
                if self.auto_ack and m['cmd'] >> 8 == host.ASYNC and m['cmd'] != host.ACK:
                    self._write(host.build_ack(m['cmd']))
                if m['cmd'] == host.TIME_SYNC_REQ:
                    self.send(host.TIME_SYNC, int(time.time() * 1000).to_bytes(8, 'big'))
                m['t'] = time.time()
                return m
            if time.time() > end:
                return None
            try:
                data = os.read(self.fd, 64 if self.hid else 512)
                self.buf += host.hid_unwrap(data) if self.hid else data
            except BlockingIOError:
                time.sleep(0.005)

    def call(self, cmd, payload=b'', timeout=2.0, replies=1):
        """Sends a command, returns its reply packets (cmd + 1); other packets go to the backlog."""
        self.send(cmd, payload)
        return self.wait_for(cmd + 1 if cmd != host.GET_SENSOR_LIST else host.SENSOR_LIST_ITEM, timeout, replies)

    def wait_for(self, cmd, timeout, count=1):
        """Packets with this command, from the backlog first, then as they come; others go to the backlog."""
        got = [m for m in self.backlog if m['cmd'] == cmd][:count]
        self.backlog = [m for m in self.backlog if not any(m is g for g in got)]
        end = time.time() + timeout
        while len(got) < count and time.time() < end:
            m = self._read(end - time.time())
            if m is None:
                break
            if m['cmd'] == cmd:
                got.append(m)
            elif m['cmd'] != host.ACK:
                self.backlog.append(m)
        return got

    def handshake(self):
        """The 5 steps wyzesense2mqtt does before the stock dongle forwards events."""
        r = {}
        for _ in range(5):  # a gateway just reset may not answer yet
            got = self.call(host.INQUIRY, timeout=1.0)
            if got:
                break
        else:
            raise TimeoutError('no answer to Inquiry')
        r['inquiry'] = got[0]['payload']
        r['enr'] = self.call(host.GET_ENR, b'0' * 16)[0]['payload']
        r['mac'] = self.call(host.GET_MAC)[0]['payload']
        got = self.call(host.GET_VERSION) or self.call(
            host.GET_VERSION
        )  # the first one after a start can go unanswered
        r['version'] = got[0]['payload']
        r['auth'] = self.call(host.FINISH_AUTH, b'\xff')[0]['payload']
        return r

    def sensors(self):
        n = self.call(host.GET_SENSOR_COUNT)[0]['payload'][0]
        return [m['payload'] for m in self.call(host.GET_SENSOR_LIST, bytes([n]), replies=n)] if n else []

    def send_ows(self, sub_id, payload=b''):
        self.send(host.OWS_CMD, bytes([sub_id]) + payload)

    def wait_ows(self, sub_id, timeout, count=1):
        """0x53F1 packets with payload[0] == sub_id; other packets go to the backlog."""
        tag = bytes([sub_id])
        got = [m for m in self.backlog if m['cmd'] == host.OWS_RSP and m['payload'][:1] == tag][:count]
        self.backlog = [m for m in self.backlog if not any(m is g for g in got)]
        end = time.time() + timeout
        while len(got) < count and time.time() < end:
            m = self._read(end - time.time())
            if m is None:
                break
            if m['cmd'] == host.OWS_RSP and m['payload'][:1] == tag:
                got.append(m)
            elif m['cmd'] != host.ACK:
                self.backlog.append(m)
        return [m['payload'][1:] for m in got]

    def ows_call(self, sub_id, payload=b'', timeout=2.0):
        """Sends 0x53F0 [sub_id, payload...], returns the matching 0x53F1 sub-payload (or None)."""
        self.send_ows(sub_id, payload)
        got = self.wait_ows(sub_id, timeout, 1)
        return got[0] if got else None

    def flags(self, set_to=None, timeout=1.0):
        """Our gateway's feature flags (R3): sets them if set_to is given; returns (on, supported), or None
        from a stock dongle, which does not answer."""
        p = self.ows_call(host.OWS_SUB_FLAGS, b'' if set_to is None else set_to.to_bytes(4, 'big'), timeout=timeout)
        if p is None or len(p) != 8:
            return None
        return int.from_bytes(p[:4], 'big'), int.from_bytes(p[4:], 'big')

    def build_info(self, timeout=1.0):
        """Our gateway's three-page build info block (OWS_SUB_BUILD_INFO, <= 56 B CH554-safe), or None."""
        p0 = self.ows_call(host.OWS_SUB_BUILD_INFO, b'\x00', timeout=timeout)
        p1 = self.ows_call(host.OWS_SUB_BUILD_INFO, b'\x01', timeout=timeout) if p0 and len(p0) == 52 else None
        p2 = self.ows_call(host.OWS_SUB_BUILD_INFO, b'\x02', timeout=timeout) if p1 and len(p1) == 54 else None
        return host.parse_build_info(p0, p1, p2)

    def ccfg_restore(self, ieee):
        """Our gateway's CCFG restore (OWS_SUB_CCFG): arm with the chip's IEEE address, confirm with the nonce.
        Returns the status text of the last step."""
        head = host.CCFG_MAGIC + bytes([1]) + ieee.to_bytes(8, 'big')
        p = self.ows_call(host.OWS_SUB_CCFG, head)
        if p is None:
            return 'no reply (not our firmware?)'
        if len(p) < 2 or p[1] != 0 or len(p) != 6:
            return host.CCFG_STATUS.get(p[1], hex(p[1])) if len(p) >= 2 else '?'
        p = self.ows_call(host.OWS_SUB_CCFG, host.CCFG_MAGIC + bytes([2]) + ieee.to_bytes(8, 'big') + p[2:6], timeout=5)
        return host.CCFG_STATUS.get(p[1], '?') if p and len(p) >= 2 else 'no reply'

    def enter_sbl(self, timeout=1.0):
        """Pinless software ROM bootloader entry on direct-UART gateways (OWS_SUB_ENTER_SBL): returns 0x00 on success,
        0xE0 when refused (e.g. USB dongle CH554 or bootloader disabled in CCFG), or None on timeout. Does not send
        HOST_ACK on 0x00 so the ROM auto-baud detector sees 0x55 0x55 first."""
        was = self.auto_ack
        self.auto_ack = False
        try:
            p = self.ows_call(host.OWS_SUB_ENTER_SBL, b'OSBL', timeout=timeout)
        finally:
            self.auto_ack = was
        if p and p[0] != 0x00 and was:
            self._write(host.build_ack(host.OWS_RSP))
        return p[0] if p else None

    def network(self, pan=None, short=None, ext=None, timeout=1.0):
        """Queries (pan is None) or sets (pan, short, ext) the gateway's coordinator identity in OWNV item 1
        (OWS_SUB_NETWORK). Returns dict(status, pan, short, ext) with uppercase hex strings, or None on timeout."""
        req = b'' if pan is None else struct.pack('<HHQ', pan, short, ext)
        p = self.ows_call(host.OWS_SUB_NETWORK, req, timeout=timeout)
        if p is None or len(p) != 13:
            return None
        st, got_pan, got_short, got_ext = struct.unpack('<BHHQ', p)
        return {'status': st, 'pan': f'{got_pan:04X}', 'short': f'{got_short:04X}', 'ext': f'{got_ext:016X}'}

    def nv_export(self, timeout=2.0):
        """Exports the gateway's coordinator identity and sensor table over OWS_SUB_NETWORK + OWS_SUB_NV_IMPORT as an
        openwyzesense-nv-json v1 dict (compatible with tools/nv_json.py)."""
        net = self.network(timeout=timeout)
        if not net or net['status'] != 0x00:
            raise RuntimeError('gateway did not answer OWS_SUB_NETWORK (not our firmware?)')
        first = self.ows_call(host.OWS_SUB_NV_IMPORT, bytes([host.OWS_NV_GET_SENSOR, 0]), timeout=timeout)
        if not first or len(first) < 4 or first[0] != host.OWS_NV_GET_SENSOR:
            raise RuntimeError('gateway did not answer OWS_NV_GET_SENSOR')
        count = first[2]
        sensors = []
        for idx in range(count):
            r = (
                first
                if idx == 0
                else self.ows_call(host.OWS_SUB_NV_IMPORT, bytes([host.OWS_NV_GET_SENSOR, idx]), timeout=timeout)
            )
            if not r or len(r) != 17 or r[0] != host.OWS_NV_GET_SENSOR or r[1] != 0x00:
                raise RuntimeError(f'failed reading sensor index {idx}')
            sub = int.from_bytes(r[4:6], 'little')
            mac = r[6:14].decode('ascii')
            short = int.from_bytes(r[14:16], 'little')
            entry = {'sub': sub, 'mac': mac, 'short': f'{short:04X}'}
            if r[16]:
                entry['type'] = f'{r[16]:02X}'
            sensors.append(entry)
        return {
            'format': nvows.JSON_FORMAT,
            'version': 1,
            'source': {'layout': 'ows'},
            'network': {'pan': net['pan'], 'short': net['short'], 'ext': net['ext']},
            'sensors': sensors,
            'join': None,
            'dropped': [],
            'unrecognised': [],
        }

    def nv_import(self, doc, timeout=2.0):
        """Validates an openwyzesense-nv-json v1 document and imports its network + sensor table into the gateway via
        OWS_SUB_NV_IMPORT (CLEAR -> SET_NET -> ADD_SENSOR*)."""
        if doc.get('format') != nvows.JSON_FORMAT or doc.get('version') != 1 or not doc.get('network'):
            raise ValueError('expected an openwyzesense-nv-json v1 document with a network entry')
        net = doc['network']
        pan, coord, ext = int(net['pan'], 16), int(net['short'], 16), int(net['ext'], 16)
        if pan in (0x0000, 0xFFFF, nvows.HOUSE_PAN) or not 0 <= coord <= 0xFFFD:
            raise ValueError(f'network PAN {pan:04X} / coordinator {coord:04X} not usable')
        parsed = []
        seen_mac, seen_short, seen_sub = set(), {coord}, set()
        for idx, s in enumerate(doc.get('sensors', [])):
            mac = s['mac'].upper().encode('ascii')
            short = int(s['short'], 16)
            sub = int(s.get('sub', idx))
            typ = int(s.get('type', '0'), 16)
            if not nvows.hex_mac(mac) or not 0x0001 <= short <= 0xFFFD or not 0 <= sub <= 0xFFFF:
                raise ValueError(f'invalid sensor entry: {s}')
            if mac in seen_mac or short in seen_short or sub in seen_sub:
                raise ValueError(f'duplicate sensor MAC, short address, or sub-ID: {s}')
            seen_mac.add(mac)
            seen_short.add(short)
            seen_sub.add(sub)
            parsed.append((mac, short, typ & 0xFF, sub))
        if len(parsed) > 64:
            raise ValueError(f'{len(parsed)} sensors exceeds gateway table capacity (64)')
        r = self.ows_call(host.OWS_SUB_NV_IMPORT, bytes([host.OWS_NV_CLEAR]), timeout=timeout)
        if r != bytes([host.OWS_NV_CLEAR, 0x00]):
            raise RuntimeError(f'OWS_NV_CLEAR failed: {r!r}')
        r = self.ows_call(
            host.OWS_SUB_NV_IMPORT,
            bytes([host.OWS_NV_SET_NET]) + struct.pack('<HHQ', pan, coord, ext),
            timeout=timeout,
        )
        if r != bytes([host.OWS_NV_SET_NET, 0x00]):
            raise RuntimeError(f'OWS_NV_SET_NET failed: {r!r}')
        for mac, short, typ, sub in parsed:
            req = bytes([host.OWS_NV_ADD_SENSOR]) + mac + struct.pack('<HBH', short, typ, sub)
            r = self.ows_call(host.OWS_SUB_NV_IMPORT, req, timeout=timeout)
            if not r or len(r) < 2 or r[0] != host.OWS_NV_ADD_SENSOR or r[1] != 0x00:
                raise RuntimeError(f'OWS_NV_ADD_SENSOR ({mac.decode()}) failed: {r!r}')
        return len(parsed)

    def ota_cancel(self, mac=b'********'):
        """Cancels a staged OTA offer for `mac` (default `********`: clear the entire staged wave)."""
        return self.ows_call(host.OWS_SUB_OTA_OFFER, mac + bytes(24))

    def ota_batch(self, macs, builds, timeout=360, progress=None, staged=None, on_alarm=None):
        """Stages `builds` ([slot_a_img, slot_b_img]) for multiple sensor MACs (or wildcard `None` / `[b'********']`)
        and serves blocks until all targeted sensors report OWS_SUB_OTA_DONE or idle `timeout` expires. Dynamically
        extends the deadline while OWS_SUB_OTA_NEED blocks are actively advancing. Returns `{mac_bytes: status_or_None}`
        (or `None` if staging itself was refused)."""

        def fields(img):
            return img[12:16][::-1] + img[16:20][::-1]  # size, CRC (header little-endian, the offer BE)

        slot_image.validate_builds(builds)
        head = builds[0]
        offer_body = head[4:8][::-1] + head[8:12][::-1] + fields(builds[0]) + fields(builds[1])
        wildcard = macs is None or list(macs) in ([], [b'********'], [b'*'])
        if wildcard:
            if self.ows_call(host.OWS_SUB_OTA_OFFER, b'********' + offer_body) != b'\x00':
                return None
            known = set(self.sensors())
            results = {}
            pending_queue = []
            in_flight = set()
        else:
            targets = []
            for m in macs:
                mb = m.encode('ascii') if isinstance(m, str) else bytes(m)
                mb = mb.upper()
                if mb not in targets:
                    targets.append(mb)
            results = {m: None for m in targets}
            in_flight = set(targets[:8])
            pending_queue = list(targets[8:])
            for m in targets[:8]:
                if self.ows_call(host.OWS_SUB_OTA_OFFER, m + offer_body) != b'\x00':
                    return None
        if staged:
            staged()
        end = time.time() + timeout
        last_off = {}
        while time.time() < end:
            if not wildcard and not in_flight and not pending_queue:
                break
            if wildcard and known and all(results.get(k) == 0 for k in known):
                break
            m = self.backlog.pop(0) if self.backlog else self._read(0.5)
            if not m:
                continue
            if m['cmd'] == host.ALARM:
                if on_alarm:
                    on_alarm(m)
                continue
            if m['cmd'] != host.OWS_RSP or not m['payload']:
                continue
            sub, p = m['payload'][0], m['payload'][1:]
            if sub == host.OWS_SUB_OTA_NEED and len(p) == 14:
                smac = p[:8]
                if not wildcard and smac not in in_flight:
                    continue
                slot, off, n = p[8], int.from_bytes(p[9:13], 'big'), p[13]
                for k in range(off, off + n, 32):
                    self.send_ows(host.OWS_SUB_OTA_DATA, k.to_bytes(4, 'big') + builds[slot][k : min(k + 32, off + n)])
                if last_off.get(smac) != (slot, off):
                    last_off[smac] = (slot, off)
                    end = max(end, time.time() + 60.0)
                if progress:
                    progress(smac, slot, off + n, len(builds[slot]))
            elif sub == host.OWS_SUB_OTA_DONE and len(p) == 13:
                smac, st = p[:8], p[8]
                if not wildcard and smac not in in_flight and smac not in results:
                    continue
                if not wildcard or st == 0 or results.get(smac) != 0:
                    results[smac] = st
                in_flight.discard(smac)
                if pending_queue:
                    nxt = pending_queue.pop(0)
                    if self.ows_call(host.OWS_SUB_OTA_OFFER, nxt + offer_body) == b'\x00':
                        in_flight.add(nxt)
                if in_flight or (wildcard and (not known or not all(results.get(k) == 0 for k in known))):
                    end = max(end, time.time() + min(timeout, 120.0))
        return results

    def ota_update(self, mac, builds, timeout=360, progress=None, staged=None):
        """Our gateway's OTA (firmware/common/ota.h): offers the update to the sensor `mac`, then serves the blocks it
        asks for until it reports. builds: the slot images (tools/slot_image.py) linked for slot A and for slot B.
        GW_FLAG_OTA must be on; the sensor takes it within 5 minutes of a short press of its button. staged: called
        once the offer is staged (e.g. to press the button). Returns the sensor's status (0 ok;
        host.OTA_DONE_STATUS), or None on timeout."""
        mb = mac.encode('ascii').upper() if isinstance(mac, str) else bytes(mac).upper()
        cb = (lambda _m, slot, done, total: progress(slot, done, total)) if progress else None
        res = self.ota_batch([mb], builds, timeout=timeout, progress=cb, staged=staged)
        return None if res is None else res.get(mb)

    def packets(self, seconds):
        """Everything the gateway sends for `seconds` (ACKs left out)."""
        out, end = list(self.backlog), time.time() + seconds
        self.backlog = []
        while time.time() < end:
            m = self._read(end - time.time())
            if m and m['cmd'] != host.ACK:
                out.append(m)
        return out


def show(m):
    t = time.strftime('%H:%M:%S', time.localtime(m['t'])) + f'.{int(m["t"] * 1000) % 1000:03d}'
    if m['cmd'] == host.ALARM:
        a = host.parse_alarm(m['payload'])
        pt = a['plaintext']
        print(
            f'{t} alarm {pt[0]:02x} {pt[1:9].decode(errors="replace")} type {pt[9]:02x} rssi -{a["rssi"]} {pt.hex()}',
            flush=True,
        )
    else:
        print(f'{t} {m["cmd"]:04x} {m["payload"].hex()}  {m["payload"]!r}', flush=True)


def pair_once(link, seconds):
    """Scanning on until a sensor shows (or seconds pass), then the rest of the pairing as wyzesense2mqtt does it."""
    link.call(host.SET_SCAN, b'\x01')
    try:
        scan = link.wait_for(host.SCAN_RESULT, seconds)
        if not scan:
            print('nothing paired', flush=True)
            return
        mac = scan[0]['payload'][1:9]
        link.call(host.GET_R1, mac + app.R1_TOKEN, timeout=15)
    finally:
        link.call(host.SET_SCAN, b'\x00')
    link.call(host.VERIFY_SENSOR, mac + b'\xff\x04')
    print(f'paired {mac.decode(errors="replace")}', flush=True)


def main():
    import argparse
    import json

    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('port', nargs='?', default=None, help='device path, dongle, launchpad, or omit to auto-detect')
    ap.add_argument('seconds_pos', nargs='?', type=float, help=argparse.SUPPRESS)  # the old form: PORT SECONDS
    ap.add_argument('--seconds', type=float, default=None)
    ap.add_argument('--pair', type=float, default=0)
    ap.add_argument('--log')
    ap.add_argument(
        '--ota',
        nargs=3,
        metavar=('MACS', 'SLOT_A_IMG', 'SLOT_B_IMG'),
        help='stage OTA for MAC, comma-separated MAC1,MAC2,..., or * / ALL for all matching paired sensors',
    )
    ap.add_argument(
        '--ota-all',
        '--ota-all-matching-board',
        dest='ota_all',
        nargs=2,
        metavar=('SLOT_A_IMG', 'SLOT_B_IMG'),
        help='stage wildcard fleet OTA for all paired sensors matching the image board',
    )
    ap.add_argument('--network', nargs='*', metavar='HEX', help='query or set PAN SHORT EXT (hex)')
    ap.add_argument(
        '--nv-export', metavar='NV.json', help='export gateway network + sensor table to JSON (- for stdout)'
    )
    ap.add_argument('--nv-import', metavar='NV.json', help='import openwyzesense-nv-json v1 into gateway Page 30 OWNV')
    args = ap.parse_args()
    if args.ota and args.ota_all:
        ap.error('use either --ota or --ota-all, not both')
    if args.network is not None and len(args.network) not in (0, 3):
        ap.error('--network takes either 0 arguments (query) or 3 hex arguments: PAN SHORT EXT')
    ota_spec = ('*', *args.ota_all) if args.ota_all else args.ota
    default_secs = 360.0 if ota_spec else 60.0
    if args.seconds_pos is not None:
        seconds = args.seconds_pos
    elif args.seconds is not None:
        seconds = args.seconds
    else:
        seconds = default_secs
    log = [] if args.log else None
    link = HostLink(args.port, log=log)
    try:
        for k, v in link.handshake().items():
            print(f'{k}: {v.hex()}  {v!r}')
        link.send(host.TIME_SYNC, int(time.time() * 1000).to_bytes(8, 'big'))
        if args.network is not None:
            res = link.network() if not args.network else link.network(*(int(x, 16) for x in args.network))
            if not res or res['status'] != 0x00:
                sys.exit(f'network command failed: {res}')
            print(f'network: PAN {res["pan"]} short {res["short"]} ext {res["ext"]}', flush=True)
            return
        if args.nv_import:
            doc = json.loads(pathlib.Path(args.nv_import).read_text())
            n = link.nv_import(doc)
            print(f'imported network {doc["network"]["pan"]}/{doc["network"]["short"]} and {n} sensors', flush=True)
            return
        if args.nv_export:
            text = json.dumps(link.nv_export(), indent=2) + '\n'
            if args.nv_export == '-':
                sys.stdout.write(text)
            else:
                pathlib.Path(args.nv_export).write_text(text)
                print(f'exported NV to {args.nv_export}', flush=True)
            return
        print('sensors:', [s.decode(errors='replace') for s in link.sensors()], flush=True)
        if ota_spec:
            raw_target = ota_spec[0].strip()
            builds = [pathlib.Path(p).read_bytes() for p in ota_spec[1:]]
            prev_flags = link.flags()
            prev_on = prev_flags[0] if prev_flags else 0
            try:
                print('flags:', link.flags(prev_on | host.FLAG_OTA), flush=True)
                if args.pair:
                    print("hold the sensor's button 3 s now (pairing)", flush=True)
                    pair_once(link, args.pair)
                wildcard = raw_target.upper() in ('*', '********', 'ALL')
                macs = (
                    None
                    if wildcard
                    else [x.strip().upper().encode('ascii') for x in raw_target.split(',') if x.strip()]
                )
                res = link.ota_batch(
                    macs,
                    builds,
                    timeout=seconds,
                    progress=lambda m, slot, done, total: print(
                        f'  {m.decode(errors="replace")} slot {"AB"[slot]}: {done}/{total}', flush=True
                    ),
                    staged=lambda: print("staged: press each target sensor's button briefly now", flush=True),
                    on_alarm=show,
                )
                if res is None:
                    print('result: refused by gateway', flush=True)
                elif not wildcard and len(macs) == 1:
                    st = res.get(macs[0])
                    print('result:', 'timeout' if st is None else host.OTA_DONE_STATUS.get(st, st), flush=True)
                else:
                    for m, st in res.items():
                        label = 'timeout' if st is None else host.OTA_DONE_STATUS.get(st, st)
                        print(f'result {m.decode(errors="replace")}: {label}', flush=True)
            finally:
                link.ota_cancel()
                link.flags(prev_on)
            return
        end, pair_until = time.time() + seconds, time.time() + args.pair
        pairing = args.pair > 0
        if pairing:
            r = link.call(host.SET_SCAN, b'\x01')
            print(f'scanning on for {args.pair:.0f} s: {r and r[0]["payload"].hex()!r}', flush=True)
        while time.time() < end:
            if pairing and time.time() > pair_until:
                link.call(host.SET_SCAN, b'\x00')
                pairing = False
                print('scanning off (nothing paired)', flush=True)
            m = link.backlog.pop(0) if link.backlog else link._read(0.2)
            if not m or m['cmd'] == host.ACK:
                continue
            show(m)
            if m['cmd'] == host.SCAN_RESULT and pairing:
                mac = m['payload'][1:9]
                pairing = False
                print(f'pairing {mac.decode(errors="replace")}: R1', flush=True)
                r1 = link.call(host.GET_R1, mac + app.R1_TOKEN, timeout=15)
                for x in r1 or []:
                    show(x)
                link.call(host.SET_SCAN, b'\x00')
                v = link.call(host.VERIFY_SENSOR, mac + b'\xff\x04')
                print(
                    f'verify: {"ok" if v else "no reply"}; sensors: '
                    f'{[s.decode(errors="replace") for s in link.sensors()]}',
                    flush=True,
                )
    except KeyboardInterrupt:
        pass
    finally:
        if args.log:
            with open(args.log, 'w') as f:
                f.writelines(json.dumps(r) + '\n' for r in log)
        link.close()


if __name__ == '__main__':
    main()
