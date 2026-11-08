// Native tests of the portable firmware code against the captured frames in reference/tests/vectors/frames.txt
// (the same vectors reference/tests/test_reference.py checks the Python reference against).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host_proto.h"
#include "mac154.h"
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

static void load_vectors(const char *path)
{
    char line[512], hex[400];
    FILE *f = fopen(path, "r");
    if (!f) {
        perror(path);
        exit(2);
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

// MAC frame of a vector (without FCS), parsed
static mac_frame_t frame_of(const char *name)
{
    mac_frame_t m;
    const vector_t *v = vec(name);
    CHECK(mac_parse(v->b, v->n - 4, &m));
    return m;
}

// decrypted payload of a vector's data frame
static size_t plaintext_of(const char *name, uint8_t *pt)
{
    mac_frame_t m = frame_of(name);
    aes128_ecb_decrypt(app_key, m.payload, pt, m.payload_len);
    return m.payload_len;
}

static void test_fcs(void)
{
    for (int i = 0; i < n_vectors; i++) {
        uint8_t fcs[4];
        mac_fcs(vectors[i].b, vectors[i].n - 4, fcs);
        CHECK(!memcmp(fcs, vectors[i].b + vectors[i].n - 4, 4));
    }
    // ACKs from the 18:21 join (decoded_frames.jsonl): 02 00 1d + 8b7b2174, 12 00 1e + d77f150f
    uint8_t ack[3], fcs[4];
    mac_build_ack(ack, 0x1D, false);
    mac_ack_fcs(ack, fcs);
    CHECK(!memcmp(ack, "\x02\x00\x1d", 3) && !memcmp(fcs, "\x8b\x7b\x21\x74", 4));
    mac_build_ack(ack, 0x1E, true);
    mac_ack_fcs(ack, fcs);
    CHECK(!memcmp(ack, "\x12\x00\x1e", 3) && !memcmp(fcs, "\xd7\x7f\x15\x0f", 4));
}

static void test_mac(void)
{
    mac_frame_t m = frame_of("E8_CLIMATE");
    uint8_t out[128];
    const vector_t *v = vec("E8_CLIMATE");
    CHECK(m.type == MAC_DATA && m.ack_req && m.dst_pan == 0x845B && m.dst == 0x3366 && m.src == 0x000D);
    CHECK(m.seq == 0xEE);
    CHECK(mac_build_data(out, 0xEE, 0x845B, 0x3366, 0x000D, m.payload, m.payload_len, true) == v->n - 4);
    CHECK(!memcmp(out, v->b, v->n - 4));

    CHECK(frame_of("DATA_REQ").cmd == MAC_CMD_DATA_REQ);
    m = frame_of("ASSOC_REQ");
    CHECK(m.cmd == MAC_CMD_ASSOC_REQ && m.dst == 0x3366 && m.src_pan == 0xFFFF && m.src_mode == MAC_ADDR_EXT);
    CHECK(m.src == 0x4434313544423737ull);  // "77BD514D" read little-endian
    m = frame_of("ASSOC_RESP");
    CHECK(m.cmd == MAC_CMD_ASSOC_RESP && m.src == 0x00124B001E9FF933ull);
    CHECK(m.payload_len == 4 && !memcmp(m.payload, "\x02\x02\x00\x00", 4));

    // IEEE 802.15.4 addressing mode 1 (0b01) is reserved and must be rejected on both dst and src.
    const uint8_t bad_dst[12] = {0x41, 0x84, 0x01, 0x5B, 0x84, 0x66, 0x33, 0x0D, 0x00, 0xAA, 0xBB, 0xCC};
    const uint8_t bad_src[12] = {0x41, 0x48, 0x01, 0x5B, 0x84, 0x66, 0x33, 0x0D, 0x00, 0xAA, 0xBB, 0xCC};
    CHECK(!mac_parse(bad_dst, sizeof bad_dst, &m));
    CHECK(!mac_parse(bad_src, sizeof bad_src, &m));
}

static void test_app(void)
{
    uint8_t pt[APP_MAX_PAYLOAD], ct[APP_MAX_PAYLOAD], built[APP_MAX_PAYLOAD];
    app_report_t r;
    size_t n = plaintext_of("E8_CLIMATE", pt);
    CHECK(app_parse_report(pt, n, &r));
    CHECK(r.msg == APP_CLIMATE && !memcmp(r.mac, "77C68217", 8) && r.sensor_type == SENSOR_CLIMATE_V2);
    CHECK(r.data_len == 3 && r.data[0] == 19 && r.data[1] == 96 && r.data[2] == 50 && r.counter == 0x1316);
    mac_frame_t m = frame_of("E8_CLIMATE");
    CHECK(app_encrypt(pt, 16 + r.data_len, ct) == m.payload_len && !memcmp(ct, m.payload, m.payload_len));

    n = plaintext_of("A2_MOTION", pt);
    CHECK(app_parse_report(pt, n, &r) && !memcmp(r.mac, "778B77D9", 8) && r.data[0] == 1 && r.counter == 0x713B);
    n = plaintext_of("ZERO_MAC", pt);
    CHECK(app_parse_report(pt, n, &r) && r.msg == APP_STATUS && r.sensor_type == SENSOR_CONTACT_V1);

    n = plaintext_of("B0_SYNC", pt);
    memset(built, 0, sizeof built);
    CHECK(app_build_time_sync(built, 1792680838506ull) == 10 && !memcmp(built, pt, n));
    n = plaintext_of("A4_TOKEN", pt);
    memset(built, 0, sizeof built);
    CHECK(app_build_join_token(built, (const uint8_t *)"Ok5HPNQ4lf77u754") == 17 && !memcmp(built, pt, n));
    n = plaintext_of("A6_DONE", pt);
    memset(built, 0, sizeof built);
    CHECK(app_build_join_done(built, (const uint8_t *)"\xff\x04", 1792718501753ull) == 11 && !memcmp(built, pt, n));
}

static int hexeq(const uint8_t *b, size_t n, const char *hex)
{
    uint8_t want[HOST_MAX_PACKET];
    return unhex(hex, want) == n && !memcmp(b, want, n);
}

static void test_host(void)
{
    uint8_t p[HOST_MAX_PACKET], q[HOST_MAX_PACKET];
    host_pkt_t pkt;
    size_t n;
    // as captured on dongle 77A85A36 (ground_truth/host/2026-05-27_*)
    CHECK(hexeq(p, host_build(p, HOST_INQUIRY, NULL, 0, false), "aa55430327016c"));
    CHECK(hexeq(p, host_build(p, HOST_FINISH_AUTH, (const uint8_t *)"\xff", 1, false), "aa55530414ff0269"));
    CHECK(hexeq(p, host_build_ack(p, HOST_ALARM, false), "aa555319ff026a"));
    CHECK(hexeq(p, host_build(p, 0x4328, (const uint8_t *)"\x01", 1, true), "55aa43042801016f"));
    CHECK(hexeq(p, host_build_ack(p, HOST_GET_VERSION, true), "55aa5316ff0267"));
    CHECK(hexeq(p, host_build(p, 0x4305, (const uint8_t *)"77A85A36", 8, true),
                "55aa430b053737413835413336"
                "0318"));

    n = host_build(p, 0x4305, (const uint8_t *)"77A85A36", 8, true);
    CHECK(host_parse(p, n, &pkt) && pkt.cmd == 0x4305 && pkt.to_host && pkt.len == 8);
    p[n - 1] ^= 1;
    CHECK(!host_parse(p, n, &pkt));
    n = host_build_ack(p, HOST_ALARM, false);
    CHECK(host_parse(p, n, &pkt) && pkt.cmd == HOST_ACK && pkt.acked == HOST_ALARM && !pkt.to_host);

    // a byte stream with garbage and a false magic between two packets
    host_rx_t rx;
    int got = 0;
    size_t m = 0;
    host_rx_init(&rx);
    q[m++] = 0x00, q[m++] = 0x55, q[m++] = 0x13;
    m += host_build(q + m, HOST_INQUIRY, NULL, 0, false);
    q[m++] = 0xAA, q[m++] = 0x55, q[m++] = 0x53;
    m += host_build(q + m, HOST_TIME_SYNC, (const uint8_t *)"\x00\x00\x01\x9e\x6c\x1f\x8d\xfe", 8, false);
    for (size_t i = 0; i < m; i++) {
        if (!host_rx_feed(&rx, q[i], &pkt)) {
            continue;
        }
        got++;
        if (got == 1) {
            CHECK(pkt.cmd == HOST_INQUIRY);
        }
        if (got == 2) {
            CHECK(pkt.cmd == HOST_TIME_SYNC && pkt.len == 8 && pkt.payload[3] == 0x9e);
        }
    }
    CHECK(got == 2);

    // 77C681D1's heartbeat as the house dongle forwarded it (ground_truth/host/2026-05-21_*, 05-22)
    uint8_t pt[32] = "\xa1"
                     "77C681D1"
                     "\x07\x13\x5e\x00\x01\x00\x0a\x75";
    n = host_alarm_payload(q, pt, sizeof pt, 0x019E4E7BF23Bull, 0x4C);
    CHECK(hexeq(p, host_build(p, HOST_ALARM, q, n, true),
                "55aa531d190000019e4e7bf23ba1373743363831443107135e0001000a754c07c7"));

    // Bounds validation: oversize payload (> 252 B) is rejected by host_build; b2 < 3 on non-ACK is rejected by
    // host_parse while b2 < 3 on an ACK (e.g. acking 0x4302 -> b2 = 0x02) remains valid.
    uint8_t big[260] = {0};
    CHECK(host_build(p, HOST_ALARM, big, 252, true) == 259);
    CHECK(host_build(p, HOST_ALARM, big, 253, true) == 0);
    CHECK(host_build(p, HOST_ALARM, big, 260, true) == 0);

    n = host_build_ack(p, 0x5302, true);  // b2 == 0x02 (< 3), cmd == 0xFF (ACK)
    CHECK(host_parse(p, n, &pkt) && pkt.cmd == HOST_ACK && pkt.acked == 0x5302);

    uint8_t bad_b2[7] = {0xAA, 0x55, 0x53, 0x02, 0x16, 0x01, 0x6A};  // non-ACK with b2 == 2
    CHECK(!host_parse(bad_b2, sizeof bad_b2, &pkt));
}

// The secured link's crypto (common/seclink.c) against the published vectors, then a seal / open round trip
static void test_seclink(void)
{
    static const uint8_t k[16] = {0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6,
                                  0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c};
    static const uint8_t m[64] = {0x6b, 0xc1, 0xbe, 0xe2, 0x2e, 0x40, 0x9f, 0x96, 0xe9, 0x3d, 0x7e, 0x11, 0x73,
                                  0x93, 0x17, 0x2a, 0xae, 0x2d, 0x8a, 0x57, 0x1e, 0x03, 0xac, 0x9c, 0x9e, 0xb7,
                                  0x6f, 0xac, 0x45, 0xaf, 0x8e, 0x51, 0x30, 0xc8, 0x1c, 0x46, 0xa3, 0x5c, 0xe4,
                                  0x11, 0xe5, 0xfb, 0xc1, 0x19, 0x1a, 0x0a, 0x52, 0xef, 0xf6, 0x9f, 0x24, 0x45,
                                  0xdf, 0x4f, 0x9b, 0x17, 0xad, 0x2b, 0x41, 0x7b, 0xe6, 0x6c, 0x37, 0x10};
    uint8_t t[16];
    // RFC 4493 section 4, examples 1-4
    sl_cmac(k, m, 0, t);
    CHECK(!memcmp(t, "\xbb\x1d\x69\x29\xe9\x59\x37\x28\x7f\xa3\x7d\x12\x9b\x75\x67\x46", 16));
    sl_cmac(k, m, 16, t);
    CHECK(!memcmp(t, "\x07\x0a\x16\xb4\x6b\x4d\x41\x44\xf7\x9b\xdd\x9d\xd0\x4a\x28\x7c", 16));
    sl_cmac(k, m, 40, t);
    CHECK(!memcmp(t, "\xdf\xa6\x67\x47\xde\x9a\xe6\x30\x30\xca\x32\x61\x14\x97\xc8\x27", 16));
    sl_cmac(k, m, 64, t);
    CHECK(!memcmp(t, "\x51\xf0\xbe\xbf\x7e\x3b\x9d\x92\xfc\x49\x74\x17\x79\x36\x3c\xfe", 16));

    // RFC 3610 packet vector #1: key C0..CF, nonce 00000003 020100 A0..A5, header 00..07, payload 08..1E
    uint8_t ck[16], nonce[13] = {0, 0, 0, 3, 2, 1, 0, 0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5}, hdr[8], data[23], tag[8];
    for (int i = 0; i < 16; i++) {
        ck[i] = (uint8_t)(0xC0 + i);
    }
    for (int i = 0; i < 8; i++) {
        hdr[i] = (uint8_t)i;
    }
    for (int i = 0; i < 23; i++) {
        data[i] = (uint8_t)(8 + i);
    }
    sl_ccm(ck, nonce, hdr, 8, data, 23, true, tag);
    CHECK(!memcmp(data, "\x58\x8c\x97\x9a\x61\xc6\x63\xd2\xf0\x66\xd0\xc2\xc0\xf9\x89\x80\x6d\x5f\x6b\x61\xda\xc3\x84",
                  23));
    CHECK(!memcmp(tag, "\x17\xe8\xd1\x2c\xfd\xf9\x26\xe0", 8));
    sl_ccm(ck, nonce, hdr, 8, data, 23, false, tag);  // and back
    CHECK(data[0] == 8 && data[22] == 30 && !memcmp(tag, "\x17\xe8\xd1\x2c\xfd\xf9\x26\xe0", 8));

    // seal / open: the right key, direction and bytes only
    uint8_t in[17] = {APP_STATUS, '7', '7', 'A', 'B', 'C', 'D', 'E', 'F', 1, 0x14, 0x60, 0, 1, 0, 0, 5};
    uint8_t f[64], pt[64];
    size_t n = sl_seal(k, SL_UP, 7, in, sizeof in, f), len = 0;
    uint32_t ctr = 0;
    CHECK(n == sizeof in + SL_OVERHEAD && f[0] == APP_OURS && f[1] == SL_FRAME && f[2] == sizeof in);
    CHECK(memcmp(f + SL_FRAME_HDR, in, sizeof in) != 0);  // encrypted
    CHECK(sl_open(k, SL_UP, f, n, pt, &len, &ctr) && len == sizeof in && ctr == 7 && !memcmp(pt, in, len));
    CHECK(!sl_open(k, SL_DOWN, f, n, pt, &len, &ctr));  // the other direction
    CHECK(!sl_open(ck, SL_UP, f, n, pt, &len, &ctr));   // another key
    f[SL_FRAME_HDR + 3] ^= 1;
    CHECK(!sl_open(k, SL_UP, f, n, pt, &len, &ctr));  // a flipped bit
    f[SL_FRAME_HDR + 3] ^= 1;
    f[6] ^= 1;
    CHECK(!sl_open(k, SL_UP, f, n, pt, &len, &ctr));  // a changed counter (associated data)
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s frames.txt\n", argv[0]);
        return 2;
    }
    load_vectors(argv[1]);
    test_fcs();
    test_mac();
    test_app();
    test_host();
    test_seclink();
    printf("%d checks, %d failed\n", checks, failures);
    return failures != 0;
}
