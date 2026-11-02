# IQ front end: raw RTL-SDR .cu8 samples -> chip strings -> decoded frames (phy.py, frame.py).
#
#   Mix the channel to 0 Hz, low-pass (25 kHz), find bursts by envelope, FM-demodulate, slice at the
#   midpoint of the burst's frequency range and quantize run lengths to 100 us chips (10 kchip/s).
import numpy as np

from . import frame, phy

CHIP_RATE = 10e3


def _cu8_to_iq(raw_u8):
    raw = raw_u8.astype(np.float32) - 127.5
    return raw[0::2] + 1j * raw[1::2]


def load_cu8(path):
    return _cu8_to_iq(np.fromfile(path, dtype=np.uint8))


def _bursts_with_end(iq, fs, offset_hz, min_chips=100, threshold=5.0):
    """Yield (start_sample, end_sample, carrier_hz, chips) for each burst in iq."""
    if len(iq) < 2:
        return
    sps = fs / CHIP_RATE
    iq = iq - iq.mean()
    iq = iq * np.exp(-2j * np.pi * offset_hz * np.arange(len(iq)) / fs)
    n = np.arange(-64, 65)
    h = np.sinc(2 * 25e3 / fs * n) * np.hamming(len(n))
    iq = np.convolve(iq, h / h.sum(), 'same')
    w = max(1, int(fs * 1e-3))
    env = np.convolve(np.abs(iq), np.ones(w) / w, 'same')
    on = env > threshold * np.percentile(env, 10)  # rtl_433 clips can be mostly burst
    d = np.diff(on.astype(np.int8))
    starts = list(np.flatnonzero(d == 1) + 1)
    ends = list(np.flatnonzero(d == -1) + 1)
    if on[0]:
        starts = [0] + starts
    if on[-1]:
        ends = ends + [len(on)]
    f0 = np.angle(iq[1:] * np.conj(iq[:-1])) * fs / (2 * np.pi)
    k = max(1, int(sps / 3))
    f0 = np.convolve(f0, np.ones(k) / k, 'same')
    for s, e in zip(starts, ends, strict=False):
        if e - s < min_chips * sps:
            continue
        f = f0[s : e - 1]
        lo, hi = np.percentile(f, [5, 95])
        mid = (lo + hi) / 2
        bb = (f > mid).astype(np.int8)
        edges = np.concatenate(([0], np.flatnonzero(np.diff(bb)) + 1, [len(bb)]))
        chips = ''.join(str(bb[x]) * int(round((z - x) / sps)) for x, z in zip(edges[:-1], edges[1:], strict=True))
        yield int(s), int(e), float(mid), chips


def bursts(iq, fs, offset_hz, min_chips=100, threshold=5.0):
    """Yield (start_sample, carrier_hz, chips) for each burst in iq. offset_hz = channel - tuned frequency."""
    for s, _, carrier, chips in _bursts_with_end(iq, fs, offset_hz, min_chips, threshold):
        yield s, carrier, chips


def _decode_chips(sample, carrier, chips):
    r = phy.decode(chips)
    if not r:
        return None
    errs, bits = r
    if len(bits) < 16:
        return None
    f = frame.parse(bits)
    if len(f['mac_frame']) < 3:
        return None
    try:
        mac = frame.parse_mac(f['mac_frame'])
    except ValueError:
        return None
    return {
        'sample': sample,
        'carrier_hz': carrier,
        'path_errors': errs,
        'psdu': f['psdu'],
        'crc_ok': f['crc_ok'],
        'mac': mac,
    }


def decode_iq(iq, fs, tuned_hz, channel_hz=906.8e6):
    """Decode every frame in iq. Returns dicts: sample, carrier_hz, path_errors, psdu, crc_ok, mac."""
    out = []
    for s, carrier, chips in bursts(iq, fs, channel_hz - tuned_hz):
        d = _decode_chips(s, carrier, chips)
        if d is not None:
            out.append(d)
    return out


def decode_file(path, fs, tuned_hz, channel_hz=906.8e6, chunk_seconds=10.0):
    """Stream-decode a .cu8 file in overlapping windows (overlap >= 450 ms > ~260 ms max frame) so bursts crossing
    chunk boundaries are never truncated or duplicated and multi-hundred-MB captures use bounded memory."""
    import pathlib

    p = pathlib.Path(path)
    total_samples = p.stat().st_size // 2
    chunk_samples = max(int(chunk_seconds * fs), int(0.8 * fs))
    if total_samples <= chunk_samples:
        return decode_iq(load_cu8(p), fs, tuned_hz, channel_hz)
    overlap = min(max(int(0.45 * fs), 512), chunk_samples // 2)
    guard = min(max(int(0.02 * fs), 64), overlap // 4)
    dedup_samples = max(int(0.01 * fs), 32)
    out = []
    offset = 0
    with open(p, 'rb') as fp:
        while offset < total_samples:
            fp.seek(2 * offset)
            raw_u8 = np.frombuffer(fp.read(2 * chunk_samples), dtype=np.uint8)
            if len(raw_u8) < 4:
                break
            chunk = _cu8_to_iq(raw_u8[: len(raw_u8) & ~1])
            is_last = offset + len(chunk) >= total_samples
            for s, e, carrier, chips in _bursts_with_end(chunk, fs, channel_hz - tuned_hz):
                if offset > 0 and s < guard:
                    continue
                if not is_last and e > len(chunk) - guard:
                    continue
                d = _decode_chips(offset + s, carrier, chips)
                if d is None:
                    continue
                if any(
                    abs(d['sample'] - prev['sample']) < dedup_samples and d['psdu'] == prev['psdu'] for prev in out[-8:]
                ):
                    continue
                out.append(d)
            if is_last:
                break
            offset += len(chunk) - overlap
    return out
