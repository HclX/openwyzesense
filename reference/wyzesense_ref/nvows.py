# Our NV page format (docs/nv-format.md, issue #61): encode, decode, and the JSON the flashing tools carry data in
# (export from a stock page or one of ours, validated; import into ours). The firmware's nvstore.c follows the same
# spec; the tests check that both read each other's pages.
import zlib

from . import nvpage

PAGE_SIZE = 0x1000
MAGIC, VERSION = b'OWNV', 1
HEADER = MAGIC + bytes([VERSION, 0xFF, 0xFF, 0xFF])
MARKER, LIVE, DELETED = 0xA5, 0xFF, 0x00
REC = 12  # record header
MAX_DATA = 64
ITEM_NETWORK, ITEM_COUNT, ITEM_SENSOR, ITEM_JOIN, ITEM_CFG = 1, 4, 5, 0x20, 0x21
HOUSE_PAN = 0x845B
JSON_FORMAT = 'openwyzesense-nv-json'
PULL_TO_U8 = {'up': 0, 'none': 1, 'down': 2, 'default': 0xFF}
U8_TO_PULL = {v: k for k, v in PULL_TO_U8.items()}
MAX_DEBOUNCE_MS = 5000


def crc(item, sub, data):
    return zlib.crc32(item.to_bytes(2, 'little') + sub.to_bytes(2, 'little') + len(data).to_bytes(2, 'little') + data)


def record(item, sub, data, live=True):
    if len(data) % 4 or len(data) > MAX_DATA:
        raise ValueError(f'item {item}/{sub}: {len(data)} bytes: a multiple of 4, at most {MAX_DATA}')
    h = bytes([MARKER, LIVE if live else DELETED]) + item.to_bytes(2, 'little') + sub.to_bytes(2, 'little')
    return h + len(data).to_bytes(2, 'little') + crc(item, sub, data).to_bytes(4, 'little') + data


def is_ows(page):
    return len(page) >= 8 and page[:8] == HEADER


def records(page):
    """page -> [(offset, item, sub, live, data)] in write order: the good records (a bad CRC is skipped), up to the
    first header that is not one."""
    if not is_ows(page):
        return []
    limit = min(len(page), PAGE_SIZE)
    out, at = [], 8
    while at + REC <= limit:
        h = page[at : at + REC]
        if h[0] != MARKER:
            break
        item, sub, n = (int.from_bytes(h[k : k + 2], 'little') for k in (2, 4, 6))
        if n % 4 or n > MAX_DATA or at + REC + n > limit:
            break
        data = page[at + REC : at + REC + n]
        if int.from_bytes(h[8:12], 'little') == crc(item, sub, data):  # a bad CRC: this record is invalid, skipped
            out.append((at, item, sub, h[1] == LIVE, data))
        at += REC + n
    return out


def latest(page):
    """{(item, sub): data}: the last live record of each (a deleted record clears earlier copies)."""
    out = {}
    for _, item, sub, live, data in records(page):
        if live:
            out[(item, sub)] = data
        else:
            out.pop((item, sub), None)
    return out


def compact(page):
    """Compact an OWNV page as nvstore.c does: drop deleted/superseded records and corrupt tail bytes, keeping current
    live records in write order."""
    cur = latest(page)
    seen, kept = set(), []
    for _, item, sub, live, data in reversed(records(page)):
        if live and (item, sub) in cur and (item, sub) not in seen:
            seen.add((item, sub))
            kept.append((item, sub, data))
    out = bytearray(HEADER)
    for item, sub, data in reversed(kept):
        out += record(item, sub, data)
    if len(out) > PAGE_SIZE:
        raise ValueError(f'{len(out)} bytes: more than a page')
    return bytes(out) + b'\xff' * (PAGE_SIZE - len(out))


def encode(items):
    """{(item, sub): data} -> a page (sorted by item, sub)."""
    page = bytearray(HEADER)
    for (item, sub), data in sorted(items.items()):
        page += record(item, sub, data)
    if len(page) > PAGE_SIZE:
        raise ValueError(f'{len(page)} bytes: more than a page')
    return bytes(page) + b'\xff' * (PAGE_SIZE - len(page))


# --- the items' data (docs/nv-format.md) ---


def h16(v):
    return f'{v:04X}'


def network_data(pan, short, ext):
    return pan.to_bytes(2, 'little') + short.to_bytes(2, 'little') + ext.to_bytes(8, 'little') + b'\x17\x00\xbe\xbe'


def sensor_data(pan, short, mac):
    r = bytearray(24)
    r[0:2], r[2:4], r[4:12], r[17] = pan.to_bytes(2, 'little'), short.to_bytes(2, 'little'), mac, 0x01
    return bytes(r)


def join_data(j):
    return (
        bytes([int(j['type'], 16), 1 if j['gw_ours'] else 0])
        + b''.join(int(j[k], 16).to_bytes(2, 'little') for k in ('pan', 'coord', 'short'))
        + b'\x00\x00'
        + int(j['gw_features'], 16).to_bytes(2, 'little')
    )


def cfg_data(c):
    raw_type = c.get('type')
    c_type = 0 if raw_type in (None, '', '00', 'default') else int(raw_type, 16)
    if not 0 <= c_type <= 0xFF:
        raise ValueError(f'cfg type {raw_type!r} out of range')
    pull_name = c.get('pull') or 'default'
    if pull_name not in PULL_TO_U8:
        raise ValueError(f'cfg pull {pull_name!r} must be one of {tuple(PULL_TO_U8)}')
    debounce_ms = int(c.get('debounce_ms', 0))
    if not 0 <= debounce_ms <= MAX_DEBOUNCE_MS:
        raise ValueError(f'cfg debounce_ms {debounce_ms} out of range 0..{MAX_DEBOUNCE_MS}')
    flags = 1 if c.get('invert') else 0
    return bytes([c_type, flags, PULL_TO_U8[pull_name], 0]) + debounce_ms.to_bytes(2, 'little') + b'\x00\x00'


def hex_mac(m):
    return len(m) == 8 and all(c in b'0123456789ABCDEF' for c in m) and m != b'00000000'


# --- export: a page (stock or ours) -> validated JSON ---


def export(page):
    if is_ows(page):
        layout, items = 'ows', latest(page)
    elif page.rstrip(b'\xff') == b'' or page[:4] not in (b'\x00\x00\x10\x00', b'\x00\x00\x00\x00'):
        layout, items = 'blank', {}
    else:
        layout, items = 'stock', nvpage.latest(page)
    out = {
        'format': JSON_FORMAT,
        'version': 1,
        'source': {'layout': layout},
        'network': None,
        'sensors': [],
        'join': None,
        'cfg': None,
        'dropped': [],
        'unrecognised': [],
    }

    def drop(what, why):
        out['dropped'].append({'what': what, 'why': why})

    pan = None
    net = items.get((ITEM_NETWORK, 0))
    if net is not None and len(net) >= 12:
        p, s, e = (
            int.from_bytes(net[0:2], 'little'),
            int.from_bytes(net[2:4], 'little'),
            int.from_bytes(net[4:12], 'little'),
        )
        if p in (0x0000, 0xFFFF) or s in (0xFFFE, 0xFFFF):
            drop('network', f'PAN {h16(p)} / coordinator {h16(s)} not usable')
        else:
            out['network'], pan = {'pan': h16(p), 'short': h16(s), 'ext': f'{e:016X}'}, p
    by_mac, by_short = {}, {}
    for (item, sub), d in sorted(items.items()):
        if item != ITEM_SENSOR:
            continue
        what = f'sensor {sub}'
        if len(d) != 24:
            drop(what, f'{len(d)} bytes, not 24')
            continue
        p, s, mac = int.from_bytes(d[0:2], 'little'), int.from_bytes(d[2:4], 'little'), d[4:12]
        if not hex_mac(mac):
            drop(what, f'MAC {mac.hex()} is not 8 hex digits')
        elif not 0x0001 <= s <= 0xFFFD:
            drop(what, f'short address {h16(s)} not usable')
        elif pan is None or p != pan:
            drop(what, f"PAN {h16(p)} is not the network's")
        elif mac in by_mac:
            drop(what, f'MAC {mac.decode()} already in sensor {by_mac[mac]}')
        elif s in by_short:  # never two on one address (roaming owns by address): the user re-pairs this one
            drop(what, f'shares short address {h16(s)} with {by_short[s]}: re-pair {mac.decode()} after the migration')
            out['dropped'][-1]['mac'] = mac.decode()
        else:
            by_mac[mac], by_short[s] = sub, mac.decode()
            out['sensors'].append({'sub': sub, 'mac': mac.decode(), 'short': h16(s)})
    j = items.get((ITEM_JOIN, 0)) if layout == 'ows' else None
    if j is not None:
        p, c, s = (int.from_bytes(j[k : k + 2], 'little') for k in (2, 4, 6))
        if len(j) != 12 or p in (0x0000, 0xFFFF, HOUSE_PAN) or not 0x0001 <= s <= 0xFFFD:
            drop('join', f'PAN {h16(p)} / short address {h16(s)} not usable')
        else:
            out['join'] = {
                'type': f'{j[0]:02X}',
                'gw_ours': bool(j[1] & 1),
                'pan': h16(p),
                'coord': h16(c),
                'short': h16(s),
                'gw_features': h16(int.from_bytes(j[10:12], 'little')),
            }
    c = items.get((ITEM_CFG, 0)) if layout == 'ows' else None
    if c is not None:
        db = int.from_bytes(c[4:6], 'little') if len(c) == 8 else -1
        if len(c) != 8 or c[2] not in U8_TO_PULL or db > MAX_DEBOUNCE_MS:
            drop('cfg', f'sensor cfg {c.hex()} not usable')
        else:
            out['cfg'] = {
                'type': f'{c[0]:02X}' if c[0] else None,
                'invert': bool(c[1] & 1),
                'pull': U8_TO_PULL[c[2]],
                'debounce_ms': db,
            }
    known = {ITEM_NETWORK, ITEM_COUNT, ITEM_SENSOR} | ({ITEM_JOIN, ITEM_CFG} if layout == 'ows' else set())
    for (item, sub), d in sorted(items.items()):
        if item not in known:
            out['unrecognised'].append({'item': item, 'sub': sub, 'data': d.hex()})
    return out


# --- import: JSON -> a page in our format ---


def import_(doc):
    if doc.get('format') != JSON_FORMAT or doc.get('version') != 1:
        raise ValueError('not an openwyzesense-nv-json version 1 document')
    items = {}
    net = doc.get('network')
    if net:
        pan = int(net['pan'], 16)
        net_short = int(net['short'], 16)
        ext = int(net['ext'], 16)
        if pan in (0x0000, 0xFFFF) or not 0 <= pan <= 0xFFFF:
            raise ValueError(f'network PAN {net["pan"]} not usable')
        if not 0x0000 <= net_short <= 0xFFFD:
            raise ValueError(f'network coordinator short address {net["short"]} not usable')
        if not 0 <= ext <= 0xFFFFFFFFFFFFFFFF:
            raise ValueError(f'network IEEE address {net["ext"]} out of range')
        items[(ITEM_NETWORK, 0)] = network_data(pan, net_short, ext)
        sensors = doc.get('sensors', [])
        if len(sensors) > 255:
            raise ValueError(f'{len(sensors)} sensors: more than a page can hold')
        seen_subs, seen_macs, seen_shorts = set(), set(), set()
        for s in sensors:
            sub = int(s['sub'])
            if not 0 <= sub <= 0xFFFF or sub in seen_subs:
                raise ValueError(f'sensor {s}: invalid or duplicate sub-ID {sub}')
            mac = s['mac'].encode()
            if not hex_mac(mac) or mac in seen_macs:
                raise ValueError(f'sensor {s}: invalid or duplicate MAC')
            s_short = int(s['short'], 16)
            if not 0x0001 <= s_short <= 0xFFFD or s_short == net_short or s_short in seen_shorts:
                raise ValueError(f'sensor {s}: invalid or duplicate short address')
            seen_subs.add(sub)
            seen_macs.add(mac)
            seen_shorts.add(s_short)
            items[(ITEM_SENSOR, sub)] = sensor_data(pan, s_short, mac)
        items[(ITEM_COUNT, 0)] = bytes([len(sensors), 0, 0, 0])
    elif doc.get('sensors'):
        raise ValueError('sensors without a network')
    if doc.get('join'):
        j = doc['join']
        j_pan = int(j['pan'], 16)
        j_coord = int(j['coord'], 16)
        j_short = int(j['short'], 16)
        j_type = int(j['type'], 16)
        j_feat = int(j['gw_features'], 16)
        if j_pan == HOUSE_PAN:
            raise ValueError('a join to the house network (PAN 845B): never')
        if j_pan in (0x0000, 0xFFFF) or not 0 <= j_pan <= 0xFFFF:
            raise ValueError(f'join PAN {j["pan"]} not usable')
        if not 0x0000 <= j_coord <= 0xFFFD or not 0x0001 <= j_short <= 0xFFFD:
            raise ValueError(f'join addresses {j["coord"]}/{j["short"]} not usable')
        if not 0 <= j_type <= 0xFF or not 0 <= j_feat <= 0xFFFF:
            raise ValueError('join type or gw_features out of range')
        items[(ITEM_JOIN, 0)] = join_data(j)
    if doc.get('cfg'):
        raw_cfg = cfg_data(doc['cfg'])
        if raw_cfg != b'\x00\x00\xff\x00\x00\x00\x00\x00':
            items[(ITEM_CFG, 0)] = raw_cfg
    return encode(items)
