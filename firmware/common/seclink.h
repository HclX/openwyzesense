// The secured link between our sensor and our gateway (docs/design/security.md; prototype, #28).
//
// Our application messages share one first byte, APP_OURS (0x4F: no stock message uses it; stock sends A1-A6, A9/AA,
// AB, AD, B0, B1, C0, D0, D2, E0, E2, E8, EA), followed by a sub-ID, as the host protocol's 0x53F0 does. They travel
// inside the stock AES-ECB envelope like every other message (app_encrypt); the security is in what they carry:
//
//   SL_KEY      gw -> sensor  4F 01 K_sensor(16)                        at pairing, after the A6 (in clear: 4.1 F1)
//   SL_HELLO    sensor -> gw  4F 02 mac(8) snonce(8) tag(8)             after a boot / on SL_RESYNC; tag proves K_sensor
//   SL_SESSION  gw -> sensor  4F 03 gnonce(8) tag(8)                    the gateway's half; tag proves K_sensor too
//   SL_FRAME    both          4F 04 len(1) ctr_be32(4) ct(len) tag(8)   AES-CCM under the session key
//   SL_RESYNC   gw -> sensor  4F 05                                     "start a new session" (the gateway lost it)
//
// Keys: K_sensor = CMAC(K_net, "OWSK" mac(8)); the session key = CMAC(K_sensor, "OWSS" snonce(8) gnonce(8)).
// SL_FRAME: CCM with an 8-byte tag and a 13-byte nonce dir(1) ctr_be32(4) 0(8) (dir: SL_UP sensor -> gateway,
// SL_DOWN the other way); the 7-byte header (4F 04 len ctr) is authenticated as associated data. Each session key is
// fresh, so a counter starting at 1 in each session never repeats a nonce.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APP_OURS 0x4F
enum { SL_KEY = 0x01, SL_HELLO = 0x02, SL_SESSION = 0x03, SL_FRAME = 0x04, SL_RESYNC = 0x05 };
enum { SL_UP = 0, SL_DOWN = 1 };

#define SL_TAG         8
#define SL_NONCE       8
#define SL_FRAME_HDR   7                            // 4F 04 len ctr(4)
#define SL_OVERHEAD    (SL_FRAME_HDR + SL_TAG)      // 15 bytes around the inner message
#define SL_HELLO_LEN   (2 + 8 + SL_NONCE + SL_TAG)  // 26
#define SL_SESSION_LEN (2 + SL_NONCE + SL_TAG)      // 18

// Provided by the platform (like aes128_ecb_*): n random bytes (the chip's TRNG; the host board: its own source).
void platform_random(uint8_t *out, size_t n);

// AES-CMAC (RFC 4493) over n bytes, on the platform's AES-ECB.
void sl_cmac(const uint8_t key[16], const uint8_t *msg, size_t n, uint8_t out[16]);

void sl_sensor_key(const uint8_t net_key[16], const uint8_t mac[8], uint8_t out[16]);
void sl_session_key(const uint8_t sensor_key[16], const uint8_t snonce[8], const uint8_t gnonce[8], uint8_t out[16]);
// The 8-byte tags of SL_HELLO (over 4F 02 mac snonce) and SL_SESSION (over 4F 03 snonce gnonce)
void sl_hello_tag(const uint8_t sensor_key[16], const uint8_t mac[8], const uint8_t snonce[8], uint8_t out[SL_TAG]);
void sl_session_tag(const uint8_t sensor_key[16], const uint8_t snonce[8], const uint8_t gnonce[8],
                    uint8_t out[SL_TAG]);

// SL_FRAME: seals n bytes of an inner message into out (n + SL_OVERHEAD bytes, returned); 0 if n is too long.
size_t sl_seal(const uint8_t key[16], uint8_t dir, uint32_t ctr, const uint8_t *pt, size_t n, uint8_t *out);
// Opens an SL_FRAME of up to n bytes (padding after it is ignored): the inner message into pt (*len bytes), its
// counter into *ctr. False: not an SL_FRAME, too short, or the tag does not match (forged, other key or direction).
bool sl_open(const uint8_t key[16], uint8_t dir, const uint8_t *in, size_t n, uint8_t *pt, size_t *len, uint32_t *ctr);

// AES-CCM (RFC 3610) with M = 8, L = 2: encrypts (or decrypts) n bytes of data in place and gives the tag; the
// associated data hdr is at most 14 bytes. Exposed for the RFC test vectors.
void sl_ccm(const uint8_t key[16], const uint8_t nonce[13], const uint8_t *hdr, size_t hdr_len, uint8_t *data, size_t n,
            bool encrypt, uint8_t tag[SL_TAG]);

// Constant-time comparison of n bytes
bool sl_equal(const uint8_t *a, const uint8_t *b, size_t n);
