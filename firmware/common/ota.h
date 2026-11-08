// OTA updates between our gateway and our sensors (docs/design/ota.md): the messages, ours only. They are our
// messages (seclink.h): first byte APP_OURS (0x4F), then the sub-ID below in place of a type byte (they were C0-C3,
// which collide with the stock gateway's 0xC0 command), sealed in an SL_FRAME when the link is secured. Our gateway
// offers an update only with GW_FLAG_OTA on and only to the sensor(s) its host stages (up to GW_MAX_OTA_TARGETS MACs
// or wildcard "********"). A sensor takes a sealed offer at any time (it can only come from its own gateway: #28),
// an unsealed one only in the window opened by a short press of its button.
//
//   offer   gateway -> sensor  4F 10, board ID, version, then size and CRC-32 of the slot-A build, of the slot-B build
//                              (4 bytes each; as in each build's slot header)
//   request sensor -> gateway  4F 11, MAC (8), slot (1: 0 A, 1 B, the build it wants), offset (4)
//   block   gateway -> sensor  4F 12, offset (4), length (1), data (length <= OTA_BLOCK)
//   done    sensor -> gateway  4F 13, MAC (8), status (1, OTA_DONE_*), version (4)
// All numbers big-endian. Offsets count from the start of the slot image (its 256-byte header, then the image: what
// tools/slot_image.py writes); the sensor writes it into its other slot, in order, and checks it at the end.
//
// The gateway keeps no image: it asks its host for each block, in pieces small enough for the dongle's USB link
// (host_proto.h HOST_OWS_CMD / HOST_OWS_RSP + OWS_SUB_OTA_*):
//   0x53F0 0x10 offer   host -> gw   MAC (8, or "********"), the C0 fields after its type (24); A size of 0: cancel
//                                    -> 0x53F1 0x10: status (0 staged, OTA_HOST_REFUSED: GW_FLAG_OTA off, malformed)
//   0x53F1 0x11 need    gw -> host   MAC (8), slot (1), offset (4), length (1)
//   0x53F0 0x12 data    host -> gw   offset (4), data (<= OTA_HOST_CHUNK) of the block needed
//   0x53F1 0x13 done    gw -> host   MAC (8), status (1), version (4)
// Up to GW_MAX_OTA_TARGETS sensors (or wildcard "********") may be staged in one wave; one active block stream at a
// time, and a distinct offer payload replaces the staged wave.
#pragma once

#include <stdint.h>

// Sub-IDs after APP_OURS (the same numbers as the host protocol's OWS_SUB_OTA_*)
enum { APP_OTA_OFFER = 0x10, APP_OTA_REQUEST = 0x11, APP_OTA_BLOCK = 0x12, APP_OTA_DONE = 0x13 };

// image bytes per block: 4F 12 off(4) len(1) + 80 = 87, sealed + 15 = 102 -> 112 encrypted, in a 127-byte frame
#define OTA_BLOCK        80
#define OTA_OFFER_LEN    24  // the offer after its sub-ID: board ID, version, size A, CRC A, size B, CRC B
#define OTA_HOST_CHUNK   32  // image bytes per 0x53F0 0x12 packet (37 B payload, 44 B in all with framing)
#define OTA_HOST_REFUSED 0xE0

enum { OTA_DONE_OK = 0, OTA_DONE_BAD_CRC = 1, OTA_DONE_REFUSED = 2, OTA_DONE_GAVE_UP = 3, OTA_DONE_FLASH = 4 };

static inline uint32_t ota_be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static inline void ota_put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24), p[1] = (uint8_t)(v >> 16), p[2] = (uint8_t)(v >> 8), p[3] = (uint8_t)v;
}
