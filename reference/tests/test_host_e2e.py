# End to end with no hardware: wyzesense2mqtt-rs, over IP, driving our gateway firmware, which pairs with and hears
# our sensor firmware; both run as programs on the host board (firmware/boards/host/host_main.c) sharing a
# simulated air. All of it local, on free ports:
#
#   mosquitto <-MQTT- wyzesense2mqtt-rs <-WebSocket- tools/ws_bridge.py | ows-relay <-pty- ows-host-gateway
#   ~air~ ows-host-sensor
#
# Checks: the dongle registers (handshake), pairing through w2m's API with the sensor's button, open/closed
# reaching MQTT as Home Assistant sees them, both devices keeping the pairing across restarts (the gateway's
# flash page in a file; w2m re-reads its sensor list), and a second sensor. Needs the wyzesense2mqtt-rs binary
# (OWS_W2M=<path>, or on PATH) and mosquitto; optionally OWS_RELAY=<path> (HclX/wyzesense_relay's ows-relay);
# builds: cmake --preset host && cmake --build --preset host.
import json
import os
import pathlib
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request

import pytest

ROOT = pathlib.Path(__file__).resolve().parents[2]
HOST_BUILD = pathlib.Path(os.environ.get('OWS_HOST_BUILD', ROOT / 'build' / 'host'))
BIN = HOST_BUILD / 'firmware'
W2M = os.environ.get('OWS_W2M') or shutil.which('wyzesense2mqtt-rs')
MOSQUITTO = shutil.which('mosquitto') or (os.path.exists('/usr/sbin/mosquitto') and '/usr/sbin/mosquitto')
MOSQUITTO_SUB = shutil.which('mosquitto_sub')
GATEWAY_IEEE = '00124B00E2E00001'

pytestmark = pytest.mark.skipif(
    not (W2M and MOSQUITTO and MOSQUITTO_SUB and (BIN / 'ows-host-gateway').exists()),
    reason='needs wyzesense2mqtt-rs (OWS_W2M=<binary>), mosquitto, and the host build (cmake --preset host)',
)


def free_port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


def wait(cond, seconds, what):
    end = time.time() + seconds
    while time.time() < end:
        v = cond()
        if v:
            return v
        time.sleep(0.1)
    raise AssertionError(f'timed out: {what}')


class Lines:
    """A process's output, collected by a thread."""

    def __init__(self, stream):
        self.lines, self.lock = [], threading.Lock()
        threading.Thread(target=self._read, args=(stream,), daemon=True).start()

    def _read(self, stream):
        for raw in iter(stream.readline, b''):
            with self.lock:
                self.lines.append(raw.decode(errors='replace').rstrip('\n'))

    def since(self, n=0):
        with self.lock:
            return self.lines[n:]

    def __len__(self):
        with self.lock:
            return len(self.lines)


class World:
    def __init__(self, tmp):
        self.tmp, self.procs = tmp, {}
        self.air, self.web, self.mqtt = free_port(), free_port(), free_port()
        while self.air > 65000:
            self.air = free_port()  # the air takes 16 ports from here

    def start(self, name, args, stdin=False, **kw):
        with open(self.tmp / f'{name}.log', 'ab') as log:  # the child keeps its own copy of the descriptor
            p = subprocess.Popen(
                args,
                stdin=subprocess.PIPE if stdin else subprocess.DEVNULL,
                stdout=subprocess.PIPE if stdin else log,
                stderr=log,
                **kw,
            )
        self.procs[name] = p
        return p

    def stop(self, name):
        p = self.procs.pop(name, None)
        if p and p.poll() is None:
            p.terminate()
            try:
                p.wait(5)
            except subprocess.TimeoutExpired:
                p.kill()
                p.wait(5)

    def stop_all(self):
        for name in list(self.procs)[::-1]:
            self.stop(name)

    def gateway(self):
        return self.start(
            'gateway',
            [
                str(BIN / 'ows-host-gateway'),
                '--air',
                str(self.air),
                '--ieee',
                GATEWAY_IEEE,
                '--console',
                'pty',
                '--pty-link',
                str(self.tmp / 'gw.tty'),
                '--nv',
                str(self.tmp / 'gw.nv'),
            ],
        )

    def sensor(self, name):
        p = self.start(
            name,
            [str(BIN / 'ows-host-sensor'), '--air', str(self.air), '--nv', str(self.tmp / f'{name}.nv')],
            stdin=True,
        )
        p.out = Lines(p.stdout)
        setattr(self, name, p)
        return p

    def api(self, path, body=None):
        req = urllib.request.Request(
            f'http://127.0.0.1:{self.web}{path}',
            method='POST' if body else 'GET',
            data=json.dumps(body).encode() if body else None,
            headers={'Content-Type': 'application/json'},
        )
        try:
            with urllib.request.urlopen(req, timeout=5) as r:
                return json.loads(r.read() or b'null')
        except (OSError, ValueError):
            return None


def say(p, line):
    p.stdin.write((line + '\n').encode())
    p.stdin.flush()


def sensor_mac(p):
    banner = wait(lambda: [line for line in p.out.since() if ' MAC ' in line], 5, 'sensor banner')[0]
    return banner.split('MAC ')[1][:8]


# ows-relay, the code the ESP32 relay runs, from HclX/wyzesense_relay's host build (OWS_RELAY=<path>)
RELAY = pathlib.Path(os.environ.get('OWS_RELAY', '/nonexistent/ows-relay'))


# the relay: tools/ws_bridge.py, and ows-relay when OWS_RELAY points at it (#22)
@pytest.fixture(params=['ws_bridge.py', 'ows-relay'])
def world(request):
    if request.param == 'ows-relay' and not RELAY.exists():
        pytest.skip('OWS_RELAY=<wyzesense_relay build/host/ows-relay> not set')
    tmp = pathlib.Path(tempfile.mkdtemp(prefix='ows-e2e-'))
    w = World(tmp)
    (tmp / 'mosquitto.conf').write_text(f'listener {w.mqtt} 127.0.0.1\nallow_anonymous true\npersistence false\n')
    (tmp / 'w2m.yaml').write_text(
        f'usb:\n  dongle: "none"\nbridge:\n  enabled: true\nweb:\n  enabled: true\n'
        f'  port: {w.web}\nmqtt:\n  enabled: true\n  host: "127.0.0.1"\n  port: {w.mqtt}\n'
    )
    try:
        w.start('mosquitto', [MOSQUITTO, '-c', str(tmp / 'mosquitto.conf')])
        time.sleep(0.5)
        sub = w.start('mqtt', [MOSQUITTO_SUB, '-h', '127.0.0.1', '-p', str(w.mqtt), '-v', '-t', '#'], stdin=True)
        w.mqtt_lines = Lines(sub.stdout)
        env = {k: v for k, v in os.environ.items() if not k.startswith(('MQTT_', 'USB_', 'WEB_', 'BRIDGE_'))}
        w.relay_kind = request.param
        w.start_w2m = lambda: w.start('w2m', [W2M, '--config', str(tmp / 'w2m.yaml')], cwd=tmp, env=env)
        w.start_w2m()
        wait(lambda: w.api('/api/dongles') is not None, 20, 'wyzesense2mqtt-rs web API')
        w.gateway()
        wait(lambda: (tmp / 'gw.tty').exists(), 5, 'gateway pty')
        relay = (
            [sys.executable, str(ROOT / 'tools' / 'ws_bridge.py')] if request.param == 'ws_bridge.py' else [str(RELAY)]
        )
        w.start('bridge', [*relay, '--device', str(tmp / 'gw.tty'), '--gateway', f'ws://127.0.0.1:{w.web}/ws/bridge'])
        yield w
    finally:
        w.stop_all()
        if hasattr(w, 'mqtt_lines'):
            (tmp / 'mqtt_messages.log').write_text('\n'.join(w.mqtt_lines.since()) + '\n')
        for name in ('sensor1', 'sensor2'):
            if hasattr(w, name):
                (tmp / f'{name}_console.log').write_text('\n'.join(getattr(w, name).out.since()) + '\n')
        print(f'logs in {tmp}')


def dongle(w):
    d = w.api('/api/dongles')
    return d['dongles'][0] if d and d.get('dongles') else None


def paired(w, mac):
    s = w.api('/api/sensors')
    return any(x.get('mac') == mac for x in (s or {}).get('sensors', []))


def mqtt_state(w, mac, since):
    """The last state published for the sensor on its state topic, after line index since."""
    for line in reversed(w.mqtt_lines.since(since)):
        topic, _, payload = line.partition(' ')
        if topic == f'wyzesense2mqtt/{mac}':
            try:
                return json.loads(payload).get('state')
            except ValueError:
                pass
    return None


def switch(w, p, mac, is_open):
    n = len(w.mqtt_lines)
    say(p, f'!input event {int(is_open)}')
    want = 'open' if is_open else 'closed'
    wait(lambda: mqtt_state(w, mac, n) == want, 10, f'{mac} {want} on MQTT')


def pair(w, p, gw_mac):
    mac = sensor_mac(p)
    assert w.api('/api/scan', {'enable': True, 'dongle_mac': gw_mac}) is not None
    say(p, '!input button 1')  # the pairing button, held >= 3.04 s (LONG_PRESS_MS: a short press opens
    time.sleep(5.5)  # the OTA window instead, sensor_app.c)
    say(p, '!input button 0')
    wait(lambda: any(line.startswith('joined ') for line in p.out.since()), 15, f'{mac} joined')
    wait(lambda: paired(w, mac), 15, f'{mac} paired in wyzesense2mqtt-rs')
    return mac


def test_wyzesense2mqtt_pairs_and_hears_our_sensor_over_ip(world):
    w = world
    d = wait(lambda: dongle(w), 20, 'our gateway registered with wyzesense2mqtt-rs')
    assert d['version'] == '0.0.0.47 V1.8 Gateway GW3U' and d['transport'] == 'bridge' and d['sensor_count'] == 0
    gw_mac = d['mac']

    s1 = w.sensor('sensor1')
    mac = pair(w, s1, gw_mac)
    assert dongle(w)['sensor_count'] == 1
    # Home Assistant discovery for it, and its state as the switch moves
    wait(
        lambda: any(line.startswith('homeassistant/') and mac in line for line in w.mqtt_lines.since()),
        10,
        'HA discovery',
    )
    switch(w, s1, mac, True)
    switch(w, s1, mac, False)

    # the gateway restarts (its flash page is a file): the bridge reconnects, w2m reads the sensor list again
    w.stop('gateway')
    wait(lambda: dongle(w) is None, 15, 'w2m noticed the gateway gone')
    w.gateway()
    d = wait(lambda: dongle(w), 30, 'gateway back')
    assert d['mac'] == gw_mac and d['sensor_count'] == 1
    switch(w, s1, mac, True)

    # the sensor resets (it keeps its join): still reporting, without pairing again
    say(s1, '!reset')
    time.sleep(0.5)
    switch(w, s1, mac, False)

    # a second sensor joins the same gateway; both report
    s2 = w.sensor('sensor2')
    mac2 = pair(w, s2, gw_mac)
    assert mac2 != mac and dongle(w)['sensor_count'] == 2
    switch(w, s2, mac2, True)
    switch(w, s1, mac, True)

    # an outage of wyzesense2mqtt-rs: ows-relay keeps the alarms (wyzesense_relay relay/buffer.c) and sends them on
    # once it is back; the event made during the outage reaches MQTT (ws_bridge.py, a thin relay, loses it)
    if w.relay_kind == 'ows-relay':
        w.stop('w2m')
        time.sleep(1)
        say(s1, '!input event 0')  # closed, while nobody listens
        time.sleep(2)
        n = len(w.mqtt_lines)
        w.start_w2m()
        wait(lambda: mqtt_state(w, mac, n) == 'closed', 30, f'{mac} closed on MQTT, sent on after the outage')
