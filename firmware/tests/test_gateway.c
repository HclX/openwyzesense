// Native tests of gateway.c: replays the captured house traffic (reference/tests/vectors/gateway_replay.txt,
// written by reference/tests/test_gateway_model.py) and checks every frame the gateway sends against what the
// stock dongle sent, byte for byte; then the host-side behaviour, as test_gateway_model.py checks the model.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "build_info.h"
#include "gateway.h"
#include "host_proto.h"
#include "mac154.h"
#include "wyze_app.h"

#define PAN     0x845B
#define SHORT   0x3366
#define EXT     0x00124B001E9FF933ull
#define MAX_OUT 16

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
    uint8_t b[HOST_MAX_PACKET];
    size_t n;
} buf_t;

// what the gateway sent: frames as PSDUs (FCS added as the radio would), host packets
static buf_t tx[MAX_OUT], host[MAX_OUT];
static size_t n_tx, n_host;

static uint32_t gaps[MAX_OUT];

static void on_tx(void *ctx, const uint8_t *frame, size_t len, uint32_t gap_us)
{
    (void)ctx;
    gaps[n_tx % MAX_OUT] = gap_us;
    buf_t *b = &tx[n_tx++ % MAX_OUT];
    memcpy(b->b, frame, len);
    if (len == 3 && (frame[0] & 7) == MAC_ACK) {
        mac_ack_fcs(frame, b->b + len);
    } else {
        mac_fcs(frame, len, b->b + len);
    }
    b->n = len + 4;
}

static void on_host(void *ctx, const uint8_t *pkt, size_t len)
{
    (void)ctx;
    buf_t *b = &host[n_host++ % MAX_OUT];
    memcpy(b->b, pkt, len);
    b->n = len;
}

static const gw_io_t io = {on_tx, on_host, NULL, NULL, NULL};

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

// What wyzesense2mqtt does on the host side while pairing: on a scan result, Get Sensor R1 with the token;
// on its reply, Verify Sensor.
static void pairing_host(gateway_t *gw)
{
    buf_t got[MAX_OUT];
    size_t n = n_host;
    memcpy(got, host, sizeof got);
    for (size_t i = 0; i < n; i++) {
        host_pkt_t h;
        uint8_t p[HOST_MAX_PACKET], payload[32];
        if (!host_parse(got[i].b, got[i].n, &h)) {
            continue;
        }
        if (h.cmd == HOST_SCAN_RESULT) {
            memcpy(payload, h.payload + 1, 8);
            memcpy(payload + 8, "Ok5HPNQ4lf77u754", 16);
            gw_host(gw, p, host_build(p, HOST_GET_R1, payload, 24, false));
        } else if (h.cmd == HOST_GET_R1 + 1) {
            memcpy(payload, h.payload + 1, 8);  // A5: a5, MAC, ...
            payload[8] = 0xFF, payload[9] = 0x04;
            gw_host(gw, p, host_build(p, HOST_VERIFY_SENSOR, payload, 10, false));
        }
    }
}

typedef struct {
    int windows, exact, uncaptured;
} stats_t;

// Replays one RX window: the frame, then the dongle frames that followed it in the capture.
static void window(gateway_t *gw, const uint8_t *rx, size_t rx_len, buf_t *expected, size_t n_expected,
                   int maybe_beacon, stats_t *st, int line)
{
    size_t i;
    int ok = 1;
    n_tx = n_host = 0;
    gw_rx(gw, rx, rx_len - 4, -60);
    if (gw->beacon_due) {  // the beacon goes out after its delay (#84); its content is what the replay compares
        gw_tick(gw, gw->beacon_at_ms);
    }
    pairing_host(gw);
    st->windows++;
    if (n_expected > n_tx || (n_expected == 0 && n_tx > 0)) {
        ok = 0;
    }
    for (i = 0; ok && i < n_expected; i++) {
        ok = tx[i].n == expected[i].n && !memcmp(tx[i].b, expected[i].b, tx[i].n);
    }
    if (ok) {
        st->exact++;
        // ACKs and beacons go out at once; the frame a pending ACK announced waits for the stock gap
        for (i = 0; i < n_tx; i++) {
            int answer = i > 0 && (tx[i].b[0] & 7) != MAC_ACK;
            CHECK(gaps[i] == (answer ? 33500u : 0u));
        }
        return;
    }
    if (maybe_beacon && n_expected == 0) {
        for (i = 0, ok = 1; i < n_tx; i++) {
            ok &= (tx[i].b[0] & 7) == MAC_BEACON;
        }
        if (ok) {
            st->uncaptured++;
            return;
        }
    }
    failures++;
    fprintf(stderr, "gateway_replay.txt:%d: sent %zu frame(s), the stock dongle %zu\n", line, n_tx, n_expected);
    for (i = 0; i < n_tx; i++) {
        fprintf(stderr, "  sent ");
        for (size_t k = 0; k < tx[i].n; k++) {
            fprintf(stderr, "%02x", tx[i].b[k]);
        }
        fprintf(stderr, "\n");
    }
}

static gateway_t last_gw;

static void test_replay(const char *path)
{
    static char lines[4096][256];
    int n = 0, sessions = 0;
    FILE *f = fopen(path, "r");
    gateway_t gw;
    stats_t st = {0};
    if (!f) {
        perror(path);
        exit(2);
    }
    while (n < 4096 && fgets(lines[n], sizeof lines[n], f)) {
        n++;
    }
    fclose(f);

    for (int i = 0; i < n; i++) {
        char *l = lines[i], name[64], mac[16];
        unsigned permit, dsn, bsn, addr;
        unsigned long long clock;
        if (sscanf(l, "SESSION %63s %u %u %u", name, &permit, &dsn, &bsn) == 4) {
            gw_init(&gw, PAN, SHORT, EXT, (const uint8_t *)"778B5F63", &io);
            gw.permit = permit, gw.dsn = (uint8_t)dsn, gw.bsn = (uint8_t)bsn;
            sessions++;
        } else if (sscanf(l, "SENSOR %15s %x", mac, &addr) == 2) {
            CHECK(gw_add_sensor(&gw, (const uint8_t *)mac, (uint16_t)addr));
        } else if (sscanf(l, "CLOCK %llu", &clock) == 1) {
            gw.clock_ms = clock;
        } else if (sscanf(l, "DSN %u", &dsn) == 1) {
            gw.dsn = (uint8_t)dsn;  // the stock dongle sent a frame the SDR missed
        } else if (!strncmp(l, "RX ", 3)) {
            static buf_t expected[MAX_OUT];
            uint8_t rx[128];
            size_t rx_len = unhex(l + 3, rx), n_expected = 0;
            int maybe_beacon = 0, start = i;
            for (; i + 1 < n; i++) {
                if (!strncmp(lines[i + 1], "TX ", 3)) {
                    expected[n_expected].n = unhex(lines[i + 1] + 3, expected[n_expected].b);
                    n_expected++;
                } else if (!strncmp(lines[i + 1], "MAYBE_BEACON", 12)) {
                    maybe_beacon = 1;
                } else {
                    break;
                }
            }
            window(&gw, rx, rx_len, expected, n_expected, maybe_beacon, &st, start + 1);
        } else if (!strncmp(l, "END", 3)) {
            last_gw = gw;
        }
    }
    printf("replay: %d sessions, %d windows, %d exact, %d with an uncaptured beacon\n", sessions, st.windows, st.exact,
           st.uncaptured);
    CHECK(sessions == 2 && st.windows > 80 && st.exact + st.uncaptured == st.windows && st.uncaptured <= 2);

    // the last session holds the two joins: a known sensor gets its old address, a new one the next free one
    int got_old = 0, got_new = 0;
    for (size_t k = 0; k < last_gw.n_sensors; k++) {
        if (!memcmp(last_gw.sensors[k].mac, "77BD514D", 8)) {
            got_old = last_gw.sensors[k].short_addr == 0x0002;
        }
        if (!memcmp(last_gw.sensors[k].mac, "77C68193", 8)) {
            got_new = last_gw.sensors[k].short_addr == 0x0012;
        }
    }
    CHECK(got_old && got_new);
}

static int sent(const buf_t *b, const char *hex)
{
    uint8_t want[HOST_MAX_PACKET];
    return unhex(hex, want) == b->n && !memcmp(b->b, want, b->n);
}

static void test_host(void)
{
    gateway_t gw;
    uint8_t p[HOST_MAX_PACKET];
    gw_init(&gw, PAN, SHORT, EXT, (const uint8_t *)"778B5F63", &io);
    gw_add_sensor(&gw, (const uint8_t *)"77BD514D", 2);
    gw_add_sensor(&gw, (const uint8_t *)"77C68193", 0x12);

    n_host = 0;
    gw_host(&gw, p, host_build(p, HOST_INQUIRY, NULL, 0, false));
    CHECK(n_host == 1 && sent(&host[0], "55aa43042801016f"));
    n_host = 0;
    gw_host(&gw, p, host_build(p, HOST_GET_MAC, NULL, 0, false));
    CHECK(n_host == 1 && sent(&host[0], "55aa430b05373738423546363303"
                                        "1e"));
    n_host = 0;
    gw_host(&gw, p, host_build(p, HOST_GET_VERSION, NULL, 0, false));
    CHECK(n_host == 2 && sent(&host[0], "55aa5316ff0267"));
    CHECK(sent(&host[1], "55aa531d17302e302e302e34372056312e3820476174657761792047573355"
                         "0850"));
    n_host = 0;
    gw_host(&gw, p, host_build(p, HOST_FINISH_AUTH, (const uint8_t *)"\xff", 1, false));
    CHECK(n_host == 2 && sent(&host[0], "55aa5314ff0265") && sent(&host[1], "55aa530315016a"));  // as captured
    gw_host(&gw, p, host_build_ack(p, 0x5315, false));
    n_host = 0;
    gw_host(&gw, p, host_build(p, HOST_GET_SENSOR_COUNT, NULL, 0, false));
    CHECK(n_host == 2 && host[1].b[5] == 2);
    gw_host(&gw, p, host_build_ack(p, 0x532F, false));
    n_host = 0;
    gw_host(&gw, p, host_build(p, HOST_GET_SENSOR_LIST, (const uint8_t *)"\x02", 1, false));
    CHECK(n_host == 3 && !memcmp(host[1].b + 5, "77BD514D", 8) && !memcmp(host[2].b + 5, "77C68193", 8));
    gw_host(&gw, p, host_build(p, HOST_TIME_SYNC, (const uint8_t *)"\x00\x00\x01\xa1\x66\x59\x77\x79", 8, false));
    CHECK(gw.clock_ms == 1792718501753ull);
    n_host = 0;
    gw_host(&gw, p, host_build(p, HOST_DELETE_SENSOR, (const uint8_t *)"77BD514D", 8, false));
    CHECK(gw.n_sensors == 1 && n_host == 2 && host[1].b[13] == 0xFF);

    // alarm forwarding: 77C681D1's heartbeat as the house dongle sent it (ground_truth/host/2026-05-21_*, 05-22)
    uint8_t pt[17], ct[32], f[64];
    memcpy(pt,
           "\xa1"
           "77C681D1"
           "\x07\x13\x5e\x00\x01\x00\x0a\x75",
           17);
    app_encrypt(pt, 17, ct);
    CHECK(gw_add_sensor(&gw, (const uint8_t *)"77C681D1", 0x000C));  // only a paired sensor is heard (#31)
    gw.clock_ms = 0x019E4E7BF23Bull;
    n_tx = n_host = 0;
    gw_rx(&gw, f, mac_build_data(f, 7, PAN, SHORT, 0x000C, ct, 32, true), -0x4C);
    CHECK(n_tx == 1 && tx[0].n == 7 && !memcmp(tx[0].b, "\x02\x00\x07", 3));  // ACK, no pending
    CHECK(n_host == 1 && sent(&host[0], "55aa531d190000019e4e7bf23ba1373743363831443107135e0001000a754c07c7"));
}

static void test_time_requests(void)
{
    gateway_t gw;
    uint8_t p[HOST_MAX_PACKET];
    host_pkt_t h;
    int requests = 0;
    gw_init(&gw, PAN, SHORT, EXT, (const uint8_t *)"778B5F63", &io);
    for (uint32_t t = 0; t < 10000; t += 10) {
        gw_tick(&gw, t);
    }
    n_host = 0;
    CHECK(n_host == 0);  // nobody has authenticated yet
    gw_host(&gw, p, host_build(p, HOST_FINISH_AUTH, (const uint8_t *)"\xff", 1, false));
    for (uint32_t t = 10000; t < 10000 + 2 * 692000 + 1000; t += 10) {
        size_t before = n_host;
        gw_tick(&gw, t);
        for (size_t i = before; i < n_host; i++) {
            if (host_parse(host[i % MAX_OUT].b, host[i % MAX_OUT].n, &h) && h.cmd == HOST_TIME_SYNC_REQ) {
                CHECK(requests || t == 9990 + 300);  // first 0.3 s after Finish Auth
                requests++;
                gw_host(&gw, p, host_build_ack(p, HOST_TIME_SYNC_REQ, false));
            }
        }
        if (n_host > MAX_OUT - 4) {
            n_host = 0;
        }
    }
    CHECK(requests == 3);  // then every 692 s
}

static void test_host_resends(void)
{
    gateway_t gw;
    uint8_t p[HOST_MAX_PACKET];
    gw_init(&gw, PAN, SHORT, EXT, (const uint8_t *)"778B5F63", &io);
    gw_tick(&gw, 1000);
    n_host = 0;
    gw_host(&gw, p, host_build(p, HOST_GET_VERSION, NULL, 0, false));  // ACK + 0x5317 (async)
    CHECK(n_host == 2);
    for (uint32_t t = 1000; t <= 2000; t += 10) {
        gw_tick(&gw, t);
    }
    CHECK(n_host == 2 + 3);  // 4 sends in all, then it gives up
    CHECK(!memcmp(host[4].b, host[1].b, host[1].n));
    n_host = 0;
    gw_host(&gw, p, host_build(p, HOST_GET_VERSION, NULL, 0, false));
    gw_tick(&gw, 2100);
    gw_host(&gw, p, host_build_ack(p, 0x5317, false));  // the host ACKs it
    for (uint32_t t = 2100; t <= 3000; t += 10) {
        gw_tick(&gw, t);
    }
    CHECK(n_host == 2);  // no resends
}

// --- feature flags (R3) ---

// The captured zero-MAC report (known-issues.md §1, vector ZERO_MAC): an A1 from short address 0x0010, FCS dropped
static const char zero_mac_frame[] =
    "6188e45b846633100082aab00b51319750340893db85585277dde376ac05df9f0ba53bda80c22e2075";

static uint32_t flags_call(gateway_t *gw, const uint8_t *set, uint32_t *supported)
{
    uint8_t req[5] = {OWS_SUB_FLAGS}, p[HOST_MAX_PACKET];
    host_pkt_t h;
    if (set) {
        memcpy(req + 1, set, 4);
    }
    n_host = 0;
    gw_host(gw, p, host_build(p, HOST_OWS_CMD, req, set ? 5 : 1, false));
    for (size_t i = 0; i < n_host; i++) {
        if (host_parse(host[i].b, host[i].n, &h) && h.cmd == HOST_OWS_RSP && h.len == 9 &&
            h.payload[0] == OWS_SUB_FLAGS) {
            if (supported) {
                *supported =
                    (uint32_t)h.payload[5] << 24 | (uint32_t)h.payload[6] << 16 | h.payload[7] << 8 | h.payload[8];
            }
            return (uint32_t)h.payload[1] << 24 | (uint32_t)h.payload[2] << 16 | h.payload[3] << 8 | h.payload[4];
        }
    }
    return 0xDEADBEEF;
}

// The alarms (0x5319) the gateway sent since n_host was cleared; mac: the first one's MAC
static int alarms(uint8_t mac[8])
{
    host_pkt_t h;
    int n = 0;
    for (size_t i = 0; i < n_host && i < MAX_OUT; i++) {
        if (host_parse(host[i].b, host[i].n, &h) && h.cmd == HOST_ALARM && h.len > 17) {
            if (!n && mac) {
                memcpy(mac, h.payload + 9, 8);
            }
            n++;
        }
    }
    return n;
}

// An A1 heartbeat (v1 contact, as in the capture) with MAC who from short address src; the alarms it brought
static int report_from(gateway_t *gw, const char *who, uint16_t src, uint8_t mac[8])
{
    uint8_t pt[32] = {APP_STATUS}, ct[32], f[64];
    memcpy(pt + 1, who, 8);
    pt[9] = SENSOR_CONTACT_V1, pt[10] = 0x11, pt[11] = 0x5A, pt[13] = 1, pt[15] = 0x0D;
    app_encrypt(pt, 17, ct);
    n_host = 0;
    gw_rx(gw, f, mac_build_data(f, 0x21, PAN, SHORT, src, ct, 32, true), -60);
    return alarms(mac);
}

// The same from the paired sensor 777ABCDE on 0x0010
static int receive_report(gateway_t *gw, uint8_t mac[8])
{
    return report_from(gw, "777ABCDE", 0x0010, mac);
}

// Update mode (WyzeSenseUpdater): the same reply as the stock dongle gave (2026-10-23 backup, info.json), so the
// dongle's CH554 sees the exchange it knows
static void test_update_mode_reply(void)
{
    static const uint8_t request[] = {0xAA, 0x55, 0x43, 0x03, 0x12, 0x01, 0x57},
                         stock[] = {0x55, 0xAA, 0x43, 0x03, 0x13, 0x01, 0x58};
    gateway_t gw;
    gw_init(&gw, PAN, SHORT, EXT, (const uint8_t *)"778B5F63", &io);
    n_host = 0;
    gw_host(&gw, request, sizeof request);
    CHECK(n_host == 1 && host[0].n == sizeof stock && !memcmp(host[0].b, stock, sizeof stock));
}

static void test_flags(void)
{
    static const uint8_t all[4] = {0xff, 0xff, 0xff, 0xff}, dedup[4] = {0, 0, 0, GW_FLAG_DEDUP};
    gateway_t gw;
    uint32_t supported = 0;
    gw_init(&gw, PAN, SHORT, EXT, (const uint8_t *)"778B5F63", &io);
    CHECK(flags_call(&gw, NULL, &supported) == 0);                                        // off after a reset
    CHECK(supported == (GW_FLAG_DEDUP | GW_FLAG_OTA | GW_FLAG_ANNOUNCE | GW_FLAG_ROAM));  // bit 1: reserved (#31)
    CHECK(flags_call(&gw, all, NULL) == GW_FLAGS_SUPPORTED);                              // only what it has
    CHECK(flags_call(&gw, dedup, NULL) == GW_FLAG_DEDUP);
    gw_init(&gw, PAN, SHORT, EXT, (const uint8_t *)"778B5F63", &io);
    CHECK(flags_call(&gw, NULL, NULL) == 0);  // not kept across a reset
}

static void test_dedup(void)
{
    static const uint8_t on[4] = {0, 0, 0, GW_FLAG_DEDUP};
    gateway_t gw;
    gw_init(&gw, PAN, SHORT, EXT, (const uint8_t *)"778B5F63", &io);
    CHECK(gw_add_sensor(&gw, (const uint8_t *)"777ABCDE", 0x0010));
    gw_tick(&gw, 1000);
    CHECK(receive_report(&gw, NULL) == 1 && receive_report(&gw, NULL) == 1);  // off: a retry goes up, as stock
    flags_call(&gw, on, NULL);
    CHECK(receive_report(&gw, NULL) == 1);
    gw_tick(&gw, 3000);
    CHECK(receive_report(&gw, NULL) == 0);  // the retry is dropped
    gw_tick(&gw, 1000 + GW_DEDUP_MS + 1);
    CHECK(receive_report(&gw, NULL) == 1);  // much later: a new report
}

// Only a sensor in the table is heard, as the stock dongle (stock-unknowns.md D8, #31): a report goes up only when
// its short address and MAC both match an entry; a zero MAC is never "recovered", and a stranger teaches no type
static void test_only_paired_sensors(void)
{
    static const uint8_t all[4] = {0xff, 0xff, 0xff, 0xff}, zero[8];
    uint8_t f[64], mac[8];
    gateway_t gw;
    gw_init(&gw, PAN, SHORT, EXT, (const uint8_t *)"778B5F63", &io);
    CHECK(receive_report(&gw, NULL) == 0);  // not paired: dropped
    CHECK(gw_add_sensor(&gw, (const uint8_t *)"777ABCDE", 0x0010));
    CHECK(receive_report(&gw, mac) == 1 && !memcmp(mac, "777ABCDE", 8));
    CHECK(report_from(&gw, "777FFFFF", 0x0010, NULL) == 0);  // another MAC on its address
    CHECK(report_from(&gw, "777ABCDE", 0x0011, NULL) == 0);  // its MAC from another address
    flags_call(&gw, all, NULL);                              // whatever the host turns on
    n_host = 0;
    gw_rx(&gw, f, unhex(zero_mac_frame, f), -60);  // the captured zero-MAC A1 from 0x0010
    CHECK(alarms(NULL) == 0);
    CHECK(report_from(&gw, (const char *)zero, 0x0010, NULL) == 0);

    // a stranger's report on a paired sensor's address teaches no type: that sensor's poll gets no B0
    gw_init(&gw, PAN, SHORT, EXT, (const uint8_t *)"778B5F63", &io);
    CHECK(gw_add_sensor(&gw, (const uint8_t *)"777C1FCA", 0x0010));
    gw.clock_ms = 1792718500000ull;
    gw_rx(&gw, f, unhex(zero_mac_frame, f), -60);
    uint8_t dreq[10] = {0x63, 0x88, 0x12, PAN & 0xFF, PAN >> 8, SHORT & 0xFF, SHORT >> 8, 0x10, 0x00, MAC_CMD_DATA_REQ};
    n_tx = 0;
    gw_rx(&gw, dreq, sizeof dreq, -60);
    CHECK(n_tx == 1 && (tx[0].b[0] & 0x10) == 0);  // the ACK alone, no frame pending
}

// A sensor associating with an all-zero address (a stock v1 contact that lost its MAC) is never paired (#31)
static void test_zero_mac_never_pairs(void)
{
    gateway_t gw;
    uint8_t f[24] = {0x23, 0xC8, 1, PAN & 0xFF, PAN >> 8, SHORT & 0xFF, SHORT >> 8, 0xFF, 0xFF};
    gw_init(&gw, PAN, SHORT, EXT, (const uint8_t *)"778B5F63", &io);
    gw.permit = true;
    f[17] = MAC_CMD_ASSOC_REQ, f[18] = 0x80;  // ext source (f[9..16]) all zero
    gw_rx(&gw, f, 19, -50);
    CHECK(gw.n_sensors == 0);
}

// Two sensors on one address in the table (a stock table can have that): the one that associates gets a new one
static void test_no_shared_address(void)
{
    static const uint8_t a[8] = {'7', '7', 'B', 'D', '5', '1', '4', 'D'},
                         b[8] = {'7', '7', 'A', '8', 'C', '7', '9', '3'};
    gateway_t gw;
    uint8_t f[24];
    gw_init(&gw, PAN, SHORT, EXT, (const uint8_t *)"778B5F63", &io);
    CHECK(gw_add_sensor(&gw, a, 0x0003) && gw_add_sensor(&gw, b, 0x0003));
    gw.permit = true;
    // b: association request (FC 0xC823, ext source = its ASCII MAC), then its data request (FC 0xC863)
    f[0] = 0x23, f[1] = 0xC8, f[2] = 1, f[3] = PAN & 0xFF, f[4] = PAN >> 8, f[5] = SHORT & 0xFF, f[6] = SHORT >> 8;
    f[7] = f[8] = 0xFF;
    memcpy(f + 9, b, 8);
    f[17] = MAC_CMD_ASSOC_REQ, f[18] = 0x80;
    gw_rx(&gw, f, 19, -50);
    f[0] = 0x63, f[2] = 2;
    memcpy(f + 7, b, 8);
    f[15] = MAC_CMD_DATA_REQ;
    size_t from = n_tx;
    gw_rx(&gw, f, 16, -50);
    int got = -1;
    for (size_t i = from; i < n_tx; i++) {
        if (tx[i % MAX_OUT].n == 25 + 4 && tx[i % MAX_OUT].b[21] == MAC_CMD_ASSOC_RESP) {
            got = tx[i % MAX_OUT].b[22] | tx[i % MAX_OUT].b[23] << 8;
        }
    }
    CHECK(got == 0x0004);
    CHECK(gw.sensors[0].short_addr == 0x0003 && gw.sensors[1].short_addr == 0x0004);
}

// An association request sent twice (the ACK of the first lost), then the sensor's data request: one response,
// and nothing left behind in the queue; a frame nobody fetches expires (issue #37)
static size_t pending_used(const gateway_t *gw)
{
    size_t n = 0;
    for (size_t i = 0; i < GW_MAX_PENDING; i++) {
        n += gw->pending[i].used;
    }
    return n;
}

static int associate_twice(gateway_t *gw, const uint8_t mac[8])
{
    uint8_t f[24];
    f[0] = 0x23, f[1] = 0xC8, f[2] = 1, f[3] = PAN & 0xFF, f[4] = PAN >> 8, f[5] = SHORT & 0xFF, f[6] = SHORT >> 8;
    f[7] = f[8] = 0xFF;
    memcpy(f + 9, mac, 8);
    f[17] = MAC_CMD_ASSOC_REQ, f[18] = 0x80;
    gw_rx(gw, f, 19, -50);
    gw_rx(gw, f, 19, -50);  // the retry: same sequence number
    f[0] = 0x63, f[2] = 2;
    memcpy(f + 7, mac, 8);
    f[15] = MAC_CMD_DATA_REQ;
    size_t from = n_tx;
    gw_rx(gw, f, 16, -50);
    int got = -1;
    for (size_t i = from; i < n_tx; i++) {
        if (tx[i % MAX_OUT].n == 25 + 4 && tx[i % MAX_OUT].b[21] == MAC_CMD_ASSOC_RESP) {
            got = tx[i % MAX_OUT].b[22] | tx[i % MAX_OUT].b[23] << 8;
        }
    }
    return got;
}

static void test_pending_queue(void)
{
    gateway_t gw;
    uint8_t mac[8] = {'7', '7', 'A', 'A', '0', '0', '0', '0'}, p[64];
    gw_init(&gw, PAN, SHORT, EXT, (const uint8_t *)"778B5F63", &io);
    gw.permit = true;
    gw_tick(&gw, 1000);
    for (int k = 0; k < 9; k++) {  // more sensors than queue slots
        mac[7] = (uint8_t)('1' + k);
        CHECK(associate_twice(&gw, mac) > 0);
    }
    CHECK(pending_used(&gw) == 0);
    // Get R1 for a sensor that never polls: its A4 waits, then expires
    uint8_t payload[24];
    memcpy(payload, mac, 8);
    memcpy(payload + 8, "Ok5HPNQ4lf77u754", 16);
    gw_host(&gw, p, host_build(p, HOST_GET_R1, payload, 24, false));
    CHECK(pending_used(&gw) == 1);
    gw_tick(&gw, 1000 + GW_PENDING_MS + 1);
    mac[7] = 'Z';
    CHECK(associate_twice(&gw, mac) > 0);  // queueing again drops what expired
    CHECK(pending_used(&gw) == 0);
}

static void test_truncated_payloads(void)
{
    gateway_t gw;
    uint8_t ct[16], frame[64];
    gw_init(&gw, PAN, SHORT, EXT, (const uint8_t *)"778B5F63", &io);
    CHECK(gw_add_sensor(&gw, (const uint8_t *)"77BD514D", 0x0002));
    n_host = 0;

    // 1. Truncated 16-byte A5 (needs 27 bytes) must not leak stack bytes via 0x5322.
    uint8_t bad_a5[16] = {APP_JOIN_RESPONSE, '7', '7', 'B', 'D', '5', '1', '4', 'D'};
    app_encrypt(bad_a5, sizeof bad_a5, ct);
    size_t flen = mac_build_data(frame, 1, PAN, SHORT, 0x0002, ct, 16, true);
    gw_rx(&gw, frame, flen, -50);
    CHECK(n_host == 0);

    // 2. Truncated 16-byte A1 report (needs >= 17 bytes) must not mutate sensor type or emit 0x5319.
    uint8_t bad_a1[16] = {APP_STATUS, '7', '7', 'B', 'D', '5', '1', '4', 'D', SENSOR_MOTION_V1, 0x14, 0x60, 0x00, 0x01};
    app_encrypt(bad_a1, sizeof bad_a1, ct);
    flen = mac_build_data(frame, 2, PAN, SHORT, 0x0002, ct, 16, true);
    gw_rx(&gw, frame, flen, -50);
    CHECK(n_host == 0 && gw.n_types == 0);
}

static size_t joining_used(const gateway_t *gw)
{
    size_t n = 0;
    for (size_t i = 0; i < GW_MAX_JOINING; i++) {
        n += gw->joining[i].used;
    }
    return n;
}

static void send_a3(gateway_t *gw, uint16_t src, const uint8_t mac[8])
{
    uint8_t pt[16] = {APP_JOIN_ANNOUNCE}, ct[16], frame[64];
    memcpy(pt + 1, mac, 8);
    pt[9] = SENSOR_CONTACT_V2;
    pt[10] = 0x21;
    app_encrypt(pt, 16, ct);
    size_t flen = mac_build_data(frame, 1, gw->pan, gw->short_addr, src, ct, 16, true);
    gw_rx(gw, frame, flen, -50);
}

static void test_joining_and_short_addr_guards(void)
{
    gateway_t gw;
    uint8_t p[HOST_MAX_PACKET], r1[24];
    gw_init(&gw, PAN, 0x0002, EXT, (const uint8_t *)"778B5F63", &io);
    gw.permit = true;
    gw_tick(&gw, 1000);

    // 1. Short-address wrap & collision guard: skips 0xFFFE/0xFFFF, existing 0x0001, and gw.short_addr (0x0002).
    CHECK(gw_add_sensor(&gw, (const uint8_t *)"77000001", 0x0001));
    CHECK(gw_add_sensor(&gw, (const uint8_t *)"7700FFFD", 0xFFFD));
    uint8_t f[24] = {0x23, 0xC8, 1, PAN & 0xFF, PAN >> 8, 0x02, 0x00, 0xFF, 0xFF};
    memcpy(f + 9, "7700NEW1", 8);
    f[17] = MAC_CMD_ASSOC_REQ;
    f[18] = 0x80;
    gw_rx(&gw, f, 19, -50);
    CHECK(gw.n_sensors == 3 && gw.sensors[2].short_addr == 0x0003);

    // 2. Five aborted A3 announcements never permanently wedge pairing; DELETE_SENSOR, timeout, and DELETE_ALL clean up.
    for (int i = 0; i < 5; i++) {
        uint8_t mac[8] = {'7', '7', 'J', 'O', 'I', 'N', '0', (uint8_t)('1' + i)};
        gw_tick(&gw, (uint32_t)(1000 + i * 100));
        send_a3(&gw, (uint16_t)(0x0010 + i), mac);
    }
    CHECK(joining_used(&gw) == GW_MAX_JOINING);
    // The 5th sensor ("77JOIN05" on 0x0014) is tracked and receives A4 on HOST_GET_R1.
    size_t pend_before = pending_used(&gw);
    memcpy(r1, "77JOIN05", 8);
    memcpy(r1 + 8, "Ok5HPNQ4lf77u754", 16);
    gw_host(&gw, p, host_build(p, HOST_GET_R1, r1, sizeof r1, false));
    CHECK(pending_used(&gw) == pend_before + 1);

    // Deleting a sensor removes its joining[] entry immediately.
    gw_host(&gw, p, host_build(p, HOST_DELETE_SENSOR, (const uint8_t *)"77JOIN05", 8, false));
    CHECK(joining_used(&gw) == GW_MAX_JOINING - 1);

    // Advancing past GW_PENDING_MS expires all remaining stale joining[] entries.
    gw_tick(&gw, 2000 + GW_PENDING_MS);
    CHECK(joining_used(&gw) == 0);

    // Re-populate one joining[] slot and verify HOST_DELETE_ALL clears it too.
    send_a3(&gw, 0x0020, (const uint8_t *)"77JOIN99");
    CHECK(joining_used(&gw) == 1);
    gw_host(&gw, p, host_build(p, HOST_DELETE_ALL, NULL, 0, false));
    CHECK(joining_used(&gw) == 0 && gw.n_sensors == 0);

    // 3. Pairing off: an A3 is not reported (stock gates 0x5320 on its permit, #60), nor tracked as joining.
    gw.permit = false;
    n_host = 0;
    send_a3(&gw, 0x0021, (const uint8_t *)"77JOIN98");
    CHECK(n_host == 0 && joining_used(&gw) == 0);
    gw.permit = true;
    send_a3(&gw, 0x0021, (const uint8_t *)"77JOIN98");
    host_pkt_t h;
    CHECK(n_host == 1 && host_parse(host[0].b, host[0].n, &h) && h.cmd == HOST_SCAN_RESULT && joining_used(&gw) == 1);
}

static void test_sensor_list_64_streaming(void)
{
    gateway_t gw;
    uint8_t p[HOST_MAX_PACKET], expected[GW_MAX_SENSORS][8];
    host_pkt_t h;
    gw_init(&gw, PAN, SHORT, EXT, (const uint8_t *)"778B5F63", &io);
    gw_tick(&gw, 1000);

    for (size_t i = 0; i < GW_MAX_SENSORS; i++) {
        char tmp[9];
        snprintf(tmp, sizeof tmp, "7764%04X", (unsigned)i);
        memcpy(expected[i], tmp, 8);
        CHECK(gw_add_sensor(&gw, expected[i], (uint16_t)(i + 1)));
    }

    n_host = 0;
    uint8_t req_cnt = GW_MAX_SENSORS;
    gw_host(&gw, p, host_build(p, HOST_GET_SENSOR_LIST, &req_cnt, 1, false));
    // Initial burst: 1 ACK (0x5330) + GW_MAX_UNACKED (4) 0x5331 items.
    CHECK(n_host == 1 + GW_MAX_UNACKED && gw.list_active && gw.list_cursor == GW_MAX_UNACKED);

    size_t received = 0;
    for (size_t i = 0; i < n_host; i++) {
        if (host_parse(host[i].b, host[i].n, &h) && h.cmd == HOST_SENSOR_LIST_ITEM) {
            CHECK(h.len == 8 && !memcmp(h.payload, expected[received], 8));
            received++;
        }
    }
    CHECK(received == GW_MAX_UNACKED);

    // Stream the remaining 60 sensors by ACKing each 0x5331 (including one simulated lost packet resent by gw_tick).
    uint32_t t = 1000;
    while (received < GW_MAX_SENSORS) {
        n_host = 0;
        if (received == 8) {
            // Simulate a lost packet: let gw_tick resend the oldest unacked 0x5331 before ACKing.
            t += 200;
            gw_tick(&gw, t);
            CHECK(n_host >= 1);
            CHECK(host_parse(host[0].b, host[0].n, &h) && h.cmd == HOST_SENSOR_LIST_ITEM);
            CHECK(!memcmp(h.payload, expected[received - GW_MAX_UNACKED], 8));
            n_host = 0;
        }
        gw_host(&gw, p, host_build_ack(p, HOST_SENSOR_LIST_ITEM, false));
        for (size_t i = 0; i < n_host; i++) {
            if (host_parse(host[i].b, host[i].n, &h) && h.cmd == HOST_SENSOR_LIST_ITEM) {
                CHECK(h.len == 8 && !memcmp(h.payload, expected[received], 8));
                received++;
            }
        }
    }
    // Drain the final 4 in-flight unacked slots with ACKs.
    for (int i = 0; i < GW_MAX_UNACKED; i++) {
        gw_host(&gw, p, host_build_ack(p, HOST_SENSOR_LIST_ITEM, false));
    }
    CHECK(received == GW_MAX_SENSORS && !gw.list_active);
    for (int i = 0; i < GW_MAX_UNACKED; i++) {
        CHECK(!gw.unacked[i].used);
    }
}

static void test_ows_build_info_pages_and_unacked64(void)
{
    gateway_t gw;
    build_info_t info;
    uint8_t p[HOST_MAX_PACKET], req[2];
    host_pkt_t h;

    memset(&info, 0, sizeof info);
    info.magic = BUILD_INFO_MAGIC;
    info.format = BUILD_INFO_FORMAT;
    info.flags = BUILD_INFO_TX;
    memcpy(info.id, "\x11\x22\x33\x44\x55\x66\x77\x88", 8);
    snprintf(info.product, sizeof info.product, "gateway-dongle");
    snprintf(info.board, sizeof info.board, "Wyze Sense USB dongle");
    snprintf(info.app, sizeof info.app, "gateway");
    snprintf(info.version, sizeof info.version, "v0.9.0-123-gdeadbeefcafe-dirty-longver!");

    gw_init(&gw, PAN, SHORT, EXT, (const uint8_t *)"778B5F63", &io);
    gw.build_info = &info;
    gw.build_info_len = sizeof info;
    gw_tick(&gw, 1000);

    // Page 0: <= 56 B CH554-safe, <= 63 B frame, tracked in gw.unacked[].pkt[64] and retried until ACKed.
    n_host = 0;
    req[0] = OWS_SUB_BUILD_INFO;
    req[1] = 0;
    gw_host(&gw, p, host_build(p, HOST_OWS_CMD, req, sizeof req, false));
    CHECK(n_host == 2 && host[1].n <= 63);
    CHECK(host_parse(host[1].b, host[1].n, &h) && h.cmd == HOST_OWS_RSP && h.len == OWS_BUILD_INFO_P0_LEN &&
          h.len <= OWS_MAX_PAYLOAD);
    CHECK(h.payload[0] == OWS_SUB_BUILD_INFO && !memcmp(h.payload + 1, &info, 40) &&
          !memcmp(h.payload + 41, info.app, 12));
    CHECK(gw.unacked[0].used && gw.unacked[0].len == host[1].n);

    // Verify retry before ACK, then silence after HOST_ACK for 0x53F1.
    n_host = 0;
    gw_tick(&gw, 1200);
    CHECK(n_host == 1 && host[0].n == gw.unacked[0].len);
    gw_host(&gw, p, host_build_ack(p, HOST_OWS_RSP, false));
    CHECK(!gw.unacked[0].used);
    n_host = 0;
    gw_tick(&gw, 1500);
    CHECK(n_host == 0);

    // Page 1: <= 56 B CH554-safe, <= 63 B frame, carries id[8] + board[24] + version[0..21].
    char ver_full[40];
    memset(ver_full, 0xAA, sizeof ver_full);
    n_host = 0;
    req[1] = 1;
    gw_host(&gw, p, host_build(p, HOST_OWS_CMD, req, sizeof req, false));
    CHECK(n_host == 2 && host[1].n <= 63);
    CHECK(host_parse(host[1].b, host[1].n, &h) && h.cmd == HOST_OWS_RSP && h.len == OWS_BUILD_INFO_P1_LEN &&
          h.len <= OWS_MAX_PAYLOAD);
    CHECK(h.payload[0] == OWS_SUB_BUILD_INFO && !memcmp(h.payload + 1, info.id, 8) &&
          !memcmp(h.payload + 9, info.board, 24) && !memcmp(h.payload + 33, info.version, 22));
    memcpy(ver_full, h.payload + 33, 22);
    gw_host(&gw, p, host_build_ack(p, HOST_OWS_RSP, false));

    // Page 2: <= 56 B CH554-safe (27 B), carries id[8] + version[22..39] (lossless 40-byte version).
    n_host = 0;
    req[1] = 2;
    gw_host(&gw, p, host_build(p, HOST_OWS_CMD, req, sizeof req, false));
    CHECK(n_host == 2 && host[1].n <= 63);
    CHECK(host_parse(host[1].b, host[1].n, &h) && h.cmd == HOST_OWS_RSP && h.len == OWS_BUILD_INFO_P2_LEN &&
          h.len <= OWS_MAX_PAYLOAD);
    CHECK(h.payload[0] == OWS_SUB_BUILD_INFO && !memcmp(h.payload + 1, info.id, 8) &&
          !memcmp(h.payload + 9, info.version + 22, 18));
    memcpy(ver_full + 22, h.payload + 9, 18);
    CHECK(strlen(info.version) == 39 && !memcmp(ver_full, info.version, 40));
    gw_host(&gw, p, host_build_ack(p, HOST_OWS_RSP, false));

    // Out-of-range page 3 replies with empty [0x02].
    n_host = 0;
    req[1] = 3;
    gw_host(&gw, p, host_build(p, HOST_OWS_CMD, req, sizeof req, false));
    CHECK(n_host == 2 && host_parse(host[1].b, host[1].n, &h) && h.cmd == HOST_OWS_RSP && h.len == 1 &&
          h.payload[0] == OWS_SUB_BUILD_INFO);
    gw_host(&gw, p, host_build_ack(p, HOST_OWS_RSP, false));

    // Legacy 0x53F0 flags compatibility (empty query & 0x00-prefixed 4-byte set) for older hosts.
    n_host = 0;
    gw_host(&gw, p, host_build(p, HOST_OWS_FLAGS, (const uint8_t *)"\x00\x00\x00\x05", 4, false));
    CHECK(n_host == 2 && host_parse(host[1].b, host[1].n, &h) && h.cmd == HOST_OWS_RSP && h.len == 8 &&
          h.payload[3] == 0x05);
    gw_host(&gw, p, host_build_ack(p, HOST_OWS_RSP, false));
    n_host = 0;
    gw_host(&gw, p, host_build(p, HOST_OWS_FLAGS, NULL, 0, false));
    CHECK(n_host == 2 && host_parse(host[1].b, host[1].n, &h) && h.cmd == HOST_OWS_RSP && h.len == 8 &&
          h.payload[3] == 0x05);
}

typedef struct {
    bool can_enter;
    unsigned entered;
} mock_sbl_t;

static bool mock_sbl(void *ctx, bool enter)
{
    mock_sbl_t *m = ctx;
    if (!m->can_enter) {
        return false;
    }
    if (enter) {
        m->entered++;
    }
    return true;
}

static void test_software_sbl_entry(void)
{
    gateway_t gw;
    mock_sbl_t m = {true, 0};
    gw_io_t sio = {on_tx, on_host, NULL, &m, mock_sbl};
    uint8_t p[64];
    host_pkt_t h;
    static const uint8_t enter_req[5] = {OWS_SUB_ENTER_SBL, 'O', 'S', 'B', 'L'};
    static const uint8_t bad_req[5] = {OWS_SUB_ENTER_SBL, 'N', 'O', 'P', 'E'};

    gw_init(&gw, PAN, SHORT, EXT, (const uint8_t *)"778B5F63", &sio);

    // 1. Bad confirmation magic: refused with 0xE0, never jumps.
    n_host = 0;
    gw_host(&gw, p, host_build(p, HOST_OWS_CMD, bad_req, sizeof bad_req, false));
    CHECK(n_host == 2 && host_parse(host[1].b, host[1].n, &h) && h.cmd == HOST_OWS_RSP && h.len == 2 &&
          h.payload[0] == OWS_SUB_ENTER_SBL && h.payload[1] == 0xE0 && m.entered == 0);
    gw_host(&gw, p, host_build_ack(p, HOST_OWS_RSP, false));

    // 2. Wyze USB dongle (CH554 bridge): OWS_SUB_ENTER_SBL refused with 0xE0; 0x4312 answers 0x4313 without SW jump.
    gw.usb_hid = true;
    n_host = 0;
    gw_host(&gw, p, host_build(p, HOST_OWS_CMD, enter_req, sizeof enter_req, false));
    CHECK(n_host == 2 && host_parse(host[1].b, host[1].n, &h) && h.cmd == HOST_OWS_RSP && h.len == 2 &&
          h.payload[0] == OWS_SUB_ENTER_SBL && h.payload[1] == 0xE0 && m.entered == 0);
    gw_host(&gw, p, host_build_ack(p, HOST_OWS_RSP, false));
    n_host = 0;
    gw_host(&gw, p, host_build(p, HOST_UPDATE_MODE, NULL, 0, false));
    CHECK(n_host == 1 && host_parse(host[0].b, host[0].n, &h) && h.cmd == HOST_UPDATE_MODE + 1 && m.entered == 0);

    // 3. Direct-UART gateway with CCFG bootloader disabled: refused with 0xE0.
    gw.usb_hid = false;
    m.can_enter = false;
    n_host = 0;
    gw_host(&gw, p, host_build(p, HOST_OWS_CMD, enter_req, sizeof enter_req, false));
    CHECK(n_host == 2 && host_parse(host[1].b, host[1].n, &h) && h.payload[1] == 0xE0 && m.entered == 0);
    gw_host(&gw, p, host_build_ack(p, HOST_OWS_RSP, false));

    // 4. Direct-UART gateway with CCFG bootloader enabled: OWS_SUB_ENTER_SBL replies 0x00 and jumps; 0x4312 jumps too.
    m.can_enter = true;
    n_host = 0;
    gw_host(&gw, p, host_build(p, HOST_OWS_CMD, enter_req, sizeof enter_req, false));
    CHECK(n_host == 2 && host_parse(host[1].b, host[1].n, &h) && h.payload[0] == OWS_SUB_ENTER_SBL &&
          h.payload[1] == 0x00 && m.entered == 1);
    n_host = 0;
    gw_host(&gw, p, host_build(p, HOST_UPDATE_MODE, NULL, 0, false));
    CHECK(n_host == 1 && host_parse(host[0].b, host[0].n, &h) && h.cmd == HOST_UPDATE_MODE + 1 && m.entered == 2);
}

static bool mem_nv_prog(void *ctx, uint32_t off, const uint8_t *d, size_t len)
{
    uint8_t *page = ctx;
    if (off + len > NV_PAGE_SIZE) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if ((page[off + i] & d[i]) != d[i]) {
            return false;
        }
        page[off + i] &= d[i];
    }
    return true;
}

static bool mem_nv_erase(void *ctx)
{
    memset(ctx, 0xFF, NV_PAGE_SIZE);
    return true;
}

static void call_ows(gateway_t *gw, const uint8_t *req, size_t req_len, host_pkt_t *rsp)
{
    static const uint8_t empty[32];
    uint8_t p[HOST_MAX_PACKET];
    *rsp = (host_pkt_t){.payload = empty};
    n_host = 0;
    gw_host(gw, p, host_build(p, HOST_OWS_CMD, req, req_len, false));
    CHECK(n_host == 2 && host_parse(host[1].b, host[1].n, rsp) && rsp->cmd == HOST_OWS_RSP);
    if (!rsp->payload) {
        rsp->payload = empty;
    }
    gw_host(gw, p, host_build_ack(p, HOST_OWS_RSP, false));
}

static void test_network_and_nv_import(void)
{
    uint8_t page[NV_PAGE_SIZE];
    memset(page, 0xFF, sizeof page);
    nvstore_t nv = {page, mem_nv_prog, mem_nv_erase, page, 0};
    CHECK(nv_open(&nv));

    gateway_t gw;
    gw_init(&gw, 0x1111, 0x2222, 0x00124B0001020304ull, (const uint8_t *)"778B5F63", &io);
    gw_load(&gw, &nv);
    CHECK(gw_save_network(&gw));

    // 1. OWS_SUB_NETWORK query returns [0x04, 0x00, pan_le16, short_le16, ext_le64] (14 B <= 56 B CH554 ceiling).
    host_pkt_t h;
    uint8_t q[1] = {OWS_SUB_NETWORK};
    call_ows(&gw, q, sizeof q, &h);
    CHECK(h.len == 14 && h.payload[0] == OWS_SUB_NETWORK && h.payload[1] == 0x00 && h.payload[2] == 0x11 &&
          h.payload[3] == 0x11 && h.payload[4] == 0x22 && h.payload[5] == 0x22);

    // 2. Rejects PAN 0x0000 and 0xFFFF, and reserved coordinator short >= 0xFFFE (any other PAN is fine, #70).
    static const uint16_t bad_pans[] = {0x0000, 0xFFFF};
    for (size_t i = 0; i < sizeof bad_pans / sizeof bad_pans[0]; i++) {
        uint8_t bad[13] = {OWS_SUB_NETWORK, (uint8_t)bad_pans[i], (uint8_t)(bad_pans[i] >> 8), 0x34, 0x12};
        call_ows(&gw, bad, sizeof bad, &h);
        CHECK(h.len == 14 && h.payload[1] == 0xE0 && gw.pan == 0x1111);
    }
    uint8_t bad_short[13] = {OWS_SUB_NETWORK, 0x34, 0x12, 0xFE, 0xFF};
    call_ows(&gw, bad_short, sizeof bad_short, &h);
    CHECK(h.len == 14 && h.payload[1] == 0xE0 && gw.pan == 0x1111);

    // 3. Valid OWS_SUB_NETWORK updates coordinator identity in RAM + Page 30 OWNV and clears scan/pending state.
    gw.permit = true;
    uint8_t set_net[13] = {OWS_SUB_NETWORK, 0x34, 0x12, 0x00, 0x00, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11};
    call_ows(&gw, set_net, sizeof set_net, &h);
    CHECK(h.len == 14 && h.payload[1] == 0x00 && gw.pan == 0x1234 && gw.short_addr == 0x0000 &&
          gw.ext == 0x1122334455667788ull && !gw.permit);

    // 4. OWS_SUB_NV_IMPORT: CLEAR, SET_NET, ADD_SENSOR (with lowercase normalization & sub-ID), GET_SENSOR, and reload.
    CHECK(gw_add_sensor(&gw, (const uint8_t *)"77OLD001", 0x0009));
    uint8_t clr[2] = {OWS_SUB_NV_IMPORT, OWS_NV_CLEAR};
    call_ows(&gw, clr, sizeof clr, &h);
    CHECK(h.len == 3 && h.payload[1] == OWS_NV_CLEAR && h.payload[2] == 0x00 && gw.n_sensors == 0);

    uint8_t imp_net[14] = {
        OWS_SUB_NV_IMPORT, OWS_NV_SET_NET, 0x42, 0x42, 0x10, 0x00, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x12, 0x00};
    call_ows(&gw, imp_net, sizeof imp_net, &h);
    CHECK(h.len == 3 && h.payload[1] == OWS_NV_SET_NET && h.payload[2] == 0x00 && gw.pan == 0x4242 &&
          gw.short_addr == 0x0010);

    // Add two sensors out of sub-ID order (sub 5 then sub 2); table sorts by sub-ID and rejects duplicates/zeros.
    uint8_t add5[15] = {OWS_SUB_NV_IMPORT,
                        OWS_NV_ADD_SENSOR,
                        '7',
                        '7',
                        'a',
                        'b',
                        'c',
                        'd',
                        'e',
                        'f',
                        0x05,
                        0x00,
                        SENSOR_CONTACT_V2,
                        0x05,
                        0x00};
    call_ows(&gw, add5, sizeof add5, &h);
    CHECK(h.len == 4 && h.payload[2] == 0x00 && h.payload[3] == 1);

    uint8_t add2[15] = {OWS_SUB_NV_IMPORT,
                        OWS_NV_ADD_SENSOR,
                        '7',
                        '7',
                        '1',
                        '2',
                        '3',
                        '4',
                        '5',
                        '6',
                        0x02,
                        0x00,
                        SENSOR_MOTION_V1,
                        0x02,
                        0x00};
    call_ows(&gw, add2, sizeof add2, &h);
    CHECK(h.len == 4 && h.payload[2] == 0x00 && h.payload[3] == 2);
    CHECK(!memcmp(gw.sensors[0].mac, "77123456", 8) && gw.sensors[0].nv_sub == 2);
    CHECK(!memcmp(gw.sensors[1].mac, "77ABCDEF", 8) && gw.sensors[1].nv_sub == 5);

    // Reject duplicate short_addr, coordinator short_addr, and all-zero MAC.
    uint8_t dup_short[13] = {
        OWS_SUB_NV_IMPORT, OWS_NV_ADD_SENSOR, '7', '7', '9', '9', '9', '9', '9', '9', 0x05, 0x00, 0};
    call_ows(&gw, dup_short, sizeof dup_short, &h);
    CHECK(h.payload[2] == 0xE0 && gw.n_sensors == 2);
    dup_short[10] = 0x10;  // coordinator's short_addr
    call_ows(&gw, dup_short, sizeof dup_short, &h);
    CHECK(h.payload[2] == 0xE0 && gw.n_sensors == 2);
    uint8_t zero_mac[13] = {
        OWS_SUB_NV_IMPORT, OWS_NV_ADD_SENSOR, '0', '0', '0', '0', '0', '0', '0', '0', 0x09, 0x00, 0};
    call_ows(&gw, zero_mac, sizeof zero_mac, &h);
    CHECK(h.payload[2] == 0xE0 && gw.n_sensors == 2);

    // GET_SENSOR exports sub-ID, uppercase MAC, short_addr, and learned/imported type within <= 18 B payload.
    uint8_t get1[3] = {OWS_SUB_NV_IMPORT, OWS_NV_GET_SENSOR, 1};
    call_ows(&gw, get1, sizeof get1, &h);
    CHECK(h.len == 18 && h.payload[1] == OWS_NV_GET_SENSOR && h.payload[2] == 0x00 && h.payload[3] == 2 &&
          h.payload[4] == 1 && h.payload[5] == 5 && !memcmp(h.payload + 7, "77ABCDEF", 8) && h.payload[15] == 0x05 &&
          h.payload[17] == SENSOR_CONTACT_V2);

    // Reload from Page 30 OWNV into a fresh gateway_t: network + sorted sensor table survive intact.
    gateway_t reloaded;
    nvstore_t nv2 = {page, mem_nv_prog, mem_nv_erase, page, 0};
    CHECK(nv_open(&nv2));
    gw_init(&reloaded, 0x9999, 0x8888, 0, (const uint8_t *)"778B5F63", &io);
    CHECK(gw_load(&reloaded, &nv2));
    CHECK(reloaded.pan == 0x4242 && reloaded.short_addr == 0x0010 && reloaded.ext == 0x0012FFEEDDCCBBAAull);
    CHECK(reloaded.n_sensors == 2 && !memcmp(reloaded.sensors[0].mac, "77123456", 8) &&
          !memcmp(reloaded.sensors[1].mac, "77ABCDEF", 8));
}

static void put_be64_test(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++) {
        p[i] = (uint8_t)(v >> (56 - 8 * i));
    }
}

static void test_keypad_alarm2(void)
{
    gateway_t gw;
    uint8_t pt[32] = {0}, ct[32], f[64];
    host_pkt_t h = {0};
    gw_init(&gw, PAN, SHORT, EXT, (const uint8_t *)"778B5F63", &io);
    gw_add_sensor(&gw, (const uint8_t *)"77BD567E", 0x0006);

    // Captured stock host frame: ea 77BD567E 05 08 13 74 00 02 01 00 00 01,
    // with -RSSI (0x1f) appended by the dongle.
    pt[0] = APP_ALARM2;
    memcpy(pt + 1, "77BD567E", 8);
    pt[9] = 0x05;   // keypad
    pt[10] = 0x08;  // payload length
    pt[11] = 0x13;
    pt[12] = 0x74;  // battery
    pt[13] = 0x00;
    pt[14] = 0x02;  // button
    pt[15] = 0x01;  // disarm
    pt[16] = 0x00;
    pt[17] = 0x00;
    pt[18] = 0x01;  // sequence
    CHECK(app_encrypt(pt, 19, ct) == 32);

    n_tx = n_host = 0;
    gw_rx(&gw, f, mac_build_data(f, 0x11, PAN, SHORT, 0x0006, ct, 32, true), -31);
    CHECK(n_tx == 1 && tx[0].n == 7);
    CHECK(n_host == 1 && host_parse(host[0].b, host[0].n, &h) && h.cmd == HOST_ALARM2 && h.len == 20);
    if (!h.payload || h.len < 20) {
        return;
    }
    CHECK(h.payload[0] == APP_ALARM2 && !memcmp(h.payload + 1, "77BD567E", 8));
    CHECK(h.payload[9] == 0x05 && h.payload[10] == 0x08 && h.payload[19] == 0x1f);

    // A truncated plaintext must not be forwarded.
    n_host = 0;
    gw_rx(&gw, f, mac_build_data(f, 0x12, PAN, SHORT, 0x0006, ct, 16, true), -31);
    CHECK(n_host == 0);
}

static void test_keypad_reply(void)
{
    gateway_t gw;
    uint8_t pkt[HOST_MAX_PACKET], pt[16];
    host_pkt_t h = {0};
    gw_init(&gw, PAN, SHORT, EXT, (const uint8_t *)"778B5F63", &io);
    gw_add_sensor(&gw, (const uint8_t *)"77BD567E", 0x0006);

    // Host 0x5353: MAC(8), 0xE9, n, data. Queue the reply for the keypad's next poll.
    uint8_t payload[14] = {'7', '7', 'B', 'D', '5', '6', '7', 'E', 0xE9, 0x04, 0x03, 0x01, 0x04, 0x00};
    size_t n = host_build(pkt, HOST_SEND_KEYPAD, payload, sizeof payload, false);
    n_host = 0;
    gw_host(&gw, pkt, n);
    CHECK(n_host == 1 && host_parse(host[0].b, host[0].n, &h) && h.cmd == HOST_ACK && h.acked == HOST_SEND_KEYPAD);

    uint8_t dreq[10] = {0x63, 0x88, 0x12, PAN & 0xFF, PAN >> 8, SHORT & 0xFF, SHORT >> 8, 0x06, 0x00, MAC_CMD_DATA_REQ};
    n_tx = 0;
    gw_rx(&gw, dreq, sizeof dreq, -50);
    CHECK(n_tx == 2);
    CHECK(tx[1].n == 29);  // 9-byte MAC header + 16-byte ciphertext + 4-byte FCS
    aes128_ecb_decrypt(app_key, tx[1].b + 9, pt, 16);
    CHECK(pt[0] == 0xE9 && pt[1] == 0x04 && pt[2] == 0x03 && pt[3] == 0x01 && pt[4] == 0x04 && pt[5] == 0x00);
}

static void test_history_log_ad(void)
{
    gateway_t gw;
    uint8_t pt[32], ct[32], f[64], ack[16];
    host_pkt_t h = {0};
    gw_init(&gw, PAN, SHORT, EXT, (const uint8_t *)"778B5F63", &io);
    gw_add_sensor(&gw, (const uint8_t *)"777B42DF", 0x0006);
    gw.clock_ms = 1792718500000ull;

    // 1. Valid stock v1 contact_v1 0xAD record (22 B padded to 32 B AES):
    //    [0]=0xAD, [1..8]=MAC, [9]=type(0x01), [10..17]=ts_be64, [18]=0xA2, [19]=state(1), [20..21]=counter(0x01A4)
    memset(pt, 0, sizeof pt);
    pt[0] = APP_HISTORY_LOG;
    memcpy(pt + 1, "777B42DF", 8);
    pt[9] = SENSOR_CONTACT_V1;
    put_be64_test(pt + 10, 1792718000000ull);
    pt[18] = APP_EVENT;
    pt[19] = 1;
    pt[20] = 0x01;
    pt[21] = 0xA4;
    CHECK(app_encrypt(pt, 22, ct) == 32);

    n_tx = n_host = 0;
    gw_rx(&gw, f, mac_build_data(f, 0x11, PAN, SHORT, 0x0006, ct, 32, true), -60);
    CHECK(n_tx == 1 && tx[0].n == 7 && !memcmp(tx[0].b, "\x02\x00\x11", 3));
    CHECK(n_host == 1 && host_parse(host[0].b, host[0].n, &h) && h.cmd == HOST_ALARM && h.len == 26);
    if (!h.payload || h.len < 26) {
        return;
    }

    uint64_t got_ts = 0;
    for (int i = 0; i < 8; i++) {
        got_ts = got_ts << 8 | h.payload[i];
    }
    CHECK(got_ts == 1792718000000ull);
    CHECK(h.payload[8] == APP_EVENT && !memcmp(h.payload + 9, "777B42DF", 8) && h.payload[17] == SENSOR_CONTACT_V1);
    CHECK(h.payload[18] == 0 && h.payload[19] == 0 && h.payload[20] == 0 && h.payload[21] == 1);
    CHECK(h.payload[22] == 1 && h.payload[23] == 0x01 && h.payload[24] == 0xA4 && h.payload[25] == 60);
    gw_host(&gw, ack, host_build_ack(ack, HOST_ALARM, false));

    // Type was learned from 0xAD: a subsequent DATA_REQ poll receives 0xB0 APP_TIME_SYNC.
    uint8_t dreq[10] = {0x63, 0x88, 0x12, PAN & 0xFF, PAN >> 8, SHORT & 0xFF, SHORT >> 8, 0x06, 0x00, MAC_CMD_DATA_REQ};
    n_tx = 0;
    gw_rx(&gw, dreq, sizeof dreq, -60);
    CHECK(n_tx == 2 && (tx[0].b[0] & 0x10) != 0);

    // 2. Unsynced sensor clock (ts=0) + consecutive tiebreaker + dedup.
    uint8_t fl[4] = {0, 0, 0, GW_FLAG_DEDUP};
    CHECK(flags_call(&gw, fl, NULL) == GW_FLAG_DEDUP);
    gw_host(&gw, ack, host_build_ack(ack, HOST_OWS_RSP, false));

    memset(pt, 0, sizeof pt);
    pt[0] = APP_HISTORY_LOG;
    memcpy(pt + 1, "777B42DF", 8);
    pt[9] = SENSOR_CONTACT_V1;
    put_be64_test(pt + 10, 0);  // unsynced sensor clock -> rebased onto gw.clock_ms
    pt[18] = APP_EVENT;
    pt[19] = 0;
    pt[20] = 0x01;
    pt[21] = 0xA5;
    app_encrypt(pt, 22, ct);

    n_host = 0;
    h = (host_pkt_t){0};
    gw_tick(&gw, 5000);
    gw_rx(&gw, f, mac_build_data(f, 0x13, PAN, SHORT, 0x0006, ct, 32, true), -55);
    CHECK(n_host == 1 && host_parse(host[0].b, host[0].n, &h) && h.cmd == HOST_ALARM && h.len == 26);
    if (!h.payload || h.len < 26) {
        return;
    }
    got_ts = 0;
    for (int i = 0; i < 8; i++) {
        got_ts = got_ts << 8 | h.payload[i];
    }
    CHECK(got_ts == gw.clock_ms && !memcmp(h.payload + 9, "777B42DF", 8) && h.payload[22] == 0 &&
          h.payload[24] == 0xA5);
    gw_host(&gw, ack, host_build_ack(ack, HOST_ALARM, false));

    // Duplicate retry of the exact same 0xAD record within GW_DEDUP_MS is suppressed.
    n_host = 0;
    gw_tick(&gw, 5200);
    gw_rx(&gw, f, mac_build_data(f, 0x13, PAN, SHORT, 0x0006, ct, 32, true), -55);
    CHECK(n_host == 0);

    // Next distinct 0xAD record with the same unsynced timestamp gets tiebreaker gw.clock_ms + 1.
    pt[19] = 1;
    pt[21] = 0xA6;
    app_encrypt(pt, 22, ct);
    h = (host_pkt_t){0};
    gw_tick(&gw, 5400);
    gw_rx(&gw, f, mac_build_data(f, 0x14, PAN, SHORT, 0x0006, ct, 32, true), -55);
    CHECK(n_host == 1 && host_parse(host[0].b, host[0].n, &h) && h.cmd == HOST_ALARM && h.len == 26);
    if (!h.payload || h.len < 26) {
        return;
    }
    got_ts = 0;
    for (int i = 0; i < 8; i++) {
        got_ts = got_ts << 8 | h.payload[i];
    }
    CHECK(got_ts == gw.clock_ms + 1 && h.payload[22] == 1 && h.payload[24] == 0xA6);
    gw_host(&gw, ack, host_build_ack(ack, HOST_ALARM, false));

    // 3. Truncated 0xAD (16 B ciphertext < 22 B), bad inner opcode, and 0xB1 decomm ACK produce no alarms.
    n_host = 0;
    gw_rx(&gw, f, mac_build_data(f, 0x15, PAN, SHORT, 0x0006, ct, 16, true), -55);
    pt[18] = 0x99;
    app_encrypt(pt, 22, ct);
    gw_rx(&gw, f, mac_build_data(f, 0x16, PAN, SHORT, 0x0006, ct, 32, true), -55);
    memset(pt, 0, 16);
    pt[0] = APP_DECOMM_ACK;
    pt[1] = 0x01;
    app_encrypt(pt, 16, ct);
    gw_rx(&gw, f, mac_build_data(f, 0x17, PAN, SHORT, 0x0006, ct, 16, true), -55);
    CHECK(n_host == 0);
}

static void test_ota_queue_and_wildcard(void)
{
    gateway_t gw;
    uint8_t ack[16], offer[1 + 8 + OTA_OFFER_LEN], pt[16], ct[16], f[64];
    host_pkt_t h;
    gw_init(&gw, PAN, SHORT, EXT, (const uint8_t *)"778B5F63", &io);
    uint8_t fl[4] = {0, 0, 0, GW_FLAG_OTA};
    CHECK(flags_call(&gw, fl, NULL) == GW_FLAG_OTA);
    gw_host(&gw, ack, host_build_ack(ack, HOST_OWS_RSP, false));

    for (int i = 0; i < 9; i++) {
        uint8_t mac[8] = {'7', '7', '0', '0', '0', '0', '0', (uint8_t)('1' + i)};
        gw_add_sensor(&gw, mac, (uint16_t)(0x10 + i));
    }

    // 1. Stage up to GW_MAX_OTA_TARGETS (8) distinct sensors in one wave; 9th is refused until one clears.
    memset(offer, 0, sizeof offer);
    offer[0] = OWS_SUB_OTA_OFFER;
    ota_put_be32(offer + 9 + 0, 0x11223344u);  // board_id
    ota_put_be32(offer + 9 + 4, 15u);          // version
    ota_put_be32(offer + 9 + 8, 1024u);        // size_a
    ota_put_be32(offer + 9 + 16, 1024u);       // size_b
    for (int i = 0; i < 8; i++) {
        uint8_t mac[8] = {'7', '7', '0', '0', '0', '0', '0', (uint8_t)('1' + i)};
        memcpy(offer + 1, mac, 8);
        call_ows(&gw, offer, sizeof offer, &h);
        CHECK(h.len == 2 && h.payload[1] == 0x00);
        gw_host(&gw, ack, host_build_ack(ack, HOST_OWS_RSP, false));
    }
    memcpy(offer + 1, "77000009", 8);
    call_ows(&gw, offer, sizeof offer, &h);
    CHECK(h.len == 2 && h.payload[1] == OTA_HOST_REFUSED);
    gw_host(&gw, ack, host_build_ack(ack, HOST_OWS_RSP, false));

    // Cancel one target (size_a = 0) -> slot opens for 77000009 without disturbing other staged targets.
    uint8_t cancel[1 + 8 + OTA_OFFER_LEN] = {OWS_SUB_OTA_OFFER};
    memcpy(cancel + 1, "77000001", 8);
    call_ows(&gw, cancel, sizeof cancel, &h);
    CHECK(h.len == 2 && h.payload[1] == 0x00 && gw.ota.staged);
    gw_host(&gw, ack, host_build_ack(ack, HOST_OWS_RSP, false));

    call_ows(&gw, offer, sizeof offer, &h);
    CHECK(h.len == 2 && h.payload[1] == 0x00);
    gw_host(&gw, ack, host_build_ack(ack, HOST_OWS_RSP, false));

    // 2. Active sensor lock yields after GW_OTA_OFFER_MS (5 s) if offered sensor ignores C0.
    gw_tick(&gw, 1000);
    uint8_t dreq2[10] = {0x63, 0x88, 1, PAN & 0xFF, PAN >> 8, SHORT & 0xFF, SHORT >> 8, 0x11, 0x00, MAC_CMD_DATA_REQ};
    uint8_t dreq3[10] = {0x63, 0x88, 2, PAN & 0xFF, PAN >> 8, SHORT & 0xFF, SHORT >> 8, 0x12, 0x00, MAC_CMD_DATA_REQ};
    n_tx = 0;
    gw_rx(&gw, dreq2, sizeof dreq2, -50);  // 0x11 ("77000002") gets C0 offer
    CHECK(n_tx == 2 && (tx[0].b[0] & 0x10) != 0);
    n_tx = 0;
    gw_tick(&gw, 2000);
    gw_rx(&gw, dreq3, sizeof dreq3, -50);  // 1 s later: 0x11 still holds offer lock, 0x12 gets empty ACK
    CHECK(n_tx == 1 && (tx[0].b[0] & 0x10) == 0);
    n_tx = 0;
    gw_tick(&gw, 6500);  // > 5 s without C1 from 0x11: lock yields to 0x12
    gw_rx(&gw, dreq3, sizeof dreq3, -50);
    CHECK(n_tx == 2 && (tx[0].b[0] & 0x10) != 0);

    // 3. Wildcard "********" offers to any paired sensor once per wave.
    memcpy(offer + 1, "********", 8);
    ota_put_be32(offer + 9 + 4, 16u);  // new version resets wave
    call_ows(&gw, offer, sizeof offer, &h);
    CHECK(h.len == 2 && h.payload[1] == 0x00 && gw.ota.wildcard);
    gw_host(&gw, ack, host_build_ack(ack, HOST_OWS_RSP, false));

    uint8_t dreq1[10] = {0x63, 0x88, 3, PAN & 0xFF, PAN >> 8, SHORT & 0xFF, SHORT >> 8, 0x10, 0x00, MAC_CMD_DATA_REQ};
    n_tx = 0;
    gw_rx(&gw, dreq1, sizeof dreq1, -50);
    CHECK(n_tx == 2 && (tx[0].b[0] & 0x10) != 0);

    // Sensor 0x10 ("77000001") finishes with C3 -> marked in wildcard_done; subsequent poll gets no second C0,
    // while sensor 0x11 ("77000002") immediately gets C0!
    memset(pt, 0, sizeof pt);
    pt[0] = APP_OTA_DONE;
    memcpy(pt + 1, "77000001", 8);
    pt[9] = OTA_DONE_OK;
    ota_put_be32(pt + 10, 16u);
    app_encrypt(pt, 14, ct);
    n_host = 0;
    gw_rx(&gw, f, mac_build_data(f, 4, PAN, SHORT, 0x10, ct, 16, true), -50);
    CHECK(n_host == 1 && host_parse(host[0].b, host[0].n, &h) && h.cmd == HOST_OWS_RSP &&
          h.payload[0] == OWS_SUB_OTA_DONE && !memcmp(h.payload + 1, "77000001", 8));
    gw_host(&gw, ack, host_build_ack(ack, HOST_OWS_RSP, false));

    n_tx = 0;
    gw_rx(&gw, dreq1, sizeof dreq1, -50);
    CHECK(n_tx == 1 && (tx[0].b[0] & 0x10) == 0);
    n_tx = 0;
    gw_rx(&gw, dreq2, sizeof dreq2, -50);
    CHECK(n_tx == 2 && (tx[0].b[0] & 0x10) != 0);

    // Sensor 0x11 ("77000002", idx 1) refuses -> marked in wildcard_skip; verify wildcard_done & wildcard_skip shift
    // cleanly when OWS_NV_ADD_SENSOR inserts at idx 0 and when HOST_DELETE_SENSOR removes an earlier entry.
    for (size_t i = 0; i < gw.n_sensors; i++) {
        gw.sensors[i].nv_sub = (uint16_t)((i + 1) * 2);  // sub 2, 4, 6, ... so sub 1 inserts at index 0
    }
    memset(pt, 0, sizeof pt);
    pt[0] = APP_OTA_DONE;
    memcpy(pt + 1, "77000002", 8);
    pt[9] = OTA_DONE_REFUSED;
    ota_put_be32(pt + 10, 15u);
    app_encrypt(pt, 14, ct);
    gw_rx(&gw, f, mac_build_data(f, 5, PAN, SHORT, 0x11, ct, 16, true), -50);
    gw_host(&gw, ack, host_build_ack(ack, HOST_OWS_RSP, false));
    CHECK(gw.ota.wildcard_done == (1ull << 0) && gw.ota.wildcard_skip == (1ull << 1));

    // Insert "77000000" (short 0x09, sub 1) at index 0 via OWS_NV_ADD_SENSOR: bits shift to idx 1 and idx 2.
    uint8_t add0[15] = {OWS_SUB_NV_IMPORT,
                        OWS_NV_ADD_SENSOR,
                        '7',
                        '7',
                        '0',
                        '0',
                        '0',
                        '0',
                        '0',
                        '0',
                        0x09,
                        0x00,
                        SENSOR_CONTACT_V1,
                        0x01,
                        0x00};
    call_ows(&gw, add0, sizeof add0, &h);
    CHECK(h.payload[2] == 0x00 && !memcmp(gw.sensors[0].mac, "77000000", 8));
    CHECK(gw.ota.wildcard_done == (1ull << 1) && gw.ota.wildcard_skip == (1ull << 2));

    // Delete "77000001" (now at index 1): its done bit is removed, and "77000002"'s skip bit shifts back to index 1.
    uint8_t del_pkt[HOST_MAX_PACKET];
    gw_host(&gw, del_pkt, host_build(del_pkt, HOST_DELETE_SENSOR, (const uint8_t *)"77000001", 8, false));
    gw_host(&gw, ack, host_build_ack(ack, HOST_DELETE_SENSOR + 1, false));
    CHECK(gw.ota.wildcard_done == 0 && gw.ota.wildcard_skip == (1ull << 1));
    n_tx = 0;
    gw_rx(&gw, dreq2, sizeof dreq2, -50);  // "77000002" still suppressed by wildcard_skip!
    CHECK(n_tx == 1 && (tx[0].b[0] & 0x10) == 0);
    uint8_t dreq0[10] = {0x63, 0x88, 6, PAN & 0xFF, PAN >> 8, SHORT & 0xFF, SHORT >> 8, 0x09, 0x00, MAC_CMD_DATA_REQ};
    n_tx = 0;
    gw_rx(&gw, dreq0, sizeof dreq0, -50);  // newly inserted "77000000" at idx 0 gets C0 offer!
    CHECK(n_tx == 2 && (tx[0].b[0] & 0x10) != 0);
}

// #84: two of our gateways side by side answered every beacon request at the same instant and collided, so a sensor
// never saw the farther one's permit. Now the beacon waits: 5-20 ms when pairing, 75-125 ms when not.
static uint32_t beacon_delay(gateway_t *gw, uint32_t now, int8_t rssi)
{
    static const uint8_t req[] = {0x03, 0x08, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, MAC_CMD_BEACON_REQ};
    gw_tick(gw, now);
    n_tx = 0;
    gw_rx(gw, req, sizeof req, rssi);
    CHECK(n_tx == 0 && gw->beacon_due);  // nothing at once
    uint32_t t = now;
    for (; n_tx == 0 && t < now + 1000; t++) {
        gw_tick(gw, t);
    }
    CHECK(n_tx == 1 && (tx[0].b[0] & 7) == MAC_BEACON && !gw->beacon_due);
    CHECK(((tx[0].b[8] & 0x80) != 0) == gw->permit);  // the permit bit as it is when sent
    return t - 1 - now;
}

static void test_beacon_delay(void)
{
    static gateway_t a, b;
    uint32_t now = 1000;
    int same = 0;
    gw_init(&a, 0x65FC, 0x0F0C, 0x00124B0019B9F882ull, (const uint8_t *)"7788CCF6", &io);
    gw_init(&b, 0x65FE, 0xD425, 0x00124B00204F48FDull, (const uint8_t *)"77A85A36", &io);
    for (int i = 0; i < 200; i++, now += 2000) {
        a.permit = true, b.permit = false;
        uint32_t da = beacon_delay(&a, now, (int8_t)(-40 - i % 30));
        uint32_t db = beacon_delay(&b, now, (int8_t)(-60 - i % 20));
        CHECK(da >= 5 && da <= 20);
        CHECK(db >= 75 && db <= 125);
        CHECK(da + 45 < db);  // the pairing gateway's beacon (~45 ms on air) has ended before the other starts
        a.permit = b.permit = false;
        same += beacon_delay(&a, now + 1000, -50) == beacon_delay(&b, now + 1000, -50);
    }
    CHECK(same < 20);  // two gateways do not keep choosing the same moment

    // more requests while one is due: one beacon
    static const uint8_t req[] = {0x03, 0x08, 0x02, 0xFF, 0xFF, 0xFF, 0xFF, MAC_CMD_BEACON_REQ};
    a.permit = true;
    gw_tick(&a, now);
    n_tx = 0;
    gw_rx(&a, req, sizeof req, -50);
    gw_rx(&a, req, sizeof req, -50);
    for (uint32_t t = now; t < now + 300; t++) {
        gw_tick(&a, t);
    }
    CHECK(n_tx == 1);
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s gateway_replay.txt\n", argv[0]);
        return 2;
    }
    test_replay(argv[1]);
    test_host();
    test_host_resends();
    test_time_requests();
    test_update_mode_reply();
    test_flags();
    test_dedup();
    test_only_paired_sensors();
    test_zero_mac_never_pairs();
    test_no_shared_address();
    test_pending_queue();
    test_truncated_payloads();
    test_joining_and_short_addr_guards();
    test_sensor_list_64_streaming();
    test_ows_build_info_pages_and_unacked64();
    test_software_sbl_entry();
    test_network_and_nv_import();
    test_history_log_ad();
    test_keypad_alarm2();
    test_keypad_reply();
    test_ota_queue_and_wildcard();
    test_beacon_delay();
    printf("%d checks, %d failed\n", checks, failures);
    return failures != 0;
}
