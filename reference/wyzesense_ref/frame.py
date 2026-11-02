# Framing: IEEE 802.15.4g PHR + PN9-whitened PSDU with CRC-32, carrying IEEE 802.15.4 MAC frames.
#
#   PHR : 16 bits MSB first: MS(1) reserved(2) FCS type(1, 0 = 4-byte) DW(1, whitening) length(11)
#   PSDU: bytes sent LSB first; whitened with PN9 x^9+x^5+1, seed 0x1FF (whitening bit = feedback bit)
#   FCS : CRC-32 (zlib) over the MAC frame, little-endian, included in the PHR length. ACKs (dongle and sensors)
#         are the exception: their FCS is CRC-32 over the 3-byte ACK plus one zero byte (ack_fcs)
import zlib


def pn9_bits(n):
    s, out = 0x1FF, []
    for _ in range(n):
        fb = (s ^ (s >> 5)) & 1
        s = (s >> 1) | (fb << 8)
        out.append(fb)
    return out


def build(mac_frame, whiten=True, fcs=None):
    """MAC frame bytes (without FCS) -> PHR+PSDU bits, ready for phy.encode()."""
    psdu = mac_frame + (fcs if fcs is not None else zlib.crc32(mac_frame).to_bytes(4, 'little'))
    phr = (int(whiten) << 11) | len(psdu)
    bits = [(phr >> (15 - i)) & 1 for i in range(16)]
    pb = [(b >> i) & 1 for b in psdu for i in range(8)]
    if whiten:
        pb = [a ^ w for a, w in zip(pb, pn9_bits(len(pb)), strict=True)]
    return bits + pb


def parse(bits):
    """PHR+PSDU bits -> dict(phr, psdu, mac_frame, crc_ok)."""
    phr = int(''.join(map(str, bits[:16])), 2)
    fcs_len = 2 if (phr >> 12) & 1 else 4
    dw, length = (phr >> 11) & 1, phr & 0x7FF
    pb = list(bits[16 : 16 + 8 * length])
    if dw:
        pb = [a ^ w for a, w in zip(pb, pn9_bits(len(pb)), strict=True)]
    psdu = bytes(sum(pb[i + j] << j for j in range(8)) for i in range(0, len(pb) - 7, 8))
    body, fcs = psdu[:-fcs_len], psdu[-fcs_len:]
    crc_ok = fcs_len == 4 and zlib.crc32(body) == int.from_bytes(fcs, 'little')
    return {'phr': phr, 'psdu': psdu, 'mac_frame': body, 'crc_ok': crc_ok}


# --- IEEE 802.15.4 MAC ---

FRAME_TYPES = {0: 'beacon', 1: 'data', 2: 'ack', 3: 'cmd'}
COMMANDS = {
    1: 'assoc-req',
    2: 'assoc-resp',
    3: 'disassoc',
    4: 'data-req',
    5: 'panid-conflict',
    6: 'orphan',
    7: 'beacon-req',
    8: 'coord-realign',
}


def parse_mac(by):
    if len(by) < 3:
        raise ValueError('MAC frame shorter than 3 bytes')
    fc = by[0] | by[1] << 8
    dst_mode, src_mode = (fc >> 10) & 3, (fc >> 14) & 3
    if dst_mode == 1 or src_mode == 1:
        raise ValueError('reserved IEEE 802.15.4 addressing mode 1')
    m = {
        'fc': fc,
        'type': FRAME_TYPES.get(fc & 7, str(fc & 7)),
        'security': (fc >> 3) & 1,
        'pending': (fc >> 4) & 1,
        'ack_req': (fc >> 5) & 1,
        'pan_comp': (fc >> 6) & 1,
        'dst_mode': dst_mode,
        'version': (fc >> 12) & 3,
        'src_mode': src_mode,
        'seq': by[2],
    }
    i = 3

    def u16():
        nonlocal i
        if i + 2 > len(by):
            raise ValueError('truncated MAC header')
        i += 2
        return by[i - 2] | by[i - 1] << 8

    def addr(mode):
        nonlocal i
        if mode == 2:
            return u16()
        if mode == 3:
            if i + 8 > len(by):
                raise ValueError('truncated extended address')
            i += 8
            return int.from_bytes(by[i - 8 : i], 'little')
        return None

    if dst_mode:
        m['dst_pan'] = u16()
        m['dst'] = addr(dst_mode)
    if src_mode:
        m['src_pan'] = m['dst_pan'] if m['pan_comp'] and dst_mode else u16()
        m['src'] = addr(src_mode)
    m['payload'] = by[i:]
    if m['type'] == 'cmd' and m['payload']:
        m['cmd'] = COMMANDS.get(m['payload'][0], hex(m['payload'][0]))
    return m


def build_data(seq, pan, dst, src, payload, ack_req=True):
    """Data frame with PAN-ID compression and short addresses (FC 0x8861 with ack request)."""
    fc = 1 | (ack_req << 5) | (1 << 6) | (2 << 10) | (2 << 14)
    return (
        fc.to_bytes(2, 'little')
        + bytes([seq])
        + pan.to_bytes(2, 'little')
        + dst.to_bytes(2, 'little')
        + src.to_bytes(2, 'little')
        + payload
    )


def build_ack(seq, pending=False):
    return (2 | (pending << 4)).to_bytes(2, 'little') + bytes([seq])


def ack_fcs(ack):
    """The FCS stock devices send on an ACK: CRC-32 over FC, seq and one zero byte, little-endian."""
    return zlib.crc32(ack + b'\x00').to_bytes(4, 'little')
