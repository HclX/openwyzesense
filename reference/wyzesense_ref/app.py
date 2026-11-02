# Application layer: the 802.15.4 MAC payload, AES-128-ECB encrypted with the FIPS-197 example key.
# Field names follow docs/FACTS.md; fields that FACTS.md lists as hypotheses are named *_raw here.
from Crypto.Cipher import AES

KEY = bytes.fromhex('2b7e151628aed2a6abf7158809cf4f3c')

SENSOR_TYPES = {
    0x01: 'contact_v1',
    0x02: 'motion_v1',
    0x03: 'leak_v2',
    0x07: 'climate_v2',
    0x0E: 'contact_v2',
    0x0F: 'motion_v2',
}
SENSOR_MSGS = (0xA1, 0xA2, 0xE8)  # share the <type MAC sensor temp batt len data counter> layout
MSG_TIME_SYNC = 0xB0
# Join handshake (FACTS.md "Join")
MSG_JOIN_ANNOUNCE, MSG_JOIN_TOKEN, MSG_JOIN_RESPONSE, MSG_JOIN_DONE = 0xA3, 0xA4, 0xA5, 0xA6
R1_TOKEN = b'Ok5HPNQ4lf77u754'  # identical in every captured join (3 joins, 2 sensors)


def decrypt(payload):
    return AES.new(KEY, AES.MODE_ECB).decrypt(payload)


def encrypt(plain):
    return AES.new(KEY, AES.MODE_ECB).encrypt(plain)


def parse(pt):
    p = {'msg': pt[0]}
    if pt[0] in SENSOR_MSGS + (0xAB,):
        p['mac'] = pt[1:9]
        p['sensor_type'] = pt[9]
    if pt[0] in SENSOR_MSGS:
        n = pt[13]  # the data length; byte 12 is the low-battery flag (FACTS.md), not its high byte
        p['temp_raw'] = pt[10]  # hypothesis: chip temperature, deg C
        p['battery_raw'] = pt[11]  # hypothesis: battery voltage, 1/32 V
        p['low_battery'] = pt[12] != 0
        p['data'] = pt[14 : 14 + n]
        p['counter'] = int.from_bytes(pt[14 + n : 16 + n], 'big')
        if pt[0] == 0xE8 and n >= 3:
            p['temperature_c'] = p['data'][0] + p['data'][1] / 100
            p['humidity_pct'] = p['data'][2]
        if pt[0] == 0xA2 and n >= 1:
            p['active'] = bool(p['data'][0])  # motion: active; contact: open (both checked vs bridge)
    if pt[0] == MSG_TIME_SYNC:
        p['time_ms'] = int.from_bytes(pt[3:9], 'big')
    if pt[0] == MSG_JOIN_ANNOUNCE:
        p['mac'], p['sensor_type'], p['fw_version'] = pt[1:9], pt[9], pt[10]
        p['unknown'] = pt[11:15]  # 11-13 unknown; 14 changes on every join
    if pt[0] == MSG_JOIN_TOKEN:
        p['token'] = pt[1:17]
    if pt[0] == MSG_JOIN_RESPONSE:
        p['mac'] = pt[1:9]
        p['response'] = pt[9:25]  # XXTEA(token) under a per-sensor key (unknown)
        p['sensor_type'], p['fw_version'] = pt[25], pt[26]
    if pt[0] == MSG_JOIN_DONE:
        p['status'] = pt[1:5]  # ff 04 00 00 in every join so far
        p['time_ms'] = int.from_bytes(pt[5:11], 'big')
    return p


def build_sensor_msg(msg, mac, sensor_type, temp_raw, battery_raw, data, counter):
    pt = (
        bytes([msg])
        + mac
        + bytes([sensor_type, temp_raw, battery_raw])
        + len(data).to_bytes(2, 'big')
        + data
        + counter.to_bytes(2, 'big')
    )
    return pt + bytes(-len(pt) % 16)
