# tools/release_bundle.py (#89): the release bundle from CI's build outputs, with fake inputs.
import hashlib
import json
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
    return fw


def test_bundle_renames_per_device_with_sums_and_readme(tmp_path):
    fw = fake_build(tmp_path, 'v0.1.0')
    names = release_bundle.bundle('v0.1.0', fw, tmp_path / 'dist')
    out = tmp_path / 'dist'
    assert len(names) == len(release_bundle.FIRMWARE) + len(release_bundle.UDEV)
    assert '99-wyze-dongle.rules' in names
    assert 'ows-gateway-dongle-v0.1.0.bin' in names and not any(n.startswith('ows-relay-') for n in names)
    sums = dict(reversed(line.split('  ')) for line in (out / 'SHA256SUMS').read_text().splitlines())
    assert set(sums) == set(names)
    for n, h in sums.items():
        assert hashlib.sha256((out / n).read_bytes()).hexdigest() == h
    manifest = json.loads((out / 'manifest.json').read_text())
    assert manifest['format'] == 'openwyzesense-release' and manifest['version'] == 'v0.1.0'
    assert {f['file']: f['sha256'] for f in manifest['files']} == sums
    dongle = next(f for f in manifest['files'] if f['product'] == 'gateway-dongle')
    assert dongle['board'] == 'wyze_dongle' and dongle['file'] == 'ows-gateway-dongle-v0.1.0.bin'
    assert dongle['size'] == (out / dongle['file']).stat().st_size
    readme = (out / 'README.md').read_text()
    assert '## Without the repository' in readme and 'ows-flash backup' in readme
    assert readme.startswith('# OpenWyzeSense v0.1.0') and all(f'`{n}`' in readme for n in names)


def test_bundle_refuses_mixed_builds_and_missing_files(tmp_path):
    fw = fake_build(tmp_path, 'v0.1.0')
    (fw / 'gateway-dongle-app.bin').write_bytes(b'image built from another commit fa614db')
    (fw / 'boot-wyze_motion_v1.bin').unlink()
    with pytest.raises(SystemExit) as e:
        release_bundle.bundle('v0.1.0', fw, tmp_path / 'dist')
    assert 'gateway-dongle-app.bin: does not carry v0.1.0' in str(e.value)
    assert 'missing:' in str(e.value) and 'boot-wyze_motion_v1.bin' in str(e.value)


def test_every_firmware_file_names_its_product_and_board(tmp_path):
    assert set(release_bundle.PRODUCT_BOARD) == {src for src, *_ in release_bundle.FIRMWARE}
    fw = fake_build(tmp_path, 'v0.2.0')
    tool = tmp_path / 'ows-flash'
    tool.write_bytes(b'ELF ows-flash 0.2.0')
    names = release_bundle.bundle('v0.2.0', fw, tmp_path / 'dist', ows_flash=tool)
    assert 'ows-flash-linux-x86_64-v0.2.0' in names
    files = json.loads((tmp_path / 'dist' / 'manifest.json').read_text())['files']
    assert all(f['product'] for f in files)


def test_the_crate_version_is_one_a_tag_can_carry():
    # the release job checks the tag vX.Y.Z against this (#42): w2m pins the crate by tag and downloads that release
    text = (ROOT / 'Cargo.toml').read_text()
    version = text.split('[workspace.package]', 1)[1].split('version = "', 1)[1].split('"', 1)[0]
    assert all(p.isdigit() for p in version.split('.')) and version.count('.') == 2
