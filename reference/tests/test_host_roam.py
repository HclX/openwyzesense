# Roaming (#90, GW_FLAG_ROAM, docs/roaming.md) with the real binaries: two ows-host-gateway processes that share
# one network identity, and an ows-host-sensor, on one air (local UDP). A small host here plays what
# wyzesense2mqtt-rs will do (#96): pairs the sensor on one gateway, sees the other one overhear it, then hands it
# over (drop at the old owner, add at the new one) and checks that the new owner reports it and the old one only
# overhears it, with no re-pairing. No hardware, no radio. Builds: cmake --preset host && cmake --build --preset host.
import os
import pathlib
import select
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
import tty

import pytest

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'reference'))
from wyzesense_ref import host  # noqa: E402

BIN = pathlib.Path(os.environ.get('OWS_HOST_BUILD', ROOT / 'build' / 'host')) / 'firmware'
pytestmark = pytest.mark.skipif(
    not (BIN / 'ows-host-gateway').exists() or not (BIN / 'ows-host-sensor').exists(),
    reason='needs the host build (cmake --preset host && cmake --build --preset host)',
)


def wait(cond, seconds, what):
    end = time.time() + seconds
    while time.time() < end:
        v = cond()
        if v:
            return v
        time.sleep(0.05)
    raise AssertionError(f'timed out waiting for {what}')


class Host:
    """The host side of one gateway's pty: ACKs its async packets, keeps what it sent, plays the pairing host."""

    def __init__(self, path):
        self.fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        tty.setraw(self.fd)
        self.got, self.lock, self.stop = [], threading.Lock(), False
        self.thread = threading.Thread(target=self._read, daemon=True)
        self.thread.start()

    def _read(self):
        buf = b''
        while not self.stop:
            r, _, _ = select.select([self.fd], [], [], 0.05)
            if not r:
                continue
            try:
                buf += os.read(self.fd, 4096)
            except OSError:
                return
            pkts = host.scan(buf)
            if pkts:
                end = buf.rfind(pkts[-1]) + len(pkts[-1])
                buf = buf[end:]
            for p in pkts:
                m = host.parse(p)
                if not m['to_host'] or m['cmd'] == host.ACK:
                    continue
                if m['cmd'] >> 8 == host.ASYNC:
                    os.write(self.fd, host.build_ack(m['cmd']))
                if m['cmd'] == host.SCAN_RESULT:  # pairing: the token, then Verify Sensor
                    self.send(host.GET_R1, m['payload'][1:9] + b'Ok5HPNQ4lf77u754')
                elif m['cmd'] == host.GET_R1 + 1:
                    self.send(host.VERIFY_SENSOR, m['payload'][1:9] + b'\xff\x04')
                with self.lock:
                    self.got.append(m)

    def send(self, cmd, payload=b''):
        os.write(self.fd, host.build(cmd, payload))

    def ows(self, sub, payload=b''):
        n = len(self.got)
        self.send(host.OWS_CMD, bytes([sub]) + payload)
        return wait(
            lambda: next(
                (m['payload'] for m in self.since(n) if m['cmd'] == host.OWS_RSP and m['payload'][0] == sub), None
            ),
            3,
            f'0x53F1 sub 0x{sub:02x}',
        )

    def since(self, n=0):
        with self.lock:
            return self.got[n:]

    def reports(self, mac, n=0):
        """(message type, own report or overheard) for each report of sensor mac since packet n."""
        out = []
        for m in self.since(n):
            p = m['payload']
            if m['cmd'] == host.ALARM and p[9:17] == mac:
                out.append((p[8], 'alarm'))
            elif m['cmd'] == host.OWS_RSP and p[0] == host.OWS_SUB_OVERHEARD and p[12:20] == mac:
                out.append((p[11], 'overheard'))
        return out

    def close(self):
        self.stop = True
        self.thread.join(1)
        os.close(self.fd)


class Lines:
    def __init__(self, stream):
        self.lines = []
        threading.Thread(target=self._read, args=(stream,), daemon=True).start()

    def _read(self, stream):
        for line in iter(stream.readline, b''):
            self.lines.append(line.decode(errors='replace').rstrip())


@pytest.fixture
def air():
    tmp = pathlib.Path(tempfile.mkdtemp(prefix='ows-roam-'))
    procs, hosts = [], []
    port = 65535
    while port > 65000:  # the air takes 16 ports from here
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sk:
            sk.bind(('127.0.0.1', 0))
            port = sk.getsockname()[1]

    def gateway(name, ieee):
        with open(tmp / f'{name}.log', 'wb') as log:  # the child keeps its own copy of the descriptor
            procs.append(
                subprocess.Popen(
                    [str(BIN / 'ows-host-gateway'), '--air', str(port), '--ieee', ieee, '--console', 'pty']
                    + ['--pty-link', str(tmp / f'{name}.tty'), '--nv', str(tmp / f'{name}.nv')],
                    stdin=subprocess.DEVNULL,
                    stdout=log,
                    stderr=log,
                )
            )
        wait(lambda: (tmp / f'{name}.tty').exists(), 5, f'{name} pty')
        hosts.append(Host(tmp / f'{name}.tty'))
        return hosts[-1]

    def sensor():
        p = subprocess.Popen(
            [str(BIN / 'ows-host-sensor'), '--air', str(port), '--nv', str(tmp / 'sensor.nv')],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )
        procs.append(p)
        p.out = Lines(p.stdout)
        return p

    yield gateway, sensor
    for h in hosts:
        h.close()
    for p in procs[::-1]:
        p.terminate()
        try:
            p.wait(5)
        except subprocess.TimeoutExpired:
            p.kill()
    shutil.rmtree(tmp, ignore_errors=True)


def say(p, line):
    p.stdin.write((line + '\n').encode())
    p.stdin.flush()


def test_two_gateways_one_network_hand_a_sensor_over(air):
    gateway, sensor = air
    a, b = gateway('a', '00124B00E2E0000A'), gateway('b', '00124B00E2E0000B')
    roam = (host.FLAG_ROAM | host.FLAG_DEDUP).to_bytes(4, 'big')
    for g in (a, b):
        flags = g.ows(host.OWS_SUB_FLAGS, roam)
        assert int.from_bytes(flags[1:5], 'big') == host.FLAG_ROAM | host.FLAG_DEDUP
    # one identity: b takes a's (as when both are migrated from one stock dongle)
    net = a.ows(host.OWS_SUB_NETWORK)
    assert net[1] == 0
    assert b.ows(host.OWS_SUB_NETWORK, net[2:14])[1] == 0
    # each hands out its own addresses
    assert a.ows(host.OWS_SUB_ADDR_RANGE, (0x0001).to_bytes(2, 'little') + (0x3FFF).to_bytes(2, 'little'))[1] == 0
    assert b.ows(host.OWS_SUB_ADDR_RANGE, (0x4000).to_bytes(2, 'little') + (0x7FFF).to_bytes(2, 'little'))[1] == 0

    # pair on b
    s = sensor()
    banner = wait(lambda: [ln for ln in s.out.lines if ' MAC ' in ln], 5, 'sensor banner')[0]
    mac = banner.split('MAC ')[1][:8].encode()
    b.send(host.SET_SCAN, b'\x01')
    say(s, '!input button 1')  # held >= 3.04 s
    time.sleep(5.5)
    say(s, '!input button 0')
    wait(lambda: any(ln.startswith('joined ') for ln in s.out.lines), 20, 'sensor joined')
    b.send(host.SET_SCAN, b'\x00')
    joins = sum(ln.startswith('joined ') for ln in s.out.lines)
    rec = b.ows(host.OWS_SUB_NV_IMPORT, bytes([host.OWS_NV_GET_SENSOR, 0]))
    assert rec[2] == 0 and rec[7:15] == mac
    addr = int.from_bytes(rec[15:17], 'little')
    assert 0x4000 <= addr <= 0x7FFF, hex(addr)  # b's range
    assert a.ows(host.OWS_SUB_NV_IMPORT, bytes([host.OWS_NV_GET_SENSOR, 0]))[2] != 0  # not in a's table

    # b reports it, a overhears it
    na, nb = len(a.got), len(b.got)
    say(s, '!input event 1')
    wait(lambda: (0xA2, 'alarm') in b.reports(mac, nb), 10, 'b reports the event')
    wait(lambda: (0xA2, 'overheard') in a.reports(mac, na), 10, 'a overhears the event')
    assert all(kind == 'overheard' for _, kind in a.reports(mac, na))
    assert all(kind == 'alarm' for _, kind in b.reports(mac, nb))

    # hand it over to a: drop at b, add at a (its address and type kept)
    b.send(host.DELETE_SENSOR, mac)
    wait(lambda: any(m['cmd'] == host.DELETE_SENSOR + 1 for m in b.since(nb)), 3, 'b dropped it')
    add = bytes([host.OWS_NV_ADD_SENSOR]) + mac + addr.to_bytes(2, 'little') + rec[17:18]
    assert a.ows(host.OWS_SUB_NV_IMPORT, add)[2] == 0
    na, nb = len(a.got), len(b.got)
    say(s, '!input event 0')
    wait(lambda: (0xA2, 'alarm') in a.reports(mac, na), 10, 'a reports the event')
    wait(lambda: (0xA2, 'overheard') in b.reports(mac, nb), 10, 'b overhears the event')
    assert all(kind == 'alarm' for _, kind in a.reports(mac, na))
    assert all(kind == 'overheard' for _, kind in b.reports(mac, nb))
    assert sum(ln.startswith('joined ') for ln in s.out.lines) == joins  # no re-pairing
