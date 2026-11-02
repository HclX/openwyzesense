// Roaming (#90, GW_FLAG_ROAM): several of our gateways sharing one network identity on one virtual air, with
// sensor.c sensors. Every gateway in range hears every sensor; a sensor hears every gateway, and two gateways sending
// in the same instant collide (it hears neither). Each gateway has its own host, which ACKs its async packets.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "gateway.h"
#include "host_proto.h"
#include "mac154.h"
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

#define N_GW  3
#define N_SN  2
#define PAN   0x65FE
#define COORD 0xD425
#define EXT   0x00124B00204F48FDull

typedef struct {
    uint8_t b[64];
    size_t n;
    int from;  // 0..N_GW-1 a gateway, 100 + i sensor i
} frame_t;

static gateway_t gws[N_GW];
static sensor_t sns[N_SN];
static int n_gw = 2;
static frame_t air[256];
static size_t n_air;

static struct {
    bool hears[N_SN];  // in range of sensor i (both ways)
    int8_t rssi[N_SN];
    int acks, beacons, other_tx;     // what it sent on the air
    int alarms[256];                 // 0x5319 by message type
    int overheard[256];              // OWS_SUB_OVERHEARD by message type
    uint16_t overheard_short;        // the last one's
    uint8_t overheard_rssi;          // ... and its RSSI byte
    uint8_t last_rsp[64], last_len;  // the last 0x53F1 that was not OVERHEARD
} st[N_GW];

static int collisions, lost;

static void air_tx(void *ctx, const uint8_t *frame, size_t len, uint32_t gap_us)
{
    (void)gap_us;
    frame_t *f = &air[n_air++ % 256];
    if (len > sizeof f->b) {
        len = sizeof f->b;
    }
    memcpy(f->b, frame, len);
    f->n = len;
    f->from = (int)(intptr_t)ctx;
    if (f->from >= 0 && f->from < N_GW) {
        uint8_t type = frame[0] & 7;
        if (type == MAC_ACK) {
            st[f->from].acks++;
        } else if (type == MAC_BEACON) {
            st[f->from].beacons++;
        } else {
            st[f->from].other_tx++;
        }
    }
}

static void host_reply(int g, const host_pkt_t *h);

// packets to the hosts, handled after the gateway call that sent them returns (as a real host would: the gateway
// keeps an async packet for resending only after handing it over)
static struct {
    int g;
    size_t n;
    uint8_t b[HOST_MAX_PACKET];
} inbox[64];
static size_t inbox_in, inbox_out;

static void host_out(void *ctx, const uint8_t *pkt, size_t len)
{
    inbox[inbox_in % 64].g = (int)(intptr_t)ctx;
    inbox[inbox_in % 64].n = len;
    memcpy(inbox[inbox_in++ % 64].b, pkt, len);
}

static void pump_hosts(void)
{
    static bool busy;
    if (busy) {
        return;
    }
    busy = true;
    while (inbox_out < inbox_in) {
        host_pkt_t h;
        size_t i = inbox_out++ % 64;
        if (host_parse(inbox[i].b, inbox[i].n, &h)) {
            host_reply(inbox[i].g, &h);
        }
    }
    busy = false;
}

static void host_send(int g, uint16_t cmd, const void *payload, size_t n)
{
    uint8_t p[HOST_MAX_PACKET];
    gw_host(&gws[g], p, host_build(p, cmd, payload, n, false));
    pump_hosts();
}

// what each gateway's host does: ACK async packets, count reports, play the pairing host
static void host_reply(int g, const host_pkt_t *h)
{
    uint8_t p[HOST_MAX_PACKET], payload[32];
    if (h->cmd >> 8 == HOST_ASYNC && h->cmd != HOST_ACK) {
        gw_host(&gws[g], p, host_build_ack(p, h->cmd, false));
    }
    if (h->cmd == HOST_ALARM && h->len >= 9) {
        st[g].alarms[h->payload[8]]++;
    } else if (h->cmd == HOST_OWS_RSP && h->len >= 12 && h->payload[0] == OWS_SUB_OVERHEARD) {
        st[g].overheard[h->payload[3 + 8]]++;
        st[g].overheard_short = (uint16_t)(h->payload[1] | h->payload[2] << 8);
        st[g].overheard_rssi = h->payload[h->len - 1];
    } else if (h->cmd == HOST_OWS_RSP && h->len <= sizeof st[g].last_rsp) {
        memcpy(st[g].last_rsp, h->payload, h->len);
        st[g].last_len = (uint8_t)h->len;
    } else if (h->cmd == HOST_SCAN_RESULT) {
        memcpy(payload, h->payload + 1, 8);
        memcpy(payload + 8, "Ok5HPNQ4lf77u754", 16);
        host_send(g, HOST_GET_R1, payload, 24);
    } else if (h->cmd == HOST_GET_R1 + 1) {
        memcpy(payload, h->payload + 1, 8);
        payload[8] = 0xFF, payload[9] = 0x04;
        host_send(g, HOST_VERIFY_SENSOR, payload, 10);
    }
}

static void ows(int g, uint8_t sub, const uint8_t *payload, size_t n)
{
    uint8_t b[OWS_MAX_PAYLOAD] = {sub};
    if (n) {
        memcpy(b + 1, payload, n);
    }
    st[g].last_len = 0;
    host_send(g, HOST_OWS_CMD, b, 1 + n);
}

static void set_flags(int g, uint32_t flags)
{
    uint8_t f[4] = {(uint8_t)(flags >> 24), (uint8_t)(flags >> 16), (uint8_t)(flags >> 8), (uint8_t)flags};
    ows(g, OWS_SUB_FLAGS, f, 4);
}

static void add_sensor(int g, const char *mac, uint16_t addr)
{
    uint8_t b[12] = {OWS_NV_ADD_SENSOR};
    memcpy(b + 1, mac, 8);
    b[9] = (uint8_t)addr, b[10] = (uint8_t)(addr >> 8);
    ows(g, OWS_SUB_NV_IMPORT, b, sizeof b);
}

static void drop_sensor(int g, const char *mac)
{
    host_send(g, HOST_DELETE_SENSOR, mac, 8);
}

// Delivers what was sent since the last call. Frames sent in the same call by two gateways collide at the sensors.
static void deliver(uint32_t now)
{
    size_t done = 0;
    for (int g = 0; g < n_gw; g++) {
        gw_tick(&gws[g], now);
    }
    pump_hosts();
    while (done < n_air) {
        size_t start = done;
        bool two = false;
        frame_t f = air[done++ % 256];
        if (f.from >= 100) {
            int i = f.from - 100;
            if (i >= N_SN) {
                continue;
            }
            for (int g = 0; g < n_gw; g++) {
                if (st[g].hears[i]) {
                    gw_rx(&gws[g], f.b, f.n, st[g].rssi[i]);
                }
            }
            continue;
        }
        // gateway frames: sent together (the replies to one sensor frame) collide if from two gateways
        while (done < n_air && air[done % 256].from < N_GW) {
            done++;
        }
        for (size_t k = start + 1; k < done; k++) {
            two |= air[k % 256].from != air[start % 256].from;
        }
        if (two) {
            collisions++;
            continue;
        }
        for (size_t k = start; k < done; k++) {
            const frame_t *gf = &air[k % 256];
            if (gf->from < 0 || gf->from >= N_GW) {
                continue;
            }
            for (int i = 0; i < N_SN; i++) {
                if (!st[gf->from].hears[i]) {
                    continue;
                }
                if (sensor_listening(&sns[i])) {
                    sensor_rx(&sns[i], gf->b, gf->n, now);
                } else {
                    lost++;
                }
            }
        }
    }
    n_air = 0;
    pump_hosts();
}

static void run(uint32_t *now, uint32_t ms)
{
    for (uint32_t end = *now + ms; *now < end; *now += 10) {
        for (int i = 0; i < N_SN; i++) {
            sensor_tick(&sns[i], *now);
        }
        deliver(*now);
    }
}

static const char *const MACS[N_SN] = {"77FFFF01", "77FFFF02"};

// n gateways with one identity (as after migrating a stock dongle to each), roaming on unless told otherwise;
// sensor 0 joined on 0x0003 (in no table yet)
static void world(int n, uint32_t flags, uint32_t *now)
{
    static const sensor_io_t sio[N_SN] = {
        {air_tx, NULL, NULL, NULL, (void *)100, NULL, NULL, NULL, NULL},
        {air_tx, NULL, NULL, NULL, (void *)101, NULL, NULL, NULL, NULL},
    };
    static const gw_io_t gio[N_GW] = {
        {air_tx, host_out, NULL, (void *)0, NULL},
        {air_tx, host_out, NULL, (void *)1, NULL},
        {air_tx, host_out, NULL, (void *)2, NULL},
    };
    memset(st, 0, sizeof st);
    inbox_in = inbox_out = 0;
    n_gw = n;
    n_air = 0;
    collisions = lost = 0;
    for (int g = 0; g < n; g++) {
        char mac[9];
        snprintf(mac, sizeof mac, "7700AA%02X", (unsigned)g & 0xFF);
        gw_init(&gws[g], PAN, COORD, EXT, (const uint8_t *)mac, &gio[g]);
        gws[g].clock_ms = 1792718501753ull;
        set_flags(g, flags);
        for (int i = 0; i < N_SN; i++) {
            st[g].hears[i] = true;
            st[g].rssi[i] = (int8_t)(-40 - 20 * g);
        }
    }
    for (int i = 0; i < N_SN; i++) {
        sensor_config_t cfg = {SENSOR_CONTACT_V2, {0}, 0x21, {0, 0, 0, 0x5a}, 0, 0, 0};
        memcpy(cfg.mac, MACS[i], 8);
        sensor_init(&sns[i], &cfg, &sio[i]);
    }
    *now = 1000;
    sensor_restore(&sns[0], PAN, COORD, 0x0003, 7, *now);
}

static int total(int g, int type)
{
    return st[g].alarms[type];
}

static void test_flag_supported(void)
{
    uint32_t now;
    world(1, 0, &now);
    ows(0, OWS_SUB_FLAGS, NULL, 0);
    CHECK(st[0].last_len == 9 && st[0].last_rsp[0] == OWS_SUB_FLAGS && (st[0].last_rsp[8] & GW_FLAG_ROAM));
    set_flags(0, GW_FLAG_ROAM);
    CHECK(gws[0].flags == GW_FLAG_ROAM);
}

// Without roaming, two gateways with the sensor in both tables both ACK each report: their ACKs collide
static void test_without_roaming_acks_collide(void)
{
    uint32_t now;
    world(2, 0, &now);
    add_sensor(0, MACS[0], 0x0003);
    add_sensor(1, MACS[0], 0x0003);
    sensor_event(&sns[0], true, now);
    run(&now, 3000);
    CHECK(st[0].acks >= 1 && st[1].acks >= 1 && collisions >= 1);
}

// Roaming: only the owner ACKs and reports it; the other gateway reports it as overheard, silent on the air
static void test_owner_only(void)
{
    uint32_t now;
    world(2, GW_FLAG_ROAM, &now);
    add_sensor(0, MACS[0], 0x0003);
    uint16_t c0 = sns[0].counter;
    sensor_event(&sns[0], true, now);
    run(&now, 3000);
    CHECK(sns[0].counter == c0 + 1);  // ACKed: no resend
    CHECK(collisions == 0 && st[0].acks == 1 && total(0, APP_EVENT) == 1);
    CHECK(st[1].acks == 0 && st[1].other_tx == 0 && total(1, APP_EVENT) == 0);
    CHECK(st[1].overheard[APP_EVENT] == 1 && st[1].overheard_short == 0x0003 && st[1].overheard_rssi == 60);
    CHECK(st[0].overheard[APP_EVENT] == 0);
    // its heartbeat too
    sensor_status(&sns[0], now);
    run(&now, 3000);
    CHECK(total(0, APP_STATUS) == 1 && st[1].overheard[APP_STATUS] == 1 && st[1].acks == 0 && collisions == 0);
}

// The host hands the sensor over: drop at the old owner, add at the new one. The sensor notices nothing.
static void test_handover(void)
{
    uint32_t now;
    world(2, GW_FLAG_ROAM, &now);
    add_sensor(0, MACS[0], 0x0003);
    sensor_event(&sns[0], true, now);
    run(&now, 3000);
    drop_sensor(0, MACS[0]);
    add_sensor(1, MACS[0], 0x0003);
    CHECK(gws[0].n_sensors == 0 && gws[1].n_sensors == 1);
    sensor_event(&sns[0], false, now);
    run(&now, 3000);
    CHECK(total(1, APP_EVENT) == 1 && st[1].acks == 1 && st[0].acks == 1);  // A's one ACK was before the handover
    CHECK(st[0].overheard[APP_EVENT] == 1 && total(0, APP_EVENT) == 1);
    CHECK(sns[0].state == SENSOR_JOINED && sns[0].short_addr == 0x0003 && collisions == 0);
}

// The owner out of range: the sensor resends, unACKed; the other gateway only listens and reports what it heard
static void test_owner_out_of_range(void)
{
    uint32_t now;
    world(2, GW_FLAG_ROAM | GW_FLAG_DEDUP, &now);
    add_sensor(0, MACS[0], 0x0003);
    st[0].hears[0] = false;
    sensor_event(&sns[0], true, now);
    run(&now, 10000);
    CHECK(st[0].acks == 0 && st[1].acks == 0 && st[1].other_tx == 0);
    CHECK(st[1].overheard[APP_EVENT] == 1);  // its resends deduplicated (GW_FLAG_DEDUP)
}

// Nobody's sensor (in no table): nobody answers it
static void test_unowned(void)
{
    uint32_t now;
    world(2, GW_FLAG_ROAM, &now);
    sensor_event(&sns[0], true, now);
    run(&now, 10000);
    CHECK(st[0].acks == 0 && st[1].acks == 0 && st[0].overheard[APP_EVENT] >= 1 && st[1].overheard[APP_EVENT] >= 1);
}

// Pairing on one gateway with its own address range: the new sensor joins it, the other one stays silent
static void test_pairing_with_ranges(void)
{
    uint32_t now;
    const uint8_t a[4] = {0x01, 0x00, 0xFF, 0x3F}, b[4] = {0x00, 0x40, 0xFF, 0x7F};
    int joined = 0;
    world(2, GW_FLAG_ROAM, &now);
    ows(0, OWS_SUB_ADDR_RANGE, a, 4);
    CHECK(st[0].last_len == 6 && st[0].last_rsp[1] == GW_CCFG_OK && gws[0].addr_last == 0x3FFF);
    ows(1, OWS_SUB_ADDR_RANGE, b, 4);
    CHECK(st[1].last_len == 6 && st[1].last_rsp[1] == GW_CCFG_OK && gws[1].addr_first == 0x4000);
    add_sensor(1, MACS[0], 0x0003);  // migrated sensors keep their addresses, in any range
    gws[1].permit = true;
    sensor_join(&sns[1], now);
    for (uint32_t end = now + 20000; now < end && !joined; now += 10) {
        for (int i = 0; i < N_SN; i++) {
            sensor_tick(&sns[i], now);
        }
        deliver(now);
        joined = sns[1].state == SENSOR_JOINED;
    }
    CHECK(joined && sns[1].short_addr == 0x4000 && sns[1].pan == PAN);
    CHECK(gws[1].n_sensors == 2 && gws[0].n_sensors == 0);
    CHECK(st[0].acks == 0 && st[0].other_tx == 0);  // the gateway not pairing only answered beacon requests
    run(&now, 10000);                               // its first reports
    CHECK(total(1, APP_STATUS) >= 1 && st[0].overheard[APP_STATUS] >= 1 && st[0].acks == 0);
}

static void test_addr_range_command(void)
{
    uint32_t now;
    const uint8_t bad[][4] = {{0, 0, 5, 0}, {6, 0, 5, 0}, {1, 0, 0xFE, 0xFF}};
    world(1, 0, &now);
    ows(0, OWS_SUB_ADDR_RANGE, NULL, 0);
    CHECK(st[0].last_len == 6 && st[0].last_rsp[1] == GW_CCFG_OK && st[0].last_rsp[2] == 1 &&
          st[0].last_rsp[5] == 0xFF && st[0].last_rsp[4] == 0xFD);
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        ows(0, OWS_SUB_ADDR_RANGE, bad[i], 4);
        CHECK(st[0].last_len == 6 && st[0].last_rsp[1] == GW_CCFG_REFUSED);
    }
    CHECK(gws[0].addr_first == 0x0001 && gws[0].addr_last == 0xFFFD);
    // a range of one: its address is handed out once, then none (refused at association)
    const uint8_t one[4] = {0x10, 0x00, 0x10, 0x00};
    ows(0, OWS_SUB_ADDR_RANGE, one, 4);
    gws[0].permit = true;
    sensor_join(&sns[1], now);
    run(&now, 20000);
    CHECK(sns[1].state == SENSOR_JOINED && sns[1].short_addr == 0x0010);
}

int main(void)
{
    test_flag_supported();
    test_without_roaming_acks_collide();
    test_owner_only();
    test_handover();
    test_owner_out_of_range();
    test_unowned();
    test_pairing_with_ranges();
    test_addr_range_command();
    printf("%d checks, %d failed\n", checks, failures);
    return failures != 0;
}
