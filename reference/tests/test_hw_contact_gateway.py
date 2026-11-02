# The gateway app on a v1 contact (product gateway-contact-v1, issue #15): the receive-only passive mirror build
# (`cmake --preset fw -B build/fw-mirror -DOWS_PASSIVE=ON -DOWS_PAN=0x845B -DOWS_SHORT=0x3366`: the house PAN, no TX
# path) flashed on the bench unit through tools/uniflash.py, its host link on the RX/TX pads (the LaunchPad's UART
# here, an ESP32 later), driven as wyzesense2mqtt does, its alarms checked against the house bridge (read-only), as
# test_hw_gateway.py does on the LaunchPad. Its MAC is the one in the unit's CCFG. Nothing transmits; it replaces the
# unit's sensor firmware (put back with test_hw_contact_v1.py or tools/uniflash.py slots).
# Opt in: OWS_HW=1 OWS_HW_CONTACT=1; OWS_HW_SECONDS (default 600).
import os
import pathlib
import sys
import tempfile
import time

import hw
import pytest
from test_firmware import Elf
from test_hw_gateway import listen_like_wyzesense2mqtt

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import uniflash  # noqa: E402
from host_link import HostLink  # noqa: E402

MIRROR = ROOT / 'build' / 'fw-mirror' / 'firmware'

pytestmark = pytest.mark.skipif(
    not (os.environ.get('OWS_HW') == '1' and os.environ.get('OWS_HW_CONTACT') == '1'),
    reason='needs a v1 contact on the LaunchPad: OWS_HW=1 OWS_HW_CONTACT=1',
)


def test_v1_contact_as_a_gateway():
    elf, app = MIRROR / 'gateway-contact-v1.elf', MIRROR / 'gateway-contact-v1-app.bin'
    assert app.exists(), (
        'build it first: cmake --preset fw -B build/fw-mirror -DOWS_PASSIVE=ON -DOWS_PAN=0x845B -DOWS_SHORT=0x3366 '
        '&& cmake --build build/fw-mirror'
    )
    e = Elf(elf)
    assert 'radio_tx' not in e.symbols and 'phy_cmd_tx' not in e.symbols  # house identity: never TX
    with tempfile.TemporaryDirectory() as t:
        mac = uniflash.read(0x1FFA8, 22, pathlib.Path(t) / 'ccfg.bin')[0x24:0x2C]
    uniflash.flash(app)
    time.sleep(1.5)
    listen_like_wyzesense2mqtt(HostLink(hw.uart()), mac)
