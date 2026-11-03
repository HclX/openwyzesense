# The OTA slot images' version (firmware/image_version.cmake, #57): major << 24 | minor << 16 | patch << 8 | commits
# since the tag, from git describe; each release field one byte, the commit count saturating so it never carries
# into the patch byte; every release above every archive-repository build (numbered by commit count, < 1000).
import pathlib
import shutil
import subprocess

import pytest

ROOT = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = ROOT / 'firmware' / 'image_version.cmake'
CMAKE = shutil.which('cmake')

pytestmark = pytest.mark.skipif(not CMAKE, reason='needs cmake')


def version(describe, count=21):
    r = subprocess.run(
        [CMAKE, f'-DDESCRIBE={describe}', f'-DCOUNT={count}', '-P', str(SCRIPT)], capture_output=True, text=True
    )
    if r.returncode:
        return None, r.stderr
    return int(r.stderr.strip().splitlines()[-1]), r.stderr


@pytest.mark.parametrize(
    'describe,want',
    [
        ('v0.2.1', 0x00020100),
        ('v0.2.1-dirty', 0x00020100),
        ('v0.2.1-3-gabcdef0', 0x00020103),
        ('v0.2.1-3-gabcdef0-dirty', 0x00020103),
        ('v1.2.3-255-g1234567', 0x010203FF),
        ('v255.255.255', 0xFFFFFF00),
        ('abcdef0', 21),  # no tag: 0.0.0 + the commit count
    ],
)
def test_encoding(describe, want):
    assert version(describe)[0] == want


def test_the_commit_count_never_carries_into_the_release():
    v, err = version('v1.2.3-300-g1234567')
    assert v == 0x010203FF and '300 commits since the tag, counted as 255' in err
    assert version('v1.2.4')[0] > v  # the next release is still newer
    assert version('abcdef0', count=100000)[0] == 255  # no tag: saturated too, below every release


def test_refused():
    for describe in ('v0.3.0-rc1', 'v0.3.0-rc1-2-gabcdef0', 'v256.0.0', 'v0.256.0', 'v0.0.256', 'release-1'):
        v, err = version(describe)
        assert v is None, describe


def test_order():
    # every release above every build numbered the old way (the archive repository: < 1000 commits), and in order
    tags = ['v0.2.2', 'v0.2.2-1-gaaaaaaa', 'v0.2.3', 'v0.3.0', 'v0.10.0', 'v1.0.0']
    vs = [version(t)[0] for t in tags]
    assert vs == sorted(vs) and len(set(vs)) == len(vs) and vs[0] > 1000
