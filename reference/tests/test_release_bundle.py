# tools/release_bundle.py (#89): the release bundle from CI's build outputs, with fake inputs.
import hashlib
import pathlib
import sys

import pytest

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import release_bundle  # noqa: E402


def fake_build(tmp, version):
    fw = tmp / 'fw'
    fw.mkdir()
    for src, *_ in release_bundle.FIRMWARE:
        body = b'image ' + src.encode() + (b'' if src in release_bundle.NO_VERSION else b' ' + version.encode())
        (fw / src).write_bytes(body)
    esp = tmp / 'idf'
    for d, *_ in release_bundle.ESP32:
        (esp / d).mkdir(parents=True)
        (esp / d / 'ows_relay.bin').write_bytes(b'relay ' + version.encode())
        (esp / d / 'ows_relay_full.bin').write_bytes(b'boot+table+relay ' + version.encode())
    e8266 = tmp / 'firmware.bin'
    e8266.write_bytes(b'esp8266 ' + version.encode())
    return fw, esp, e8266


def test_bundle_renames_per_device_with_sums_and_readme(tmp_path):
    fw, esp, e8266 = fake_build(tmp_path, 'v0.1.0')
    names = release_bundle.bundle('v0.1.0', fw, tmp_path / 'dist', esp, e8266)
    out = tmp_path / 'dist'
    assert len(names) == len(release_bundle.FIRMWARE) + 2 * len(release_bundle.ESP32) + 1
    assert 'ows-gateway-dongle-v0.1.0.bin' in names and 'ows-relay-esp32-full-v0.1.0.bin' in names
    assert 'ows-relay-esp8266-v0.1.0.bin' in names
    sums = dict(reversed(line.split('  ')) for line in (out / 'SHA256SUMS').read_text().splitlines())
    assert set(sums) == set(names)
    for n, h in sums.items():
        assert hashlib.sha256((out / n).read_bytes()).hexdigest() == h
    readme = (out / 'README.md').read_text()
    assert readme.startswith('# OpenWyzeSense v0.1.0') and all(f'`{n}`' in readme for n in names)


def test_bundle_refuses_mixed_builds_and_missing_files(tmp_path):
    fw, esp, e8266 = fake_build(tmp_path, 'v0.1.0')
    (fw / 'gateway-dongle-app.bin').write_bytes(b'image built from another commit fa614db')
    (esp / 'build-esp32' / 'ows_relay_full.bin').unlink()
    with pytest.raises(SystemExit) as e:
        release_bundle.bundle('v0.1.0', fw, tmp_path / 'dist', esp, e8266)
    assert 'gateway-dongle-app.bin: does not carry the version v0.1.0' in str(e.value)
    assert 'missing:' in str(e.value) and 'ows_relay_full.bin' in str(e.value)


def test_partial_bundle_without_the_relays(tmp_path):
    fw, _, _ = fake_build(tmp_path, 'abc1234')
    names = release_bundle.bundle('abc1234', fw, tmp_path / 'dist')
    assert len(names) == len(release_bundle.FIRMWARE)
