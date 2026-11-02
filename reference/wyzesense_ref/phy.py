# PHY: TI SimpleLink Long Range "Mode 2" (SWRA642) as used by Wyze Sense, at the coded-bit level.
#
#   On air: 906.8 MHz, 2-GFSK, 20 ksym/s, +/-5 kHz. DSSS = 2 spreads each coded bit over two equal
#   symbols, so one coded bit = one 10 kchip/s "chip" (100 us). This module works on chip strings
#   ('0'/'1'), i.e. after FM demodulation and slicing at 10 kchip/s.
#
#   Header: 64-symbol sync word 0xCCC3C3CCC33C3333 (LSB first) sent 3 times, the first 2 inverted.
#   Body:   rate-1/2 K=7 convolutional code, generators 171/133 octal (171 output first), encoder
#           starting from the zero state, zero tail bits; the coded stream is inverted relative to
#           the sync-word polarity used here. Verified bit-exact against captured bursts.

SYNC_WORD = 0xCCC3C3CCC33C3333
G0, G1, K = 0o171, 0o133, 7
TAIL_BITS = 10  # captures show 9-10 tail bits before the carrier drops


def sync_chips(inverted):
    s = [((SYNC_WORD >> i) & 1) ^ inverted for i in range(64)]
    return ''.join(str(s[i]) for i in range(0, 64, 2))  # DSSS=2: symbol pairs collapse to one chip


HEADER = sync_chips(1) + sync_chips(1) + sync_chips(0)  # 96 chips


def parity(x):
    return bin(x).count('1') & 1


def conv_encode(bits, state=0):
    out = []
    for b in bits:
        reg = (state << 1) | b
        out += [parity(reg & G0), parity(reg & G1)]
        state = reg & ((1 << (K - 1)) - 1)
    return out


def viterbi(coded):
    """Hard-decision decode with a free start state. Returns (path_errors, bits)."""
    S = 1 << (K - 1)
    INF = 10**9
    pm = [0] * S
    hist = []
    for i in range(len(coded) // 2):
        r0, r1 = coded[2 * i], coded[2 * i + 1]
        npm = [INF] * S
        back = [None] * S
        for s in range(S):
            if pm[s] >= INF:
                continue
            for b in (0, 1):
                reg = (s << 1) | b
                ns = reg & (S - 1)
                m = pm[s] + (parity(reg & G0) != r0) + (parity(reg & G1) != r1)
                if m < npm[ns]:
                    npm[ns], back[ns] = m, (s, b)
        pm = npm
        hist.append(back)
    s = min(range(S), key=lambda k: pm[k])
    best = pm[s]
    out = []
    for back in reversed(hist):
        ps, b = back[s]
        out.append(b)
        s = ps
    return best, out[::-1]


def encode(bits):
    """PHR+PSDU bits (as sent, see frame.py) -> chip string including the sync header."""
    coded = conv_encode(list(bits) + [0] * TAIL_BITS)
    return HEADER + ''.join(str(1 - c) for c in coded)


def decode(chips):
    """Chip string (either FM polarity) -> (path_errors, bits after the header) or None."""
    for pol in (0, 1):
        c = chips if pol == 0 else chips.translate(str.maketrans('01', '10'))
        i = c.find(HEADER)
        if i < 0:
            continue
        errs, dec = viterbi([1 - int(x) for x in c[i + len(HEADER) :]])
        return errs, dec
    return None
