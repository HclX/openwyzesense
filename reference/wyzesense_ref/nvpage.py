# The stock dongle's flash page 30 (0x1E000-0x1EFFF): TI NV-on-chip storage. Each record is its data followed
# by an 8-byte header: system ID 0x06, flags (0x40 valid, 0x00 deleted), item ID (2), sub-ID (2), data
# length (2), little-endian; parsed from the last header backwards. The page starts with 4 bytes of its own.
# Items (FACTS.md §5): 1 network (16 bytes), 4 sensor count (4), 5/n sensor n (24), 6 a 4-byte value.
PAGE, PAGE_SIZE = 0x1E000, 0x1000


def records(page):
    """page (4096 bytes) -> [(offset, item, sub, valid, data)], in write order (oldest first)."""
    end = len(page.rstrip(b'\xff'))
    end += -end % 4  # records end on a 4-byte boundary
    out = []
    while end >= 12:
        h = page[end - 8 : end]
        if h[0] != 0x06:
            break
        item, sub, n = (int.from_bytes(h[k : k + 2], 'little') for k in (2, 4, 6))
        start = end - 8 - n
        if start < 4:
            break
        out.append((start, item, sub, bool(h[1] & 0x40), page[start : end - 8]))
        end = start
    return out[::-1]


def latest(page):
    """{(item, sub): data} for the valid records, the last one written winning."""
    out = {}
    for _, item, sub, valid, data in records(page):
        if valid:
            out[(item, sub)] = data
        else:
            out.pop((item, sub), None)
    return out


def network(page):
    """Item 1: PAN, coordinator short address, IEEE address, and the 2 bytes after them."""
    d = latest(page).get((1, 0))
    if d is None:
        return None
    return {
        'pan': int.from_bytes(d[0:2], 'little'),
        'short': int.from_bytes(d[2:4], 'little'),
        'ext': int.from_bytes(d[4:12], 'little'),
        'extra': d[12:14],
    }


def sensors(page):
    """Item 5: {sub: (pan, short address, ASCII MAC)}."""
    return {
        sub: (int.from_bytes(d[0:2], 'little'), int.from_bytes(d[2:4], 'little'), d[4:12])
        for (item, sub), d in latest(page).items()
        if item == 5 and len(d) == 24
    }
