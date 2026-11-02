// IEEE 802.15.4 MAC frames as Wyze uses them (docs/FACTS.md §2-3). Port of reference/wyzesense_ref/frame.py.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { MAC_BEACON = 0, MAC_DATA = 1, MAC_ACK = 2, MAC_CMD = 3 };
enum { MAC_CMD_ASSOC_REQ = 1, MAC_CMD_ASSOC_RESP = 2, MAC_CMD_DATA_REQ = 4, MAC_CMD_BEACON_REQ = 7 };
enum { MAC_ADDR_NONE = 0, MAC_ADDR_SHORT = 2, MAC_ADDR_EXT = 3 };

typedef struct {
    uint16_t fc;
    uint8_t type, seq;
    bool pending, ack_req, pan_comp;
    uint8_t dst_mode, src_mode;
    uint16_t dst_pan, src_pan;
    uint64_t dst, src;  // short or extended address; an extended one as read little-endian
    const uint8_t *payload;
    size_t payload_len;
    int cmd;  // MAC command id for command frames, else -1
} mac_frame_t;

// CRC-32 (IEEE 802.3 / zlib)
uint32_t crc32_ieee(const uint8_t *data, size_t n);

// Parses a MAC frame without its FCS. Returns false if it is too short for its own header.
bool mac_parse(const uint8_t *frame, size_t n, mac_frame_t *m);

// Data frame, PAN-ID compression, short addresses (FC 0x8861 with ack request). Returns its length.
size_t mac_build_data(uint8_t *out, uint8_t seq, uint16_t pan, uint16_t dst, uint16_t src, const uint8_t *payload,
                      size_t n, bool ack_req);

// ACK: FC 0x0002 or 0x0012 (pending) + seq. Returns 3.
// Whether the frame has pan as its source or destination PAN (radio_tx refuses the house network's).
bool mac_on_pan(const uint8_t *frame, size_t n, uint16_t pan);

size_t mac_build_ack(uint8_t *out, uint8_t seq, bool pending);

// The FCS stock devices put on an ACK: CRC-32 over FC, seq and one zero byte, little-endian.
void mac_ack_fcs(const uint8_t ack[3], uint8_t fcs[4]);

// The normal FCS: CRC-32 over the MAC frame, little-endian.
void mac_fcs(const uint8_t *frame, size_t n, uint8_t fcs[4]);
