// The apps themselves (apps/), unchanged, on the host board (boards/host/): a gateway, a sensor and a sniffer
// sharing one simulated air, the test playing wyzesense2mqtt on the gateway's console and typing the sensor's
// console commands.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board.h"
#include "build_info.h"
#include "boot_slot.h"
#include "ccfg_check.h"
#include "gateway.h"
#include "gateway_app.h"
#include "host_board.h"
#include "host_proto.h"
#include "mac154.h"
#include "ota.h"
#include "sensor_app.h"
#include "sniffer_app.h"
#include "seclink.h"
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

static gateway_app_t gw_app;
static sensor_app_t sensor_app;
static sniffer_app_t sniffer_app;
static host_dev_t *gw, *sn, *sniff;

// --- the host side of the gateway: what wyzesense2mqtt does ---

typedef struct {
    uint16_t cmd;
    uint8_t p[HOST_MAX_PACKET];
    size_t n;
    uint64_t at_us;
} pkt_t;

static pkt_t got[4096];
static size_t n_got;
static host_rx_t host_rx;
static const uint64_t unix_ms0 = 1792800000000ull;

static void host_write(uint16_t cmd, const void *payload, size_t n)
{
    uint8_t b[HOST_MAX_PACKET];
    host_console_send(gw, b, host_build(b, cmd, payload, n, false));
}

// The host's side of an OTA update (common/ota.h): the slot image it serves, an offer made when a candidate shows
static struct {
    const uint8_t *img[2];  // the slot-A and slot-B builds of the update
    size_t len[2];
    int corrupt_at;  // >= 0: serve this byte flipped
    unsigned needs;  // 0x53F6 packets seen
} host_ota;

static const slot_header_t *header_of(const uint8_t *img)
{
    return (const slot_header_t *)(const void *)img;
}

// 0x53F4: MAC, board ID, version, size and CRC of the slot-A build, of the slot-B build
static void ota_offer_payload(uint8_t out[8 + OTA_OFFER_LEN], const uint8_t mac[8])
{
    memcpy(out, mac, 8);
    ota_put_be32(out + 8, header_of(host_ota.img[0])->board_id);
    ota_put_be32(out + 12, header_of(host_ota.img[0])->version);
    for (int s = 0; s < 2; s++) {
        ota_put_be32(out + 16 + 8 * s, header_of(host_ota.img[s])->size);
        ota_put_be32(out + 20 + 8 * s, header_of(host_ota.img[s])->crc);
    }
}

static void host_write(uint16_t cmd, const void *payload, size_t n);

// OWS_SUB_OTA_NEED (0x53F1 0x11): the bytes asked for, of the build asked for, in 0x53F0 0x12 pieces of OTA_HOST_CHUNK
static void serve_need(const host_pkt_t *p)
{
    const uint8_t *q = p->payload + 1;
    uint8_t slot = q[8], len = q[13], d[5 + OTA_HOST_CHUNK];
    uint32_t off = ota_be32(q + 9);
    host_ota.needs++;
    if (slot > 1 || !host_ota.img[slot] || off + len > host_ota.len[slot]) {
        return;
    }
    const uint8_t *img = host_ota.img[slot];
    d[0] = OWS_SUB_OTA_DATA;
    for (uint32_t done = 0; done < len;) {
        uint32_t n = len - done < OTA_HOST_CHUNK ? len - done : OTA_HOST_CHUNK;
        ota_put_be32(d + 1, off + done);
        memcpy(d + 5, img + off + done, n);
        if (host_ota.corrupt_at >= (int)(off + done) && host_ota.corrupt_at < (int)(off + done + n)) {
            d[5 + host_ota.corrupt_at - (off + done)] ^= 0x10;
        }
        host_write(HOST_OWS_CMD, d, 5 + n);
        done += n;
    }
}

// Reads the gateway's console: ACKs what needs it, answers time requests and OTA needs, keeps the rest
static void pump_host(void)
{
    uint8_t b[4096], a[16];
    size_t n;
    host_pkt_t p;
    while ((n = host_console_take(gw, b, sizeof b))) {
        for (size_t i = 0; i < n; i++) {
            if (!host_rx_feed(&host_rx, b[i], &p)) {
                continue;
            }
            if (p.cmd >> 8 == HOST_ASYNC && p.cmd != HOST_ACK) {
                host_console_send(gw, a, host_build_ack(a, p.cmd, false));
            }
            if (p.cmd == HOST_TIME_SYNC_REQ) {
                uint64_t t = unix_ms0 + host_now_us() / 1000;
                uint8_t ts[8];
                for (int k = 0; k < 8; k++) {
                    ts[k] = (uint8_t)(t >> (56 - 8 * k));
                }
                host_write(HOST_TIME_SYNC, ts, 8);
            }
            if (p.cmd == HOST_OWS_RSP && p.len == 15 && p.payload[0] == OWS_SUB_OTA_NEED) {
                serve_need(&p);
            }
            if (n_got < sizeof got / sizeof got[0]) {
                pkt_t *g = &got[n_got++];
                g->cmd = p.cmd;
                g->n = p.len;
                memcpy(g->p, p.payload, p.len);
                g->at_us = host_now_us();
            }
        }
    }
}

typedef struct {
    uint16_t cmd;
    uint8_t sub;  // non-zero: also require got[i].p[0] == sub
    size_t from;
    const pkt_t *found;
} want_t;

static bool host_has(void *arg)
{
    want_t *w = arg;
    pump_host();
    for (size_t i = w->from; i < n_got; i++) {
        if (got[i].cmd == w->cmd && (!w->sub || (got[i].n >= 1 && got[i].p[0] == w->sub))) {
            return (w->found = &got[i]) != NULL;
        }
    }
    return false;
}

// Waits up to ms for a packet cmd from the gateway (one arriving from now on)
static const pkt_t *wait_host(uint16_t cmd, uint32_t ms)
{
    want_t w = {cmd, 0, n_got, NULL};
    pump_host();
    w.from = n_got;
    return host_run_until(host_has, &w, ms) ? w.found : NULL;
}

static const pkt_t *wait_ows(uint8_t sub, uint32_t ms)
{
    want_t w = {HOST_OWS_RSP, sub, n_got, NULL};
    pump_host();
    w.from = n_got;
    return host_run_until(host_has, &w, ms) ? w.found : NULL;
}

static const pkt_t *call(uint16_t cmd, const void *payload, size_t n)
{
    want_t w = {cmd == HOST_GET_SENSOR_LIST ? HOST_SENSOR_LIST_ITEM : (uint16_t)(cmd + 1), 0, 0, NULL};
    pump_host();
    w.from = n_got;
    host_write(cmd, payload, n);
    return host_run_until(host_has, &w, 2000) ? w.found : NULL;
}

static const pkt_t *ows_call(uint8_t sub, const void *payload, size_t n)
{
    uint8_t buf[OWS_MAX_PAYLOAD];
    want_t w = {HOST_OWS_RSP, sub, 0, NULL};
    buf[0] = sub;
    if (n) {
        memcpy(buf + 1, payload, n);
    }
    pump_host();
    w.from = n_got;
    host_write(HOST_OWS_CMD, buf, 1 + n);
    return host_run_until(host_has, &w, 2000) ? w.found : NULL;
}

static void handshake(void)
{
    const pkt_t *r;
    r = call(HOST_INQUIRY, NULL, 0);
    CHECK(r && r->n == 1 && r->p[0] == 1);
    r = call(HOST_GET_ENR, "0000000000000000", 16);
    CHECK(r && r->n == 16);
    r = call(HOST_GET_MAC, NULL, 0);
    CHECK(r && r->n == 8 && !memcmp(r->p, "77", 2));
    r = call(HOST_GET_VERSION, NULL, 0);
    CHECK(r && r->n == 26 && !memcmp(r->p, "0.0.0.47 V1.8 Gateway GW3U", 26));
    r = call(HOST_FINISH_AUTH, "\xff", 1);
    CHECK(r && r->n == 0);
    CHECK(wait_host(HOST_TIME_SYNC_REQ, 1000) != NULL);  // the dongle asks for the time right away
    r = ows_call(OWS_SUB_FLAGS, NULL, 0);                // our feature flags (R3): all off after a reset
    CHECK(r && r->n == 9 && r->p[0] == OWS_SUB_FLAGS && !memcmp(r->p + 1, "\0\0\0\x08", 4) &&
          r->p[8] == GW_FLAGS_SUPPORTED);
}

static int sensor_count(void)
{
    const pkt_t *r = call(HOST_GET_SENSOR_COUNT, NULL, 0);
    return r && r->n == 1 ? r->p[0] : -1;
}

// --- the sensor's console ---

static char sn_text[1 << 16];
static size_t sn_len;

static void pump_sensor(void)
{
    sn_len += host_console_take(sn, (uint8_t *)sn_text + sn_len, sizeof sn_text - 1 - sn_len);
    sn_text[sn_len] = 0;
}

typedef struct {
    const char *text;
    size_t from;
} line_t;

static bool sensor_said(void *arg)
{
    line_t *l = arg;
    pump_sensor();
    pump_host();
    return strstr(sn_text + l->from, l->text) != NULL;
}

static bool say(const char *cmd, const char *expect, uint32_t ms)
{
    line_t l = {expect, 0};
    pump_sensor();
    l.from = sn_len;
    host_console_send(sn, cmd, strlen(cmd));
    host_console_send(sn, "\n", 1);
    return host_run_until(sensor_said, &l, ms);
}

// Lets time pass with both consoles read all along, as a host and a terminal would
static bool keep_reading(void *arg)
{
    (void)arg;
    pump_host();
    pump_sensor();
    return false;
}

static void run(uint32_t ms)
{
    host_run_until(keep_reading, NULL, ms);
}

static void trace(void)
{
    if (!getenv("OWS_TRACE")) {
        return;
    }
    fprintf(stderr, "--- sensor console:\n%s\n--- air:\n", sn_text);
    for (size_t i = 0; i < host_n_air && i < HOST_AIR_LOG; i++) {
        fprintf(stderr, "%10.3f-%10.3f %d ", host_air[i].start_us / 1e3, host_air[i].end_us / 1e3, host_air[i].from);
        for (int k = 0; k < host_air[i].len; k++) {
            fprintf(stderr, "%02x", host_air[i].frame[k]);
        }
        fprintf(stderr, "\n");
    }
    for (size_t i = 0; i < n_got; i++) {
        fprintf(stderr, "host %10.3f %04x ", got[i].at_us / 1e3, got[i].cmd);
        for (size_t k = 0; k < got[i].n; k++) {
            fprintf(stderr, "%02x", got[i].p[k]);
        }
        fprintf(stderr, "\n");
    }
}

// --- tests ---

static void boot_world(void)
{
    host_reset();
    n_got = sn_len = 0;
    host_rx_init(&host_rx);
    gw = host_add("gateway", &gw_app, HOST_APP(gateway));
    sn = host_add("sensor", &sensor_app, HOST_APP(sensor));
    sniff = host_add("sniffer", &sniffer_app, HOST_APP(sniffer));
    host_boot(gw);
    host_boot(sn);
    host_boot(sniff);
    host_run(100);
    CHECK(gw->led[0] && !gw->led[1] && sn->led[0] && !sn->led[1]);
}

// The next alarm (0x5319) of type from the sensor, within ms
static const pkt_t *alarm_from(const uint8_t mac[8], uint8_t type, uint32_t ms)
{
    uint64_t until = host_now_us() + ms * 1000ull;
    while (host_now_us() < until) {
        const pkt_t *a = wait_host(HOST_ALARM, (uint32_t)((until - host_now_us()) / 1000) + 1);
        if (!a) {
            return NULL;
        }
        if (a->n > 22 && a->p[8] == type && !memcmp(a->p + 9, mac, 8)) {
            return a;
        }
    }
    return NULL;
}

// Pairing, as wyzesense2mqtt does it; the sensor starts joining by its console ("join") or its button held 3.04 s
static void pair_as(bool by_button, uint8_t type)
{
    const uint8_t *mac = sensor_app.sensor.cfg.mac;
    uint8_t token[24];
    const pkt_t *r;

    pump_sensor();
    size_t console_from = sn_len;  // "joined" from this pairing, not an earlier one
    r = call(HOST_SET_SCAN, "\x01", 1);
    CHECK(r && r->n == 0);
    if (by_button) {
        line_t l = {"button long", sn_len};
        host_input(sn, INPUT_BUTTON, true);
        CHECK(host_run_until(sensor_said, &l, 3300));  // pairing starts at 3.04 s, the button still down
        host_input(sn, INPUT_BUTTON, false);
    } else {
        CHECK(say("join", "ok", 100));
    }
    r = wait_host(HOST_SCAN_RESULT, 10000);
    CHECK(r && r->n == 11 && r->p[0] == APP_JOIN_ANNOUNCE && !memcmp(r->p + 1, mac, 8) && r->p[9] == type);
    memcpy(token, mac, 8);
    for (int i = 0; i < 16; i++) {
        token[8 + i] = (uint8_t)(0x40 + i);
    }
    r = call(HOST_GET_R1, token, sizeof token);
    CHECK(r && r->n == 27 && r->p[0] == APP_JOIN_RESPONSE && !memcmp(r->p + 9, token + 8, 16));
    r = call(HOST_SET_SCAN, "\x00", 1);
    CHECK(r && r->n == 0);
    uint8_t verify[10];
    memcpy(verify, mac, 8);
    verify[8] = 0xff, verify[9] = 0x04;
    r = call(HOST_VERIFY_SENSOR, verify, sizeof verify);
    CHECK(r && r->n == 0);
    line_t joined = {"joined ", console_from};
    CHECK(host_run_until(sensor_said, &joined, 10000));
    CHECK(sensor_count() == 1);
}

static void pair(bool by_button)
{
    pair_as(by_button, SENSOR_CONTACT_V2);
}

static void test_pairing_and_events(void)
{
    const uint8_t *mac = sensor_app.sensor.cfg.mac;
    const pkt_t *r;

    boot_world();
    handshake();
    CHECK(sensor_count() == 0);
    pair(false);
    // its first reports, as a stock v2 contact sends them: its state (A2), then A1
    r = alarm_from(mac, APP_EVENT, 5000);
    CHECK(r && r->p[8 + 14] == 0);
    CHECK(alarm_from(mac, APP_STATUS, 5000) != NULL);
    run(10000);

    // an event reaches the host, once
    size_t from = n_got;
    CHECK(say("event 1", "ok", 100));
    r = alarm_from(mac, APP_EVENT, 5000);
    CHECK(r && r->p[8 + 14] == 1);
    run(5000);
    int alarms = 0;
    for (size_t i = from; i < n_got; i++) {
        alarms += got[i].cmd == HOST_ALARM && got[i].p[8] == APP_EVENT;
    }
    CHECK(alarms == 1);
    // the alarm carries the host's clock (from the time sync after the handshake)
    uint64_t t = 0;
    for (int k = 0; k < 8; k++) {
        t = t << 8 | r->p[k];
    }
    CHECK(t >= unix_ms0 && t <= unix_ms0 + host_now_us() / 1000);

    // between events the sensor's receiver is off
    uint64_t on0 = host_rx_on_us(sn);
    run(600000);
    CHECK(host_rx_on_us(sn) == on0);

    // the sniffer heard every frame (none collided)
    trace();
    uint8_t b[1 << 16];
    size_t n = host_console_take(sniff, b, sizeof b - 1), lines = 0;
    b[n] = 0;
    for (char *p = (char *)b; (p = strstr(p, "\nRX -")); p += 5) {
        lines++;  // not the "RX on" banner
    }
    if (lines != host_n_air) {
        fprintf(stderr, "sniffer: %zu lines, %zu frames on the air\n", lines, host_n_air);
    }
    CHECK(lines == host_n_air && host_n_air > 6);
}

static void test_reset_keeps_the_pairing(void)
{
    const uint8_t *mac = sensor_app.sensor.cfg.mac;
    uint16_t short_addr = sensor_app.sensor.short_addr;

    // an event writes no flash: the counter is RAM only (#38); the join is written when it joins
    uint8_t page[HOST_PAGE_SIZE];
    memcpy(page, sn->page, sizeof page);
    CHECK(say("event 1", "ok", 100));
    CHECK(alarm_from(mac, APP_EVENT, 5000) != NULL);
    CHECK(!memcmp(page, sn->page, sizeof page));

    host_boot(gw);  // both lose their RAM, not their NV page
    host_boot(sn);
    host_rx_init(&host_rx);
    run(100);
    handshake();
    CHECK(sensor_count() == 1);
    const pkt_t *r = call(HOST_GET_SENSOR_LIST, "\x01", 1);
    CHECK(r && r->n >= 8 && !memcmp(r->p, mac, 8));
    CHECK(sensor_app.sensor.state == SENSOR_JOINED && sensor_app.sensor.short_addr == short_addr);
    CHECK(say("event 0", "ok", 100));
    r = alarm_from(mac, APP_EVENT, 5000);
    CHECK(r && r->p[8 + 14] == 0);
    CHECK(r && r->p[8 + 15] == 0 && r->p[8 + 16] == 1);  // the counter started over: the first event is 1
}

static void test_heartbeat(void)
{
    const uint8_t *mac = sensor_app.sensor.cfg.mac;
    uint64_t on0 = host_rx_on_us(sn), t0 = host_now_us();
    // a v2 contact sensor's heartbeat (A1) comes every 120 min
    const pkt_t *r = alarm_from(mac, APP_STATUS, 121 * 60000);
    CHECK(r && r->at_us - t0 > 110 * 60000000ull);
    run(1000);
    // its receiver was on only around its own frames: well under 0.1 % of the time
    CHECK((host_rx_on_us(sn) - on0) * 1000 < host_now_us() - t0);
    CHECK(gw->nv_bad_writes == 0 && sn->nv_bad_writes == 0);
}

static int event_alarms(size_t from, int state)
{
    int n = 0;
    pump_host();
    for (size_t i = from; i < n_got; i++) {
        n += got[i].cmd == HOST_ALARM && got[i].n > 22 && got[i].p[8] == APP_EVENT && got[i].p[8 + 14] == state;
    }
    return n;
}

// The board's inputs, debounced: the button pairs, the switch reports once per settled change, glitches nothing
static void test_inputs(void)
{
    boot_world();
    handshake();
    pair(true);
    run(15000);  // past the first reports
    CHECK(!sensor_listening(&sensor_app.sensor));

    // a reed switch that bounces for 20 ms as it opens: one report, open
    size_t from = n_got;
    for (int i = 0; i < 5; i++) {
        host_input(sn, INPUT_EVENT, i % 2 == 0);
        run(4);
    }
    host_input(sn, INPUT_EVENT, true);
    run(5000);
    CHECK(event_alarms(from, 1) == 1 && event_alarms(from, 0) == 0);
    CHECK(strstr(sn_text, "input event 1") != NULL);

    // closing again: one report, closed
    from = n_got;
    host_input(sn, INPUT_EVENT, false);
    run(5000);
    CHECK(event_alarms(from, 0) == 1 && event_alarms(from, 1) == 0);

    // a 10 ms glitch: nothing, and the sensor slept through it with its receiver off
    from = n_got;
    uint64_t on0 = host_rx_on_us(sn);
    unsigned tx0 = sn->tx_frames;
    host_input(sn, INPUT_EVENT, true);
    run(10);
    host_input(sn, INPUT_EVENT, false);
    run(5000);
    CHECK(event_alarms(from, 0) + event_alarms(from, 1) == 0);
    CHECK(sn->tx_frames == tx0 && host_rx_on_us(sn) == on0);
}

// The sensor misses the gateway's ACK and sends its report again: the host gets it twice, as from a stock dongle,
// unless the host turned on GW_FLAG_DEDUP
static void test_retried_report(void)
{
    static const uint8_t dedup[4] = {0, 0, 0, GW_FLAG_DEDUP};
    boot_world();
    handshake();
    pair(false);
    run(15000);
    for (int on = 0; on < 2; on++) {
        if (on) {
            const pkt_t *r = ows_call(OWS_SUB_FLAGS, dedup, 4);
            CHECK(r && r->n == 9 && r->p[4] == GW_FLAG_DEDUP);
        }
        size_t from = n_got;
        unsigned tx0 = sn->tx_frames;
        sn->miss_frames = 1;  // the ACK of its report
        CHECK(say(on ? "event 0" : "event 1", "ok", 100));
        run(5000);
        CHECK(sn->tx_frames - tx0 >= 2);  // it did send the report again
        CHECK(event_alarms(from, !on) == (on ? 1 : 2));
    }
}

// --- CCFG restore (HOST_OWS_CCFG) ---

static void test_ccfg_is_safe(void)
{
    uint8_t c[CCFG_SIZE];
    const char *why;
    CHECK(ccfg_is_safe(host_safe_ccfg, &why) && why == NULL);
    // each thing that would lock a chip out
    static const struct {
        unsigned off;
        uint8_t v;
    } bad[] = {
        {0x33, 0x00},  // BL_CONFIG: ROM bootloader off
        {0x30, 0x00},  // BL_CONFIG: backdoor off
        {0x31, 0x40},  // BL_CONFIG: backdoor pin 64
        {0x3E, 0x00},  // TAP_DAP_0: CPU debug port off
        {0x35, 0xFE},  // ERASE_CONF: chip erase disabled
        {0x34, 0xFE},  // ERASE_CONF: bank erase disabled
        {0x44, 0x01},  // IMAGE_VALID: not 0
        {0x48, 0x7F},  // CCFG_PROT_31_0: sector 7 write-protected
        {0x57, 0xFE},  // CCFG_PROT_127_96
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        memcpy(c, host_safe_ccfg, CCFG_SIZE);
        c[bad[i].off] = bad[i].v;
        CHECK(!ccfg_is_safe(c, &why) && why != NULL);
    }
    // a restore keeps the device's identity (a sensor's MAC in IEEE_MAC_1 + IEEE_BLE_0)
    uint8_t cur[CCFG_SIZE], out[CCFG_SIZE];
    memcpy(cur, host_safe_ccfg, CCFG_SIZE);
    memcpy(cur + 0x24, "777A9A46", 8);
    cur[0x30] = 0x00;
    ccfg_merge(host_safe_ccfg, cur, out);
    CHECK(!memcmp(out + 0x24, "777A9A46", 8) && out[0x30] == 0xC5 && ccfg_is_safe(out, NULL));
}

// OWS_SUB_CCFG on the gateway: step, magic, address and nonce, payload as gateway.h; returns the 0x53F1 0x03 reply
static const pkt_t *ccfg_call(uint8_t step, const char *magic, uint64_t ieee, const uint8_t *nonce)
{
    uint8_t p[29];
    memcpy(p, magic, 16);
    p[16] = step;
    for (int i = 0; i < 8; i++) {
        p[17 + i] = (uint8_t)(ieee >> (56 - 8 * i));
    }
    if (nonce) {
        memcpy(p + 25, nonce, 4);
    }
    return ows_call(OWS_SUB_CCFG, p, nonce ? 29 : 25);
}

static void test_ccfg_restore(void)
{
    const pkt_t *r;
    uint8_t nonce[4], bad_nonce[4];
    host_reset();
    host_rx_init(&host_rx);
    n_got = 0;
    gw = host_add("gateway", &gw_app, HOST_APP(gateway));
    for (int i = 0; i < HOST_CCFG_OFF; i++) {
        gw->ccfg_page[i] = (uint8_t)(i * 7);  // stock code before the CCFG
    }
    host_boot(gw);
    run(100);
    uint64_t me = gw->ieee;
    uint8_t *cur = gw->ccfg_page + HOST_CCFG_OFF;

    r = ccfg_call(1, GW_CCFG_MAGIC, me, NULL);  // right already: not armed
    CHECK(r && r->n == 3 && r->p[1] == 1 && r->p[2] == GW_CCFG_ALREADY_RIGHT);
    // the chip's CCFG goes wrong (backdoor off); its MAC field holds a sensor's MAC
    cur[0x30] = 0x00;
    memcpy(cur + 0x24, "777A9A46", 8);
    r = ccfg_call(1, "OWS-CCFG-RESTORX", me, NULL);  // wrong magic
    CHECK(r && r->p[2] == GW_CCFG_REFUSED);
    r = ccfg_call(1, GW_CCFG_MAGIC, me ^ 1, NULL);  // another chip's address
    CHECK(r && r->p[2] == GW_CCFG_REFUSED);
    r = ccfg_call(2, GW_CCFG_MAGIC, me, (const uint8_t *)"\0\0\0\0");  // confirm before any arm
    CHECK(r && r->p[2] == GW_CCFG_REFUSED && gw->ccfg_rewrites == 0);
    r = ccfg_call(1, GW_CCFG_MAGIC, me, NULL);  // arm
    CHECK(r && r->n == 7 && r->p[1] == 1 && r->p[2] == GW_CCFG_OK);
    memcpy(nonce, r->p + 3, 4);
    memcpy(bad_nonce, nonce, 4);
    bad_nonce[3] ^= 1;
    r = ccfg_call(2, GW_CCFG_MAGIC, me, bad_nonce);  // wrong nonce: refused, and disarmed
    CHECK(r && r->p[2] == GW_CCFG_REFUSED);
    r = ccfg_call(2, GW_CCFG_MAGIC, me, nonce);
    CHECK(r && r->p[2] == GW_CCFG_REFUSED && gw->ccfg_rewrites == 0);
    r = ccfg_call(1, GW_CCFG_MAGIC, me, NULL);  // arm, then too late
    memcpy(nonce, r->p + 3, 4);
    run(GW_CCFG_ARM_MS + 100);
    r = ccfg_call(2, GW_CCFG_MAGIC, me, nonce);
    CHECK(r && r->p[2] == GW_CCFG_REFUSED && gw->ccfg_rewrites == 0);
    r = ccfg_call(1, GW_CCFG_MAGIC, me, NULL);  // arm, confirm: rewritten
    memcpy(nonce, r->p + 3, 4);
    r = ccfg_call(2, GW_CCFG_MAGIC, me, nonce);
    CHECK(r && r->n == 3 && r->p[1] == 2 && r->p[2] == GW_CCFG_OK && gw->ccfg_rewrites == 1);
    CHECK(cur[0x30] == 0xC5 && ccfg_is_safe(cur, NULL));
    CHECK(!memcmp(cur + 0x24, "777A9A46", 8));  // the device's identity kept
    int page_kept = 1;
    for (int i = 0; i < HOST_CCFG_OFF; i++) {
        page_kept &= gw->ccfg_page[i] == (uint8_t)(i * 7);
    }
    CHECK(page_kept);
    r = ccfg_call(2, GW_CCFG_MAGIC, me, nonce);  // the nonce is good for one confirm
    CHECK(r && r->p[2] == GW_CCFG_REFUSED && gw->ccfg_rewrites == 1);
    // a firmware whose own CCFG would lock the chip out never writes it
    cur[0x30] = 0x00;
    gw->ccfg_builtin[0x3E] = 0x00;  // its debug port off
    r = ccfg_call(1, GW_CCFG_MAGIC, me, NULL);
    CHECK(r && r->p[2] == GW_CCFG_NOT_SAFE && gw->ccfg_rewrites == 1);
    gw->ccfg_builtin[0x3E] = 0xC5;
    gw->ccfg_fail = true;  // a write that does not verify
    r = ccfg_call(1, GW_CCFG_MAGIC, me, NULL);
    memcpy(nonce, r->p + 3, 4);
    r = ccfg_call(2, GW_CCFG_MAGIC, me, nonce);
    CHECK(r && r->p[2] == GW_CCFG_FAILED);
}

// The same sensor leaves and pairs again as another type, on the address it had (as test_hw_sensor.py does
// against a dongle): what the gateway knew about the old one (its type: B0 after each poll for a climate sensor)
// must not reach the new one mid-join. Failed on the dongle on 2026-10-23: a B0 instead of the A4.
static void test_pair_again_as_another_type(void)
{
    char cmd[16];
    boot_world();
    handshake();
    static const uint8_t types[] = {SENSOR_CLIMATE_V2, SENSOR_MOTION_V1, SENSOR_CONTACT_V2};
    for (size_t i = 0; i < sizeof types; i++) {
        CHECK(say("leave", "ok", 100));
        snprintf(cmd, sizeof cmd, "type %02X", types[i]);
        CHECK(say(cmd, "ok", 100));
        pair_as(false, types[i]);
        CHECK(sensor_app.sensor.state == SENSOR_JOINED);
        CHECK(say(types[i] == SENSOR_CLIMATE_V2 ? "climate 2146 48" : "event 1", "ok", 100));
        run(8000);  // the reports, and for a climate sensor its B0 polls
    }
}

// Any PAN is a network like any other (#70: the house-network ban is lifted): a gateway with 0x845B in its NV page
// answers the host and the air
static void test_gateway_on_any_pan(void)
{
    static const gw_io_t io = {NULL, NULL, NULL, NULL, NULL};
    gateway_t house;
    nvstore_t nv;
    const pkt_t *r;

    host_reset();
    gw = host_add("gateway", &gw_app, HOST_APP(gateway));
    host_nv(gw, &nv);
    CHECK(nv_open(&nv));
    gw_init(&house, 0x845B, 0x3366, 0x00124B0000003366ull, (const uint8_t *)"778B5F63", &io);
    house.nv = &nv;
    CHECK(gw_save_network(&house));
    host_boot(gw);
    host_rx_init(&host_rx);
    n_got = 0;
    r = call(HOST_GET_VERSION, NULL, 0);
    CHECK(r && r->n == 26);
    CHECK(gw_app.radio_ok && gw_app.gw.pan == 0x845B && gw->led[BOARD_LED_OK] && !gw->led[BOARD_LED_ERROR]);
}

// A gateway whose radio does not start still answers the host (#70: never a silent dongle), shows the error LED,
// and brings the radio up once it starts
static void test_gateway_without_radio(void)
{
    const pkt_t *r;

    host_reset();
    gw = host_add("gateway", &gw_app, HOST_APP(gateway));
    gw->radio_broken = true;
    host_boot(gw);
    host_rx_init(&host_rx);
    n_got = 0;
    r = call(HOST_GET_VERSION, NULL, 0);
    CHECK(r && r->n == 26);
    r = ows_call(OWS_SUB_FLAGS, NULL, 0);
    CHECK(r && r->p[0] == OWS_SUB_FLAGS);
    CHECK(!gw_app.radio_ok && !gw_app.in_sbl && gw->led[BOARD_LED_ERROR] && !gw->led[BOARD_LED_OK]);
    gw->radio_broken = false;
    host_run(1500);  // the next retry
    CHECK(gw_app.radio_ok && gw->led[BOARD_LED_OK] && !gw->led[BOARD_LED_ERROR]);
}

static void test_sensor_without_radio(void)
{
    host_reset();
    sn = host_add("sensor", &sensor_app, HOST_APP(sensor));
    sn->radio_broken = true;
    sn_len = 0;
    host_boot(sn);
    host_run(100);
    pump_sensor();
    CHECK(strstr(sn_text, "radio init FAILED") != NULL);
    CHECK(sensor_app.halted && sn->led[BOARD_LED_ERROR]);
}

// --- OTA updates (docs/design/ota.md, common/ota.h) ---

#define IMG_MAX (SLOT_IMAGE_OFFSET + 9000)
static uint8_t img_old[IMG_MAX] __attribute__((aligned(8)));
static uint8_t img_new[2][IMG_MAX] __attribute__((aligned(8)));

// A slot image as tools/slot_image.py makes it: its header (for the host board and target slot), then valid ARM
// vector table + size bytes of "code"
static size_t make_image(int slot, uint8_t *img, uint32_t version, uint32_t size, uint8_t seed, bool confirmed)
{
    slot_header_t h;
    memset(img, 0xFF, SLOT_IMAGE_OFFSET);
    for (uint32_t i = 0; i < size; i++) {
        img[SLOT_IMAGE_OFFSET + i] = (uint8_t)(i * 7 + version + seed);
    }
    if (size >= 8) {
        uint32_t sp = 0x20005000u;
        uint32_t reset = (boot_slot_base(slot) + SLOT_IMAGE_OFFSET + (size > 16 ? 16u : 0u)) | 1u;
        memcpy(img + SLOT_IMAGE_OFFSET, &sp, 4);
        memcpy(img + SLOT_IMAGE_OFFSET + 4, &reset, 4);
    }
    memset(&h, 0xFF, sizeof h);
    h.magic = SLOT_MAGIC;
    h.board_id = boot_board_id(BOARD_NAME);
    h.version = version;
    h.size = size;
    h.crc = crc32_ieee(img + SLOT_IMAGE_OFFSET, size);
    h.slot = (uint32_t)slot;
    if (confirmed) {
        h.confirmed = 0;
    }
    memcpy(img, &h, sizeof h);
    return SLOT_IMAGE_OFFSET + size;
}

// The update the host has: version v, its slot-A build 8800 bytes, its slot-B build 9000 (they differ, as builds
// linked for different addresses do)
static void host_has_update(uint32_t v)
{
    host_ota.len[0] = make_image(0, img_new[0], v, 8800, 0xA0, false);
    host_ota.len[1] = make_image(1, img_new[1], v, 9000, 0xB0, false);
    host_ota.img[0] = img_new[0];
    host_ota.img[1] = img_new[1];
}

static uint32_t running_version(void)
{
    return sn->slot < 0 ? 0 : header_of(sn->flash + boot_slot_base(sn->slot))->version;
}

static bool slot_holds(int slot, const uint8_t *img, size_t len)
{
    return !memcmp(sn->flash + boot_slot_base(slot) + SLOT_IMAGE_OFFSET, img + SLOT_IMAGE_OFFSET,
                   len - SLOT_IMAGE_OFFSET);
}

// The gateway, and a sensor that boots through the boot selector: version 10 in slot A (confirmed), slot B empty.
// The host has version 11 and offers it when the sensor's scan result shows.
static void boot_ota_world(uint32_t flags)
{
    uint8_t f[4] = {(uint8_t)(flags >> 24), (uint8_t)(flags >> 16), (uint8_t)(flags >> 8), (uint8_t)flags};
    host_reset();
    n_got = sn_len = 0;
    host_rx_init(&host_rx);
    gw = host_add("gateway", &gw_app, HOST_APP(gateway));
    sn = host_add("sensor", &sensor_app, HOST_APP(sensor));
    sn->slots = true;
    host_install_slot(sn, 0, img_old, make_image(0, img_old, 10, 8000, 0, true));
    memset(&host_ota, 0, sizeof host_ota);
    host_has_update(11);
    host_ota.corrupt_at = -1;
    host_boot(gw);
    host_boot(sn);
    host_run(100);
    CHECK(sn->booted && sn->slot == 0 && running_version() == 10);
    handshake();
    const pkt_t *r = ows_call(OWS_SUB_FLAGS, f, 4);
    CHECK(r && r->n == 9 && r->p[4] == (flags & GW_FLAGS_SUPPORTED));
}

// The next OWS_SUB_OTA_DONE (0x53F1 0x13: the sensor's result, for the version offered) within ms: its status, -1 if none came
static int ota_result(uint32_t ms)
{
    const pkt_t *r = wait_ows(OWS_SUB_OTA_DONE, ms);
    pump_sensor();
    if (!r) {
        return -1;
    }
    CHECK(r->n == 14 && !memcmp(r->p + 1, sensor_app.sensor.cfg.mac, 8) &&
          ota_be32(r->p + 10) == header_of(host_ota.img[0])->version);
    return r->p[9];
}

static size_t count_got(uint16_t cmd, size_t from)
{
    size_t n = 0;
    pump_host();
    for (size_t i = from; i < n_got; i++) {
        n += got[i].cmd == cmd;
    }
    return n;
}

static size_t count_ows(uint8_t sub, size_t from)
{
    size_t n = 0;
    pump_host();
    for (size_t i = from; i < n_got; i++) {
        n += got[i].cmd == HOST_OWS_RSP && got[i].n >= 1 && got[i].p[0] == sub;
    }
    return n;
}

// The host stages the update it has for the sensor (OWS_SUB_OTA_OFFER); the gateway's answer, -1 if none
static int stage(void)
{
    uint8_t o[8 + OTA_OFFER_LEN];
    ota_offer_payload(o, sensor_app.sensor.cfg.mac);
    const pkt_t *r = ows_call(OWS_SUB_OTA_OFFER, o, sizeof o);
    return r && r->n == 2 ? r->p[1] : -1;
}

// A short press of the sensor's button (0.3 s): its OTA window, and a check-in
static void short_press(void)
{
    line_t l = {"button short", sn_len};
    pump_sensor();
    l.from = sn_len;
    host_input(sn, INPUT_BUTTON, true);
    host_run_until(keep_reading, NULL, 300);
    host_input(sn, INPUT_BUTTON, false);
    CHECK(host_run_until(sensor_said, &l, 200));
}

// What the user does for an update: the host stages it, then a short press of the sensor's button
static void stage_and_press(void)
{
    CHECK(stage() == 0);
    short_press();
}

static const uint8_t net_key[16];  // the secured link's network key in these tests (defined with its tests below)

// The secured link (#28): an update for a secured sensor comes sealed from its own gateway, so it needs no button: the
// host stages it, and the sensor takes it at its next check-in (here an A1 from the console, as a heartbeat would)
static void test_secure_ota_without_button(void)
{
    boot_ota_world(GW_FLAG_OTA | GW_FLAG_ANNOUNCE);
    const pkt_t *r = ows_call(OWS_SUB_NET_KEY, net_key, sizeof net_key);
    CHECK(r && r->n == 2 && r->p[1] == 1);
    pair(false);
    run(15000);
    CHECK(sensor_app.sensor.sl.session && (sensor_app.sensor.gw_features & A6_OURS_OTA));
    CHECK(stage() == 0);
    run(5000);
    CHECK(!strstr(sn_text, "ota offer"));  // nothing yet: the sensor has not polled
    CHECK(say("status", "", 100));         // its next check-in (no button)
    int res = ota_result(120000);
    if (res != OTA_DONE_OK) {
        fprintf(stderr, "secure ota: result %d\n--- sensor:\n%s\n", res, sn_text);
    }
    CHECK(res == OTA_DONE_OK);
    run(500);
    CHECK(!sensor_app.ota_window && strstr(sn_text, "ota offer 11, slot B, 9000 bytes: taken"));
    CHECK(sn->slot == 1 && running_version() == 11);
}

// With OTA on, a paired sensor, the update staged, a short press of its button: offered version 11, the sensor (in
// A) takes the slot-B build, writes slot B, says so, restarts into it; it reports as before and confirms itself
// after its first minute. It never paired again. Then version 12, staged after the press (found by the window's
// polls), into slot A.
static void test_ota_update(void)
{
    const uint8_t *mac = sensor_app.sensor.cfg.mac;
    boot_ota_world(GW_FLAG_OTA);
    pair(false);
    run(15000);
    size_t from = n_got;
    uint64_t t0 = host_now_us();
    stage_and_press();
    size_t done_at = n_got;
    CHECK(ota_result(120000) == OTA_DONE_OK);
    uint64_t took_ms = (host_now_us() - t0) / 1000;
    fprintf(stderr, "ota: %zu bytes in %llu ms from the button press, %u needs\n", host_ota.len[1],
            (unsigned long long)took_ms, host_ota.needs);
    CHECK(host_ota.needs == (host_ota.len[1] + OTA_BLOCK - 1) / OTA_BLOCK);
    CHECK(took_ms < 50000);
    CHECK(count_got(HOST_SCAN_RESULT, from) == 0);  // no pairing
    run(500);
    CHECK(strstr(sn_text, "ota offer 11, slot B, 9000 bytes: taken") && strstr(sn_text, "ota done, restarting"));
    CHECK(sn->resets == 1 && sn->booted && sn->slot == 1 && running_version() == 11);
    CHECK(slot_holds(1, img_new[1], host_ota.len[1]));
    CHECK(sn->flash_erases == 3);  // 9256 bytes: three pages of slot B, each once
    CHECK(strstr(sn_text, "slot B") != NULL);
    // still on its network: its A1 at start-up (the ACK it confirms on), then events as before
    CHECK(sensor_app.sensor.state == SENSOR_JOINED);
    run(5000);
    bool a1 = false;
    for (size_t i = done_at; i < n_got; i++) {
        a1 |= got[i].cmd == HOST_ALARM && got[i].n > 22 && got[i].p[8] == APP_STATUS &&
              got[i].at_us > t0 + took_ms * 1000;
    }
    CHECK(a1);
    CHECK(say("event 1", "ok", 100));
    CHECK(alarm_from(mac, APP_EVENT, 5000) != NULL);
    const slot_header_t *hb = header_of(sn->flash + SLOT_B_BASE);
    CHECK(hb->confirmed == 0xFFFFFFFF && hb->attempts[0] == 0 && hb->attempts[1] == 0xFFFFFFFF);
    run(61000);
    CHECK(hb->confirmed == 0 && strstr(sn_text, "image confirmed"));
    host_boot(sn);  // a later reset: B, for good
    run(100);
    CHECK(sn->slot == 1 && hb->attempts[1] == 0xFFFFFFFF);

    // version 12: now the slot-A build, over version 10; the press first, the update staged 4 s later
    host_has_update(12);
    host_ota.needs = 0;
    short_press();
    run(4000);
    CHECK(stage() == 0);
    CHECK(wait_ows(OWS_SUB_OTA_NEED, 11000) != NULL);  // the window's next poll (every 10 s) takes the offer
    CHECK(ota_result(120000) == OTA_DONE_OK);
    run(500);
    CHECK(strstr(sn_text, "ota offer 12, slot A, 8800 bytes: taken"));
    CHECK(host_ota.needs == (host_ota.len[0] + OTA_BLOCK - 1) / OTA_BLOCK);
    CHECK(sn->resets == 2 && sn->slot == 0 && running_version() == 12 && slot_holds(0, img_new[0], host_ota.len[0]));
    CHECK(header_of(sn->flash + SLOT_B_BASE)->version == 11 && header_of(sn->flash + SLOT_B_BASE)->confirmed == 0);
    CHECK(gw->nv_bad_writes == 0 && sn->nv_bad_writes == 0);
    trace();
}

// Frames lost on the way (blocks, ACKs): the update still completes, every byte right
static void test_ota_lossy(void)
{
    boot_ota_world(GW_FLAG_OTA);
    pair(false);
    stage_and_press();
    int st = -1, lost = 0;
    for (int i = 0; i < 200 && st < 0; i++) {                         // every 1.5 s the sensor misses a frame, and
        lost += !sn->miss_frames + (i % 3 == 1 && !gw->miss_frames);  // every 4.5 s the gateway too
        sn->miss_frames = 1;
        gw->miss_frames = i % 3 == 0;
        st = ota_result(1500);
    }
    CHECK(st == OTA_DONE_OK);
    CHECK(lost > 30);  // (a lost block comes again from the gateway's
    run(500);          // buffer, a lost request is sent again)
    CHECK(sn->slot == 1 && running_version() == 11 && slot_holds(1, img_new[1], host_ota.len[1]));
}

// The sensor resets half-way: version 10 keeps running from A, slot B is not taken; a new press starts over
static void test_ota_reset_midway(void)
{
    boot_ota_world(GW_FLAG_OTA);
    pair(false);
    stage_and_press();
    while (host_ota.needs < 40) {
        run(200);
    }
    host_boot(sn);
    run(10000);
    CHECK(sn->slot == 0 && running_version() == 10);
    CHECK(!strstr(sn_text, "ota done"));
    short_press();  // the window again: the same offer, from the start
    CHECK(ota_result(120000) == OTA_DONE_OK);
    run(500);
    CHECK(sn->slot == 1 && running_version() == 11 && slot_holds(1, img_new[1], host_ota.len[1]));
}

// A byte wrong in what the host serves: the CRC check fails, the sensor says so and stays on version 10
static void test_ota_bad_crc(void)
{
    boot_ota_world(GW_FLAG_OTA);
    host_ota.corrupt_at = SLOT_IMAGE_OFFSET + 5000;
    pair(false);
    stage_and_press();
    CHECK(ota_result(120000) == OTA_DONE_BAD_CRC);
    run(2000);
    CHECK(sn->resets == 0 && sn->slot == 0 && strstr(sn_text, "ota failed: bad CRC"));
    host_boot(sn);
    run(100);
    CHECK(sn->slot == 0 && running_version() == 10);  // the selector does not take the broken B

    // Wrong-slot image served for slot B (e.g. slot-A build duplicated): CRC holds, but boot_written_ok() rejects it
    boot_ota_world(GW_FLAG_OTA);
    host_ota.img[1] = img_new[0];
    host_ota.len[1] = host_ota.len[0];
    pair(false);
    stage_and_press();
    CHECK(ota_result(120000) == OTA_DONE_BAD_CRC);
    run(2000);
    CHECK(sn->resets == 0 && sn->slot == 0 && running_version() == 10);
}

// Battery too low when offered: refused, nothing erased; low half-way: stopped before next page erase, version 10 stays
static void test_ota_battery(void)
{
    boot_ota_world(GW_FLAG_OTA);
    pair(false);
    sn->battery_mv = 2300;
    stage_and_press();
    CHECK(ota_result(30000) == OTA_DONE_REFUSED);
    CHECK(strstr(sn_text, "bytes: battery low") && sn->flash_erases == 0);

    boot_ota_world(GW_FLAG_OTA);
    pair(false);
    stage_and_press();
    while (host_ota.needs < 30) {
        run(200);
    }
    uint32_t erases_before = sn->flash_erases;
    CHECK(erases_before == 1);  // only page 0 of slot B erased so far (30 * 96 = 2880 B < 4096 B)
    sn->battery_mv = 2300;
    CHECK(ota_result(30000) == OTA_DONE_FLASH);
    run(2000);
    CHECK(sn->flash_erases == erases_before);  // subsequent 4 KB pages were never erased
    CHECK(slot_holds(0, img_old, SLOT_IMAGE_OFFSET + 8000));
    CHECK(sn->resets == 0 && sn->slot == 0 && strstr(sn_text, "ota stopped: battery low"));
}

// Refused: not newer than what runs; offered without a button press (no window), or when the window has closed.
// Nothing written any time.
static void test_ota_refused(void)
{
    boot_ota_world(GW_FLAG_OTA);
    host_has_update(9);  // older than 10
    pair(false);
    stage_and_press();
    CHECK(ota_result(30000) == OTA_DONE_REFUSED);
    CHECK(strstr(sn_text, "ota offer 9, slot B, 9000 bytes: not newer") && sn->flash_erases == 0);

    boot_ota_world(GW_FLAG_OTA);
    pair(true);  // a long press pairs: no window
    run(15000);
    CHECK(stage() == 0);
    CHECK(say("status", "ok", 100));  // an A1, then its poll: the offer goes out
    CHECK(ota_result(10000) == OTA_DONE_REFUSED);
    CHECK(strstr(sn_text, "offer 11, slot B, 9000 bytes: no window") && sn->flash_erases == 0 && sn->slot == 0);

    short_press();  // a window, unused for its 5 minutes
    run(5 * 60000 + 1000);
    CHECK(!sensor_app.ota_window && stage() == 0);
    CHECK(say("status", "ok", 100));
    CHECK(ota_result(10000) == OTA_DONE_REFUSED);
    CHECK(sn->flash_erases == 0 && sn->slot == 0);
}

// R3: with GW_FLAG_OTA off, the gateway takes no offer and nothing changes on the air; a sensor that is no slot
// image refuses one
static void test_ota_off_and_not_a_slot_image(void)
{
    boot_ota_world(0);
    pair(false);
    CHECK(stage() == OTA_HOST_REFUSED);
    short_press();
    run(15000);
    CHECK(count_ows(OWS_SUB_OTA_NEED, 0) == 0 && count_ows(OWS_SUB_OTA_DONE, 0) == 0);

    boot_world();  // the sensor as an image at 0: no slots
    handshake();
    const pkt_t *r = ows_call(OWS_SUB_FLAGS, "\0\0\0\x04", 4);
    CHECK(r && r->p[4] == GW_FLAG_OTA);
    memset(&host_ota, 0, sizeof host_ota);
    host_has_update(11);
    host_ota.corrupt_at = -1;
    pair(false);
    stage_and_press();
    CHECK(ota_result(30000) == OTA_DONE_REFUSED);
    CHECK(strstr(sn_text, "bytes: not a slot image") && count_ows(OWS_SUB_OTA_NEED, 0) == 0);
}

// R3, GW_FLAG_ANNOUNCE: off, the A6 of a pairing is the stock one (its padding zero) and the sensor takes its gateway
// for stock; on, the A6 carries our mark and features, and the sensor knows its gateway is ours, across a reset
static bool last_a6(size_t from, uint8_t pt[16])
{
    bool found = false;
    for (size_t i = from; i < host_n_air && i < HOST_AIR_LOG; i++) {
        mac_frame_t m;
        if (host_air[i].from != 0 || !mac_parse(host_air[i].frame, host_air[i].len, &m) || m.type != MAC_DATA ||
            m.payload_len != 16) {
            continue;
        }
        uint8_t p[16];
        aes128_ecb_decrypt(app_key, m.payload, p, 16);
        if (p[0] == APP_JOIN_DONE) {
            memcpy(pt, p, 16), found = true;
        }
    }
    return found;
}

static void test_gateway_announces_itself(void)
{
    static const uint8_t zero[5];
    uint8_t a6[16];
    boot_world();
    handshake();
    size_t air0 = host_n_air;
    pair(false);
    CHECK(last_a6(air0, a6) && !memcmp(a6 + 11, "OW\x01\x00\x00", 5));  // on by default (#28), no features yet
    CHECK(sensor_app.sensor.gw_ours && sensor_app.sensor.gw_features == 0);

    const pkt_t *r = ows_call(OWS_SUB_FLAGS, "\0\0\0\0", 4);  // the host turns it off: stock
    CHECK(r && r->p[4] == 0);
    air0 = host_n_air;
    pair(true);
    CHECK(last_a6(air0, a6) && !memcmp(a6 + 11, zero, 5));
    CHECK(!sensor_app.sensor.gw_ours);

    r = ows_call(OWS_SUB_FLAGS, "\0\0\0\x0c", 4);  // ANNOUNCE + OTA
    CHECK(r && r->p[4] == (GW_FLAG_ANNOUNCE | GW_FLAG_OTA));
    air0 = host_n_air;
    pair(true);  // paired again, by the button
    CHECK(last_a6(air0, a6) && !memcmp(a6 + 11, "OW\x01\x00\x01", 5));
    CHECK(sensor_app.sensor.gw_ours && sensor_app.sensor.gw_features == A6_OURS_OTA);
    CHECK(strstr(sn_text, "joined") && strstr(sn_text, ", gateway ours"));
    host_boot(sn);  // kept with the join
    run(100);
    CHECK(say("info", "gw ours 0001", 200));
    CHECK(say("leave", "ok", 200));
    CHECK(!sensor_app.sensor.gw_ours);
}

// --- the secured link (common/seclink.h, docs/design/security.md; prototype, #28) ---

static const uint8_t net_key[16] = {0x10, 0x32, 0x54, 0x76, 0x98, 0xba, 0xdc, 0xfe,
                                    0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef};

// The last data frame the sensor sent after air index from: its plaintext (ECB envelope opened) into pt; its MAC frame
// into raw (for replaying it)
static size_t last_from_sensor(size_t from, uint8_t pt[APP_MAX_PAYLOAD], uint8_t raw[128], size_t *raw_len)
{
    size_t n = 0;
    for (size_t i = from; i < host_n_air && i < HOST_AIR_LOG; i++) {
        mac_frame_t m;
        if (host_air[i].from != 1 || !mac_parse(host_air[i].frame, host_air[i].len, &m) || m.type != MAC_DATA ||
            !m.payload_len || m.payload_len % 16 || m.payload_len > APP_MAX_PAYLOAD) {
            continue;
        }
        aes128_ecb_decrypt(app_key, m.payload, pt, m.payload_len);
        n = m.payload_len;
        if (raw) {
            memcpy(raw, host_air[i].frame, host_air[i].len);
            *raw_len = host_air[i].len;
        }
    }
    return n;
}

// Alarms (0x5319) of type from mac since got index from
static int alarms_since(size_t from, const uint8_t mac[8], uint8_t type)
{
    int k = 0;
    for (size_t i = from; i < n_got; i++) {
        k += got[i].cmd == HOST_ALARM && got[i].n > 22 && got[i].p[8] == type && !memcmp(got[i].p + 9, mac, 8);
    }
    return k;
}

// A frame on the air from an attacker's radio (a device in "another process")
static void inject(const uint8_t *frame, size_t len)
{
    host_air_t a = {HOST_REMOTE + 7, host_now_us() + 1000, host_now_us() + 1000 + 2000 * len, -50, (uint8_t)len, {0}};
    memcpy(a.frame, frame, len);
    host_air_receive(&a);
}

static void push_net_key(void)
{
    const pkt_t *r = ows_call(OWS_SUB_NET_KEY, net_key, sizeof net_key);
    CHECK(r && r->n == 2 && r->p[1] == 1);
}

static void test_secure_link(void)
{
    const uint8_t *mac = sensor_app.sensor.cfg.mac;
    uint8_t pt[APP_MAX_PAYLOAD], raw[128], f[128];
    size_t raw_len = 0;

    boot_world();
    handshake();
    const pkt_t *r = ows_call(OWS_SUB_NET_KEY, NULL, 0);  // no key until the host pushes one
    CHECK(r && r->n == 2 && r->p[1] == 0);
    push_net_key();
    size_t air0 = host_n_air;
    pair(false);
    uint8_t a6[16];
    CHECK(last_a6(air0, a6) && !memcmp(a6 + 11, "OW\x01\x00\x02", 5));  // ours, A6_OURS_SECURE
    run(10000);
    // the key came after the A6, the sensor said hello, and the session is up
    CHECK(sensor_app.sensor.sl.have_key && sensor_app.sensor.sl.session);
    uint8_t ks[16];
    sl_sensor_key(net_key, mac, ks);
    CHECK(!memcmp(sensor_app.sensor.sl.key, ks, 16));
    CHECK(gw_app.gw.n_sensors == 1 && gw_app.gw.sensors[0].secure);

    // an event: sealed on the air, an ordinary alarm at the host
    size_t from = n_got;
    air0 = host_n_air;
    CHECK(say("event 1", "ok", 100));
    r = alarm_from(mac, APP_EVENT, 5000);
    CHECK(r && r->p[8 + 14] == 1);
    CHECK(last_from_sensor(air0, pt, raw, &raw_len) && pt[0] == APP_OURS && pt[1] == SL_FRAME);
    CHECK(memmem(pt, sizeof pt, mac, 8) == NULL);  // the MAC is not in clear any more (confidentiality)
    run(3000);
    CHECK(alarms_since(from, mac, APP_EVENT) == 1);

    // a replay of that frame: dropped
    from = n_got;
    inject(raw, raw_len);
    run(1000);
    CHECK(alarms_since(from, mac, APP_EVENT) == 0);

    // a forged stock-format A2 from its address and MAC: dropped (the sensor is secured)
    uint8_t forged[17] = {APP_EVENT};
    memcpy(forged + 1, mac, 8);
    forged[9] = SENSOR_CONTACT_V2, forged[10] = 0x14, forged[11] = 0x60, forged[13] = 1, forged[14] = 0;
    forged[16] = 0x40;
    uint8_t ct[32];
    size_t clen = app_encrypt(forged, sizeof forged, ct);
    inject(f,
           mac_build_data(f, 0x77, gw_app.gw.pan, gw_app.gw.short_addr, sensor_app.sensor.short_addr, ct, clen, true));
    run(1000);
    CHECK(alarms_since(from, mac, APP_EVENT) == 0);
    // (control: the same frame does get through for a sensor that is not secured, so the drop above is the guard)
    gw_app.gw.sensors[0].secure = false;
    forged[16] = 0x41;  // not a retry of the one above
    clen = app_encrypt(forged, sizeof forged, ct);
    inject(f,
           mac_build_data(f, 0x78, gw_app.gw.pan, gw_app.gw.short_addr, sensor_app.sensor.short_addr, ct, clen, true));
    run(1000);
    CHECK(alarms_since(from, mac, APP_EVENT) == 1);
    gw_app.gw.sensors[0].secure = true;

    // the sensor restarts: its key from flash, a new session at its next report
    host_boot(sn);
    run(2000);
    CHECK(sensor_app.sensor.sl.have_key && !sensor_app.sensor.sl.session);
    CHECK(say("event 0", "ok", 100));
    r = alarm_from(mac, APP_EVENT, 10000);
    CHECK(r && r->p[8 + 14] == 0 && sensor_app.sensor.sl.session);

    // the gateway restarts and its host pushes the key again: the sensor's next report finds no session (no ACK),
    // it says hello at once, and the report goes again
    host_boot(gw);
    host_rx_init(&host_rx);
    run(500);
    handshake();
    push_net_key();
    from = n_got;
    CHECK(say("event 1", "ok", 100));
    r = alarm_from(mac, APP_EVENT, 15000);
    CHECK(r && r->p[8 + 14] == 1);
    run(3000);
    CHECK(alarms_since(from, mac, APP_EVENT) == 1);

    // the gateway restarts and nobody pushes the key: the report waits (no ACK, backing off), not lost; once the key
    // is there, the next try delivers it
    host_boot(gw);
    host_rx_init(&host_rx);
    run(500);
    handshake();
    from = n_got;
    CHECK(say("event 0", "ok", 100));
    run(20000);
    CHECK(alarms_since(from, mac, APP_EVENT) == 0 && sensor_app.sensor.n_queued >= 1);
    push_net_key();
    r = alarm_from(mac, APP_EVENT, 70000);  // the backoff's second step: 60 s
    CHECK(r && r->p[8 + 14] == 0);
    trace();
}

// OWS_SUB_BUILD_INFO (#76, TASK-10): paged <= 55 B query of the image's build info block (and legacy 0x53FA alias)
static void test_gateway_build_info(void)
{
    boot_world();
    uint8_t part0 = 0, part1 = 1;
    const pkt_t *r0 = ows_call(OWS_SUB_BUILD_INFO, &part0, 1);
    const pkt_t *r1 = ows_call(OWS_SUB_BUILD_INFO, &part1, 1);
    CHECK(r0 && r0->n == OWS_BUILD_INFO_P0_LEN && r0->n <= OWS_MAX_PAYLOAD);
    CHECK(r1 && r1->n == OWS_BUILD_INFO_P1_LEN && r1->n <= OWS_MAX_PAYLOAD);
    if (r0 && r0->n == OWS_BUILD_INFO_P0_LEN && r1 && r1->n == OWS_BUILD_INFO_P1_LEN) {
        build_info_t b;
        memset(&b, 0, sizeof b);
        memcpy(&b, r0->p + 1, 40);
        memcpy(b.app, r0->p + 41, 12);
        memcpy(b.board, r1->p + 9, 24);
        memcpy(b.version, r1->p + 33, 22);
        CHECK(!memcmp(r0->p + 9, r1->p + 1, 8));
        CHECK(b.magic == BUILD_INFO_MAGIC && b.format == BUILD_INFO_FORMAT && !strcmp(b.product, "host") &&
              !strcmp(b.app, "host") && !strcmp(b.board, BOARD_NAME) && !strcmp(b.version, "host"));
    }
    const pkt_t *legacy = call(HOST_OWS_BUILD_INFO_LEGACY, NULL, 0);
    CHECK(legacy && legacy->n == sizeof(build_info_t) && !memcmp(legacy->p, &build_info, sizeof(build_info_t)));
}

// A stray byte before a packet (a UART line settling, the ESP relay's start) does not cost the packet (#39): the
// gateway answers an Inquiry that follows noise, and one that follows a partial packet
static void test_gateway_skips_stray_bytes(void)
{
    static const uint8_t noise[][3] = {{0x00}, {0xff, 0x55}, {0xaa, 0x55, 0x43}};
    static const size_t n_noise[] = {1, 2, 3};
    boot_world();
    for (size_t i = 0; i < sizeof n_noise / sizeof n_noise[0]; i++) {
        uint8_t b[HOST_MAX_PACKET + 3];
        memcpy(b, noise[i], n_noise[i]);
        size_t n = n_noise[i] + host_build(b + n_noise[i], HOST_INQUIRY, NULL, 0, false);
        pump_host();
        want_t w = {HOST_INQUIRY + 1, 0, n_got, NULL};
        host_console_send(gw, b, n);
        CHECK(host_run_until(host_has, &w, 2000));
    }
}

// The console's "ota" (a bench, the console wired) does what a short press does: the window and a check-in
static void test_ota_from_the_console(void)
{
    boot_ota_world(GW_FLAG_OTA);
    pair(false);
    run(15000);
    CHECK(stage() == 0);
    CHECK(say("ota", "ok", 200));
    CHECK(ota_result(120000) == OTA_DONE_OK);
    run(500);
    CHECK(sn->slot == 1 && running_version() == 11 && slot_holds(1, img_new[1], host_ota.len[1]));
}

// A battery board (board_battery) never leaves its LED on: a blink at start-up, on a button press, on joining
// The MAC a board keeps in its CCFG (board_mac): used when it is 8 hex digits; a zeroed one (the v1 contacts' zero-MAC
// failure) must not leak into the MAC, which then comes from the chip as on a board without one (issue #36)
static bool hex_mac(const uint8_t m[8])
{
    for (int i = 0; i < 8; i++) {
        if (!((m[i] >= '0' && m[i] <= '9') || (m[i] >= 'A' && m[i] <= 'F'))) {
            return false;
        }
    }
    return true;
}

static void test_board_mac(void)
{
    static const uint8_t good[8] = {'7', '7', '7', 'B', '4', '2', 'D', 'F'}, zero[8];
    host_reset();
    sn = host_add("sensor", &sensor_app, HOST_APP(sensor));
    sn->has_mac = true;
    memcpy(sn->mac, good, 8);
    host_boot(sn);
    CHECK(!memcmp(sensor_app.sensor.cfg.mac, good, 8));
    host_reset();
    sn = host_add("sensor", &sensor_app, HOST_APP(sensor));
    sn->has_mac = true;
    memcpy(sn->mac, zero, 8);
    host_boot(sn);
    CHECK(!memcmp(sensor_app.sensor.cfg.mac, "77FE", 4) && hex_mac(sensor_app.sensor.cfg.mac));
    host_reset();
    gw = host_add("gateway", &gw_app, HOST_APP(gateway));
    gw->has_mac = true;
    memcpy(gw->mac, zero, 8);
    host_boot(gw);
    CHECK(!memcmp(gw_app.gw.mac, "77", 2) && hex_mac(gw_app.gw.mac));
}

// Whatever the NV page holds (a power cut left garbage, a stock page 29 that is all zeros...), both apps start and a
// sensor still pairs; the data may be lost, the boot never (the user's rule, #38)
static void test_any_nv_page_boots(void)
{
    static const char *const what[] = {"all zeros", "random", "a page header, then garbage", "a torn record"};
    unsigned seed = 11;
    for (int k = 0; k < 4; k++) {
        host_reset();
        n_got = sn_len = 0;
        host_rx_init(&host_rx);
        gw = host_add("gateway", &gw_app, HOST_APP(gateway));
        sn = host_add("sensor", &sensor_app, HOST_APP(sensor));
        for (int d = 0; d < 2; d++) {
            uint8_t *pg = d ? sn->page : gw->page;
            for (size_t i = 0; i < HOST_PAGE_SIZE; i++) {
                seed = seed * 1103515245 + 12345;
                pg[i] = k == 0 ? 0 : k == 3 && i >= 64 ? 0xFF : (uint8_t)(seed >> 16);
            }
            if (k >= 2) {
                memcpy(pg, "OWNV\x01\xff\xff\xff", 8);
            }
            if (k == 3) {  // valid OWNV header, then a corrupt record followed by a header cut after 3 bytes
                memset(pg + 8, 0xFF, 56);
                memcpy(pg + 8, "\xa5\xff\x01\x00\x00\x00\x04\x00\xde\xad\xbe\xef\x11\x22\x33\x44\xa5\xff\x02", 19);
            }
        }
        host_boot(gw);
        host_boot(sn);
        run(500);
        handshake();
        int before = failures;
        pair(false);
        CHECK(sensor_app.sensor.state == SENSOR_JOINED);
        if (failures != before) {
            fprintf(stderr, "  with an NV page of %s\n", what[k]);
        }
    }
}

static void check_blinks(host_dev_t *d, uint32_t half_ms, int count)
{
    for (int i = 0; i < 2 * count; i++) {
        CHECK(d->led[0] == (i % 2 == 0));
        run(half_ms);
    }
    CHECK(!d->led[0]);
}

static void test_battery_led(void)
{
    // 1. Normal boot on battery sensor: 1 x 250 ms blink; mains gateway keeps OK LED steadily on.
    host_reset();
    n_got = sn_len = 0;
    host_rx_init(&host_rx);
    gw = host_add("gateway", &gw_app, HOST_APP(gateway));
    sn = host_add("sensor", &sensor_app, HOST_APP(sensor));
    sn->battery = true;
    host_boot(gw);
    host_boot(sn);
    run(100);
    CHECK(gw->led[0]);
    check_blinks(sn, 250, 1);
    CHECK(gw->led[0]);

    // 2. Low-battery boot (<= 2.59 V) and missing-CCFG-MAC boot: 5 x 200 ms error blink.
    sn->battery_mv = 2500;
    host_boot(sn);
    run(80);
    check_blinks(sn, 200, 5);
    sn->battery_mv = 3000;
    sn->has_mac = true;
    memset(sn->mac, 0xFF, 8);
    host_boot(sn);
    run(80);
    check_blinks(sn, 200, 5);
    sn->has_mac = false;
    host_boot(sn);
    run(350);
    CHECK(!sn->led[0]);

    // 3. Short press (40..1000 ms): does NOT blink while held, blinks 1 x 100 ms on release.
    host_input(sn, INPUT_BUTTON, true);
    run(200);
    CHECK(!sn->led[0]);
    host_input(sn, INPUT_BUTTON, false);
    run(60);  // past 40 ms release debounce
    check_blinks(sn, 100, 1);

    // 4. Mid press (1.5 s, between 1.0 s and 3.04 s): neither short press nor pairing, LED stays off.
    pump_sensor();
    size_t mid_from = sn_len;
    host_input(sn, INPUT_BUTTON, true);
    run(1500);
    CHECK(!sn->led[0] && sensor_app.sensor.state == SENSOR_IDLE);
    host_input(sn, INPUT_BUTTON, false);
    run(200);
    pump_sensor();
    CHECK(!sn->led[0] && sensor_app.sensor.state == SENSOR_IDLE);
    CHECK(!strstr(sn_text + mid_from, "button short") && !strstr(sn_text + mid_from, "button long"));

    // 5. While-held 3.04 s pairing trigger (without releasing button!) + 3 x 100 ms blink on join failure.
    host_input(sn, INPUT_BUTTON, true);
    run(2900);
    CHECK(!sn->led[0] && sensor_app.sensor.state == SENSOR_IDLE);
    run(200);  // t = 3100 ms from press, button STILL held down
    CHECK(sensor_app.sensor.state != SENSOR_IDLE);
    check_blinks(sn, 400, 3);
    host_input(sn, INPUT_BUTTON, false);
    line_t failed = {"join failed", mid_from};
    CHECK(host_run_until(sensor_said, &failed, 15000));
    run(30);
    check_blinks(sn, 100, 3);

    // 6. Gateway scan-mode LED (300 ms toggle while HOST_SET_SCAN = 1, steady ON when scan ends).
    handshake();
    CHECK(call(HOST_SET_SCAN, "\x01", 1) != NULL);
    run(100);
    CHECK(!gw->led[0]);
    run(300);
    CHECK(gw->led[0]);
    run(300);
    CHECK(!gw->led[0]);
    CHECK(call(HOST_SET_SCAN, "\x00", 1) != NULL);
    run(50);
    CHECK(gw->led[0]);

    // 7. Join 0xA6 success blinks 3 x 400 ms; normal reed events & heartbeats keep LED strictly OFF;
    //    gateway pulses LED briefly on valid RF frame RX.
    pair(true);
    CHECK(sn->led[0]);
    run(2100);
    CHECK(!sn->led[0]);
    run(15000);
    CHECK(!sn->led[0] && gw->led[0]);
    host_input(sn, INPUT_EVENT, true);
    bool sn_blinked = false, gw_dipped = false;
    for (int i = 0; i < 40; i++) {
        run(10);
        sn_blinked |= sn->led[0];
        gw_dipped |= !gw->led[0];
    }
    CHECK(!sn_blinked && gw_dipped);
    run(200);
    CHECK(gw->led[0]);
    run(600000);
    CHECK(!sn->led[0]);

    // 8. OTA blink patterns on battery sensor: 5 x 200 ms on low-battery refusal, 2 x 100 ms on accept,
    //    and 3 x 250 ms on unconfirmed probation boot after OTA reset.
    boot_ota_world(GW_FLAG_OTA);
    sn->battery = true;
    host_boot(sn);
    run(350);
    pair(false);
    run(15000);
    sn->battery_mv = 2400;
    CHECK(stage() == 0);
    line_t refused = {"battery low", sn_len};
    short_press();
    CHECK(host_run_until(sensor_said, &refused, 5000));
    run(50);
    check_blinks(sn, 200, 5);

    sn->battery_mv = 3000;
    CHECK(stage() == 0);
    line_t taken = {"taken", sn_len};
    short_press();
    CHECK(host_run_until(sensor_said, &taken, 5000));
    CHECK(sn->led[0]);  // 2 x 100 ms accept blink started
    CHECK(ota_result(120000) == OTA_DONE_OK);
    run(100);  // booted into unconfirmed slot B: 3 x 250 ms blink
    CHECK(sn->slot == 1 && !sensor_app.confirmed);
    check_blinks(sn, 250, 3);
}

static void test_sniffer_max_frame(void)
{
    host_reset();
    sniff = host_add("sniffer", &sniffer_app, HOST_APP(sniffer));
    host_boot(sniff);
    uint8_t discard[256];
    host_console_take(sniff, discard, sizeof discard);

    // Advance virtual time past 1000 s so 4 MHz rat_time is 10 digits (4,000,000,000),
    // and deliver a 127-byte (RADIO_MAX_FRAME) frame at -100 dBm.
    host_set_now_us(1000000000ull);
    host_air_t f = {0};
    f.from = HOST_REMOTE;
    f.start_us = 1000000000ull;
    f.end_us = 1000220000ull;
    f.rssi = -100;
    f.len = RADIO_MAX_FRAME;
    for (size_t i = 0; i < RADIO_MAX_FRAME; i++) {
        f.frame[i] = (uint8_t)(0xA0u + (i & 0x0Fu));
    }
    host_air_receive(&f);
    host_run(300);

    uint8_t out[512];
    size_t n = host_console_take(sniff, out, sizeof out - 1);
    out[n] = 0;
    CHECK(strstr((char *)out, "RX -100 4000000000 a0a1a2a3") == (char *)out);
    CHECK(n >= 19 + 2 * RADIO_MAX_FRAME + 2 && out[n - 2] == '\r' && out[n - 1] == '\n');
}

static void test_sensor_app_edge_cases(void)
{
    // 1. restore() sets fw_version / a3_extra via set_type(), and respects only_pan.
    boot_world();
    handshake();
    CHECK(say("type 07", "ok", 100));
    pair_as(false, SENSOR_CLIMATE_V2);
    run(5000);
    host_boot(sn);
    run(100);
    CHECK(sensor_app.sensor.state == SENSOR_JOINED);
    CHECK(sensor_app.sensor.cfg.type == SENSOR_CLIMATE_V2);
    CHECK(sensor_app.sensor.cfg.fw_version == 0x0F && sensor_app.sensor.cfg.a3_extra[0] == 0x66);

    // 2. watch <sec> while joined (heartbeat hours away) polls every 20 ms without sleeping through pin transitions.
    sn->dio_levels = 0;
    pump_sensor();
    size_t watch_from = sn_len;
    CHECK(say("watch 1", "ok", 100));
    CHECK(strstr(sn_text + watch_from, "dio 0000") != NULL);
    sn->dio_levels = 0x0010u;
    line_t dio_chg = {"dio 0010", watch_from};
    CHECK(host_run_until(sensor_said, &dio_chg, 100));
    line_t watch_done = {"watch done", watch_from};
    CHECK(host_run_until(sensor_said, &watch_done, 1100));

    // 3. Re-triggering join mid-handshake works cleanly without losing `was`, and 30 s timeout restores `was`.
    const pkt_t *r = call(HOST_SET_SCAN, "\x01", 1);
    CHECK(r && r->n == 0);
    CHECK(say("join", "ok", 100));
    CHECK(wait_host(HOST_SCAN_RESULT, 10000) != NULL);  // sensor associated & entered SENSOR_HANDSHAKE, host silent
    CHECK(sensor_app.sensor.state == SENSOR_HANDSHAKE && sensor_app.was.active);
    r = call(HOST_SET_SCAN, "\x00", 1);  // stop gateway pairing so restart times out cleanly
    CHECK(r && r->n == 0);
    pump_sensor();
    size_t rejoin_from = sn_len;
    CHECK(say("join", "ok", 100));  // restart join from SENSOR_HANDSHAKE
    CHECK(sensor_app.sensor.state == SENSOR_SCANNING && sensor_app.was.active);
    line_t restored = {"back on the network it had", rejoin_from};
    CHECK(host_run_until(sensor_said, &restored, 10000));
    CHECK(sensor_app.sensor.state == SENSOR_JOINED);

    // 4. Delayed ACK arriving after CONFIRM_MS confirms a probation slot image immediately in the same step.
    boot_ota_world(GW_FLAG_OTA);
    pair(false);
    stage_and_press();
    CHECK(ota_result(120000) == OTA_DONE_OK);
    // Right as slot B boots, deafen the gateway so start-up A1 gets 0 ACKs during the first 60 s.
    gw->miss_frames = 100;
    run(65000);
    const slot_header_t *hb = header_of(sn->flash + SLOT_B_BASE);
    CHECK(hb->confirmed == 0xFFFFFFFF && sensor_app.sensor.acks == 0);
    gw->miss_frames = 0;
    pump_sensor();
    size_t conf_from = sn_len;
    CHECK(say("status", "ok", 100));
    line_t confirmed = {"image confirmed", conf_from};
    CHECK(host_run_until(sensor_said, &confirmed, 2000));
    CHECK(hb->confirmed == 0);
}

static void test_battery_console_awake(void)
{
    // 1. Untouched battery board sleeps after its 10 s power-up window; stray non-ASCII glitches do not extend it.
    host_reset();
    sn_len = 0;
    sn = host_add("sensor", &sensor_app, HOST_APP(sensor));
    sn->battery = true;
    host_boot(sn);
    CHECK(host_console_awake(sn));
    host_run(5000);
    host_console_send(sn, "\x00\xff", 2);  // floating-pad break/framing glitches: ignored
    host_run(100);
    CHECK(host_console_awake(sn));
    host_run(5000);  // past 10 s total
    CHECK(!host_console_awake(sn));
    pump_sensor();
    size_t asleep_from = sn_len;
    CHECK(!say("info", "info ", 200));  // UART RX is off in standby
    CHECK(sn_len == asleep_from);

    // 2. Valid ASCII during the 10 s window extends console awake mode via a sliding 5-minute inactivity timer.
    host_boot(sn);
    host_run(5000);
    CHECK(say("info", "info ", 100));
    host_run(10000);  // 15 s since boot, 10 s since last command: still awake!
    CHECK(host_console_awake(sn));
    CHECK(say("info", "info ", 100));  // slides the 5-minute window forward
    host_run(295000);
    CHECK(host_console_awake(sn));
    host_run(6000);  // > 300 s since last command: asleep
    CHECK(!host_console_awake(sn));

    // 3. The "sleep" command prints "ok" and immediately ends the console awake window, even with CRLF ("\r\n").
    host_boot(sn);
    host_run(1000);
    CHECK(say("info", "info ", 100));
    CHECK(host_console_awake(sn));
    CHECK(say("sleep", "ok", 100));
    CHECK(!host_console_awake(sn));
    CHECK(!say("info", "info ", 200));
}

// Hardened software SBL entry (TASK-11): direct-UART gateway (0x53F0 0x05 "OSBL" and 0x4312) and sensor ("sbl")
// shut down radio RX and jump to ROM SBL, while gateway-dongle (usb_hid) and disabled CCFG BL_CONFIG refuse cleanly.
static void test_software_sbl_entry_apps(void)
{
    // 1. USB dongle (CH554): OWS_SUB_ENTER_SBL refused with 0xE0, 0x4312 answers 0x4313 without software SBL jump.
    host_reset();
    gw = host_add("dongle", &gw_app, HOST_APP(gateway));
    gw->usb_hid = true;
    host_boot(gw);
    n_got = 0;
    const pkt_t *r = ows_call(OWS_SUB_ENTER_SBL, "OSBL", 4);
    CHECK(r && r->n == 2 && r->p[0] == OWS_SUB_ENTER_SBL && r->p[1] == 0xE0 && !gw->in_sbl && gw->booted);
    const pkt_t *u = call(HOST_UPDATE_MODE, NULL, 0);
    CHECK(u && u->cmd == HOST_UPDATE_MODE + 1 && !gw->in_sbl && gw->booted);

    // 2. Direct-UART gateway with CCFG BOOTLOADER_ENABLE != 0xC5: refused with 0xE0.
    host_reset();
    gw = host_add("gw", &gw_app, HOST_APP(gateway));
    gw->ccfg_page[HOST_CCFG_OFF + 0x33] = 0x00;
    host_boot(gw);
    n_got = 0;
    r = ows_call(OWS_SUB_ENTER_SBL, "OSBL", 4);
    CHECK(r && r->n == 2 && r->p[1] == 0xE0 && !gw->in_sbl && gw->booted && gw->rx_on);

    // 3. Direct-UART gateway with CCFG enabled: OWS_SUB_ENTER_SBL replies 0x00, turns off radio RX, enters SBL.
    gw->ccfg_page[HOST_CCFG_OFF + 0x33] = 0xC5;
    host_boot(gw);
    n_got = 0;
    r = ows_call(OWS_SUB_ENTER_SBL, "OSBL", 4);
    CHECK(r && r->n == 2 && r->p[1] == 0x00 && gw->in_sbl && !gw->booted && !gw->rx_on && gw->sbl_entries == 1);

    // 4. Direct-UART gateway also enters SBL on 0x4312 after sending 0x4313.
    host_boot(gw);
    n_got = 0;
    u = call(HOST_UPDATE_MODE, NULL, 0);
    CHECK(u && u->cmd == HOST_UPDATE_MODE + 1 && gw->in_sbl && !gw->booted && !gw->rx_on && gw->sbl_entries == 2);

    // 5. Sensor console "sbl": refuses with "error" if CCFG disabled, replies "ok" and enters SBL when enabled.
    sn_len = 0;
    sn = host_add("sensor", &sensor_app, HOST_APP(sensor));
    sn->ccfg_page[HOST_CCFG_OFF + 0x33] = 0x00;
    host_boot(sn);
    CHECK(say("sbl", "error", 100) && !sn->in_sbl && sn->booted);
    sn->ccfg_page[HOST_CCFG_OFF + 0x33] = 0xC5;
    host_boot(sn);
    CHECK(say("sbl", "ok", 100) && sn->in_sbl && !sn->booted && !sn->rx_on && sn->sbl_entries == 1);
}

static void test_zero_ladder_migration(void)
{
    // Pair sensor on gateway #1, export network + sensor via OWS_SUB_NETWORK + OWS_NV_GET_SENSOR, then wipe
    // gateway #1's flash page & change its chip IEEE (simulating replacing a USB dongle with a new IP gateway),
    // import via OWS_SUB_NV_IMPORT, reboot the gateway, and verify the untouched sensor's reed event arrives!
    boot_world();
    handshake();
    pair(false);
    run(15000);
    const uint8_t *mac = sensor_app.sensor.cfg.mac;

    const pkt_t *net = ows_call(OWS_SUB_NETWORK, NULL, 0);
    CHECK(net && net->n == 14 && net->p[1] == 0x00);
    if (!net) {
        return;
    }
    uint8_t saved_net[12];
    memcpy(saved_net, net->p + 2, 12);

    uint8_t get0[2] = {OWS_NV_GET_SENSOR, 0};
    const pkt_t *s0 = ows_call(OWS_SUB_NV_IMPORT, get0, sizeof get0);
    CHECK(s0 && s0->n == 18 && s0->p[1] == OWS_NV_GET_SENSOR && s0->p[2] == 0x00 && s0->p[3] == 1);
    if (!s0) {
        return;
    }
    uint8_t saved_sub[2] = {s0->p[5], s0->p[6]};
    uint8_t saved_short[2] = {s0->p[15], s0->p[16]};
    uint8_t saved_type = s0->p[17];

    // Replace the gateway with a blank unit on a different IEEE address: sensor events do not reach it yet.
    memset(gw->page, 0xFF, sizeof gw->page);
    gw->ieee = 0x00124B00CAFEBEEFull;
    host_boot(gw);
    handshake();
    CHECK(sensor_count() == 0);

    // Import network + sensor table over OWS_SUB_NV_IMPORT, then reboot gateway to prove Page 30 OWNV persistence.
    uint8_t clr[1] = {OWS_NV_CLEAR};
    const pkt_t *r = ows_call(OWS_SUB_NV_IMPORT, clr, sizeof clr);
    CHECK(r && r->n == 3 && r->p[2] == 0x00);
    uint8_t set_net[13];
    set_net[0] = OWS_NV_SET_NET;
    memcpy(set_net + 1, saved_net, 12);
    r = ows_call(OWS_SUB_NV_IMPORT, set_net, sizeof set_net);
    CHECK(r && r->n == 3 && r->p[2] == 0x00);
    uint8_t add[14];
    add[0] = OWS_NV_ADD_SENSOR;
    memcpy(add + 1, mac, 8);
    add[9] = saved_short[0];
    add[10] = saved_short[1];
    add[11] = saved_type;
    add[12] = saved_sub[0];
    add[13] = saved_sub[1];
    r = ows_call(OWS_SUB_NV_IMPORT, add, sizeof add);
    CHECK(r && r->n == 4 && r->p[2] == 0x00 && r->p[3] == 1);

    host_boot(gw);
    handshake();
    CHECK(sensor_count() == 1);
    host_input(sn, INPUT_EVENT, true);
    const pkt_t *a = alarm_from(mac, APP_EVENT, 5000);
    CHECK(a && a->p[8 + 14] == 1);
}

// TASK-13: sensor-motion-v1 PIR 120 s retriggerable clear + 0xAB 12-bucket summary + dual-slot OTA
static void pir_pulse(void)
{
    host_input(sn, INPUT_EVENT, true);
    run(80);
    host_input(sn, INPUT_EVENT, false);
}

static void test_motion_v1_pir_and_ota(void)
{
    boot_ota_world(GW_FLAG_OTA);
    sn->battery = true;
    host_boot(sn);
    run(350);  // past boot LED blink
    CHECK(say("type 02", "ok", 100));
    pair_as(true, SENSOR_MOTION_V1);
    const uint8_t *mac = sensor_app.sensor.cfg.mac;
    CHECK(alarm_from(mac, APP_STATUS, 5000) != NULL);
    run(10000);  // settle post-join polls and LED blinks
    CHECK(!sn->led[0] && !sensor_listening(&sensor_app.sensor));

    // Initial PIR pulse at t=0 -> immediate 0xA2 (state=1), LED stays off
    size_t from = n_got;
    pir_pulse();
    const pkt_t *a1 = alarm_from(mac, APP_EVENT, 2000);
    CHECK(a1 && a1->p[8 + 9] == SENSOR_MOTION_V1 && a1->p[8 + 14] == 1 && !sn->led[0]);
    run(1000);
    uint64_t rx_on_before = host_rx_on_us(sn);

    // Two pulses in bucket 1 (+12 s, +14 s) and one pulse in bucket 4 (+42 s): no extra A2s, receiver stays asleep!
    run(11000);
    pir_pulse();
    run(2000);
    pir_pulse();
    run(28000);
    pir_pulse();
    run(70000);  // ~112.4 s since initial trigger: still quiet on the air
    CHECK(event_alarms(from, 1) == 1 && event_alarms(from, 0) == 0);
    CHECK(host_rx_on_us(sn) == rx_on_before);

    // At 120 s from initial trigger: gateway forwards 0xAB (23 B plaintext) with exact 12 buckets; still active!
    const pkt_t *ab = alarm_from(mac, APP_MOTION_SUMMARY, 10000);
    static const uint8_t want_buckets[12] = {1, 2, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0};
    CHECK(ab && ab->n == 8 + 23 + 1 && ab->p[8 + 9] == SENSOR_MOTION_V1 && ab->p[8 + 22] == 0x01);
    if (ab && ab->n == 32) {
        CHECK(!memcmp(ab->p + 8 + 10, want_buckets, 12));
    }
    CHECK(event_alarms(from, 0) == 0 && sensor_app.sensor.pir.active);

    // At 120 s after the last pulse (+42 s + 120 s = +162 s): 0xA2 (state=0) clears motion
    const pkt_t *a0 = alarm_from(mac, APP_EVENT, 45000);
    CHECK(a0 && a0->p[8 + 14] == 0 && !sensor_app.sensor.pir.active);

    // Dual-slot OTA updates sensor-motion-v1 into Slot B while preserving SENSOR_MOTION_V1 in NV
    CHECK(stage() == 0);
    short_press();
    CHECK(ota_result(120000) == OTA_DONE_OK);
    run(500);
    CHECK(sn->slot == 1 && running_version() == 11 && sensor_app.sensor.cfg.type == SENSOR_MOTION_V1);
}

static size_t count_ota_ok(size_t from)
{
    size_t n = 0;
    pump_host();
    for (size_t i = from; i < n_got; i++) {
        if (got[i].cmd == HOST_OWS_RSP && got[i].n >= 10 && got[i].p[0] == OWS_SUB_OTA_DONE &&
            got[i].p[9] == OTA_DONE_OK) {
            n++;
        }
    }
    return n;
}

// Multi-sensor batch & wildcard fleet OTA (TASK-16): two sensors on the same radio air short-pressed back-to-back
// update cleanly one after the other without block collisions or extra button presses.
static void test_ota_multi_sensor_and_wildcard(void)
{
    static sensor_app_t sensor2_app;
    host_dev_t *sn2;
    uint8_t o[8 + OTA_OFFER_LEN];

    host_reset();
    n_got = sn_len = 0;
    host_rx_init(&host_rx);
    gw = host_add("gateway", &gw_app, HOST_APP(gateway));
    sn = host_add("sensor1", &sensor_app, HOST_APP(sensor));
    sn2 = host_add("sensor2", &sensor2_app, HOST_APP(sensor));
    sn->slots = sn2->slots = true;
    sn->has_mac = sn2->has_mac = true;
    memcpy(sn->mac, "77AAA001", 8);
    memcpy(sn2->mac, "77AAA002", 8);
    size_t old_len = make_image(0, img_old, 10, 6000, 0, true);
    host_install_slot(sn, 0, img_old, old_len);
    host_install_slot(sn2, 0, img_old, old_len);

    memset(&host_ota, 0, sizeof host_ota);
    for (int s = 0; s < 2; s++) {
        host_ota.len[s] = make_image(s, img_new[s], 11, 6000, 0, false);
        host_ota.img[s] = img_new[s];
    }
    host_ota.corrupt_at = -1;

    host_boot(gw);
    host_boot(sn);
    host_boot(sn2);
    host_run(100);
    handshake();
    uint8_t fl[4] = {0, 0, 0, GW_FLAG_OTA};
    CHECK(ows_call(OWS_SUB_FLAGS, fl, 4) != NULL);

    // Pair sensor 1 ("77AAA001") and sensor 2 ("77AAA002")
    pair(false);
    run(10000);
    CHECK(call(HOST_SET_SCAN, "\x01", 1) != NULL);
    host_console_send(sn2, (const uint8_t *)"join\r\n", 6);
    const pkt_t *sr = wait_host(HOST_SCAN_RESULT, 15000);
    CHECK(sr && !memcmp(sr->p + 1, "77AAA002", 8));
    uint8_t tok[24];
    memcpy(tok, "77AAA0020123456789ABCDEF", 24);
    CHECK(call(HOST_GET_R1, tok, sizeof tok) != NULL);
    CHECK(call(HOST_SET_SCAN, "\x00", 1) != NULL);
    CHECK(call(HOST_VERIFY_SENSOR, "77AAA002\xff\x04", 10) != NULL);
    run(10000);
    CHECK(sensor2_app.sensor.state == SENSOR_JOINED && sensor_count() == 2);

    // 1. Batch-stage BOTH MACs, then walk over and short-press BOTH sensors 1 s apart!
    ota_offer_payload(o, (const uint8_t *)"77AAA001");
    const pkt_t *r1 = ows_call(OWS_SUB_OTA_OFFER, o, sizeof o);
    ota_offer_payload(o, (const uint8_t *)"77AAA002");
    const pkt_t *r2 = ows_call(OWS_SUB_OTA_OFFER, o, sizeof o);
    CHECK(r1 && r1->p[1] == 0 && r2 && r2->p[1] == 0);

    size_t done_from = n_got;
    short_press();
    run(1500);
    host_input(sn2, INPUT_BUTTON, true);
    run(300);
    host_input(sn2, INPUT_BUTTON, false);

    // Run virtual air: sensor 1 updates to v11 (Slot B), then sensor 2's open 5-min OTA window picks up C0 and updates!
    for (int step = 0; step < 240 && count_ota_ok(done_from) < 2; step++) {
        run(1000);
    }
    CHECK(count_ota_ok(done_from) == 2);
    run(5000);
    CHECK(sn->slot == 1 && header_of(sn->flash + SLOT_B_BASE)->version == 11);
    CHECK(sn2->slot == 1 && header_of(sn2->flash + SLOT_B_BASE)->version == 11);
    CHECK(!gw_app.gw.ota.staged);

    // 2. Wildcard "********" wave to v12 (Slot A): even if sn2 is still finishing post-reboot polls (refusing with
    // "no window" before its button is pressed), pressing sn2's button sends A1 check-in and updates both to v12!
    for (int s = 0; s < 2; s++) {
        host_ota.len[s] = make_image(s, img_new[s], 12, 6000, 0, false);
        host_ota.img[s] = img_new[s];
    }
    ota_offer_payload(o, (const uint8_t *)"********");
    const pkt_t *rw = ows_call(OWS_SUB_OTA_OFFER, o, sizeof o);
    CHECK(rw && rw->p[1] == 0);
    done_from = n_got;
    short_press();
    run(1500);
    host_input(sn2, INPUT_BUTTON, true);
    run(300);
    host_input(sn2, INPUT_BUTTON, false);
    for (int step = 0; step < 240 && count_ota_ok(done_from) < 2; step++) {
        run(1000);
    }
    CHECK(count_ota_ok(done_from) == 2);
    run(5000);
    CHECK(sn->slot == 0 && header_of(sn->flash + SLOT_A_BASE)->version == 12);
    CHECK(sn2->slot == 0 && header_of(sn2->flash + SLOT_A_BASE)->version == 12);
}

// TASK-19: Runtime DIY sensor-type & pin-profile configuration in OWNV (NV_CFG 0x21) across reboots and dual-slot OTA.
static void test_repurposed_diy_profile_and_ota(void)
{
    nvstore_t nv;
    uint16_t cfg_len = 0;

    boot_ota_world(GW_FLAG_OTA);
    sn->battery = true;
    // On wyze_contact_v1, DIO7 open/dry is high (un-inverted board logical level = true); shorted to GND is false.
    host_input(sn, INPUT_EVENT, true);
    host_boot(sn);
    run(350);

    // Apply built-in resistive water-leak profile: type 03, invert 1, pull up, debounce 100 ms.
    CHECK(say("profile leak", "ok", 100));
    CHECK(sn->event_invert && sn->event_pull == BOARD_PULL_UP && sensor_app.event_debounce_ms == 100);
    CHECK(sensor_app.sensor.cfg.type == SENSOR_LEAK_V2 && sensor_app.sensor.state_byte == 0);
    CHECK(say("cfg", "cfg type 03 invert 1 pull up debounce 100", 100));
    CHECK(say("info", "invert 1 pull up debounce 100", 100));
    host_nv(sn, &nv);
    CHECK(nv_open(&nv));
    const uint8_t *raw = nv_find(&nv, 0x21, 0, &cfg_len);
    CHECK(raw && cfg_len == 8 && raw[0] == SENSOR_LEAK_V2 && raw[1] == 1 && raw[2] == BOARD_PULL_UP && raw[4] == 100);

    // Pair with gateway: initial A1 status reports sensor_type 0x03 and state 0 (dry).
    pair_as(false, SENSOR_LEAK_V2);
    const uint8_t *mac = sensor_app.sensor.cfg.mac;
    const pkt_t *st = alarm_from(mac, APP_STATUS, 5000);
    CHECK(st && st->p[8 + 9] == SENSOR_LEAK_V2 && st->p[8 + 14] == 0);
    run(10000);

    // A 60 ms splash glitch (< 100 ms custom debounce, even though > 40 ms default) is ignored!
    size_t from = n_got;
    host_input(sn, INPUT_EVENT, false);
    run(60);
    host_input(sn, INPUT_EVENT, true);
    run(2000);
    CHECK(event_alarms(from, 1) == 0 && event_alarms(from, 0) == 0);

    // Sustained water across DIO7 and GND (>= 100 ms) sends 0xA2 leak alarm (state = 1); drying sends state = 0.
    host_input(sn, INPUT_EVENT, false);
    const pkt_t *wet = alarm_from(mac, APP_EVENT, 3000);
    CHECK(wet && wet->p[8 + 9] == SENSOR_LEAK_V2 && wet->p[8 + 14] == 1);
    host_input(sn, INPUT_EVENT, true);
    const pkt_t *dry = alarm_from(mac, APP_EVENT, 3000);
    CHECK(dry && dry->p[8 + 9] == SENSOR_LEAK_V2 && dry->p[8 + 14] == 0);
    run(2000);

    // Reboot sensor: host_boot resets HAL pin state, proving sensor_app_init restores NV_CFG (0x21) before sampling.
    host_boot(sn);
    run(500);
    CHECK(sn->event_invert && sn->event_pull == BOARD_PULL_UP && sensor_app.event_debounce_ms == 100);
    CHECK(sensor_app.sensor.cfg.type == SENSOR_LEAK_V2 && sensor_app.sensor.state_byte == 0);

    // Dual-slot OTA from Slot A (v10) to Slot B (v11) preserves the custom DIY leak profile in Page 30 OWNV!
    CHECK(stage() == 0);
    // 60 ms button press still triggers short press (INPUT_BUTTON keeps 40 ms debounce despite 100 ms event debounce).
    host_input(sn, INPUT_BUTTON, true);
    run(60);
    host_input(sn, INPUT_BUTTON, false);
    CHECK(ota_result(120000) == OTA_DONE_OK);
    run(1000);
    CHECK(sn->slot == 1 && running_version() == 11);
    CHECK(sn->event_invert && sn->event_pull == BOARD_PULL_UP && sensor_app.event_debounce_ms == 100);
    CHECK(sensor_app.sensor.cfg.type == SENSOR_LEAK_V2);
    host_input(sn, INPUT_EVENT, false);
    const pkt_t *wet_after_ota = alarm_from(mac, APP_EVENT, 5000);
    CHECK(wet_after_ota && wet_after_ota->p[8 + 9] == SENSOR_LEAK_V2 && wet_after_ota->p[8 + 14] == 1);
    host_input(sn, INPUT_EVENT, true);
    run(3000);

    // Leaving and resetting profile to default deletes NV_CFG (0x21) cleanly; individual commands also validate args.
    CHECK(!say("profile contact", "ok", 100));  // refused while joined to a different sensor_type
    CHECK(say("leave", "ok", 100));
    CHECK(say("invert 0", "ok", 100) && !sn->event_invert);
    CHECK(say("pull none", "ok", 100) && sn->event_pull == BOARD_PULL_NONE);
    CHECK(say("debounce 250", "ok", 100) && sensor_app.event_debounce_ms == 250);
    CHECK(say("type contact", "ok", 100) && sensor_app.sensor.cfg.type == SENSOR_CONTACT_V1);
    CHECK(say("debounce 9999", "error", 100));
    CHECK(say("pull bogus", "error", 100));
    CHECK(say("invert 2", "error", 100));
    CHECK(say("profile default", "ok", 100));
    CHECK(!sn->event_invert && sn->event_pull == INPUT_PULL_DEFAULT && sensor_app.event_debounce_ms == 0);
    host_nv(sn, &nv);
    CHECK(nv_open(&nv) && nv_find(&nv, 0x21, 0, &cfg_len) == NULL);
}

// The battery under load (#75): a weak cell reads 3.0 V at rest and sags while a frame goes out. The reports carry
// the loaded reading (byte 11, 1/32 V; a report is built before its own frame, so it carries the frames' before it),
// the low-battery flag (byte 12) rises after 3 loaded readings below 2.625 V, and a fresh cell (>= 2.8 V loaded)
// clears it. Alarm payload: p[8 + 11] / p[8 + 12].
static void test_battery_under_load(void)
{
    const uint8_t *mac = sensor_app.sensor.cfg.mac;
    const pkt_t *r;
    boot_world();
    handshake();
    sn->flash_sag_mv = 400;  // the battery under a flash write: the pairing saves the join
    pair(false);
    CHECK(alarm_from(mac, APP_EVENT, 5000) && alarm_from(mac, APP_STATUS, 5000));
    CHECK(say("bat", "flash 2600 min 2600", 100));
    sn->flash_sag_mv = 0;
    run(15000);

    CHECK(say("event 1", "ok", 100));  // a good cell: 3.0 V under load too
    r = alarm_from(mac, APP_EVENT, 5000);
    CHECK(r && r->p[8 + 11] == 96 && r->p[8 + 12] == 0);
    run(15000);

    sn->tx_sag_mv = 500;  // a weak one: 2.5 V while sending, still 3.0 V at rest
    CHECK(say("event 0", "ok", 100));
    r = alarm_from(mac, APP_EVENT, 5000);
    CHECK(r && r->p[8 + 11] == 96);  // built before its frame: the earlier reading
    run(15000);
    for (int i = 0; i < 2; i++) {  // a report is one frame here: 3 low frames take 3 reports
        CHECK(say(i % 2 ? "event 0" : "event 1", "ok", 100));
        r = alarm_from(mac, APP_EVENT, 5000);
        CHECK(r && r->p[8 + 11] == 80 && r->p[8 + 12] == 0);  // 2.5 V under load, not low yet
        run(15000);
    }
    CHECK(say("bat", "bat idle 3000 loaded 2500 low 1 last 2500", 100));
    CHECK(say("event 1", "ok", 100));
    r = alarm_from(mac, APP_EVENT, 5000);
    CHECK(r && r->p[8 + 11] == 80 && r->p[8 + 12] == 1);  // after the third low frame: low
    run(15000);

    sn->tx_sag_mv = 100;  // a new cell: 2.9 V under load
    CHECK(say("event 0", "ok", 100));
    CHECK(alarm_from(mac, APP_EVENT, 5000) != NULL);
    run(15000);
    CHECK(say("event 1", "ok", 100));
    r = alarm_from(mac, APP_EVENT, 5000);
    CHECK(r && r->p[8 + 11] == 92 && r->p[8 + 12] == 0);  // 2.9 V, and the flag cleared
    sn->tx_sag_mv = 0;
}

int main(void)
{
    test_pairing_and_events();
    test_battery_under_load();
    test_reset_keeps_the_pairing();
    test_heartbeat();
    test_inputs();
    test_battery_led();
    test_battery_console_awake();
    test_any_nv_page_boots();
    test_board_mac();
    test_retried_report();
    test_pair_again_as_another_type();
    test_ccfg_is_safe();
    test_ccfg_restore();
    test_gateway_on_any_pan();
    test_gateway_without_radio();
    test_sensor_without_radio();
    test_gateway_announces_itself();
    test_secure_link();
    test_gateway_skips_stray_bytes();
    test_gateway_build_info();
    test_software_sbl_entry_apps();
    test_zero_ladder_migration();
    test_motion_v1_pir_and_ota();
    test_ota_multi_sensor_and_wildcard();
    test_repurposed_diy_profile_and_ota();
    test_ota_update();
    test_secure_ota_without_button();
    test_ota_lossy();
    test_ota_from_the_console();
    test_ota_reset_midway();
    test_ota_bad_crc();
    test_ota_battery();
    test_ota_refused();
    test_ota_off_and_not_a_slot_image();
    test_sniffer_max_frame();
    test_sensor_app_edge_cases();
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
