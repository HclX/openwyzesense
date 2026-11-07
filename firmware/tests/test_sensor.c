// Native tests of sensor.c:
//  1. the captured join of v2 contact sensor 77BD514D (2026-10-22 18:21:41), from the sensor's side: given the
//     dongle's frames, the sensor sends the captured frames, byte for byte (A5's XXTEA response aside)
//  2. every sensor type joining our gateway (gateway.c) over a virtual air, then reporting
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gateway.h"
#include "host_proto.h"
#include "mac154.h"
#include "ota.h"
#include "seclink.h"
#include "sensor.h"
#include "wyze_app.h"

static int failures, checks;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        checks++;                                                                    \
        if (!(cond)) {                                                               \
            failures++;                                                              \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                            \
    } while (0)

typedef struct {
    char name[32];
    uint8_t b[128];
    size_t n;
} vector_t;

static vector_t vectors[32];
static int n_vectors;

static size_t unhex(const char *h, uint8_t *out)
{
    size_t n = 0;
    for (; h[0] && h[1] && h[0] != '\n'; h += 2) {
        unsigned v;
        sscanf(h, "%2x", &v);
        out[n++] = (uint8_t)v;
    }
    return n;
}

static const vector_t *vec(const char *name)
{
    for (int i = 0; i < n_vectors; i++) {
        if (!strcmp(vectors[i].name, name)) {
            return &vectors[i];
        }
    }
    fprintf(stderr, "no vector %s\n", name);
    exit(2);
}

// --- what a sensor (or the gateway) sent ---

typedef struct {
    uint8_t b[64];
    size_t n;
    int from;  // virtual air: 1 gateway, 2 sensor
} frame_t;

static frame_t sent[64];
static size_t n_sent;

static void on_tx(void *ctx, const uint8_t *frame, size_t len, uint32_t gap_us)
{
    (void)ctx, (void)gap_us;
    memcpy(sent[n_sent % 64].b, frame, len);
    sent[n_sent++ % 64].n = len;
}

static int joined_result = -1;

static void on_joined(void *ctx, bool ok)
{
    (void)ctx;
    joined_result = ok;
}

// the last frame sent equals a captured frame (without its FCS)
static int last_is(const char *name)
{
    const vector_t *v = vec(name);
    const frame_t *f = &sent[(n_sent - 1) % 64];
    return n_sent && f->n == v->n - 4 && !memcmp(f->b, v->b, f->n);
}

static int last_is_hex(const char *hex)
{
    uint8_t want[64];
    const frame_t *f = &sent[(n_sent - 1) % 64];
    return n_sent && unhex(hex, want) == f->n && !memcmp(f->b, want, f->n);
}

static void rx_vector(sensor_t *s, const char *name, uint32_t t)
{
    const vector_t *v = vec(name);
    sensor_rx(s, v->b, v->n - 4, t);
}

static void rx_hex(sensor_t *s, const char *hex, uint32_t t)
{
    uint8_t f[64];
    sensor_rx(s, f, unhex(hex, f), t);
}

// Runs time forward to t in 10 ms steps; returns the number of frames sent meanwhile.
static size_t run(sensor_t *s, uint32_t *now, uint32_t t)
{
    size_t before = n_sent;
    for (; *now < t; *now += 10) {
        sensor_tick(s, *now);
    }
    return n_sent - before;
}

static void decrypt_last(uint8_t *pt, size_t *n)
{
    mac_frame_t m;
    const frame_t *f = &sent[(n_sent - 1) % 64];
    mac_parse(f->b, f->n, &m);
    aes128_ecb_decrypt(app_key, m.payload, pt, m.payload_len);
    *n = m.payload_len;
}

#define MAC8(s) {(s)[0], (s)[1], (s)[2], (s)[3], (s)[4], (s)[5], (s)[6], (s)[7]}

static void test_captured_join(void)
{
    // times in ms from the capture (ground_truth/rf/2026-10-22_1821_*, 18:21:41.059 = 1059)
    static const sensor_io_t io = {on_tx, on_joined, NULL, NULL, NULL, NULL, NULL, NULL, NULL};
    sensor_config_t cfg = {SENSOR_CONTACT_V2, MAC8("77BD514D"), 0x21, {0x2c, 0x01, 0x00, 0xe3}, 0, 0, 0};
    sensor_t s;
    uint32_t now = 1000;
    uint8_t pt[48];
    size_t n;
    sensor_init(&s, &cfg, &io);
    s.seq = 0x1c;
    n_sent = 0;

    sensor_join(&s, 1059);
    CHECK(last_is_hex("03081cffffffff07"));  // beacon request
    {                                        // a PAN it must not join: ignored
        sensor_t t = s;
        t.cfg.never_pan = 0x845B;
        rx_vector(&t, "BEACON_PERMIT", 1102);
        CHECK(!t.got_beacon);
    }
    rx_vector(&s, "BEACON_PERMIT", 1102);        // the house dongle, pairing on
    rx_hex(&s, "0080f9fe6525d4ff4f0000", 1205);  // the second dongle: pairing off
    CHECK(run(&s, &now, 1950) == 1 && last_is("ASSOC_REQ"));
    rx_hex(&s, "02001d", now);                                   // its ACK
    CHECK(run(&s, &now, 3030) == 1 && last_is("DATA_REQ_EXT"));  // ~1.05 s later, from the extended address
    rx_hex(&s, "12001e", now);                                   // ACK, frame pending
    rx_vector(&s, "ASSOC_RESP", now + 30);
    CHECK(last_is_hex("02000d"));  // our ACK of it
    CHECK(s.state == SENSOR_HANDSHAKE && s.short_addr == 0x0002);
    CHECK(run(&s, &now, now + 150) == 1 && last_is_hex("63881f5b846633020004"));
    rx_hex(&s, "02001f", now);
    CHECK(run(&s, &now, now + 100) == 1 && last_is("A3_ANNOUNCE"));  // byte for byte, A3 encrypted
    rx_hex(&s, "020020", now);
    CHECK(run(&s, &now, now + 650) == 1 && last_is_hex("6388215b846633020004"));
    rx_hex(&s, "120021", now);
    rx_vector(&s, "A4_TOKEN", now + 40);
    CHECK(last_is_hex("02000e"));
    CHECK(run(&s, &now, now + 250) == 1);  // A5
    decrypt_last(pt, &n);
    CHECK(sent[(n_sent - 1) % 64].b[2] == 0x22 && pt[0] == APP_JOIN_RESPONSE && !memcmp(pt + 1, "77BD514D", 8));
    CHECK(pt[25] == 0x0E && pt[26] == 0x21);  // type, firmware (response: open key)
    rx_hex(&s, "020022", now);
    CHECK(run(&s, &now, now + 450) == 1 && last_is_hex("6388235b846633020004"));
    rx_hex(&s, "120023", now);
    s.temp_raw = 0x12, s.battery_raw = 0x29, s.state_byte = 1;  // the sensor's readings at the time
    rx_vector(&s, "A6_DONE", now + 40);
    CHECK(last_is_hex("02000f") && joined_result == 1 && s.clock_ms == 1792718501753ull);
    CHECK(run(&s, &now, now + 50) == 1);  // A2, the state, counter restarts
    decrypt_last(pt, &n);
    CHECK(!memcmp(pt,
                  "\xa2"
                  "77BD514D"
                  "\x0e\x12\x29\x00\x01\x01\x00\x01",
                  17));
    CHECK(sent[(n_sent - 1) % 64].b[2] == 0x24);
    rx_hex(&s, "020024", now);
    s.temp_raw = 0x15;
    CHECK(run(&s, &now, now + 200) == 1);  // A1 repeats the counter
    decrypt_last(pt, &n);
    CHECK(!memcmp(pt,
                  "\xa1"
                  "77BD514D"
                  "\x0e\x15\x29\x00\x01\x01\x00\x01",
                  17));
    rx_hex(&s, "020025", now);
    size_t polls = 0;  // v2 contact: polls ~10 times, 0.7 s apart
    for (uint32_t end = now + 8000; now < end; now += 10) {
        size_t before = n_sent;
        sensor_tick(&s, now);
        if (n_sent == before) {
            continue;
        }
        const frame_t *f = &sent[(n_sent - 1) % 64];
        uint8_t ack[3] = {0x02, 0x00, f->b[2]};  // the dongle ACKs each, nothing pending
        CHECK(f->n == 10 && f->b[9] == MAC_CMD_DATA_REQ);
        sensor_rx(&s, ack, 3, now);
        polls++;
    }
    CHECK(polls == 10);
}

// --- sensors joining our gateway over a virtual air ---

static gateway_t gw;
static sensor_t sn;
static frame_t air[64];
static size_t n_air, air_lost;
static uint8_t host_out[16][HOST_MAX_PACKET];
static size_t host_len[16], n_host, host_seen;  // host_seen: packets the pairing host has looked at

static void air_tx(void *ctx, const uint8_t *frame, size_t len, uint32_t gap_us)
{
    (void)gap_us;
    memcpy(air[n_air % 64].b, frame, len);
    air[n_air % 64].from = (int)(intptr_t)ctx;
    air[n_air++ % 64].n = len;
}

static void gw_host_out(void *ctx, const uint8_t *pkt, size_t len)
{
    (void)ctx;
    if (n_host == 16) {
        return;
    }
    memcpy(host_out[n_host], pkt, len);
    host_len[n_host++] = len;
}

// Delivers queued frames: sensor -> gateway and gateway -> sensor, until quiet; then plays the pairing host.
static void deliver(uint32_t now)
{
    size_t done = 0;
    gw_tick(&gw, now);  // its timers: a beacon goes out a few ms after the request (#84)
    while (done < n_air) {
        frame_t f = air[done++ % 64];
        if (f.from == 2) {
            gw_rx(&gw, f.b, f.n, -50);  // each end hears the other only,
        } else if (sensor_listening(&sn)) {
            sensor_rx(&sn, f.b, f.n, now);  // the sensor only while it listens
        } else {
            air_lost++;
        }
    }
    n_air = 0;
    for (size_t i = host_seen; i < n_host; i++) {
        host_pkt_t h;
        uint8_t p[HOST_MAX_PACKET], payload[32];
        if (!host_parse(host_out[i], host_len[i], &h)) {
            continue;
        }
        if (h.cmd >> 8 == HOST_ASYNC && h.cmd != HOST_ACK) {  // as a host does: else gw_tick sends it again
            gw_host(&gw, p, host_build_ack(p, h.cmd, false));
        }
        if (h.cmd == HOST_SCAN_RESULT) {
            memcpy(payload, h.payload + 1, 8);
            memcpy(payload + 8, "Ok5HPNQ4lf77u754", 16);
            gw_host(&gw, p, host_build(p, HOST_GET_R1, payload, 24, false));
        } else if (h.cmd == HOST_GET_R1 + 1) {
            memcpy(payload, h.payload + 1, 8);  // A5: a5, MAC, ...
            payload[8] = 0xFF, payload[9] = 0x04;
            gw_host(&gw, p, host_build(p, HOST_VERIFY_SENSOR, payload, 10, false));
        }
    }
    host_seen = n_host;
}

static int alarms[256];  // per message type: 0x5319 alarms the host got

static void count_alarms(void)
{
    for (size_t i = 0; i < n_host && i < 16; i++) {
        host_pkt_t h;
        if (host_parse(host_out[i], host_len[i], &h) && h.cmd == HOST_ALARM) {
            alarms[h.payload[8]]++;
        }
    }
    n_host = host_seen = 0;
}

static void test_join_our_gateway(uint8_t type)
{
    static const gw_io_t gio = {air_tx, gw_host_out, NULL, (void *)1, NULL};
    static const sensor_io_t sio = {air_tx, on_joined, NULL, NULL, (void *)2, NULL, NULL, NULL, NULL};
    sensor_config_t cfg = {type, MAC8("77FFFF01"), 0x10, {0, 0, 0, 0x5a}, 0, 0, 0};
    uint32_t now = 100;
    gw_init(&gw, 0x65FE, 0x0F0F, 0x00124B0000000001ull, (const uint8_t *)"7700AA55", &gio);
    gw.permit = true;
    gw.clock_ms = 1792718501753ull;
    sensor_init(&sn, &cfg, &sio);
    joined_result = -1;
    memset(alarms, 0, sizeof alarms);
    n_air = n_host = host_seen = air_lost = 0;

    sensor_join(&sn, now);
    for (; now < 20000 && joined_result < 0; now += 10) {
        sensor_tick(&sn, now);
        deliver(now);
    }
    CHECK(joined_result == 1 && sn.state == SENSOR_JOINED && sn.short_addr == 1 && sn.pan == 0x65FE);
    CHECK(gw.n_sensors == 1 && !memcmp(gw.sensors[0].mac, "77FFFF01", 8));
    count_alarms();
    uint8_t dsn_joined = gw.dsn;
    for (uint32_t end = now + 10000; now < end; now += 10) {  // the first reports, the polls after A1
        sensor_tick(&sn, now);
        deliver(now);
        count_alarms();
    }
    CHECK(alarms[APP_STATUS] == 1);
    CHECK(alarms[APP_EVENT] == (type == SENSOR_CONTACT_V2 || type == SENSOR_MOTION_V2));
    CHECK(alarms[APP_CLIMATE] == (type == SENSOR_CLIMATE_V2));
    // the gateway answers the poll after A1 with a B0, except for v2 contact / motion (FACTS.md §3)
    CHECK((uint8_t)(gw.dsn - dsn_joined) == (type == SENSOR_CONTACT_V2 || type == SENSOR_MOTION_V2 ? 0 : 1));
    CHECK(sn.clock_ms == gw.clock_ms && sn.polls_left == 0);

    // events and climate later: one alarm each, the counter advancing
    uint16_t c0 = sn.counter;
    if (type == SENSOR_CLIMATE_V2) {
        sensor_climate(&sn, 2146, 48, now);
    } else {
        sensor_event(&sn, true, now);
    }
    for (uint32_t end = now + 3000; now < end; now += 10) {
        sensor_tick(&sn, now);
        deliver(now);
        count_alarms();
    }
    CHECK(sn.counter == c0 + 1);
    // the sensor missed nothing the gateway sent, though its radio was off most of the time
    CHECK(air_lost == 0);
    CHECK(!sensor_listening(&sn) && sensor_next_event(&sn, now) == sn.next_heartbeat);
    CHECK(alarms[type == SENSOR_CLIMATE_V2 ? APP_CLIMATE : APP_EVENT] ==
          1 + (type == SENSOR_CONTACT_V2 || type == SENSOR_MOTION_V2 || type == SENSOR_CLIMATE_V2));
}

static void test_heartbeat_and_climate(void)
{
    static const sensor_io_t io = {on_tx, on_joined, NULL, NULL, NULL, NULL, NULL, NULL, NULL};
    sensor_config_t cfg = {SENSOR_CLIMATE_V2, MAC8("77FFFF02"), 0x0F, {0}, 0, 0, 0};
    sensor_t s;
    uint8_t pt[48];
    size_t n;
    sensor_init(&s, &cfg, &io);
    sensor_restore(&s, 0x65FE, 0x0F0F, 0x0001, 100, 1000);
    n_sent = 0;
    // climate: E8 only for a change of 0.5 degC or 3 % since the last E8 sent
    const struct {
        int16_t c;
        uint8_t h;
        int sends;
    } samples[] = {
        {2100, 50, 1}, {2149, 52, 0}, {2050, 48, 1}, {2010, 51, 1}, {2059, 49, 0}, {2060, 49, 1},
    };
    uint32_t now = 1000;
    for (size_t i = 0; i < sizeof samples / sizeof samples[0]; i++, now += 25860) {
        size_t before = n_sent;
        sensor_climate_sample(&s, samples[i].c, samples[i].h, now);
        sensor_tick(&s, now);
        CHECK((n_sent - before) == (size_t)samples[i].sends);
        if (n_sent > before) {
            uint8_t ack[3] = {0x02, 0x00, sent[(n_sent - 1) % 64].b[2]};
            sensor_rx(&s, ack, 3, now + 5);
        }
    }
    decrypt_last(pt, &n);
    CHECK(pt[0] == APP_CLIMATE && pt[14] == 20 && pt[15] == 60 && pt[16] == 49 && s.counter == 104);
    // the heartbeat: an A1 every 2 h for a climate sensor
    size_t before = n_sent;
    for (now = 1000; now < 1000 + 120u * 60 * 1000 + 100; now += 1000) {
        sensor_tick(&s, now);
    }
    CHECK(n_sent == before + 1);
    decrypt_last(pt, &n);
    CHECK(pt[0] == APP_STATUS && s.counter == 104);
}

static int ota_offers_seen, ota_writes_seen;

static uint8_t fake_ota_offer(void *ctx, sensor_ota_offer_t *o)
{
    (void)ctx;
    ota_offers_seen++;
    o->slot = 0;
    return OTA_DONE_OK;
}

static bool fake_ota_write(void *ctx, uint32_t off, const uint8_t *data, size_t len)
{
    (void)ctx;
    (void)off;
    (void)data;
    (void)len;
    ota_writes_seen++;
    return true;
}

static void send_enc_downlink(sensor_t *s, const uint8_t *pt, size_t pt_len, uint32_t now)
{
    uint8_t ct[APP_MAX_PAYLOAD], frame[128];
    size_t ct_len = app_encrypt(pt, pt_len, ct);
    size_t flen = mac_build_data(frame, 0x55, s->pan, s->short_addr, s->coord, ct, ct_len, true);
    sensor_rx(s, frame, flen, now);
}

static void test_truncated_payloads(void)
{
    static const sensor_io_t io = {on_tx, on_joined, NULL, NULL, NULL, fake_ota_offer, fake_ota_write, NULL, NULL};
    sensor_config_t cfg = {SENSOR_CONTACT_V1, MAC8("77FFFF03"), 0x10, {0}, 0, 0, 0};
    sensor_t s;
    sensor_init(&s, &cfg, &io);

    // 1. Truncated 16-byte A4 (needs 17 bytes) during SENSOR_HANDSHAKE must be ignored.
    s.state = SENSOR_HANDSHAKE;
    s.pan = 0x65FE;
    s.coord = 0x0F0F;
    s.short_addr = 0x0007;
    memset(s.token, 0xAA, sizeof s.token);
    uint8_t bad_a4[16] = {APP_JOIN_TOKEN, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    send_enc_downlink(&s, bad_a4, sizeof bad_a4, 1000);
    CHECK(s.token[0] == 0xAA && !s.step_armed);

    // 2. Truncated 16-byte OTA offer (4F 10 ..., needs 26 bytes) must not invoke ota_offer callback.
    sensor_restore(&s, 0x65FE, 0x0F0F, 0x0007, 1, 1000);
    ota_offers_seen = ota_writes_seen = 0;
    uint8_t bad_c0[16] = {APP_OURS, APP_OTA_OFFER, 0x11, 0x22, 0x33, 0x44};
    send_enc_downlink(&s, bad_c0, sizeof bad_c0, 1100);
    CHECK(ota_offers_seen == 0 && !s.ota.active);

    // 3. Valid 26-byte offer starts OTA; a truncated 16-byte block claiming len=80 must not write out-of-bounds.
    uint8_t good_c0[26] = {APP_OURS, APP_OTA_OFFER};
    ota_put_be32(good_c0 + 2, 0x12345678u);
    ota_put_be32(good_c0 + 6, 2u);
    ota_put_be32(good_c0 + 10, 1024u);
    ota_put_be32(good_c0 + 14, 0xAABBCCDDu);
    send_enc_downlink(&s, good_c0, sizeof good_c0, 1200);
    CHECK(ota_offers_seen == 1 && s.ota.active && s.ota.offset == 0);

    uint8_t bad_c2[16] = {APP_OURS, APP_OTA_BLOCK, 0, 0, 0, 0, OTA_BLOCK, 0xDE, 0xAD};
    send_enc_downlink(&s, bad_c2, sizeof bad_c2, 1300);
    CHECK(ota_writes_seen == 0 && s.ota.offset == 0);

    // A block whose length fits inside the 16-byte decrypted payload (7 + 9 <= 16) succeeds cleanly.
    uint8_t ok_c2[16] = {APP_OURS, APP_OTA_BLOCK, 0, 0, 0, 0, 9, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    send_enc_downlink(&s, ok_c2, sizeof ok_c2, 1400);
    CHECK(ota_writes_seen == 1 && s.ota.offset == 9);
}

static void test_handshake_and_queue_edge_cases(void)
{
    static const sensor_io_t io = {on_tx, on_joined, NULL, NULL, NULL, fake_ota_offer, fake_ota_write, NULL, NULL};
    sensor_config_t cfg = {SENSOR_CONTACT_V1, MAC8("77FFFF04"), 0x10, {0}, 0, 0, 0};
    sensor_t s;
    uint8_t pt[48];
    size_t n;
    sensor_init(&s, &cfg, &io);
    n_sent = 0;

    // 1. sensor_join() clears stale OTA & check_in state, and stale 0xB0 during SENSOR_HANDSHAKE is ignored.
    s.ota.active = true;
    s.ota.total = 4096;
    s.check_in = true;
    joined_result = -1;
    sensor_join(&s, 1000);
    CHECK(!s.ota.active && s.ota.total == 0 && !s.check_in && s.state == SENSOR_SCANNING);

    s.state = SENSOR_HANDSHAKE;
    s.pan = 0x65FE;
    s.coord = 0x0F0F;
    s.short_addr = 0x0009;
    s.polls_left = 5;
    s.clock_ms = 0x123456u;
    uint8_t stale_b0[9] = {APP_TIME_SYNC, 0, 0, 0x01, 0x90, 0, 0, 0, 1};
    send_enc_downlink(&s, stale_b0, sizeof stale_b0, 1500);
    CHECK(s.polls_left == 5 && s.clock_ms == 0x123456u);

    // 2. Missed 3-byte MAC ACK on DATA_REQ is disarmed when coordinator downlink (A4) arrives directly.
    s.awaiting_ack = true;
    s.out[0] = MAC_CMD;
    uint32_t acks_before = s.acks;
    uint8_t good_a4[17] = {APP_JOIN_TOKEN, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    send_enc_downlink(&s, good_a4, sizeof good_a4, 1600);
    CHECK(!s.awaiting_ack && s.acks == acks_before + 1 && s.step_armed);

    // 3. Overall 30 s JOIN_TIMEOUT_MS expires a wedged handshake cleanly back to SENSOR_IDLE.
    uint32_t next_ev = sensor_next_event(&s, 30000);
    CHECK(next_ev <= 31000);
    sensor_tick(&s, 31000);
    CHECK(s.state == SENSOR_IDLE && joined_result == 0);

    // 4. Rapid open -> close preserves both polarities, and in-flight queued[0] is never evicted on overflow.
    sensor_restore(&s, 0x65FE, 0x0F0F, 0x0009, 10, 40000);
    n_sent = 0;
    sensor_event(&s, true, 40100);   // queued[0]: open (1)
    sensor_event(&s, false, 40105);  // queued[1]: close (0) before tick transmits queued[0]
    CHECK(s.n_queued == 2 && s.queued[0].data[0] == 1 && s.queued[1].data[0] == 0);

    sensor_tick(&s, 40110);  // transmits queued[0] (open = 1), awaiting_ack = true
    CHECK(s.awaiting_ack && s.step == 7);
    decrypt_last(pt, &n);
    CHECK(pt[0] == APP_EVENT && pt[14] == 1);
    uint8_t first_seq = sent[(n_sent - 1) % 64].b[2];

    // Flood 4 more events while queued[0] is in-flight: queued[0] stays intact, oldest unsent queued[1..] drops.
    sensor_event(&s, true, 40112);
    sensor_event(&s, false, 40114);
    sensor_event(&s, true, 40116);
    sensor_event(&s, false, 40118);
    CHECK(s.n_queued == SENSOR_QUEUE_LEN && s.queued[0].data[0] == 1);
    CHECK(s.queued[1].data[0] == 0 && s.queued[2].data[0] == 1 && s.queued[3].data[0] == 0);

    // ACKing first_seq pops queued[0], and next report sends queued[1] (close = 0).
    uint8_t ack0[3] = {0x02, 0x00, first_seq};
    sensor_rx(&s, ack0, 3, 40120);
    CHECK(!s.awaiting_ack && s.n_queued == 3 && s.queued[0].data[0] == 0);
    uint32_t now = 40120;
    CHECK(run(&s, &now, 40260) == 1);
    decrypt_last(pt, &n);
    CHECK(pt[0] == APP_EVENT && pt[14] == 0);
}

static void ack_last_tx(sensor_t *s, uint32_t now)
{
    uint8_t ack[3] = {0x02, 0x00, sent[(n_sent - 1) % 64].b[2]};
    sensor_rx(s, ack, 3, now);
}

static void test_motion_v1_pir(void)
{
    static const sensor_io_t io = {on_tx, on_joined, NULL, NULL, NULL, NULL, NULL, NULL, NULL};
    sensor_config_t cfg = {SENSOR_MOTION_V1, MAC8("777A9A46"), 0x19, {0}, 0, 0, 0};
    sensor_t s;
    uint8_t pt[48];
    size_t n;
    sensor_init(&s, &cfg, &io);
    sensor_restore(&s, 0x65FE, 0x0F0F, 0x0002, 5, 1000);
    n_sent = 0;

    // 1. Single PIR pulse at t=1000 ms (pin goes low at 1100 ms): A2(1) immediately, AB + A2(0) at t=121000 ms.
    uint32_t now = 1000;
    sensor_pir(&s, true, now);
    sensor_tick(&s, now);
    CHECK(n_sent == 1 && s.pir.active && s.counter == 6);
    decrypt_last(pt, &n);
    CHECK(pt[0] == APP_EVENT && pt[9] == SENSOR_MOTION_V1 && pt[14] == 1);
    ack_last_tx(&s, now + 5);
    sensor_pir(&s, false, now + 100);
    CHECK(run(&s, &now, 120990) == 0);  // quiet across the eleven 10 s bucket rotations
    CHECK(sensor_next_event(&s, now) == 121000);

    // At t=121000 ms: queues APP_MOTION_SUMMARY (0xAB) then APP_EVENT (0xA2 state=0), without advancing counter on AB.
    CHECK(run(&s, &now, 121020) == 1 && !s.pir.active);
    decrypt_last(pt, &n);
    CHECK(pt[0] == APP_MOTION_SUMMARY && !memcmp(pt + 1, "777A9A46", 8) && pt[9] == SENSOR_MOTION_V1);
    CHECK(pt[10] == 1 && pt[22] == 0x01 && s.counter == 6);
    for (int i = 1; i < 12; i++) {
        CHECK(pt[10 + i] == 0);
    }
    ack_last_tx(&s, now);
    CHECK(run(&s, &now, now + 200) == 1);
    decrypt_last(pt, &n);
    CHECK(pt[0] == APP_EVENT && pt[14] == 0 && s.counter == 7);
    ack_last_tx(&s, now);
    CHECK(sensor_next_event(&s, now) == s.next_heartbeat);

    // 2. Saturating 300 pulses in bucket 0 + retriggering pulse at +55 s: 0xAB at +120 s, 0xA2(0) at +175 s.
    uint32_t t0 = 200000;
    now = t0;
    for (int i = 0; i < 300; i++) {
        sensor_pir(&s, true, t0 + (uint32_t)i);
        sensor_pir(&s, false, t0 + (uint32_t)i);
    }
    sensor_tick(&s, now);
    decrypt_last(pt, &n);
    CHECK(pt[0] == APP_EVENT && pt[14] == 1);
    ack_last_tx(&s, now + 5);
    CHECK(run(&s, &now, t0 + 55000) == 0);
    sensor_pir(&s, true, now);
    sensor_pir(&s, false, now + 50);
    CHECK(run(&s, &now, t0 + 120020) == 1 && s.pir.active);  // summary at +120 s, still active!
    decrypt_last(pt, &n);
    CHECK(pt[0] == APP_MOTION_SUMMARY && pt[10] == 255 && pt[10 + 5] == 1 && pt[22] == 0x01);
    ack_last_tx(&s, now);
    CHECK(run(&s, &now, t0 + 174990) == 0 && s.pir.active);
    CHECK(run(&s, &now, t0 + 175020) == 1 && !s.pir.active);  // clears 120 s after +55 s pulse
    decrypt_last(pt, &n);
    CHECK(pt[0] == APP_EVENT && pt[14] == 0);
    ack_last_tx(&s, now);
}

int main(int argc, char **argv)
{
    char line[512], hex[400];
    FILE *f;
    if (argc != 2) {
        fprintf(stderr, "usage: %s frames.txt\n", argv[0]);
        return 2;
    }
    f = fopen(argv[1], "r");
    if (!f) {
        perror(argv[1]);
        return 2;
    }
    while (fgets(line, sizeof line, f)) {
        vector_t *v = &vectors[n_vectors];
        if (line[0] == '#' || sscanf(line, "%31s %399s", v->name, hex) != 2) {
            continue;
        }
        v->n = unhex(hex, v->b);
        n_vectors++;
    }
    fclose(f);

    test_captured_join();
    test_heartbeat_and_climate();
    test_truncated_payloads();
    test_handshake_and_queue_edge_cases();
    test_motion_v1_pir();
    const uint8_t types[] = {SENSOR_CONTACT_V1, SENSOR_MOTION_V1,  SENSOR_LEAK_V2,
                             SENSOR_CLIMATE_V2, SENSOR_CONTACT_V2, SENSOR_MOTION_V2};
    for (size_t i = 0; i < sizeof types; i++) {
        int before = failures;
        test_join_our_gateway(types[i]);
        if (failures != before) {
            fprintf(stderr, "  (sensor type 0x%02x)\n", types[i]);
        }
    }
    printf("%d checks, %d failed\n", checks, failures);
    return failures != 0;
}
