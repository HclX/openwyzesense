# `ows-flash ota` (crates/ows-host's OTA, #47) end to end with no hardware: our gateway and sensor firmware as host
# programs (firmware/boards/host) on a simulated air, the gateway's host link on a pty. The sensor boots slot A of a
# two-slot flash (version 5); ows-flash offers version 6 through the gateway, the sensor takes it after a short press
# of its button, and starts again from slot B. Needs the host build (cmake --preset host) and the Rust build
# (cargo build -p ows-flash; OWS_FLASH=<binary> to use another).
import os
import pathlib
import shutil
import socket
import struct
import subprocess
import sys
import threading
import time

import pytest

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT / 'tools'), str(ROOT / 'reference')]
import slot_image  # noqa: E402
from host_link import HostLink  # noqa: E402
from wyzesense_ref import app, host  # noqa: E402

BIN = pathlib.Path(os.environ.get('OWS_HOST_BUILD', ROOT / 'build' / 'host')) / 'firmware'
OWS_FLASH = pathlib.Path(os.environ.get('OWS_FLASH') or ROOT / 'target' / 'debug' / 'ows-flash')

pytestmark = pytest.mark.skipif(
    not ((BIN / 'ows-host-gateway').exists() and (BIN / 'ows-host-sensor').exists() and OWS_FLASH.exists()),
    reason='needs the host build (cmake --preset host) and ows-flash (cargo build -p ows-flash)',
)


def slot(which, version):
    """A slot image for the host board: a vector table linked for the slot, then filler."""
    base = slot_image.SLOT_BASES[which] + slot_image.OFFSET
    raw = b'\xff' * slot_image.OFFSET + struct.pack('<II', 0x20004000, base + 0x41) + bytes(range(256)) * 12
    return slot_image.make(raw, 'host', version, slot='ab'[which])


class Proc:
    def __init__(self, args, log):
        self.p = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=log)
        self.lines, self.lock = [], threading.Lock()
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        for raw in iter(self.p.stdout.readline, b''):
            with self.lock:
                self.lines.append(raw.decode(errors='replace').rstrip('\n'))

    def say(self, line):
        self.p.stdin.write((line + '\n').encode())
        self.p.stdin.flush()

    def wait(self, text, seconds, since=0):
        end = time.time() + seconds
        while time.time() < end:
            with self.lock:
                hit = [x for x in self.lines[since:] if text in x]
            if hit:
                return hit[0]
            time.sleep(0.1)
        raise AssertionError(f'timed out waiting for {text!r}; got {self.lines[since:][-10:]}')

    def stop(self):
        if self.p.poll() is None:
            self.p.terminate()
            try:
                self.p.wait(5)
            except subprocess.TimeoutExpired:
                self.p.kill()


def free_port():
    while True:
        with socket.socket() as s:
            s.bind(('127.0.0.1', 0))
            port = s.getsockname()[1]
        if port < 65000:  # the air takes 16 ports from here
            return port


def test_ows_flash_updates_a_sensor_over_the_air(tmp_path):
    air = str(free_port())
    a5, a6, b6 = slot(0, 5), slot(0, 6), slot(1, 6)
    for name, img in (('a5', a5), ('a6', a6), ('b6', b6)):
        (tmp_path / f'{name}.img').write_bytes(img)
    with open(tmp_path / 'stderr.log', 'ab') as log:
        gw = subprocess.Popen(
            [str(BIN / 'ows-host-gateway'), '--air', air, '--console', 'pty', '--pty-link', str(tmp_path / 'gw.tty')]
            + ['--nv', str(tmp_path / 'gw.nv')],
            stdin=subprocess.DEVNULL,
            stdout=log,
            stderr=log,
        )
        sensor = Proc(
            [
                str(BIN / 'ows-host-sensor'),
                '--air',
                air,
                '--nv',
                str(tmp_path / 's.nv'),
                '--slot-a',
                str(tmp_path / 'a5.img'),
            ],
            log,
        )
        try:
            mac = sensor.wait(' MAC ', 5).split('MAC ')[1][:8].encode()
            for _ in range(50):
                if (tmp_path / 'gw.tty').exists():
                    break
                time.sleep(0.1)
            # pair it, wyzesense2mqtt-style (the console's "join": no OTA window)
            link = HostLink(str(tmp_path / 'gw.tty'))
            link.handshake()
            assert link.call(host.SET_SCAN, b'\x01')
            sensor.say('join')
            scan = link.wait_for(host.SCAN_RESULT, 15)
            assert scan and scan[0]['payload'][1:9] == mac
            assert link.call(host.GET_R1, mac + app.R1_TOKEN, timeout=10)
            link.call(host.SET_SCAN, b'\x00')
            assert link.call(host.VERIFY_SENSOR, mac + b'\xff\x04')
            sensor.wait('joined', 15)
            link.close()

            n = len(sensor.lines)
            ota = subprocess.Popen(
                [str(OWS_FLASH), 'ota', mac.decode(), str(tmp_path / 'a6.img'), str(tmp_path / 'b6.img')]
                + ['--device', str(tmp_path / 'gw.tty'), '--seconds', '90'],
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
            )
            out = []
            for raw in iter(ota.stdout.readline, b''):
                line = raw.decode(errors='replace').rstrip()
                out.append(line)
                if line.startswith('staged for'):
                    sensor.say('!input button 1')  # a short press: its OTA window
                    time.sleep(0.3)
                    sensor.say('!input button 0')
            assert ota.wait(30) == 0, '\n'.join(out)
            assert f'result {mac.decode()}: ok' in out, '\n'.join(out)
            sensor.wait('slot B', 30, since=n)  # it starts again, from the new slot
        finally:
            sensor.stop()
            gw.terminate()
            gw.wait(5)
            if os.environ.get('OWS_KEEP'):
                shutil.copytree(tmp_path, pathlib.Path(os.environ['OWS_KEEP']), dirs_exist_ok=True)
