# Gateway model: what the stock dongle does, as a pure state machine (no timing). It is the executable spec for
# the gateway firmware; test_gateway_model.py replays captured exchanges through it.
#
#   rx(psdu, ...)  : a frame from the air -> (frames to send, host packets)
#   host(packet)   : a packet from the host -> (frames to send, host packets)
#
# Frames are PSDUs (MAC frame + FCS). Where a rule is only a hypothesis in FACTS.md, the comment says so.
import collections
import zlib

from . import app, frame, host

SUPERFRAME, SUPERFRAME_PERMIT = 0x4FFF, 0xCFFF  # non-beacon PAN coordinator; association permit off / on
NO_TIME_SYNC = (0x0E, 0x0F)  # v2 contact / v2 motion: data requests never get a B0
VERSION = b'0.0.0.47 V1.8 Gateway GW3U'


def psdu(mac_frame):
    return mac_frame + zlib.crc32(mac_frame).to_bytes(4, 'little')


class Gateway:
    def __init__(self, pan, short, ext, mac, sensors=(), dsn=0, bsn=0, clock_ms=0):
        self.pan, self.short, self.ext, self.mac = pan, short, ext, mac  # ext: 64-bit IEEE address (int)
        self.sensors = dict(sensors)  # ASCII MAC -> short address (FACTS hyp. 8: kept in flash page 30)
        self.types = {}  # short address -> sensor type, from its reports
        self.queue = collections.defaultdict(collections.deque)  # dst address -> frame builders, run on poll
        self.dsn, self.bsn = dsn, bsn
        self.clock_ms = clock_ms  # dongle clock, Unix ms, set by the host (0x5333)
        self.permit = False  # pairing on (host 0x531C)
        self.pending_mac = {}  # short address -> ASCII MAC while a join is in progress

    # --- air ---

    def rx(self, rx_psdu, rssi=0):
        m = frame.parse_mac(rx_psdu[:-4])
        tx, out = [], []
        if m['type'] == 'cmd' and m.get('cmd') == 'beacon-req':
            tx.append(psdu(self._beacon()))
            return tx, out
        if m.get('dst_pan') != self.pan or m.get('dst') != self.short:
            return tx, out
        if m['type'] == 'cmd' and m.get('cmd') == 'assoc-req':
            tx.append(self._ack(m['seq']))
            if self.permit:
                self._associate(m['src'].to_bytes(8, 'little'), m['src'])
        elif m['type'] == 'cmd' and m.get('cmd') == 'data-req':
            q = self.queue[m['src']]
            if not q and self.types.get(m['src'], NO_TIME_SYNC[0]) not in NO_TIME_SYNC:
                q.append(self._data(m['src'], self._time_sync))
            tx.append(self._ack(m['seq'], pending=bool(q)))
            if q:
                tx.append(psdu(q.popleft()()))  # built when sent: DSN and clock are current
        elif m['type'] == 'data' and m['ack_req']:
            tx.append(self._ack(m['seq']))
            out += self._sensor_message(m['src'], app.decrypt(m['payload']), rssi)
        return tx, out

    def _sensor_message(self, src, pt, rssi):
        msg = pt[0]
        if msg in app.SENSOR_MSGS:
            self.types[src] = pt[9]
        if msg in app.SENSOR_MSGS + (0xAB,):
            return [host.build(host.ALARM, host.alarm_payload(pt, self.clock_ms, rssi), to_host=True)]
        if msg == app.MSG_JOIN_ANNOUNCE:  # A3 -> scan result (layout from wyzesense2mqtt-rs)
            self.types[src] = pt[9]
            self.pending_mac[src] = pt[1:9]
            return [host.build(host.SCAN_RESULT, pt[:11], to_host=True)]
        if msg == app.MSG_JOIN_RESPONSE:  # A5 -> reply to Get Sensor R1: A5 bytes 0-26
            return [host.build(host.GET_R1 + 1, pt[:27], to_host=True)]
        return []

    def _associate(self, mac, ext):
        key = mac.decode(errors='replace')
        short = self.sensors.get(key)
        if short is None:  # next free address after the highest in use, skipping coordinator short address
            used = set(self.sensors.values()) | {self.short}
            short = max(self.sensors.values(), default=0) + 1
            while short in used and short < 0xFFFD:
                short += 1
        self.types.pop(short, None)
        self.sensors[key] = short
        fc = 3 | (1 << 5) | (1 << 6) | (3 << 10) | (3 << 14)  # cmd, ack req, PAN-ID compression, ext -> ext
        self.queue[ext].append(
            lambda: (
                fc.to_bytes(2, 'little')
                + bytes([self._next_dsn()])
                + self.pan.to_bytes(2, 'little')
                + mac
                + self.ext.to_bytes(8, 'little')
                + bytes([0x02])
                + short.to_bytes(2, 'little')
                + b'\x00'
            )
        )

    def _beacon(self):
        sf = SUPERFRAME_PERMIT if self.permit else SUPERFRAME
        seq, self.bsn = self.bsn, (self.bsn + 1) & 0xFF
        # FC 0x8000: beacon, short source address; then superframe spec, no GTS, no pending addresses
        return (
            (0x8000).to_bytes(2, 'little')
            + bytes([seq])
            + self.pan.to_bytes(2, 'little')
            + self.short.to_bytes(2, 'little')
            + sf.to_bytes(2, 'little')
            + b'\x00\x00'
        )

    def _ack(self, seq, pending=False):
        a = frame.build_ack(seq, pending)
        return a + frame.ack_fcs(a)

    def _next_dsn(self):
        seq, self.dsn = self.dsn, (self.dsn + 1) & 0xFF
        return seq

    def _data(self, dst, plaintext):
        def build():
            pt = plaintext()
            return frame.build_data(self._next_dsn(), self.pan, dst, self.short, app.encrypt(pt + bytes(-len(pt) % 16)))

        return build

    def _time_sync(self):
        return bytes([app.MSG_TIME_SYNC, 0, 0]) + self.clock_ms.to_bytes(6, 'big') + b'\x28'

    def _short_of(self, mac):
        return next(
            (s for s, m in self.pending_mac.items() if m == mac), self.sensors.get(mac.decode(errors='replace'))
        )

    # --- host ---

    def host(self, pkt):
        m = host.parse(pkt)
        if m is None or m['to_host']:
            return [], []
        cmd, p = m['cmd'], m['payload']
        if cmd == host.ACK:
            return [], []
        out = [host.build_ack(cmd, to_host=True)] if cmd >> 8 == host.ASYNC else []

        def reply(payload=b''):
            out.append(host.build(cmd + 1, payload, to_host=True))

        if cmd == host.INQUIRY:
            reply(b'\x01')
        elif cmd == host.GET_ENR:
            reply(bytes(16))  # stock: a per-dongle function of the token (open)
        elif cmd == host.GET_MAC:
            reply(self.mac)
        elif cmd == host.GET_VERSION:
            reply(VERSION)
        elif cmd == host.FINISH_AUTH:
            reply()
        elif cmd == host.SET_SCAN:
            self.permit = p[:1] == b'\x01'
            reply()
        elif cmd == host.GET_R1:  # MAC + 16-byte token -> A4 to that sensor
            short = self._short_of(p[:8])
            if short is not None:
                self.queue[short].append(self._data(short, lambda: bytes([app.MSG_JOIN_TOKEN]) + p[8:24]))
        elif cmd == host.VERIFY_SENSOR:  # MAC + ff 04 -> A6 (ff 04 00 00 + clock)
            short = self._short_of(p[:8])
            if short is not None:
                self.queue[short].append(
                    self._data(
                        short,
                        lambda: bytes([app.MSG_JOIN_DONE]) + p[8:10] + b'\x00\x00' + self.clock_ms.to_bytes(6, 'big'),
                    )
                )
                self.pending_mac.pop(short, None)
            reply()
        elif cmd == host.GET_SENSOR_COUNT:
            reply(bytes([len(self.sensors)]))
        elif cmd == host.GET_SENSOR_LIST:
            for mac in list(self.sensors)[: p[0] if p else len(self.sensors)]:
                out.append(host.build(host.SENSOR_LIST_ITEM, mac.encode(), to_host=True))
        elif cmd == host.DELETE_SENSOR:
            self.sensors.pop(p[:8].decode(errors='replace'), None)
            reply(p[:8] + b'\xff')
        elif cmd == host.DELETE_ALL:
            self.sensors.clear()
            reply(b'\xff')
        elif cmd == host.TIME_SYNC:
            self.clock_ms = int.from_bytes(p[:8], 'big')
        return [], out

    def time_sync_request(self):
        """The dongle asks the host for the time about every 11.5 min."""
        return host.build(host.TIME_SYNC_REQ, to_host=True)
