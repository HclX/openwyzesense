# An OTA update of a v1 contact sensor over the air, no hands (issue #3): the bench unit (docs/design/v1-contact.md,
# wired to the LaunchPad's XDS110) gets the boot selector and version N in slot A through tools/uniflash.py, joined to
# our gateway on the spare dongle (PAN 0x65FE; it keeps its join from earlier, or joins now from the console); the host
# stages N+1 (both builds), the console's "ota" opens the window as a short press would; the unit takes the slot-B
# build, restarts into it, reports, and confirms itself. Transmits (test PAN): OWS_HW=1 OWS_HW_TX=1 OWS_HW_CONTACT=1.
import os
import pathlib
import re
import struct
import subprocess
import sys
import tempfile
import time

import pytest
from test_hw_contact_v1 import Console
from test_hw_sensor import DONGLE, SPARE_MAC

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import uniflash  # noqa: E402
from host_link import HostLink  # noqa: E402
from wyzesense_ref import app, host  # noqa: E402

B = ROOT / 'build' / 'fw' / 'firmware'

pytestmark = pytest.mark.skipif(
    not (
        os.environ.get('OWS_HW') == '1'
        and os.environ.get('OWS_HW_TX') == '1'
        and os.environ.get('OWS_HW_CONTACT') == '1'
    ),
    reason='needs a v1 contact on the LaunchPad and the spare dongle: OWS_HW=1 OWS_HW_TX=1 OWS_HW_CONTACT=1',
)


def builds(version, tmp):
    out = []
    for s in 'ab':
        img = tmp / f'slot-{s}-v{version}.img'
        subprocess.run(
            [
                sys.executable,
                str(ROOT / 'tools' / 'slot_image.py'),
                str(B / f'sensor-contact-v1-slot-{s}.raw'),
                str(img),
                '--board-name',
                'Wyze Sense v1 contact',
                '--version',
                str(version),
                '--slot',
                s,
            ],
            check=True,
        )
        out.append(img.read_bytes())
    return out


def joined(con, dongle, mac):
    """The unit on our gateway's network: kept from earlier, else paired now from its console."""
    if ' state 4 pan 65FE ' in con.ask('info'):
        return
    assert dongle.call(host.SET_SCAN, b'\x01')[0]['payload'] == b''
    try:
        os.write(con.fd, b'join\n')
        scan = dongle.wait_for(host.SCAN_RESULT, 15)
        assert scan and scan[0]['payload'][1:9] == mac
        assert dongle.call(host.GET_R1, mac + app.R1_TOKEN, timeout=10)
    finally:
        dongle.call(host.SET_SCAN, b'\x00')
    assert dongle.call(host.VERIFY_SENSOR, mac + b'\xff\x04')[0]['payload'] == b''
    assert 'joined 65FE' in con.read(8)


def test_ota_update_of_a_v1_contact():
    for f in ('boot-wyze_contact_v1.bin', 'sensor-contact-v1-slot-a.img', 'sensor-contact-v1-slot-b.raw'):
        assert (B / f).exists(), f'build it first: cmake --preset fw && cmake --build --preset fw ({f})'
    tmp = pathlib.Path(tempfile.mkdtemp(prefix='ows-contact-ota-'))
    version = struct.unpack_from('<I', (B / 'sensor-contact-v1-slot-a.img').read_bytes(), 8)[0]
    update = builds(version + 1, tmp)
    con = Console()
    dongle = None
    try:
        uniflash.slots(B / 'boot-wyze_contact_v1.bin', B / 'sensor-contact-v1-slot-a.img')
        banner = con.read(4)
        assert f'slot A v{version}' in banner, banner
        mac = re.search(r'MAC ([0-9A-F]{8})', banner).group(1).encode()

        dongle = HostLink(DONGLE)
        assert dongle.handshake()['mac'] == SPARE_MAC, 'not the spare dongle'
        dongle.send(host.TIME_SYNC, int(time.time() * 1000).to_bytes(8, 'big'))
        assert dongle.flags(host.FLAG_OTA | host.FLAG_ANNOUNCE)[0] == host.FLAG_OTA | host.FLAG_ANNOUNCE
        joined(con, dongle, mac)
        con.read(2)

        t0 = time.time()
        status = dongle.ota_update(mac, update, timeout=600, staged=lambda: os.write(con.fd, b'ota\n'))
        took = time.time() - t0
        print(f'update: status {status}, {len(update[1])} bytes in {took:.0f} s', flush=True)
        assert status == 0, host.OTA_DONE_STATUS.get(status, status)
        after = con.read(6)
        assert f'slot B v{version + 1}' in after, after
        assert f'slot B v{version + 1}' in con.ask('info')  # extends the 5-minute console awake window across 60 s
        assert 'image confirmed' in con.read(75), 'not confirmed within 75 s'
        assert f'slot B v{version + 1}' in con.ask('info')
    finally:
        if dongle:
            dongle.flags(0)
            dongle.close()
        con.close()
