# The build info block and the image ID (firmware/common/build_info.h, tools/image_id.py, #76). The user's rule: the
# same code gives the same ID, or relays would reflash their gateways for nothing (and an OTA rollback would
# downgrade them for nothing). So the gateway for the v1 contact is built twice, in two fresh build directories with
# different version strings: same ID, and the images differ in the version field only. A change that changes the
# image (the debug build) changes the ID. Needs the ARM toolchain (tools/toolchain.sh); about a minute.
import pathlib
import shutil
import subprocess
import sys

import pytest

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import image_id  # noqa: E402

GCC = ROOT / '.toolchain'
PRODUCT = 'gateway-contact-v1'

pytestmark = pytest.mark.skipif(
    not any(GCC.glob('arm-gnu-toolchain-*/bin/arm-none-eabi-gcc')), reason='no ARM toolchain: tools/toolchain.sh'
)


def build(tmp: pathlib.Path, name: str, *defs: str) -> bytes:
    d = tmp / name
    cmake = shutil.which('cmake')
    subprocess.run([cmake, '--preset', 'fw', '-B', str(d), *defs], cwd=ROOT, check=True, capture_output=True)
    subprocess.run([cmake, '--build', str(d), '--target', f'{PRODUCT}.elf'], cwd=ROOT, check=True, capture_output=True)
    return (d / 'firmware' / f'{PRODUCT}-app.bin').read_bytes()


@pytest.fixture(scope='module')
def images(tmp_path_factory):
    tmp = tmp_path_factory.mktemp('image_id')
    return {
        'a': build(tmp, 'a', '-DOWS_VERSION_OVERRIDE=v1.0-test-a'),
        'b': build(tmp, 'b', '-DOWS_VERSION_OVERRIDE=v2.7-123-gdeadbeef-dirty'),
        'debug': build(tmp, 'debug', '-DOWS_VERSION_OVERRIDE=v1.0-test-a', '-DOWS_DEBUG=ON'),
    }


def test_same_code_same_id(images):
    a, b = image_id.parse(images['a']), image_id.parse(images['b'])
    assert (a['version'], b['version']) == ('v1.0-test-a', 'v2.7-123-gdeadbeef-dirty')
    assert a['id'] == b['id'] == image_id.image_id(images['a']).hex()
    assert len(images['a']) == len(images['b'])
    at = a['offset'] + image_id.VERSION_AT
    differ = [i for i, (x, y) in enumerate(zip(images['a'], images['b'], strict=True)) if x != y]
    assert differ and all(at <= i < at + image_id.VERSION_LEN for i in differ)


def test_the_block(images):
    a = image_id.parse(images['a'])
    assert a['offset'] < image_id.SEARCH and a['format'] == 1
    assert (a['product'], a['app'], a['board']) == (PRODUCT, 'gateway', 'Wyze Sense v1 contact')
    assert a['tx'] and not a['debug']


def test_another_image_another_id(images):
    d = image_id.parse(images['debug'])
    assert d['debug'] and d['id'] != image_id.parse(images['a'])['id']
