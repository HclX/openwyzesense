// Application layer: the 802.15.4 MAC payload, AES-128-ECB encrypted (docs/FACTS.md §4).
// Port of reference/wyzesense_ref/app.py.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    APP_STATUS = 0xA1,
    APP_EVENT = 0xA2,
    APP_JOIN_ANNOUNCE = 0xA3,
    APP_JOIN_TOKEN = 0xA4,
    APP_JOIN_RESPONSE = 0xA5,
    APP_JOIN_DONE = 0xA6,
    APP_MOTION_SUMMARY = 0xAB,
    APP_HISTORY_LOG = 0xAD,
    APP_TIME_SYNC = 0xB0,
    APP_DECOMM_ACK = 0xB1,
    APP_CLIMATE = 0xE8,
};

enum {
    SENSOR_CONTACT_V1 = 0x01,
    SENSOR_MOTION_V1 = 0x02,
    SENSOR_LEAK_V2 = 0x03,
    SENSOR_CLIMATE_V2 = 0x07,
    SENSOR_CONTACT_V2 = 0x0E,
    SENSOR_MOTION_V2 = 0x0F,
};

#define APP_MAX_PAYLOAD 112  // longest stock message: 32 bytes; ours: an OTA block (common/ota.h)

extern const uint8_t app_key[16];

// Provided by the platform: AES-128-ECB over n bytes (n a multiple of 16), in and out may be the same buffer.
void aes128_ecb_encrypt(const uint8_t key[16], const uint8_t *in, uint8_t *out, size_t n);
void aes128_ecb_decrypt(const uint8_t key[16], const uint8_t *in, uint8_t *out, size_t n);

// Zero-pads plaintext to a multiple of 16 and encrypts it with app_key. Returns the padded length.
size_t app_encrypt(const uint8_t *pt, size_t n, uint8_t *out);

// A1 / A2 / E8 report fields: type MAC(8) sensor_type temp battery len(2, BE) data[len] counter(2, BE).
typedef struct {
    uint8_t msg, sensor_type, temp_raw, battery_raw;
    const uint8_t *mac;
    const uint8_t *data;
    uint16_t data_len, counter;
    bool low_battery;  // byte 12
} app_report_t;

bool app_parse_report(const uint8_t *pt, size_t n, app_report_t *r);

// Builders for what the gateway sends; each returns the plaintext length (before padding).
size_t app_build_time_sync(uint8_t *pt, uint64_t time_ms);                           // B0
size_t app_build_join_token(uint8_t *pt, const uint8_t token[16]);                   // A4
size_t app_build_join_done(uint8_t *pt, const uint8_t status[2], uint64_t time_ms);  // A6

// Ours (R3: only with the gateway's GW_FLAG_ANNOUNCE on): the A6 is 11 bytes, sent padded to 16 with zeros; our
// gateway puts "OW", a version and its features for sensors (A6_OURS_*, 2 bytes BE) in bytes 11-15, so our sensor
// knows from its pairing on that its gateway is ours. A stock sensor is expected to ignore them (to be checked:
// docs/stock-unknowns.md J6).
#define A6_OURS_VERSION 1
enum { A6_OURS_OTA = 1u << 0 };  // the gateway takes OTA offers from its host (GW_FLAG_OTA)

// Marks an A6 built by app_build_join_done (n = 11) as ours; returns the new length, 16
size_t app_mark_ours(uint8_t *pt, size_t n, uint16_t features);

// Whether a received A6 (n bytes of plaintext, padding included) carries our mark; its features in *features
bool app_is_ours(const uint8_t *pt, size_t n, uint16_t *features);
