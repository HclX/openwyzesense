# Our NV page format and the JSON the flashing tools carry data in (docs/nv-format.md, issue #61): encode / decode,
# a write cut at every byte is ignored rather than misread, every stock page in ground_truth exports as FACTS.md
# describes it (the v1.4 dongle's two shared addresses and its zero MAC dropped), and import(export()) keeps it.
import json
import os
import pathlib
import subprocess
import sys

import pytest
from wyzesense_ref import nvows

ROOT = pathlib.Path(__file__).resolve().parents[2]
HOST_BUILD = pathlib.Path(os.environ.get('OWS_HOST_BUILD', ROOT / 'build' / 'host'))
# page 30 (the sensor table) of each stock dump in ground_truth/firmware, which are not published themselves
NV = pathlib.Path(__file__).parent / 'vectors' / 'nv'


def page30_path(path):
    return NV / f'{path.split("/")[0]}.page30.bin'


def page30(path):
    return page30_path(path).read_bytes()


def test_encode_decode():
    items = {(1, 0): nvows.network_data(0x65FE, 0xD425, 0x00124B0012345678), (5, 3): bytes(24), (0x20, 0): bytes(12)}
    page = nvows.encode(items)
    assert len(page) == 4096 and nvows.is_ows(page) and page[:8] == b'OWNV\x01\xff\xff\xff'
    assert nvows.latest(page) == items
    with pytest.raises(ValueError):
        nvows.record(1, 0, b'\x00' * 6)  # not whole words


def test_a_cut_write_is_ignored_never_misread():
    base = nvows.encode({(1, 0): nvows.network_data(0x65FE, 0xD425, 1)})
    end = len(base.rstrip(b'\xff'))
    rec = nvows.record(5, 1, nvows.sensor_data(0x65FE, 0x0005, b'77A8C793'))
    for k in range(len(rec) + 1):  # the power cut after k bytes of the new record
        page = base[:end] + rec[:k] + base[end + k :]
        got = nvows.latest(page)
        assert got[(1, 0)] == base[20:36]  # page header 8, record header 12, then the data
        assert ((5, 1) in got) == (k == len(rec))


def test_stock_pages_export_as_facts_says():
    v14 = nvows.export(page30('gateway_2025-02-02_v1.4/FLASH.bin'))
    assert v14['source']['layout'] == 'stock'
    assert v14['network'] == {'pan': '2357', 'short': 'C907', 'ext': '00124B001D90494A'}
    assert len(v14['sensors']) == 10
    why = {d['what']: d['why'] for d in v14['dropped']}
    assert why == {
        'sensor 5': 'shares short address 0004 with 777C1FCA: re-pair 7788CCF6 after the migration',
        'sensor 6': 'MAC 0000000000000000 is not 8 hex digits',
    }
    assert [d.get('mac') for d in v14['dropped'] if d['what'] == 'sensor 5'] == ['7788CCF6']  # the one to re-pair
    spare = nvows.export(page30('dongle_77A85A36_2026-10-23_before_ours/flash.bin'))
    assert spare['network'] and len(spare['sensors']) == 2 and len(spare['dropped']) == 1
    contact = nvows.export(page30('contact_v1_777B42DF_2026-10-24/flash.bin'))  # every record deleted
    assert contact['network'] is None and contact['sensors'] == [] and contact['join'] is None
    blank = nvows.export(b'\xff' * 4096)
    assert blank['source']['layout'] == 'blank' and blank['network'] is None


@pytest.mark.parametrize(
    'dump',
    [
        'gateway_2025-02-02_v1.4/FLASH.bin',
        'gateway_2025-02-08_v1.8/FLASH.bin',
        'dongle_77A85A36_2026-10-23_before_ours/flash.bin',
    ],
)
def test_import_of_an_export_keeps_it(dump):
    doc = nvows.export(page30(dump))
    ours = nvows.import_(doc)
    assert nvows.is_ows(ours)
    again = nvows.export(ours)
    assert again['source']['layout'] == 'ows'
    for k in ('network', 'sensors'):
        assert again[k] == doc[k]
    assert again['dropped'] == []  # nothing invalid is carried over


def test_join_round_trip_and_the_house_network_refused():
    j = {'type': '01', 'gw_ours': True, 'pan': '65FE', 'coord': 'D425', 'short': '0006', 'gw_features': '0001'}
    doc = {'format': nvows.JSON_FORMAT, 'version': 1, 'network': None, 'sensors': [], 'join': j}
    assert nvows.export(nvows.import_(doc))['join'] == j
    with pytest.raises(ValueError):
        nvows.import_({**doc, 'join': {**j, 'pan': '845B'}})


def test_sensor_cfg_round_trip_and_validation():
    cfg = {'type': '03', 'invert': True, 'pull': 'up', 'debounce_ms': 100}
    doc = {'format': nvows.JSON_FORMAT, 'version': 1, 'network': None, 'sensors': [], 'join': None, 'cfg': cfg}
    page = nvows.import_(doc)
    exported = nvows.export(page)
    assert exported['cfg'] == cfg and exported['dropped'] == [] and exported['unrecognised'] == []

    # Board-default cfg writes no unnecessary record on Page 30.
    dflt = {'type': None, 'invert': False, 'pull': 'default', 'debounce_ms': 0}
    assert nvows.records(nvows.import_({**doc, 'cfg': dflt})) == []

    # Invalid pull or debounce_ms in an OWNV page is dropped on export; rejected on import.
    bad_page = nvows.encode({(nvows.ITEM_CFG, 0): b'\x03\x01\x09\x00\x64\x00\x00\x00'})
    bad_exp = nvows.export(bad_page)
    assert bad_exp['cfg'] is None and len(bad_exp['dropped']) == 1 and bad_exp['dropped'][0]['what'] == 'cfg'
    for bad_cfg in ({**cfg, 'pull': 'floating'}, {**cfg, 'debounce_ms': 6000}, {**cfg, 'debounce_ms': -1}):
        with pytest.raises(ValueError):
            nvows.import_({**doc, 'cfg': bad_cfg})


def test_deleted_records_compact_and_import_validation():
    net = nvows.network_data(0x65FE, 0xD425, 1)
    s1 = nvows.sensor_data(0x65FE, 0x0005, b'77A8C793')
    s2 = nvows.sensor_data(0x65FE, 0x0006, b'777B42DF')
    raw = bytearray(nvows.HEADER)
    raw += nvows.record(1, 0, net)
    raw += nvows.record(5, 1, s1)
    raw += nvows.record(5, 2, s2)
    raw += nvows.record(5, 1, s1, live=False)  # tombstone clears (5, 1)
    raw += b'\xa5\xff\x05\x00\x03\x00\x03\x00'  # corrupt non-multiple-of-4 tail header
    page = bytes(raw) + b'\xff' * (nvows.PAGE_SIZE - len(raw))
    assert nvows.latest(page) == {(1, 0): net, (5, 2): s2}

    compacted = nvows.compact(page)
    assert nvows.latest(compacted) == {(1, 0): net, (5, 2): s2}
    assert len(nvows.records(compacted)) == 2
    end = nvows.records(compacted)[-1][0] + nvows.REC + len(s2)
    assert compacted[end:] == b'\xff' * (nvows.PAGE_SIZE - end)

    # Non-OWNV page returns no records
    assert nvows.records(b'\xff' * nvows.PAGE_SIZE) == []

    base_net = {'pan': '65FE', 'short': 'D425', 'ext': '00124B0012345678'}
    for bad in (
        {'network': {**base_net, 'pan': '0000'}, 'sensors': []},
        {'network': {**base_net, 'short': 'FFFE'}, 'sensors': []},
        {
            'network': base_net,
            'sensors': [{'sub': 0, 'mac': '77A8C793', 'short': '0005'}, {'sub': 1, 'mac': '77A8C793', 'short': '0006'}],
        },
        {
            'network': base_net,
            'sensors': [{'sub': 0, 'mac': '77A8C793', 'short': '0005'}, {'sub': 1, 'mac': '777B42DF', 'short': '0005'}],
        },
        {
            'network': base_net,
            'sensors': [{'sub': 0, 'mac': '77A8C793', 'short': '0005'}, {'sub': 0, 'mac': '777B42DF', 'short': '0006'}],
        },
        {'network': base_net, 'sensors': [{'sub': 0, 'mac': '77A8C793', 'short': 'D425'}]},
        {
            'network': base_net,
            'sensors': [{'sub': i, 'mac': f'7700{i:04X}', 'short': f'{i + 1:04X}'} for i in range(113)],
        },
    ):
        with pytest.raises(ValueError):
            nvows.import_({'format': nvows.JSON_FORMAT, 'version': 1, 'join': None, **bad})


def test_the_tool(tmp_path):
    tool = [sys.executable, str(ROOT / 'tools' / 'nv_json.py')]
    js, pg = tmp_path / 'nv.json', tmp_path / 'page.bin'
    r = subprocess.run(
        [*tool, 'export', str(page30_path('gateway_2025-02-02_v1.4/FLASH.bin')), '-o', str(js)],
        capture_output=True,
        text=True,
        check=True,
    )
    assert 'dropped sensor 6' in r.stderr and len(json.loads(js.read_text())['sensors']) == 10
    subprocess.run([*tool, 'import', str(js), '-o', str(pg)], capture_output=True, check=True)
    assert nvows.is_ows(pg.read_bytes()) and len(nvows.export(pg.read_bytes())['sensors']) == 10


# The firmware's nvstore.c (built for the host: build/host/firmware/test_nvstore) and this reference read each
# other's pages: a page imported here (the v1.4 dongle's table) reads the same in C; a page the C gateway code writes
# exports here as what it wrote.
NVSTORE = HOST_BUILD / 'firmware' / 'test_nvstore'


@pytest.mark.skipif(not NVSTORE.exists(), reason='build the host tests first: cmake --build --preset host')
def test_c_reads_a_python_page(tmp_path):
    page = nvows.import_(nvows.export(page30('gateway_2025-02-02_v1.4/FLASH.bin')))
    (tmp_path / 'p.bin').write_bytes(page)
    out = subprocess.run([str(NVSTORE), '--dump', str(tmp_path / 'p.bin')], capture_output=True, text=True, check=True)
    got = {(int(i), int(s)): bytes.fromhex(d) for i, s, d in (line.split() for line in out.stdout.splitlines())}
    assert got == nvows.latest(page) and len(got) == 12  # network, count, 10 sensors


@pytest.mark.skipif(not NVSTORE.exists(), reason='build the host tests first: cmake --build --preset host')
def test_python_reads_a_c_page(tmp_path):
    subprocess.run([str(NVSTORE), '--write', str(tmp_path / 'c.bin')], check=True)
    doc = nvows.export((tmp_path / 'c.bin').read_bytes())
    assert doc['source']['layout'] == 'ows' and doc['dropped'] == []
    assert doc['network'] == {'pan': '65FE', 'short': 'D425', 'ext': '00124B0012345678'}
    assert [(s['mac'], s['short']) for s in doc['sensors']] == [('77A8C793', '0005'), ('777B42DF', '0006')]


HOST_GW = HOST_BUILD / 'firmware' / 'ows-host-gateway'


@pytest.mark.skipif(not HOST_GW.exists(), reason='build the host tests first: cmake --build --preset host')
def test_host_link_nv_import_export(tmp_path):
    import time

    sys.path.insert(0, str(ROOT / 'tools'))
    import host_link

    pty_path = tmp_path / 'gw.tty'
    nv_path = tmp_path / 'gw.nv'
    proc = subprocess.Popen(
        [str(HOST_GW), '--console', 'pty', '--pty-link', str(pty_path), '--nv', str(nv_path)],
        stderr=subprocess.PIPE,
    )
    try:
        deadline = time.time() + 3.0
        while not pty_path.exists() and time.time() < deadline:
            time.sleep(0.02)
        assert pty_path.exists()

        # Export stock v1.4 dongle (10 valid sensors), import into live gateway over HostLink, and re-export.
        v14 = nvows.export(page30('gateway_2025-02-02_v1.4/FLASH.bin'))
        link = host_link.HostLink(str(pty_path))
        try:
            link.handshake()
            refused = link.network(0x845B, 0x0001, 0x00124B0000000001)
            assert refused and refused['status'] == 0xE0
            ok_net = link.network(0x55AA, 0x0000, 0x00124B00AABBCCDD)
            assert ok_net == {'status': 0x00, 'pan': '55AA', 'short': '0000', 'ext': '00124B00AABBCCDD'}

            assert link.nv_import(v14) == 10
            got = link.nv_export()
            assert got['network'] == v14['network']
            assert got['sensors'] == v14['sensors']
            assert [m.decode() for m in link.sensors()] == [s['mac'] for s in v14['sensors']]
        finally:
            link.close()
    finally:
        proc.terminate()
        proc.wait(timeout=3)

    # The persisted Page 30 file written by ows-host-gateway matches nvows.export() identically.
    persisted = nvows.export(nv_path.read_bytes())
    assert persisted['network'] == v14['network']
    assert persisted['sensors'] == v14['sensors']


HOST_SN = HOST_BUILD / 'firmware' / 'ows-host-sensor'


@pytest.mark.skipif(not HOST_SN.exists(), reason='build the host tests first: cmake --build --preset host')
def test_c_sensor_and_python_nv_cfg_interop(tmp_path):
    import os
    import select
    import time

    pty_path = tmp_path / 'sn.tty'
    nv_path = tmp_path / 'sn.nv'
    nv_path.write_bytes(
        nvows.import_(
            {
                'format': nvows.JSON_FORMAT,
                'version': 1,
                'network': None,
                'sensors': [],
                'join': None,
                'cfg': {'type': '03', 'invert': True, 'pull': 'up', 'debounce_ms': 100},
            }
        )
    )
    proc = subprocess.Popen(
        [str(HOST_SN), '--console', 'pty', '--pty-link', str(pty_path), '--nv', str(nv_path)],
        stderr=subprocess.PIPE,
    )
    try:
        deadline = time.time() + 3.0
        while not pty_path.exists() and time.time() < deadline:
            time.sleep(0.02)
        assert pty_path.exists()
        fd = os.open(pty_path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        try:
            os.write(fd, b'info\ncfg\nprofile motion\n')
            buf = b''
            end = time.time() + 2.0
            while time.time() < end and buf.count(b'ok\r\n') < 1:
                r, _, _ = select.select([fd], [], [], 0.05)
                if r:
                    buf += os.read(fd, 4096)
            text = buf.decode('latin1')
            assert 'type 03 ' in text and 'invert 1 pull up debounce 100' in text
            assert 'cfg type 03 invert 1 pull up debounce 100' in text
        finally:
            os.close(fd)
    finally:
        proc.terminate()
        proc.wait(timeout=3)

    exported = nvows.export(nv_path.read_bytes())
    assert exported['cfg'] == {'type': '02', 'invert': False, 'pull': 'none', 'debounce_ms': 40}


def test_golden_vectors_are_current():
    # tools/nv_golden.py: what the Rust crate (crates/ows-host, tests/golden.rs) is held to, byte for byte (#42)
    sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / 'tools'))
    import nv_golden

    assert nv_golden.main(['--check']) == 0, 'run: uv run tools/nv_golden.py'
