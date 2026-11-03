#include "mac154.h"

#include <string.h>

uint32_t crc32_ieee(const uint8_t *data, size_t n)
{
    uint32_t crc = 0xFFFFFFFFu;
    while (n--) {
        crc ^= *data++;
        for (int i = 0; i < 8; i++) {
            crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1));
        }
    }
    return ~crc;
}

static void put_le16(uint8_t *out, uint16_t v)
{
    out[0] = (uint8_t)v;
    out[1] = (uint8_t)(v >> 8);
}

static void put_le32(uint8_t *out, uint32_t v)
{
    for (int i = 0; i < 4; i++) {
        out[i] = (uint8_t)(v >> (8 * i));
    }
}

void mac_fcs(const uint8_t *frame, size_t n, uint8_t fcs[4])
{
    put_le32(fcs, crc32_ieee(frame, n));
}

void mac_ack_fcs(const uint8_t ack[3], uint8_t fcs[4])
{
    const uint8_t padded[4] = {ack[0], ack[1], ack[2], 0};
    put_le32(fcs, crc32_ieee(padded, 4));
}

bool mac_parse(const uint8_t *b, size_t n, mac_frame_t *m)
{
    size_t i = 3;
    memset(m, 0, sizeof *m);
    m->cmd = -1;
    if (n < 3) {
        return false;
    }
    m->fc = (uint16_t)(b[0] | b[1] << 8);
    m->type = m->fc & 7;
    m->pending = (m->fc >> 4) & 1;
    m->ack_req = (m->fc >> 5) & 1;
    m->pan_comp = (m->fc >> 6) & 1;
    m->dst_mode = (m->fc >> 10) & 3;
    m->src_mode = (m->fc >> 14) & 3;
    if (m->dst_mode == 1 || m->src_mode == 1) {
        return false;
    }
    m->seq = b[2];

    if (m->dst_mode) {
        if (i + 2 > n) {
            return false;
        }
        m->dst_pan = (uint16_t)(b[i] | b[i + 1] << 8);
        i += 2;
    }
    for (int side = 0; side < 2; side++) {
        uint8_t mode = side ? m->src_mode : m->dst_mode;
        uint64_t *addr = side ? &m->src : &m->dst;
        if (side && mode) {
            if (m->pan_comp && m->dst_mode) {
                m->src_pan = m->dst_pan;
            } else {
                if (i + 2 > n) {
                    return false;
                }
                m->src_pan = (uint16_t)(b[i] | b[i + 1] << 8);
                i += 2;
            }
        }
        size_t len = mode == MAC_ADDR_SHORT ? 2 : mode == MAC_ADDR_EXT ? 8 : 0;
        if (i + len > n) {
            return false;
        }
        for (size_t k = 0; k < len; k++) {
            *addr |= (uint64_t)b[i + k] << (8 * k);
        }
        i += len;
    }
    m->payload = b + i;
    m->payload_len = n - i;
    if (m->type == MAC_CMD && m->payload_len) {
        m->cmd = m->payload[0];
    }
    return true;
}

size_t mac_build_data(uint8_t *out, uint8_t seq, uint16_t pan, uint16_t dst, uint16_t src, const uint8_t *payload,
                      size_t n, bool ack_req)
{
    uint16_t fc = MAC_DATA | (ack_req << 5) | (1 << 6) | (MAC_ADDR_SHORT << 10) | (MAC_ADDR_SHORT << 14);
    put_le16(out, fc);
    out[2] = seq;
    put_le16(out + 3, pan);
    put_le16(out + 5, dst);
    put_le16(out + 7, src);
    memcpy(out + 9, payload, n);
    return 9 + n;
}

size_t mac_build_ack(uint8_t *out, uint8_t seq, bool pending)
{
    out[0] = (uint8_t)(MAC_ACK | (pending << 4));
    out[1] = 0;
    out[2] = seq;
    return 3;
}
