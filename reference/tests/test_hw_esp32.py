# The ESP32-S3 relay on hardware (#22): the S3 wired to a v1 contact board running our gateway (esp32/README.md), on
# the LAN. This PC runs a private wyzesense2mqtt-rs (bridge on) and mosquitto; the test points the S3's bridge URL at
# it (POST /setup) and checks, in order:
#   1. the relay connects and wyzesense2mqtt-rs lists the contact's gateway (its MAC) as a bridged dongle;
#   2. POST /contact/reset: the gateway comes back;
#   3. GET/POST /contact/nv: exports nv.json, imports a test sensor table, verifies round-trip, and restores;
#   4. with OWS_HW_TX=1 (the image transmits: the user's permission, the test PAN): POST /contact/flash with the
#      fw gateway, checked against the contact's MAC; the gateway comes back;
#   5. POST /ota with this tree's firmware: the S3 restarts on its other app slot and the relay connects again.
# At the end the S3's bridge URL is put back. Its Wi-Fi must be set up once by hand (the setup access point).
# Opt in: OWS_HW=1 OWS_HW_ESP32=<its IP> [OWS_HW_ESP32_AUTH=<secret or 'Bearer ...'>]
#         [OWS_HW_ESP32_PORT=/dev/ttyACMx: flash it first] [OWS_HW_CONTACT_MAC=...]
#         OWS_W2M=<wyzesense2mqtt-rs binary>; build first: tools/idf.sh build, cmake --build --preset fw.
import json
import os
import pathlib
import re
import socket
import subprocess
import tempfile
import time
import urllib.parse
import urllib.request

import pytest
from test_host_e2e import MOSQUITTO, W2M, World, free_port, wait

ROOT = pathlib.Path(__file__).resolve().parents[2]
ESP = os.environ.get('OWS_HW_ESP32')
AUTH = os.environ.get('OWS_HW_ESP32_AUTH')
PORT = os.environ.get('OWS_HW_ESP32_PORT')
MAC = os.environ.get('OWS_HW_CONTACT_MAC', '777B42DF')
FIRMWARE = pathlib.Path(os.environ.get('OWS_HW_ESP32_FIRMWARE', ROOT / 'esp32' / 'idf' / 'build' / 'ows_relay.bin'))
FIRMWARE_8266 = ROOT / 'esp32' / 'esp8266' / '.pio' / 'build' / 'd1_mini' / 'firmware.bin'
GATEWAY = pathlib.Path(
    os.environ.get('OWS_HW_GATEWAY', ROOT / 'build' / 'fw' / 'firmware' / 'gateway-contact-v1-app.bin')
)

pytestmark = pytest.mark.skipif(
    not (os.environ.get('OWS_HW') == '1' and ESP and W2M and MOSQUITTO),
    reason='needs the S3 relay on the LaunchPad bench: OWS_HW=1 OWS_HW_ESP32=<ip>, OWS_W2M, mosquitto',
)


def auth_header():
    if not AUTH:
        return {}
    if AUTH.lower().startswith(('bearer ', 'basic ')):
        return {'Authorization': AUTH}
    return {'Authorization': f'Bearer {AUTH}'}


def http(path, data=None, timeout=60):
    req = urllib.request.Request(
        f'http://{ESP}{path}', data=data, headers=auth_header(), method='POST' if data is not None else 'GET'
    )
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read().decode(errors='replace')


def multipart(field, name, data):  # the ESP8266 takes uploads only as multipart/form-data
    b = '----ows-test-boundary'
    body = (
        (
            f'--{b}\r\nContent-Disposition: form-data; name="{field}"; filename="{name}"\r\n'
            'Content-Type: application/octet-stream\r\n\r\n'
        ).encode()
        + data
        + f'\r\n--{b}--\r\n'.encode()
    )
    return body, f'multipart/form-data; boundary={b}'


def upload(path, field, name, data, esp8266, timeout=180):
    if not esp8266:
        return http(path, data, timeout)
    body, ctype = multipart(field, name, data)
    req = urllib.request.Request(
        f'http://{ESP}{path}', data=body, headers={**auth_header(), 'Content-Type': ctype}, method='POST'
    )
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read().decode(errors='replace')


def status():
    try:
        return json.loads(http('/api/status', timeout=3))
    except (OSError, ValueError):
        return None


def local_ip():  # this PC's address on the S3's network
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.connect((ESP, 80))
        return s.getsockname()[0]


def dongles(w):
    d = w.api('/api/dongles')
    return {x['mac']: x for x in d['dongles']} if d and d.get('dongles') is not None else {}


def setup(url):
    http('/setup', urllib.parse.urlencode({'url': url}).encode())
    time.sleep(3)  # it restarts
    wait(status, 60, 'the S3 back after its restart')


def test_esp32_relay_on_hardware():
    if PORT:  # flash this tree's firmware over USB first (its NVS, the Wi-Fi setup, is kept)
        subprocess.run([str(ROOT / 'tools' / 'idf.sh'), '-p', PORT, 'flash'], check=True, timeout=300)
        wait(status, 60, 'the S3 on the network after flashing')
    st = status()
    assert st, f'the S3 does not answer at http://{ESP}/api/status'
    esp8266 = st.get('chip') == 'ESP8266'  # the same test for the ESP8266 relay (NodeMCU / D1 mini)
    old_url = st.get('url', st.get('bridge'))
    old_url = old_url if isinstance(old_url, str) and old_url.startswith(('ws://', 'wss://')) else None

    tmp = pathlib.Path(tempfile.mkdtemp(prefix='ows-esp32-'))
    w = World(tmp)
    w.web = free_port()
    (tmp / 'mosquitto.conf').write_text(f'listener {w.mqtt} 127.0.0.1\nallow_anonymous true\npersistence false\n')
    (tmp / 'w2m.yaml').write_text(
        f'usb:\n  dongle: "none"\nbridge:\n  enabled: true\nweb:\n  enabled: true\n  port: {w.web}\n'
        f'mqtt:\n  enabled: true\n  host: "127.0.0.1"\n  port: {w.mqtt}\n'
    )
    try:
        w.start('mosquitto', [MOSQUITTO, '-c', str(tmp / 'mosquitto.conf')])
        env = {k: v for k, v in os.environ.items() if not k.startswith(('MQTT_', 'USB_', 'WEB_', 'BRIDGE_'))}
        w.start('w2m', [W2M, '--config', str(tmp / 'w2m.yaml')], cwd=tmp, env=env)
        wait(lambda: w.api('/api/dongles') is not None, 20, 'wyzesense2mqtt-rs web API')

        # 1. the relay to this PC's wyzesense2mqtt-rs: the contact's gateway shows up as a bridged dongle
        setup(f'ws://{local_ip()}:{w.web}/ws/bridge')
        d = wait(lambda: dongles(w).get(MAC), 60, f'{MAC} bridged by the S3')
        assert d['transport'] == 'bridge', d
        assert (status() or {}).get('relay') == 'connected'

        # 2. the contact reset from the S3: the gateway comes back (wyzesense2mqtt-rs handshakes it again)
        assert 'contact reset' in http('/contact/reset', b'')
        wait(lambda: dongles(w).get(MAC), 60, f'{MAC} back after the reset')

        # 3. GET/POST /contact/nv: export, import a synthetic sensor, verify round-trip, and restore original NV
        nv_before_raw = http('/contact/nv')
        nv_before = json.loads(nv_before_raw)
        assert nv_before.get('format') == 'openwyzesense-nv-json'
        nv_test = dict(nv_before)
        nv_test['sensors'] = list(nv_before.get('sensors', [])) + [{'mac': '77FFEE99', 'short': '007E'}]
        assert 'imported' in http('/contact/nv', json.dumps(nv_test).encode())
        nv_after = json.loads(http('/contact/nv'))
        assert any(s['mac'] == '77FFEE99' for s in nv_after.get('sensors', [])), nv_after
        assert 'imported' in http('/contact/nv', nv_before_raw.encode())
        wait(lambda: dongles(w).get(MAC), 60, f'{MAC} back after /contact/nv restore')

        # 4. the contact reflashed from the S3 (a transmitting image: OWS_HW_TX=1, the test PAN)
        if os.environ.get('OWS_HW_TX') == '1':
            assert GATEWAY.exists(), 'build it first: cmake --preset fw && cmake --build --preset fw'
            image = GATEWAY.read_bytes()
            image += b'\xff' * (-len(image) % 4)
            out = upload(f'/contact/flash?mac={MAC}&nv=keep', 'image', GATEWAY.name, image, esp8266)
            assert 'written and verified' in out, out
            wait(lambda: dongles(w).get(MAC), 60, f'{MAC} back after the reflash')

        # 5. the relay's own OTA: it restarts (an ESP32 on its other app slot), and the relay connects again
        before = status() or {}
        slot_match = re.search(r'\((ota_\d)\)', before.get('build', ''))
        assert esp8266 or slot_match
        image = (FIRMWARE_8266 if esp8266 else FIRMWARE).read_bytes()
        assert 'restart' in upload('/ota', 'image', 'firmware.bin', image, esp8266)
        time.sleep(5)
        st = wait(
            lambda: (lambda x: x if x and x.get('uptime', 1e9) < before.get('uptime', 0) + 5 else None)(status()),
            120,
            'the relay back after OTA',
        )
        if not esp8266:
            after_match = re.search(r'\((ota_\d)\)', st.get('build', ''))
            assert after_match and after_match.group(1) != slot_match.group(1), st
        wait(lambda: dongles(w).get(MAC), 60, f'{MAC} bridged again after OTA')
    finally:
        if old_url and status():
            setup(old_url)
        w.stop_all()
        print(f'logs in {tmp}')
