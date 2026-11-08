# Checks on the built firmware images (build/fw/firmware/<product>.elf, from `cmake --build --preset fw`):
# its radio settings are the stock dongle's, its CCFG keeps debug + bootloader access, its RF patches are the
# stock ones, and the sniffer image has no transmit path. Skipped when the image has not been built.
import pathlib
import struct

import pytest

ROOT = pathlib.Path(__file__).resolve().parents[2]
import gt  # noqa: E402

# the stock dumps are not published (ground_truth/README.md), their CCFGs are: vectors/ccfg/<dump folder>.ccfg.bin
CCFG_VECTORS = ROOT / 'reference' / 'tests' / 'vectors' / 'ccfg'


def stock_ccfg(dump):
    return (CCFG_VECTORS / f'{dump.split("/")[0]}.ccfg.bin').read_bytes()


BUILDS = {
    'fw': ROOT / 'build' / 'fw' / 'firmware' / 'gateway-launchpad.elf',
}
PLATFORM_OBJS = 'firmware/CMakeFiles/soc_cc13x0.dir/soc/cc13x0'  # under build/<preset>/


class Elf:
    """Just enough of ELF32 (little-endian) to read symbols and the initial bytes at an address."""

    def __init__(self, path):
        self.b = path.read_bytes()
        assert self.b[:4] == b'\x7fELF' and self.b[4] == 1 and self.b[5] == 1
        (shoff,) = struct.unpack_from('<I', self.b, 0x20)
        shentsize, shnum, shstrndx = struct.unpack_from('<HHH', self.b, 0x2E)
        sh = [struct.unpack_from('<IIIIIIIIII', self.b, shoff + i * shentsize) for i in range(shnum)]
        names = sh[shstrndx][4]
        self.by_index = sh
        self.sections = {self._str(names, s[0]): s for s in sh}
        self.symbols = {}
        symtab, strtab = self.sections['.symtab'], self.sections['.strtab']
        for off in range(symtab[4], symtab[4] + symtab[5], 16):
            name, value, size, info, _, shndx = struct.unpack_from('<IIIBBH', self.b, off)
            if name:
                self.symbols[self._str(strtab[4], name)] = (value, size, shndx)

    def _str(self, table_off, i):
        end = self.b.index(b'\0', table_off + i)
        return self.b[table_off + i : end].decode()

    def initial(self, name):
        """Initial bytes of a symbol: what the file holds for it (for .data: what is copied to RAM at reset).
        Works for linked images (symbol = address) and object files (symbol = offset in its section)."""
        value, size, shndx = self.symbols[name]
        s = self.by_index[shndx]
        off = s[4] + value - s[3]
        return self.b[off : off + size]


def image(kind='fw'):
    p = BUILDS[kind]
    if not p.exists():
        pytest.skip(f'{p.relative_to(ROOT)} not built')
    return Elf(p)


def platform_objects(kind):
    image(kind)  # skips if not built
    return {p.name: Elf(p) for p in (ROOT / 'build' / kind / PLATFORM_OBJS).glob('*.obj')}


def stock_ram():
    return gt.read('firmware/gateway_2025-02-08_v1.8/RAM.bin')


def same_except(ours, stock, *skip):
    """Byte-compare two command structs, ignoring the given (offset, length) fields (status, pointers)."""
    assert len(ours) == len(stock)
    a, b = bytearray(ours), bytearray(stock)
    for off, n in skip:
        a[off : off + n] = b[off : off + n] = bytes(n)
    return a == b


def test_radio_settings_are_the_stock_ones():
    elf, ram = image(), stock_ram()
    STATUS = (2, 2)
    # the stock setup was captured chained to its next command (pNextOp, condition), which the RF driver sets
    assert same_except(
        elf.initial('phy_cmd_setup'), ram[0x2648 : 0x2648 + 40], STATUS, (4, 4), (13, 1), (28, 4)
    )  # pRegOverride
    assert same_except(elf.initial('phy_cmd_fs'), ram[0x25D8 : 0x25D8 + 24], STATUS)
    # the carrier sense chained before each frame (#85), as stock's (TI 15.4 MAC RF_cmdPropCs); pNextOp: its transmit
    assert same_except(elf.initial('phy_cmd_cs'), ram[0x25F0 : 0x25F0 + 28], STATUS, (4, 4))
    assert same_except(elf.initial('phy_cmd_rx'), ram[0x2670 : 0x2670 + 48], STATUS, (36, 12))  # pAddr, pQueue, pOutput
    ovr = elf.initial('phy_overrides')
    assert ovr == ram[0x2728 : 0x2728 + len(ovr)] and ovr.endswith(b'\xff' * 4)


def test_ccfg_keeps_debug_and_bootloader():
    ccfg = image().initial('__ccfg')

    def word(i):
        return struct.unpack_from('<I', ccfg, 4 * i)[0]

    assert word(12) == 0xC5FE0DC5  # BL_CONFIG: backdoor on, DIO13 (LaunchPad BTN-1) active low
    assert word(15) == 0xFFC500C5  # CCFG_TAP_DAP_0: CPU DAP enabled, as on the stock devices


def test_rf_patches_are_the_stock_ones():
    elf = image()
    stock = gt.read('firmware/gateway_2025-02-08_v1.8/FLASH.bin')
    assert elf.initial('patchSl_longrangeMce') == stock[0x2EC : 0x2EC + 1024]
    rfe = elf.initial('patchSl_longrangeRfe')
    assert stock[0x6EC : 0x6EC + len(rfe)] == rfe


def test_sniffer_cannot_transmit():
    # sniffer_app.c never calls radio_tx(), so --gc-sections drops radio_tx and phy_cmd_tx from its ELF
    p = ROOT / 'build' / 'fw' / 'firmware' / 'sniffer-launchpad.elf'
    if not p.exists():
        pytest.skip('sniffer-launchpad.elf not built')
    elf = Elf(p)
    assert 'radio_tx' not in elf.symbols and 'phy_cmd_tx' not in elf.symbols


@pytest.mark.parametrize(
    ('product', 'want_tx'),
    [
        ('sniffer-launchpad', False),
        ('gateway-dongle', True),
        ('sensor-contact-v1', True),
    ],
)
def test_build_info_tx_flag_matches_app(product, want_tx):
    p = ROOT / 'build' / 'fw' / 'firmware' / f'{product}.elf'
    if not p.exists():
        pytest.skip(f'{product}.elf not built')
    magic, fmt, flags, _ = struct.unpack_from('<IBBH', Elf(p).initial('build_info'), 0)
    assert magic == 0x4253574F and fmt == 1
    tx = bool(flags & 1)
    assert tx == want_tx


def test_tx_build_has_the_stock_tx_command():
    objs, ram = platform_objects('fw'), stock_ram()
    # status, pktLen (per frame) and pPkt differ at run time
    tx = objs['phy_settings.c.obj'].initial('phy_cmd_tx')
    assert same_except(tx, ram[0x2628 : 0x2628 + 32], (2, 2), (16, 2), (28, 4))


STOCK_CCFG = {
    'gateway-dongle': 'gateway_2025-02-08_v1.8/FLASH.bin',
    'sensor-motion-v1': 'motion_v1_777A9A46/dump.bin',
}


@pytest.mark.parametrize('product', sorted(STOCK_CCFG))
def test_wyze_board_ccfg_is_the_stock_one(product):
    # on Wyze hardware the CCFG must be what the stock firmware had: power (DC/DC, VDDR), LF clock source,
    # bootloader backdoor, debug port; a sensor's MAC only when built with -DOWS_SENSOR_MAC (=777A9A46 here)
    p = ROOT / 'build' / 'fw' / 'firmware' / f'{product}.elf'
    if not p.exists():
        pytest.skip(f'{product} not built')
    stock = stock_ccfg(STOCK_CCFG[product])
    ours = Elf(p).initial('__ccfg')
    unit_mac = {'sensor-motion-v1': b'777A9A46'}.get(product)
    if unit_mac and ours[0x24:0x2C] != unit_mac:
        # built without a MAC (or another one): the unit keeps its own, the rest must be the stock CCFG
        assert ours[:0x20] + ours[0x30:] == stock[:0x20] + stock[0x30:]
        if ours[0x24:0x2C] == b'\xff' * 8:
            return
        pytest.skip('built for another sensor MAC')
    assert ours == stock


@pytest.mark.parametrize('product', ['gateway-contact-v1', 'sensor-contact-v1'])
def test_contact_v1_ccfg_enables_sbl_on_w_pad(product):
    # Stock contact_v1 disabled ROM SBL (BL_CONFIG 0x00FE0100); our wyze_contact_v1 CCFG keeps stock power/clock/DAP
    # words while enabling ROM SBL on W pad DIO10 active-low (0xC5FE0AC5) so gateway-contact-v1 never locks out SBL.
    stock = stock_ccfg('contact_v1_777B42DF_2026-10-24/flash.bin')
    found = False
    for p in sorted((ROOT / 'build').glob(f'fw*/firmware/{product}.elf')):
        found = True
        ours = Elf(p).initial('__ccfg')
        assert struct.unpack_from('<I', ours, 0x30)[0] == 0xC5FE0AC5
        assert ccfg_problems(ours) == []
        assert ours[:0x20] + ours[0x34:] == stock[:0x20] + stock[0x34:]
    if not found:
        pytest.skip(f'{product} not built')


def ccfg_problems(c):
    """firmware/common/ccfg_check.c ccfg_is_safe(), in Python: what would lock a chip out."""

    def w(off):
        return int.from_bytes(c[off : off + 4], 'little')

    bl, erase, tap = w(0x30), w(0x34), w(0x3C)
    out = []
    if bl >> 24 != 0xC5:
        out.append('bootloader disabled')
    if bl & 0xFF != 0xC5:
        out.append('backdoor disabled')
    if (bl >> 8) & 0xFF > 31:
        out.append('backdoor pin')
    if (tap >> 16) & 0xFF != 0xC5:
        out.append('debug port disabled')
    if not erase & 0x100 or not erase & 1:
        out.append('erase disabled')
    if w(0x44) != 0:
        out.append('image not valid')
    if any(w(0x48 + 4 * i) != 0xFFFFFFFF for i in range(4)):
        out.append('write protection')
    return out


@pytest.mark.parametrize(
    'stock',
    [
        'gateway_2025-02-08_v1.8/FLASH.bin',
        'gateway_2025-02-02_v1.4/FLASH.bin',
        'dongle_77A85A36_2026-10-23_before_ours/flash.bin',
        'motion_v1_777A9A46/dump.bin',
    ],
)
def test_the_stock_ccfgs_pass_the_safety_check(stock):
    assert ccfg_problems(stock_ccfg(stock)) == []


@pytest.mark.parametrize('product', ['gateway-dongle', 'gateway-launchpad', 'gateway-contact-v1'])
def test_the_built_in_ccfg_copy_is_the_image_ccfg(product):
    # the copy ccfg_restore() writes (soc/cc13x0/ccfg_builtin.c) is the board's CCFG, and a safe one; on the
    # dongle that is the stock one (test_wyze_board_ccfg_is_the_stock_one)
    p = ROOT / 'build' / 'fw' / 'firmware' / f'{product}.elf'
    if not p.exists():
        pytest.skip(f'{product} (fw) not built')
    e = Elf(p)
    assert e.initial('ows_ccfg_builtin') == e.initial('__ccfg')
    assert ccfg_problems(e.initial('ows_ccfg_builtin')) == []


def test_only_slot_builds_have_a_slot_header():
    """An image linked at 0 has code in slot A's range: it must not think it runs from slot A (it once did, and
    "took an attempt" by zeroing its own code at 0x1040). Only the slot builds carry ows_slot_header, at their base."""
    elfs = sorted((ROOT / 'build').glob('fw*/firmware/*.elf'))
    if not elfs:
        pytest.skip('no firmware built')
    for p in elfs:
        if p.name.startswith('boot-'):
            continue
        sym = Elf(p).symbols.get('ows_slot_header')
        if '-slot-' in p.name:
            base = 0x1000 if p.name.endswith('-slot-a.elf') else 0xF000
            assert sym and sym[0] == base, p
        else:
            assert not sym or sym[0] == 0, p


def test_sensor_images_never_write_the_ccfg():
    """The user's rule (2026-10-24): our sensor firmware never writes the CCFG while it runs (a Wyze sensor keeps its
    MAC there; the zero-MAC units lost theirs elsewhere, but a CCFG write is the one change a reflash of the app
    cannot undo). Only the gateways have a CCFG writer (ccfg_restore: the host's two-step recovery command)."""
    elfs = sorted((ROOT / 'build').glob('fw*/firmware/sensor-*.elf'))
    if not elfs:
        pytest.skip('no sensor firmware built')
    for p in elfs:
        assert 'ccfg_restore' not in Elf(p).symbols, p
