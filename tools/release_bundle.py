#!/usr/bin/env python3
"""The release bundle (#89): CI's build outputs, renamed per device, with SHA256SUMS and a README saying which file goes
on which device with which tool, and manifest.json (#42: each file's device, board, product, size and SHA-256, read by
crates/ows-host's Manifest, so a host program such as wyzesense2mqtt-rs downloads an image and checks it). Every CI
run assembles it as an artifact; a v* tag publishes it as a GitHub Release.

    uv run tools/release_bundle.py --version v0.1.0 --firmware build/fw/firmware --out dist

Every image that carries a version string must carry --version: a bundle never mixes builds. --ows-flash adds the
ows-flash binary (crates/ows-flash, Linux x86-64). The ESP32 / ESP8266 relay images are released from
HclX/wyzesense_relay, which carries ows-gateway-contact-v1.
"""

import argparse
import hashlib
import json
import pathlib
import shutil
import sys

# (source name in --firmware, bundle name, device, what it is, how to install); {v} is the version
FIRMWARE = [
    (
        'gateway-dongle-app.bin',
        'ows-gateway-dongle-{v}.bin',
        'Wyze USB dongle',
        'gateway',
        '`ows-flash backup DIR` (once: the stock image), then `ows-flash flash FILE --backup DIR --yes`',
    ),
    (
        'gateway-contact-v1-app.bin',
        'ows-gateway-contact-v1-{v}.bin',
        'Wyze v1 contact',
        'gateway (for an ESP relay)',
        'carried by the ESP relay images (HclX/wyzesense_relay); or `ows-flash v1 provision --gateway --app FILE`',
    ),
    (
        'boot-wyze_contact_v1.bin',
        'ows-sensor-contact-v1-boot-{v}.bin',
        'Wyze v1 contact',
        'sensor: boot selector',
        '`ows-flash v1 provision --sensor --boot FILE --slot-a <slot A>`',
    ),
    (
        'sensor-contact-v1-slot-a.img',
        'ows-sensor-contact-v1-slot-a-{v}.img',
        'Wyze v1 contact',
        'sensor: slot A',
        'provision (above); OTA: `ows-flash ota MAC SLOT_A SLOT_B` (or wyzesense2mqtt-rs)',
    ),
    (
        'sensor-contact-v1-slot-b.img',
        'ows-sensor-contact-v1-slot-b-{v}.img',
        'Wyze v1 contact',
        'sensor: slot B',
        'OTA only (`ows-flash ota`, with slot A)',
    ),
    (
        'boot-wyze_motion_v1.bin',
        'ows-sensor-motion-v1-boot-{v}.bin',
        'Wyze v1 motion',
        'sensor: boot selector',
        '`ows-flash v1 provision --sensor --board wyze_motion_v1 --boot FILE --slot-a <slot A>`',
    ),
    (
        'sensor-motion-v1-slot-a.img',
        'ows-sensor-motion-v1-slot-a-{v}.img',
        'Wyze v1 motion',
        'sensor: slot A',
        'provision (above); OTA: `ows-flash ota MAC SLOT_A SLOT_B` (or wyzesense2mqtt-rs)',
    ),
    (
        'sensor-motion-v1-slot-b.img',
        'ows-sensor-motion-v1-slot-b-{v}.img',
        'Wyze v1 motion',
        'sensor: slot B',
        'OTA only (`ows-flash ota`, with slot A)',
    ),
]
# (source name in --firmware) -> (product, board directory), as firmware/products.cmake names them
PRODUCT_BOARD = {
    'gateway-dongle-app.bin': ('gateway-dongle', 'wyze_dongle'),
    'gateway-contact-v1-app.bin': ('gateway-contact-v1', 'wyze_contact_v1'),
    'boot-wyze_contact_v1.bin': ('boot', 'wyze_contact_v1'),
    'sensor-contact-v1-slot-a.img': ('sensor-contact-v1', 'wyze_contact_v1'),
    'sensor-contact-v1-slot-b.img': ('sensor-contact-v1', 'wyze_contact_v1'),
    'boot-wyze_motion_v1.bin': ('boot', 'wyze_motion_v1'),
    'sensor-motion-v1-slot-a.img': ('sensor-motion-v1', 'wyze_motion_v1'),
    'sensor-motion-v1-slot-b.img': ('sensor-motion-v1', 'wyze_motion_v1'),
}
ROOT = pathlib.Path(__file__).resolve().parents[1]
GUIDE = 'https://github.com/HclX/openwyzesense'
# The README's section for users without a clone of the repository (#46)
QUICK_START = [
    '## Without the repository: a Wyze USB dongle',
    '',
    'Everything for moving a dongle system over is in this release (Linux x86-64):',
    '',
    '1. `chmod +x ows-flash-linux-x86_64-*`, and install `99-wyze-dongle.rules` (command in the table above; or run',
    '   `ows-flash` with `sudo`).',
    '2. Back up the dongle (its stock firmware and sensor list; keep this directory, it is the way back):',
    '   `ows-flash backup backups/dongle-1`',
    '3. Flash our gateway; the paired sensors are carried over (`--nv migrate`; it lists any it has to drop, and',
    '   saves the list as `nv.json` in the backup): `ows-flash flash ows-gateway-dongle-<version>.bin --backup',
    '   backups/dongle-1 --yes`. If it says so, unplug the dongle and plug it back in.',
    '4. Back to stock at any time: `ows-flash restore --backup backups/dongle-1 --yes`.',
    '',
    'Moving the sensors to an ESP relay instead: `ows-flash nv export backups/dongle-1/flash.bin -o nv.json`, then',
    "Import `nv.json` on the relay's page (the relay: HclX/wyzesense_relay).",
    '',
    '## Without the repository: v1 sensors and the contact-board gateway',
    '',
    'Prepared once on the bench, wired to a LaunchPad (the wiring: the user guide, levels 2-4): `ows-flash',
    "fetch-uniflash` (TI UniFlash from TI, ~380 MB, once; TI's own tool, not redistributed here), install",
    '`71-ti-xds110.rules`, then',
    '`ows-flash v1 provision --sensor --boot <boot>.bin --slot-a <slot A>.img` (a sensor) or `ows-flash v1 provision',
    "--gateway --app ows-gateway-contact-v1-<version>.bin` (the relay's contact board). It backs the unit up first",
    '(`backups/<board>/<MAC>`). Later updates go over the air: `ows-flash ota MAC <slot A> <slot B>`.',
    '',
]
UDEV = [
    ('99-wyze-dongle.rules', 'Wyze USB dongle'),
    ('71-ti-xds110.rules', 'LaunchPad XDS110 (v1 sensor bench)'),
]
MANIFEST_FORMAT = 'openwyzesense-release'
NO_VERSION = {'boot-wyze_contact_v1.bin', 'boot-wyze_motion_v1.bin'}  # boot selectors carry no version string


def bundle(version, firmware, out, ows_flash=None):
    out = pathlib.Path(out)
    out.mkdir(parents=True, exist_ok=True)
    rows, errors, files = [], [], []

    def add(src, name, device, what, how, versioned=True, product=None, board=None):
        src = pathlib.Path(src)
        if not src.is_file():
            errors.append(f'missing: {src}')
            return
        data = src.read_bytes()
        # a relay carries the gateway image, which holds the version too: a relay is checked by its own tag (#55)
        want = (versioned if isinstance(versioned, bytes) else b'') + version.encode()
        if versioned and want not in data:
            errors.append(
                f'{src}: does not carry {want.decode()} (a different build, or one that did not know its version?)'
            )
        shutil.copyfile(src, out / name)
        rows.append((name, device, what, how))
        files.append(
            {
                'file': name,
                'device': device,
                'what': what,
                'board': board,
                'product': product,
                'size': len(data),
                'sha256': hashlib.sha256(data).hexdigest(),
            }
        )

    for src, name, device, what, how in FIRMWARE:
        add(
            pathlib.Path(firmware) / src,
            name.format(v=version),
            device,
            what,
            how,
            src not in NO_VERSION,
            *PRODUCT_BOARD[src],
        )
    if ows_flash:
        add(
            ows_flash,
            f'ows-flash-linux-x86_64-{version}',
            'Linux PC (x86-64)',
            'tool: back up, flash, restore a Wyze USB dongle',
            '`ows-flash backup DIR`, then `ows-flash flash FILE --backup DIR --yes` (`--help`)',
            versioned=False,
            product='ows-flash',
        )
    # the udev rules (#46): the dongle and the LaunchPad's probe without root
    for rule, device in UDEV:
        add(
            ROOT / 'tools' / 'udev' / rule,
            rule,
            device,
            'Linux udev rule: access without root',
            f'`sudo cp {rule} /etc/udev/rules.d/ && sudo udevadm control --reload && sudo udevadm trigger`',
            versioned=False,
            product='udev',
        )
    if errors:
        raise SystemExit('release bundle not made:\n  ' + '\n  '.join(errors))

    sums = ''.join(f'{hashlib.sha256((out / n).read_bytes()).hexdigest()}  {n}\n' for n, *_ in rows)
    (out / 'SHA256SUMS').write_text(sums)
    manifest = {'format': MANIFEST_FORMAT, 'version': version, 'files': files}
    (out / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    lines = [
        f'# OpenWyzeSense {version}',
        '',
        'Firmware for Wyze Sense v1 hardware (the ESP32/ESP8266 IP relay: HclX/wyzesense_relay), and the `ows-flash`',
        'tool. Step by step,',
        f'with only these files: the user guide, {GUIDE}/tree/{version}/docs/guide. Check the files with',
        "`sha256sum -c SHA256SUMS`; manifest.json lists the same with each file's board and product, for programs",
        '(crates/ows-host).',
        '',
        'Before the first write to a Wyze device, back up its stock firmware (the tools do this, or refuse',
        'without one).',
        '',
        '| File | Device | What | How to install |',
        '|---|---|---|---|',
        *(f'| `{n}` | {dev} | {what} | {how} |' for n, dev, what, how in rows),
        '',
        *QUICK_START,
    ]
    (out / 'README.md').write_text('\n'.join(lines))
    return [r[0] for r in rows]


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--version', required=True)
    ap.add_argument('--firmware', required=True, help='build/fw/firmware (CI: the firmware artifact)')
    ap.add_argument('--ows-flash', help='the ows-flash binary (cargo build --release -p ows-flash)')
    ap.add_argument('--out', required=True)
    a = ap.parse_args(argv)
    names = bundle(a.version, a.firmware, a.out, a.ows_flash)
    print(f'{len(names)} files in {a.out}', file=sys.stderr)


if __name__ == '__main__':
    main()
