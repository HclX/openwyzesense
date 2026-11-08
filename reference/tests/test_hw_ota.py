# An OTA update over the air (docs/design/ota.md): our sensor on the LaunchPad (preset fw: the boot selector in page 0,
# sensor-launchpad in slot A as built, version N) is paired with our gateway on the spare dongle (77A85A36,
# /dev/hidraw6, PAN 0x65FE); the host stages version N + 1 (both builds, as tools/slot_image.py makes them) for the
# sensor's MAC, and a short press of its button (its OTA window, a check-in) gets it going; the sensor takes the
# slot-B build, restarts into it, reports, and confirms itself. It TRANSMITS
# (test PAN only): opt in with OWS_HW=1 and OWS_HW_TX=1. The dongle must run our gateway with GW_FLAG_OTA (built
# from this tree, preset fw, gateway-dongle-app.bin, through ows-flash). OWS_HW_SAVE=<dir> keeps the
# dongle's host traffic and the board's log there.
import json
import os
import pathlib
import struct
import subprocess
import sys
import time

import hw
import pytest
from test_hw_sensor import BTN_PAIR, DONGLE, SPARE_MAC, UART, Board, reset_board

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from host_link import HostLink  # noqa: E402
from wyzesense_ref import app, host  # noqa: E402

B = ROOT / 'build' / 'fw' / 'firmware'
SLOT_A, SLOT_B = 0x01000, 0x0F000

pytestmark = pytest.mark.skipif(
    os.environ.get('OWS_HW') != '1' or os.environ.get('OWS_HW_TX') != '1',
    reason="transmits: set OWS_HW=1 and OWS_HW_TX=1 (with the user's permission)",
)


def jtag(*cmds):
    for _ in range(3):
        out = hw.openocd('init', *cmds, 'shutdown')
        if 'all zeroes' not in out and 'BYPASS' not in out:
            return out
        hw.usb_reset()
    raise RuntimeError(out[-1000:])


def builds(version, tmp):
    """The update: the slot-A and slot-B builds of this tree as slot images with this version."""
    out = []
    for s in 'ab':
        img = tmp / f'slot-{s}-v{version}.img'
        subprocess.run(
            [
                sys.executable,
                str(ROOT / 'tools' / 'slot_image.py'),
                str(B / f'sensor-launchpad-slot-{s}.raw'),
                str(img),
                '--board-name',
                'LAUNCHXL-CC1310',
                '--version',
                str(version),
                '--slot',
                s,
            ],
            check=True,
        )
        out.append(img.read_bytes())
    return out


def install(board):
    """The CCFG of this tree (from the full image), the selector in page 0, slot A as built, slot B erased; the
    NV page's join forgotten. Returns the banner."""
    for f in (
        'sensor-launchpad.elf',
        'boot-launchxl_cc1310.bin',
        'sensor-launchpad-slot-a.img',
        'sensor-launchpad-slot-b.raw',
    ):
        assert (B / f).exists(), f'build it first: cmake --build --preset fw ({f})'
    for _ in range(3):
        out = jtag(
            'reset halt',
            f'program {B / "sensor-launchpad.elf"}',
            f'flash erase_address {SLOT_B:#x} 0xE000',
            f'flash erase_address {hw.NV_PAGE:#x} 0x1000',
            f'program {B / "boot-launchxl_cc1310.bin"} 0x0',
            f'program {B / "sensor-launchpad-slot-a.img"} {SLOT_A:#x}',
        )
        if out.count('Programming Finished') == 3:
            break
        time.sleep(2)
    else:
        raise RuntimeError(out[-1500:])
    return reset_board(board)


def app_reset(board):
    """The app's own reset (console "reset": chip_reset, as after an update). The banner after it, or None."""
    t0 = time.time()
    board.say('reset')
    return board.wait('OpenWyzeSense sensor', 5, since=t0)


def pair(board, dongle, mac):
    """Pairing from the console ("join": no OTA window), wyzesense2mqtt-style."""
    t0 = time.time()
    assert dongle.call(host.SET_SCAN, b'\x01')[0]['payload'] == b''
    try:
        board.say('join')
        scan = dongle.wait_for(host.SCAN_RESULT, 15)
        assert scan and scan[0]['payload'][1:9] == mac
        assert dongle.call(host.GET_R1, mac + app.R1_TOKEN, timeout=10)
    finally:
        dongle.call(host.SET_SCAN, b'\x00')
    assert dongle.call(host.VERIFY_SENSOR, mac + b'\xff\x04')[0]['payload'] == b''
    for _ in range(50):
        dongle.packets(0.2)
        joined = board.wait('joined', 0, since=t0)
        if joined:
            assert joined.endswith(', gateway ours'), joined  # the A6 mark (GW_FLAG_ANNOUNCE)
            return
    raise AssertionError('not joined')


def short_press():
    """A short press of the pairing button (well under 5 s): the sensor's OTA window and check-in."""
    hw.press(BTN_PAIR)
    hw.release(BTN_PAIR)


@pytest.mark.parametrize('secure', [False, True], ids=['plain', 'secured'])
def test_ota_update_over_the_air(secure):
    """plain: the gateway without the network key (restarted first: its key is RAM only), the update started by a short
    press of the sensor's button. secured (#28): the host pushes the key, the sensor gets a secured session, and the
    sealed update needs no button: it starts at the sensor's next check-in (an A1 status, here asked for from its
    console; in the field its periodic heartbeat)."""
    from test_hw_secure import A6_OURS_SECURE, restart_gateway

    tmp = pathlib.Path(os.environ.get('OWS_HW_SAVE') or '/tmp/ows-ota') / ('secured' if secure else 'plain')
    tmp.mkdir(parents=True, exist_ok=True)
    log = []
    board = Board(UART)
    dongle = None
    try:
        banner = install(board)
        assert banner and 'slot A v' in banner and 'TX path built in' in banner, banner
        version = int(banner.split('slot A v')[1].split()[0])
        for _ in range(3):  # its reset comes back every time (chip_reset: through the selector)
            b = app_reset(board)
            assert b and f'slot A v{version}' in b, b
        assert version == struct.unpack_from('<I', (B / 'sensor-launchpad-slot-a.img').read_bytes(), 8)[0]
        mac = banner.split('MAC ')[1][:8].encode()
        update = builds(version + 1, tmp)

        restart_gateway()  # no key left from an earlier run
        dongle = HostLink(DONGLE, log=log)
        hs = dongle.handshake()
        assert hs['mac'] == SPARE_MAC, 'not the spare dongle'
        dongle.send(host.TIME_SYNC, int(time.time() * 1000).to_bytes(8, 'big'))
        flags = dongle.flags(host.FLAG_OTA | host.FLAG_ANNOUNCE)
        assert flags and flags[0] == host.FLAG_OTA | host.FLAG_ANNOUNCE, f'not a gateway with OTA: {flags}'
        if mac in dongle.sensors():
            dongle.call(host.DELETE_SENSOR, mac)
        if secure:
            assert dongle.net_key(os.urandom(16)), 'the gateway did not take the network key'

        pair(board, dongle, mac)
        dongle.packets(10)  # its first reports and polls
        t_info = time.time()
        board.say('info')
        line = board.wait('info', 2, since=t_info) or ''
        if secure:
            assert ' secure session' in line and int(line.split('gw ours ')[1][:4], 16) & A6_OURS_SECURE, line
        else:
            assert ' secure' not in line, line
        t0 = time.time()
        last = [0.0]

        def progress(slot, done, total):
            if time.time() - last[0] > 10:
                last[0] = time.time()
                print(f'  slot {"AB"[slot]}: {done}/{total} bytes, {time.time() - t0:.0f} s', flush=True)

        status = dongle.ota_update(
            mac, update, timeout=600, progress=progress, staged=(lambda: board.say('status')) if secure else short_press
        )
        took = time.time() - t0
        print(
            f'update ({"secured" if secure else "plain"}): status {status}, {len(update[1])} bytes in {took:.0f} s',
            flush=True,
        )
        assert status == 0, host.OTA_DONE_STATUS.get(status, status)
        if secure:
            assert not board.wait('button', 0, since=t0)  # nobody pressed it
        else:
            assert board.wait('button short', 1, since=t0)
        assert board.wait(f'ota offer {version + 1}, slot B', 1, since=t0)
        assert board.wait('ota done, restarting', 5, since=t0)
        banner = board.wait('OpenWyzeSense sensor', 10, since=t0)
        assert banner and f'slot B v{version + 1}' in banner, banner

        # the new image on its network: its A1 at start-up, then an event, both at the dongle
        t1 = time.time()
        pkts = dongle.packets(8)
        mine = [host.parse_alarm(m['payload'])['plaintext'] for m in pkts if m['cmd'] == host.ALARM]
        assert any(pt[0] == 0xA1 and pt[1:9] == mac for pt in mine), [pt.hex() for pt in mine]
        board.say('event 1')
        pkts = dongle.packets(5)
        mine = [host.parse_alarm(m['payload'])['plaintext'] for m in pkts if m['cmd'] == host.ALARM]
        assert any(pt[0] == 0xA2 and pt[1:9] == mac for pt in mine), [pt.hex() for pt in mine]
        assert board.wait('image confirmed', 75, since=t1)
        # and it stays: after a reset, B again
        banner = reset_board(board)
        assert banner and f'slot B v{version + 1}' in banner, banner
    finally:
        if dongle:
            dongle.call(host.SET_SCAN, b'\x00')
            dongle.call(host.DELETE_SENSOR, mac)
            dongle.flags(0)
            dongle.close()
        board.say('leave')
        board.close()
        (tmp / 'dongle_host.jsonl').write_text(''.join(json.dumps(r) + '\n' for r in log))
        (tmp / 'launchpad_uart.log').write_text(''.join(f'{t:.3f} {line}\n' for t, line in board.lines))
