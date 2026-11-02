# The battery image of the v1 contact on the bench unit (issue #5): preset fw (standby between events), installed
# as the boot selector + slot A with tools/uniflash.py. Untouched, it sleeps after its 10 s power-up window (in standby
# its UART is off, so the console stops answering); if console input arrives during the 10 s window, a 5-minute sliding
# inactivity timer keeps it awake until "sleep" puts it to sleep immediately. A board reset wakes it.
# Events and button presses need hands (issue #8). Opt in: OWS_HW=1 OWS_HW_TX=1 OWS_HW_CONTACT=1.
import os
import pathlib
import sys
import time

import pytest
from test_hw_contact_v1 import Console

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import uniflash  # noqa: E402

B = ROOT / 'build' / 'fw' / 'firmware'

pytestmark = pytest.mark.skipif(
    not (
        os.environ.get('OWS_HW') == '1'
        and os.environ.get('OWS_HW_TX') == '1'
        and os.environ.get('OWS_HW_CONTACT') == '1'
    ),
    reason='needs a v1 contact on the LaunchPad: OWS_HW=1 OWS_HW_TX=1 OWS_HW_CONTACT=1',
)


def test_battery_image_sleeps_after_its_debug_window():
    for f in ('boot-wyze_contact_v1.bin', 'sensor-contact-v1-slot-a.img'):
        assert (B / f).exists(), f'build it first: cmake --preset fw && cmake --build --preset fw ({f})'
    con = Console()
    try:
        uniflash.slots(B / 'boot-wyze_contact_v1.bin', B / 'sensor-contact-v1-slot-a.img')
        t0 = time.time()
        banner = con.read(2)
        assert 'on Wyze Sense v1 contact, MAC 777' in banner and 'slot A' in banner, banner
        time.sleep(max(0.0, t0 + 14 - time.time()))
        answers = sum('info 777' in con.ask('info', 1.0) for _ in range(3))
        assert answers == 0, f'still answering after untouched 10 s window ({answers}/3): not sleeping'
        uniflash.reset()
        t1 = time.time()
        assert 'OpenWyzeSense sensor' in con.read(3), 'no start after the board reset'
        assert 'info 777' in con.ask('info'), 'no answer inside the 10 s debug window'
        time.sleep(max(0.0, t1 + 14 - time.time()))
        assert 'info 777' in con.ask('info'), '5-minute console awake window did not keep it awake'
        assert 'ok' in con.ask('sleep', 1.0)
        assert 'info 777' not in con.ask('info', 1.0), 'sleep command did not allow standby'
    finally:
        con.close()
