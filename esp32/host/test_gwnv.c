// Native tests of esp32/contact/gwnv.c against firmware/common/gateway.c + nvstore.c (TASK-14).
#include <stdio.h>
#include <string.h>

#include "gateway.h"
#include "gwnv.h"
#include "gwsync.h"
#include "host_proto.h"
#include "nvstore.h"
#include "wyze_app.h"

static int checks, failures;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        checks++;                                                                    \
        if (!(cond)) {                                                               \
            failures++;                                                              \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                            \
    } while (0)

typedef struct {
    gateway_t gw;
    host_rx_t to_gw;
    uint8_t rx_buf[4096];
    size_t rx_head, rx_tail;
    unsigned drop_writes;
    unsigned acks_5319;
} harness_t;

static void noop_tx(void *ctx, const uint8_t *frame, size_t len, uint32_t gap_us)
{
    (void)ctx;
    (void)frame;
    (void)len;
    (void)gap_us;
}

static void gw_to_esp(void *ctx, const uint8_t *pkt, size_t len)
{
    harness_t *h = ctx;
    for (size_t i = 0; i < len && h->rx_tail - h->rx_head < sizeof h->rx_buf; i++) {
        h->rx_buf[h->rx_tail++ % sizeof h->rx_buf] = pkt[i];
    }
}

static bool esp_write(void *ctx, const uint8_t *data, size_t len)
{
    harness_t *h = ctx;
    if (h->drop_writes) {
        h->drop_writes--;
        return true;  // simulate lost UART packet to exercise gwnv retry loop
    }
    for (size_t i = 0; i < len; i++) {
        host_pkt_t p;
        if (!host_rx_feed(&h->to_gw, data[i], &p)) {
            continue;
        }
        if (p.cmd == HOST_ACK && p.acked == HOST_ALARM) {
            h->acks_5319++;
        }
        gw_host(&h->gw, p.payload - 5, p.len + 7);
    }
    return true;
}

static size_t esp_read(void *ctx, uint8_t *buf, size_t cap, uint32_t timeout_ms)
{
    (void)timeout_ms;
    harness_t *h = ctx;
    size_t n = 0;
    while (n < cap && h->rx_head < h->rx_tail) {
        buf[n++] = h->rx_buf[h->rx_head++ % sizeof h->rx_buf];
    }
    return n;
}

static void esp_flush(void *ctx)
{
    harness_t *h = ctx;
    h->rx_head = h->rx_tail = 0;
}

static bool mem_prog(void *ctx, uint32_t off, const uint8_t *d, size_t len)
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

static bool mem_erase(void *ctx)
{
    memset(ctx, 0xFF, NV_PAGE_SIZE);
    return true;
}

static void test_json_validation(void)
{
    static const char *pretty =
        "{\n"
        "  \"format\": \"openwyzesense-nv-json\",\n"
        "  \"version\": 1,\n"
        "  \"source\": { \"layout\": \"stock\", \"note\": \"escaped \\\"quote\\\" and \\u0041\" },\n"
        "  \"network\": { \"ext\": \"00124B001D90494A\", \"pan\": \"2357\", \"short\": \"C907\" },\n"
        "  \"sensors\": [\n"
        "    { \"sub\": 3, \"mac\": \"77abcdef\", \"short\": \"0004\", \"type\": \"0E\" },\n"
        "    { \"mac\": \"77123456\", \"short\": \"0009\" }\n"
        "  ],\n"
        "  \"join\": null,\n"
        "  \"dropped\": [ { \"what\": \"sensor 6\", \"why\": \"zero MAC\" } ],\n"
        "  \"unrecognised\": []\n"
        "}\n";
    gwnv_doc_t doc;
    char err[128] = "";
    CHECK(gwnv_parse_json(pretty, strlen(pretty), &doc, err, sizeof err));
    CHECK(doc.pan == 0x2357 && doc.short_addr == 0xC907 && doc.ext == 0x00124B001D90494Aull);
    CHECK(doc.n_sensors == 2 && doc.sensors[0].sub == 3 && !strcmp(doc.sensors[0].mac, "77ABCDEF") &&
          doc.sensors[0].type == SENSOR_CONTACT_V2);
    CHECK(doc.sensors[1].sub == 1 && !strcmp(doc.sensors[1].mac, "77123456") && doc.sensors[1].short_addr == 0x0009);

    // Reject PAN FFFF, duplicate short_addr, all-zero MAC, wrong version, and trailing garbage.
    static const char *bad[] = {
        "{\"format\":\"openwyzesense-nv-json\",\"version\":1,\"network\":{\"pan\":\"FFFF\",\"short\":\"0001\",\"ext\":"
        "\"1\"}}",
        "{\"format\":\"openwyzesense-nv-json\",\"version\":2,\"network\":{\"pan\":\"1234\",\"short\":\"0001\",\"ext\":"
        "\"1\"}}",
        "{\"format\":\"openwyzesense-nv-json\",\"version\":1,\"network\":{\"pan\":\"1234\",\"short\":\"0001\",\"ext\":"
        "\"1\"},\"sensors\":[{\"mac\":\"00000000\",\"short\":\"0002\"}]}",
        "{\"format\":\"openwyzesense-nv-json\",\"version\":1,\"network\":{\"pan\":\"1234\",\"short\":\"0001\",\"ext\":"
        "\"1\"},\"sensors\":[{\"sub\":0,\"mac\":\"77000001\",\"short\":\"0002\"},{\"sub\":1,\"mac\":\"77000002\","
        "\"short\":\"0002\"}]}",
        "{\"format\":\"openwyzesense-nv-json\",\"version\":1,\"network\":{\"pan\":\"1234\",\"short\":\"0001\",\"ext\":"
        "\"1\"}} trailing",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        CHECK(!gwnv_parse_json(bad[i], strlen(bad[i]), &doc, err, sizeof err) && err[0] != 0);
    }

    // Programmatic C callers passing lowercase MACs get normalized uppercase MACs and case-insensitive dup detection.
    gwnv_doc_t dup_doc = {
        .pan = 0x1234,
        .short_addr = 0x0001,
        .ext = 1,
        .n_sensors = 2,
        .sensors = {{.sub = 0, .mac = "77abcdef", .short_addr = 2}, {.sub = 1, .mac = "77AbCdEf", .short_addr = 3}},
    };
    gwnv_io_t dummy_io = {esp_write, esp_read, esp_flush, NULL};
    err[0] = '\0';
    CHECK(!gwnv_import(&dummy_io, &dup_doc, err, sizeof err) && strstr(err, "duplicate sensor MAC") != NULL);
}

static void test_64_sensor_import_export_roundtrip(void)
{
    static harness_t h;
    uint8_t page[NV_PAGE_SIZE];
    memset(&h, 0, sizeof h);
    memset(page, 0xFF, sizeof page);
    nvstore_t nv = {page, mem_prog, mem_erase, page, 0};
    CHECK(nv_open(&nv));
    host_rx_init(&h.to_gw);
    gw_io_t gw_io = {noop_tx, gw_to_esp, NULL, &h, NULL};
    gw_init(&h.gw, 0x1111, 0x2222, 0x00124B0000000001ull, (const uint8_t *)"778B5F63", &gw_io);
    gw_load(&h.gw, &nv);
    CHECK(gw_save_network(&h.gw));

    // Build a full 64-sensor document, format to JSON, parse back, import with simulated UART drop, and re-export.
    gwnv_doc_t want = {.pan = 0x65FE, .short_addr = 0xD425, .ext = 0x00124B0099887766ull, .n_sensors = 64};
    for (size_t i = 0; i < 64; i++) {
        want.sensors[i].sub = (uint16_t)i;
        want.sensors[i].short_addr = (uint16_t)(i + 1);
        want.sensors[i].type = i % 2 ? SENSOR_MOTION_V1 : SENSOR_CONTACT_V2;
        snprintf(want.sensors[i].mac, sizeof want.sensors[i].mac, "77AA00%02X", (unsigned)i);
    }
    char json[4608], err[128] = "";
    size_t jlen = gwnv_format_json(&want, json, sizeof json);
    CHECK(jlen > 0 && jlen < 4096);  // fits inside ESP8266's 4 KB sbl_page!

    gwnv_doc_t parsed;
    CHECK(gwnv_parse_json(json, jlen, &parsed, err, sizeof err));
    // Also exercise programmatic lowercase MAC normalization in gwnv_import():
    memcpy(parsed.sensors[0].mac, "77aa0000", 9);

    // Inject a stray 0x5319 alarm in rx_buf and drop 1 write to verify gwnv retries and NEVER ACKs 0x5319!
    uint8_t fake_alarm[32] = {0};
    uint8_t alarm_pkt[64];
    size_t ap_len = host_build(alarm_pkt, HOST_ALARM, fake_alarm, 26, true);
    gw_to_esp(&h, alarm_pkt, ap_len);
    h.drop_writes = 1;

    gwnv_io_t io = {esp_write, esp_read, esp_flush, &h};
    CHECK(gwnv_import(&io, &parsed, err, sizeof err));
    CHECK(!strcmp(parsed.sensors[0].mac, "77AA0000"));
    CHECK(h.acks_5319 == 0);
    CHECK(h.gw.pan == 0x65FE && h.gw.short_addr == 0xD425 && h.gw.n_sensors == 64);

    gwnv_doc_t got;
    CHECK(gwnv_export(&io, &got, err, sizeof err));
    CHECK(got.pan == want.pan && got.short_addr == want.short_addr && got.ext == want.ext && got.n_sensors == 64);
    for (size_t i = 0; i < 64; i++) {
        CHECK(got.sensors[i].sub == want.sensors[i].sub && got.sensors[i].short_addr == want.sensors[i].short_addr &&
              got.sensors[i].type == want.sensors[i].type && !strcmp(got.sensors[i].mac, want.sensors[i].mac));
    }
}

typedef struct {
    harness_t h;
    uint8_t img[512];
    unsigned resets, bootloaders;
} probe_ctx_t;

static void probe_reset(void *ctx)
{
    ((probe_ctx_t *)ctx)->resets++;
}

static void probe_bl(void *ctx)
{
    ((probe_ctx_t *)ctx)->bootloaders++;
}

static void probe_sleep(void *ctx, uint32_t ms)
{
    (void)ctx;
    (void)ms;
}

static bool probe_img(void *ctx, uint32_t off, uint8_t *buf, size_t len)
{
    probe_ctx_t *p = ctx;
    if (off + len > sizeof p->img) {
        return false;
    }
    memcpy(buf, p->img + off, len);
    return true;
}

static void test_gwsync_probe_safe_mode(void)
{
    static probe_ctx_t p;
    memset(&p, 0, sizeof p);
    host_rx_init(&p.h.to_gw);

    build_info_t running_bi = {.magic = BUILD_INFO_MAGIC,
                               .format = BUILD_INFO_FORMAT,
                               .id = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88},
                               .product = "gateway-contact-v1",
                               .board = "wyze_contact_v1",
                               .app = "gateway",
                               .version = "custom-dev"};
    build_info_t carried_bi = running_bi;
    carried_bi.id[0] = 0x99;
    snprintf(carried_bi.version, sizeof carried_bi.version, "bundled-v1");
    memcpy(p.img + 64, &carried_bi, sizeof carried_bi);

    gw_io_t gw_io = {noop_tx, gw_to_esp, NULL, &p.h, NULL};
    gw_init(&p.h.gw, 0x1111, 0x2222, 0x00124B0000000001ull, (const uint8_t *)"778B5F63", &gw_io);
    p.h.gw.build_info = &running_bi;
    p.h.gw.build_info_len = sizeof running_bi;

    uint8_t scratch[SBL_PAGE];
    gwsync_report_t rep;
    gwsync_io_t io = {esp_write, esp_read, esp_flush, probe_reset, probe_bl,       probe_sleep,
                      probe_img, NULL,     &p,        NULL,        GWSYNC_VIA_AUTO};
    CHECK(gwsync_probe(&io, sizeof p.img, scratch, &rep) == GWSYNC_SAME);
    CHECK(p.resets == 1 && p.bootloaders == 0);
    CHECK(rep.answered && !strcmp(rep.running.version, "custom-dev") && !strcmp(rep.carried.version, "bundled-v1"));
    CHECK(!strcmp(rep.mac, "778B5F63"));
    CHECK(strstr(rep.msg, "auto-sync paused") != NULL);
}

// #55: the carried image has the running gateway's code (the same image ID) but another version string: nothing is
// flashed, and the report says what the radio itself runs (its build's version), not the carried version
static void test_gwsync_same_code_reports_its_own_version(void)
{
    static probe_ctx_t p;
    memset(&p, 0, sizeof p);
    host_rx_init(&p.h.to_gw);
    build_info_t running_bi = {.magic = BUILD_INFO_MAGIC,
                               .format = BUILD_INFO_FORMAT,
                               .id = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88},
                               .product = "gateway-contact-v1",
                               .board = "wyze_contact_v1",
                               .app = "gateway",
                               .version = "40f8584-dirty"};
    build_info_t carried_bi = running_bi;
    snprintf(carried_bi.version, sizeof carried_bi.version, "v0.2.0");
    memcpy(p.img + 64, &carried_bi, sizeof carried_bi);
    gw_io_t gw_io = {noop_tx, gw_to_esp, NULL, &p.h, NULL};
    gw_init(&p.h.gw, 0x1111, 0x2222, 0x00124B0000000001ull, (const uint8_t *)"778B5F63", &gw_io);
    p.h.gw.build_info = &running_bi;
    p.h.gw.build_info_len = sizeof running_bi;

    uint8_t scratch[SBL_PAGE];
    sbl_t sbl;
    gwsync_report_t rep;
    gwsync_io_t io = {esp_write, esp_read, esp_flush, probe_reset, probe_bl,       probe_sleep,
                      probe_img, NULL,     &p,        NULL,        GWSYNC_VIA_AUTO};
    CHECK(gwsync_run(&io, sizeof p.img, &sbl, scratch, &rep) == GWSYNC_SAME);
    CHECK(p.bootloaders == 0);  // nothing flashed
    CHECK(!strcmp(rep.running.version, "40f8584-dirty") && !strcmp(rep.carried.version, "v0.2.0"));
    CHECK(strstr(rep.msg, "built as 40f8584-dirty") != NULL);

    // the same version: the usual message
    snprintf(running_bi.version, sizeof running_bi.version, "v0.2.0");
    memset(&p.h.to_gw, 0, sizeof p.h.to_gw);
    host_rx_init(&p.h.to_gw);
    CHECK(gwsync_run(&io, sizeof p.img, &sbl, scratch, &rep) == GWSYNC_SAME);
    CHECK(!strcmp(rep.running.version, "v0.2.0") && strstr(rep.msg, "runs the carried gateway v0.2.0") != NULL);
}

int main(void)
{
    test_json_validation();
    test_64_sensor_import_export_roundtrip();
    test_gwsync_probe_safe_mode();
    test_gwsync_same_code_reports_its_own_version();
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
