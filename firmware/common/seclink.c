// The secured link's crypto (seclink.h): AES-CMAC and AES-CCM built on the platform's AES-ECB, so the same code
// runs on the CC1310 (its AES engine) and on the host board (tests/aes_ref.c).
#include "seclink.h"

#include <string.h>

#include "wyze_app.h"

static void block(const uint8_t key[16], const uint8_t in[16], uint8_t out[16])
{
    aes128_ecb_encrypt(key, in, out, 16);
}

static void dbl(uint8_t b[16])  // multiplication by x in GF(2^128), for the CMAC subkeys
{
    uint8_t carry = b[0] & 0x80;
    for (int i = 0; i < 15; i++) {
        b[i] = (uint8_t)(b[i] << 1 | b[i + 1] >> 7);
    }
    b[15] = (uint8_t)(b[15] << 1);
    if (carry) {
        b[15] ^= 0x87;
    }
}

void sl_cmac(const uint8_t key[16], const uint8_t *msg, size_t n, uint8_t out[16])
{
    uint8_t k1[16] = {0}, k2[16], x[16] = {0}, last[16];
    block(key, k1, k1);
    dbl(k1);
    memcpy(k2, k1, 16);
    dbl(k2);
    size_t blocks = n ? (n + 15) / 16 : 1;
    bool full = n && n % 16 == 0;
    for (size_t b = 0; b + 1 < blocks; b++) {
        for (int i = 0; i < 16; i++) {
            x[i] ^= msg[16 * b + i];
        }
        block(key, x, x);
    }
    size_t rest = n - 16 * (blocks - 1);
    memset(last, 0, 16);
    memcpy(last, msg + 16 * (blocks - 1), rest);
    if (!full) {
        last[rest] = 0x80;
    }
    for (int i = 0; i < 16; i++) {
        x[i] ^= last[i] ^ (full ? k1[i] : k2[i]);
    }
    block(key, x, out);
}

void sl_sensor_key(const uint8_t net_key[16], const uint8_t mac[8], uint8_t out[16])
{
    uint8_t m[12] = {'O', 'W', 'S', 'K'};
    memcpy(m + 4, mac, 8);
    sl_cmac(net_key, m, sizeof m, out);
}

void sl_session_key(const uint8_t sensor_key[16], const uint8_t snonce[8], const uint8_t gnonce[8], uint8_t out[16])
{
    uint8_t m[20] = {'O', 'W', 'S', 'S'};
    memcpy(m + 4, snonce, 8);
    memcpy(m + 12, gnonce, 8);
    sl_cmac(sensor_key, m, sizeof m, out);
}

void sl_hello_tag(const uint8_t sensor_key[16], const uint8_t mac[8], const uint8_t snonce[8], uint8_t out[SL_TAG])
{
    uint8_t m[18] = {APP_OURS, SL_HELLO}, t[16];
    memcpy(m + 2, mac, 8);
    memcpy(m + 10, snonce, 8);
    sl_cmac(sensor_key, m, sizeof m, t);
    memcpy(out, t, SL_TAG);
}

void sl_session_tag(const uint8_t sensor_key[16], const uint8_t snonce[8], const uint8_t gnonce[8], uint8_t out[SL_TAG])
{
    uint8_t m[18] = {APP_OURS, SL_SESSION}, t[16];
    memcpy(m + 2, snonce, 8);
    memcpy(m + 10, gnonce, 8);
    sl_cmac(sensor_key, m, sizeof m, t);
    memcpy(out, t, SL_TAG);
}

bool sl_equal(const uint8_t *a, const uint8_t *b, size_t n)
{
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++) {
        d |= a[i] ^ b[i];
    }
    return d == 0;
}

void sl_ccm(const uint8_t key[16], const uint8_t nonce[13], const uint8_t *hdr, size_t hdr_len, uint8_t *data, size_t n,
            bool encrypt, uint8_t tag[SL_TAG])
{
    uint8_t x[16], a[16], s[16];
    // CTR keystream block i: flags (L - 1) nonce i_be16
    a[0] = 0x01;
    memcpy(a + 1, nonce, 13);
    if (!encrypt) {  // decrypt first: the MAC is over the plaintext
        for (size_t off = 0; off < n; off += 16) {
            uint16_t i = (uint16_t)(1 + off / 16);
            a[14] = (uint8_t)(i >> 8), a[15] = (uint8_t)i;
            block(key, a, s);
            for (size_t j = 0; j < 16 && off + j < n; j++) {
                data[off + j] ^= s[j];
            }
        }
    }
    // CBC-MAC: B0 = flags(adata, M, L) nonce len_be16; then len_be16(adata) adata, zero-padded; then the plaintext
    x[0] = 0x40 | ((SL_TAG - 2) / 2) << 3 | 0x01;
    memcpy(x + 1, nonce, 13);
    x[14] = (uint8_t)(n >> 8), x[15] = (uint8_t)n;
    block(key, x, x);
    uint8_t b[16] = {0};
    b[0] = (uint8_t)(hdr_len >> 8), b[1] = (uint8_t)hdr_len;
    memcpy(b + 2, hdr, hdr_len);  // hdr_len <= 14
    for (int i = 0; i < 16; i++) {
        x[i] ^= b[i];
    }
    block(key, x, x);
    for (size_t off = 0; off < n; off += 16) {
        for (size_t j = 0; j < 16 && off + j < n; j++) {
            x[j] ^= data[off + j];
        }
        block(key, x, x);
    }
    a[14] = a[15] = 0;  // block 0 encrypts the tag
    block(key, a, s);
    for (int i = 0; i < SL_TAG; i++) {
        tag[i] = x[i] ^ s[i];
    }
    if (encrypt) {
        for (size_t off = 0; off < n; off += 16) {
            uint16_t i = (uint16_t)(1 + off / 16);
            a[14] = (uint8_t)(i >> 8), a[15] = (uint8_t)i;
            block(key, a, s);
            for (size_t j = 0; j < 16 && off + j < n; j++) {
                data[off + j] ^= s[j];
            }
        }
    }
}

// AES-CCM (RFC 3610) as SL_FRAME uses it: the nonce dir ctr_be32 0(8), the header as associated data
static void ccm(const uint8_t key[16], uint8_t dir, uint32_t ctr, const uint8_t *hdr, size_t hdr_len, uint8_t *data,
                size_t n, bool encrypt, uint8_t tag[SL_TAG])
{
    uint8_t nonce[13] = {dir, (uint8_t)(ctr >> 24), (uint8_t)(ctr >> 16), (uint8_t)(ctr >> 8), (uint8_t)ctr};
    sl_ccm(key, nonce, hdr, hdr_len, data, n, encrypt, tag);
}

size_t sl_seal(const uint8_t key[16], uint8_t dir, uint32_t ctr, const uint8_t *pt, size_t n, uint8_t *out)
{
    if (n > 255 || n > APP_MAX_PAYLOAD - SL_OVERHEAD) {
        return 0;
    }
    out[0] = APP_OURS, out[1] = SL_FRAME, out[2] = (uint8_t)n;
    out[3] = (uint8_t)(ctr >> 24), out[4] = (uint8_t)(ctr >> 16), out[5] = (uint8_t)(ctr >> 8), out[6] = (uint8_t)ctr;
    memmove(out + SL_FRAME_HDR, pt, n);
    ccm(key, dir, ctr, out, SL_FRAME_HDR, out + SL_FRAME_HDR, n, true, out + SL_FRAME_HDR + n);
    return n + SL_OVERHEAD;
}

bool sl_open(const uint8_t key[16], uint8_t dir, const uint8_t *in, size_t n, uint8_t *pt, size_t *len, uint32_t *ctr)
{
    uint8_t tag[SL_TAG];
    if (n < SL_OVERHEAD || in[0] != APP_OURS || in[1] != SL_FRAME || in[2] > n - SL_OVERHEAD) {
        return false;
    }
    size_t m = in[2];
    uint32_t c = (uint32_t)in[3] << 24 | (uint32_t)in[4] << 16 | (uint32_t)in[5] << 8 | in[6];
    memcpy(pt, in + SL_FRAME_HDR, m);
    ccm(key, dir, c, in, SL_FRAME_HDR, pt, m, false, tag);
    if (!sl_equal(tag, in + SL_FRAME_HDR + m, SL_TAG)) {
        memset(pt, 0, m);
        return false;
    }
    *len = m;
    *ctr = c;
    return true;
}
