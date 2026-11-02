#include "wyze_app.h"

#include <string.h>

// The FIPS-197 example key; every Wyze network uses it (gateway RAM 0x14E0)
const uint8_t app_key[16] = {0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6,
                             0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c};

size_t app_encrypt(const uint8_t *pt, size_t n, uint8_t *out)
{
    size_t padded = (n + 15) & ~(size_t)15;
    memmove(out, pt, n);
    memset(out + n, 0, padded - n);
    aes128_ecb_encrypt(app_key, out, out, padded);
    return padded;
}

bool app_parse_report(const uint8_t *pt, size_t n, app_report_t *r)
{
    if (n < 16 || (pt[0] != APP_STATUS && pt[0] != APP_EVENT && pt[0] != APP_CLIMATE)) {
        return false;
    }
    r->msg = pt[0];
    r->mac = pt + 1;
    r->sensor_type = pt[9];
    r->temp_raw = pt[10];
    r->battery_raw = pt[11];
    r->low_battery = pt[12] != 0;  // FACTS.md: 0x01 once a stock sensor latched it; not a length's high byte
    r->data_len = pt[13];
    if (16u + r->data_len > n) {
        return false;
    }
    r->data = pt + 14;
    r->counter = (uint16_t)(pt[14 + r->data_len] << 8 | pt[15 + r->data_len]);
    return true;
}

static void put_be48(uint8_t *out, uint64_t v)
{
    for (int i = 0; i < 6; i++) {
        out[i] = (uint8_t)(v >> (40 - 8 * i));
    }
}

size_t app_build_time_sync(uint8_t *pt, uint64_t time_ms)
{
    // b0 00 00, clock (6 bytes), 0x28 (in every B0 captured; meaning open)
    pt[0] = APP_TIME_SYNC;
    pt[1] = pt[2] = 0;
    put_be48(pt + 3, time_ms);
    pt[9] = 0x28;
    return 10;
}

size_t app_build_join_token(uint8_t *pt, const uint8_t token[16])
{
    pt[0] = APP_JOIN_TOKEN;
    memcpy(pt + 1, token, 16);
    return 17;
}

size_t app_build_join_done(uint8_t *pt, const uint8_t status[2], uint64_t time_ms)
{
    // a6, the host's Verify Sensor bytes (ff 04), 00 00, clock (6 bytes)
    pt[0] = APP_JOIN_DONE;
    pt[1] = status[0];
    pt[2] = status[1];
    pt[3] = pt[4] = 0;
    put_be48(pt + 5, time_ms);
    return 11;
}

size_t app_mark_ours(uint8_t *pt, size_t n, uint16_t features)
{
    (void)n;
    pt[11] = 'O', pt[12] = 'W', pt[13] = A6_OURS_VERSION;
    pt[14] = (uint8_t)(features >> 8), pt[15] = (uint8_t)features;
    return 16;
}

bool app_is_ours(const uint8_t *pt, size_t n, uint16_t *features)
{
    if (n < 16 || pt[11] != 'O' || pt[12] != 'W' || pt[13] != A6_OURS_VERSION) {
        return false;
    }
    *features = (uint16_t)(pt[14] << 8 | pt[15]);
    return true;
}
