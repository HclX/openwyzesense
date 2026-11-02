#!/usr/bin/env python3
"""The release bundle (#89): CI's build outputs, renamed per device, with SHA256SUMS and a README saying which file goes
on which device with which tool, and manifest.json (#42: each file's device, board, product, size and SHA-256, read by
crates/ows-host's Manifest, so a host program such as wyzesense2mqtt-rs downloads an image and checks it). Every CI
run assembles it as an artifact; a v* tag publishes it as a GitHub Release.

    uv run tools/release_bundle.py --version v0.1.0 --firmware build/fw/firmware --esp32 esp32/idf \
        --esp8266 esp32/esp8266/.pio/build/d1_mini/firmware.bin --out dist

--esp32 is the directory holding build/ (ESP32-S3) and build-esp32/ (classic ESP32), each with ows_relay.bin and the
merged first-flash image ows_relay_full.bin (esptool merge_bin, made in the esp32 job). Every image that carries a
version string must carry --version: a bundle never mixes builds. --esp32 / --esp8266 may be left out (a partial
bundle, for local use). --ows-flash adds the ows-flash binary (crates/ows-flash, Linux x86-64).
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
        '`tools/dongle_sbl.py flash FILE --backup <stock backup> --fast`',
    ),
    (
        'gateway-contact-v1-app.bin',
        'ows-gateway-contact-v1-{v}.bin',
        'Wyze v1 contact',
        'gateway (for an ESP relay)',
        'carried by the ESP relay images below; or `tools/uniflash.py provision --gateway --app FILE`',
    ),
    (
        'boot-wyze_contact_v1.bin',
        'ows-sensor-contact-v1-boot-{v}.bin',
        'Wyze v1 contact',
        'sensor: boot selector',
        '`tools/uniflash.py provision --sensor --boot FILE --slot-a <slot A>`',
    ),
    (
        'sensor-contact-v1-slot-a.img',
        'ows-sensor-contact-v1-slot-a-{v}.img',
        'Wyze v1 contact',
        'sensor: slot A',
        'provision (above); OTA: `tools/host_link.py PORT --ota MAC SLOT_A SLOT_B`',
    ),
    (
        'sensor-contact-v1-slot-b.img',
        'ows-sensor-contact-v1-slot-b-{v}.img',
        'Wyze v1 contact',
        'sensor: slot B',
        'OTA only (`tools/host_link.py --ota`)',
    ),
    (
        'boot-wyze_motion_v1.bin',
        'ows-sensor-motion-v1-boot-{v}.bin',
        'Wyze v1 motion',
        'sensor: boot selector',
        '`tools/uniflash.py provision --sensor --board wyze_motion_v1 --boot FILE --slot-a <slot A>`',
    ),
    (
        'sensor-motion-v1-slot-a.img',
        'ows-sensor-motion-v1-slot-a-{v}.img',
        'Wyze v1 motion',
        'sensor: slot A',
        'provision (above); OTA: `tools/host_link.py PORT --ota MAC SLOT_A SLOT_B`',
    ),
    (
        'sensor-motion-v1-slot-b.img',
        'ows-sensor-motion-v1-slot-b-{v}.img',
        'Wyze v1 motion',
        'sensor: slot B',
        'OTA only (`tools/host_link.py --ota`)',
    ),
]
ESP32 = [  # (build directory under --esp32, chip name)
    ('build', 'esp32s3', 'ESP32-S3'),
    ('build-esp32', 'esp32', 'classic ESP32 (LOLIN32)'),
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
MANIFEST_FORMAT = 'openwyzesense-release'
NO_VERSION = {'boot-wyze_contact_v1.bin', 'boot-wyze_motion_v1.bin'}  # boot selectors carry no version string


def bundle(version, firmware, out, esp32=None, esp8266=None, ows_flash=None):
    out = pathlib.Path(out)
    out.mkdir(parents=True, exist_ok=True)
    rows, errors, files = [], [], []

    def add(src, name, device, what, how, versioned=True, product=None, board=None):
        src = pathlib.Path(src)
        if not src.is_file():
            errors.append(f'missing: {src}')
            return
        data = src.read_bytes()
        if versioned and version.encode() not in data:
            errors.append(f'{src}: does not carry the version {version} (a different build?)')
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
    if esp32:
        for d, chip, label in ESP32:
            b = pathlib.Path(esp32) / d
            add(
                b / 'ows_relay_full.bin',
                f'ows-relay-{chip}-full-{version}.bin',
                label,
                'relay: first flash',
                f'`esptool.py --chip {chip} write_flash 0x0 FILE` (also clears its settings: Wi-Fi set up '
                'again via the OWS-relay-XXXX access point)',
                product=f'relay-{chip}-full',
            )
            add(
                b / 'ows_relay.bin',
                f'ows-relay-{chip}-ota-{version}.bin',
                label,
                'relay: update',
                'the relay page: Update, or `curl --data-binary @FILE http://<relay>/ota`',
                product=f'relay-{chip}-ota',
            )
    if esp8266:
        add(
            esp8266,
            f'ows-relay-esp8266-{version}.bin',
            'ESP8266 (D1 mini, NodeMCU)',
            'relay: first flash and update',
            'first: `esptool.py --chip esp8266 write_flash 0x0 FILE`; then the relay page: Update',
            product='relay-esp8266',
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
    if errors:
        raise SystemExit('release bundle not made:\n  ' + '\n  '.join(errors))

    sums = ''.join(f'{hashlib.sha256((out / n).read_bytes()).hexdigest()}  {n}\n' for n, *_ in rows)
    (out / 'SHA256SUMS').write_text(sums)
    manifest = {'format': MANIFEST_FORMAT, 'version': version, 'files': files}
    (out / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    lines = [
        f'# OpenWyzeSense {version}',
        '',
        'Firmware for Wyze Sense v1 hardware and the ESP32/ESP8266 IP relay. The tools are in the repository at this',
        'tag; see docs/user-guide.md. Check the files with `sha256sum -c SHA256SUMS`; manifest.json lists the same',
        "with each file's board and product, for programs (crates/ows-host).",
        '',
        'Before the first write to a Wyze device, back up its stock firmware (the tools do this, or refuse',
        'without one).',
        '',
        '| File | Device | What | How to install |',
        '|---|---|---|---|',
        *(f'| `{n}` | {dev} | {what} | {how} |' for n, dev, what, how in rows),
        '',
    ]
    (out / 'README.md').write_text('\n'.join(lines))
    return [r[0] for r in rows]


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--version', required=True)
    ap.add_argument('--firmware', required=True, help='build/fw/firmware (CI: the firmware artifact)')
    ap.add_argument('--esp32', help='esp32/idf, holding build/ and build-esp32/ (CI: the esp32-relay artifact)')
    ap.add_argument('--esp8266', help='the ESP8266 firmware.bin (CI: the esp8266-relay artifact)')
    ap.add_argument('--ows-flash', help='the ows-flash binary (cargo build --release -p ows-flash)')
    ap.add_argument('--out', required=True)
    a = ap.parse_args(argv)
    names = bundle(a.version, a.firmware, a.out, a.esp32, a.esp8266, a.ows_flash)
    print(f'{len(names)} files in {a.out}', file=sys.stderr)


if __name__ == '__main__':
    main()
