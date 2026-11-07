// The gateway: what the stock dongle does, as a portable state machine (no hardware, no timing).
// Port of reference/wyzesense_ref/gateway.py; tests/test_gateway.c replays the captured house traffic through it
// (reference/tests/vectors/gateway_replay.txt) and checks every frame it sends against the stock dongle's.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nvstore.h"
#include "ota.h"

#define GW_MAX_SENSORS 64
#define GW_MAX_PENDING 8      // frames waiting for their sensor's next data request
#define GW_PENDING_MS  30000  // ... at most this long (802.15.4's transaction persistence time, generously)
#define GW_MAX_JOINING 4      // sensors between their A3 and their Verify Sensor
#define GW_MAX_UNACKED 4      // async packets to the host awaiting its ACK

// OWS_SUB_CCFG (0x53F0 0x03 -> 0x53F1 0x03): put the firmware's built-in CCFG (hal/ccfg.h) back in the chip, in two
// steps so that no stray or replayed packet can do it:
//   arm:     GW_CCFG_MAGIC, 0x01, the chip's IEEE address (8, BE)         -> 0x53F1 0x03: 0x01, status[, nonce (4)]
//   confirm: GW_CCFG_MAGIC, 0x02, the chip's IEEE address, the nonce      -> 0x53F1 0x03: 0x02, status
// within GW_CCFG_ARM_MS of the arm. Status: GW_CCFG_OK (armed / rewritten and verified), GW_CCFG_ALREADY_RIGHT (no
// need: nothing armed or written), GW_CCFG_REFUSED (bad magic, address, step or nonce; not armed; expired),
// GW_CCFG_NOT_SAFE (the built-in CCFG fails ccfg_is_safe()), GW_CCFG_FAILED (flash erase / program / verify).
#define GW_CCFG_MAGIC  "OWS-CCFG-RESTORE"
#define GW_CCFG_ARM_MS 10000
enum {
    GW_CCFG_OK = 0x00,
    GW_CCFG_ALREADY_RIGHT = 0x01,
    GW_CCFG_REFUSED = 0xE0,
    GW_CCFG_NOT_SAFE = 0xE1,
    GW_CCFG_FAILED = 0xE2
};

typedef struct {
    // a MAC frame to send (the radio adds PHR and FCS), starting no earlier than gap_us after the start of the
    // previous frame sent (0: as soon as possible)
    void (*tx)(void *ctx, const uint8_t *frame, size_t len, uint32_t gap_us);
    // a packet for the host (FACTS.md §9)
    void (*host)(void *ctx, const uint8_t *pkt, size_t len);
    // OWS_SUB_CCFG (may be NULL: not supported). write false: 0 if a rewrite is needed, else 1 (already right) / 2
    // (built-in not safe); write true: the rewrite (hal/ccfg.h ccfg_restore: 0 done, 1, 2, 3 failed)
    int (*ccfg)(void *ctx, bool write);
    void *ctx;
    // Software ROM bootloader entry on direct-UART gateways (may be NULL: not supported). enter false: true if SBL
    // entry is permitted right now; enter true: flush UART TX and jump to ROM SBL.
    bool (*sbl)(void *ctx, bool enter);
} gw_io_t;

// Feature flags (R3, docs/design/requirements.md): what changes the protocol a stock peer sees is off until the host
// turns it on with OWS_SUB_FLAGS (0x53F0 0x01; sub-payload: 4 bytes BE flags wanted, or empty to ask; legacy 0- or
// 0x00-prefixed 4-byte 0x53F0 packets are also accepted). The reply (0x53F1 0x01) carries the flags now on, then the
// flags this firmware supports (4 + 4 bytes BE). A stock dongle does not answer it, which is how a host tells them
// apart. Not kept across a reset: the host sets them again.
enum {
    GW_FLAG_DEDUP = 1u << 0,  // a report the gateway already forwarded (a sensor's retry) is not forwarded again
    // 1u << 1: reserved (was GW_FLAG_FIX_ZERO_MAC, removed in #31: a zero-MAC report is dropped, never recovered)
    GW_FLAG_OTA = 1u << 2,       // OTA updates of our sensors (common/ota.h): the host's offers taken
    GW_FLAG_ANNOUNCE = 1u << 3,  // the A6 of a pairing says the gateway is ours, and its features
                                 // (wyze_app.h A6_OURS_*), in the bytes a stock dongle leaves 0
    GW_FLAG_ROAM = 1u << 4,      // roaming (#90, design/roaming.md): several gateways share one network identity;
                                 // each ACKs and answers only the sensors in its own table (and associations
                                 // only while pairing), and forwards reports it overhears from the others'
                                 // sensors as OWS_SUB_OVERHEARD, unACKed on the air
};
#define GW_FLAGS_SUPPORTED (GW_FLAG_DEDUP | GW_FLAG_OTA | GW_FLAG_ANNOUNCE | GW_FLAG_ROAM)
#define GW_DEDUP_ENTRIES   8
#define GW_DEDUP_MS        30000  // a retry comes within seconds; the same A1 two hours on is a new one
#define GW_MAX_OTA_TARGETS 8      // sensor MACs staged in one OTA wave (plus wildcard "********")
#define GW_OTA_OFFER_MS    5000   // C0 offer claim before C1 arrives: yields if the sensor ignores it
#define GW_OTA_IDLE_MS     30000  // active C1/C2 block stream idle timeout before yielding to another sensor

typedef struct {
    uint8_t mac[8];  // ASCII
    uint16_t short_addr;
    uint16_t nv_sub;  // its record in the NV page (item 5, this sub-ID)
    bool secure;      // it proved its key (SL_HELLO): its stock-format reports are dropped (RAM only, prototype)
} gw_sensor_t;

// The secured link (seclink.h, docs/design/security.md): sessions in RAM, the oldest replaced when full (that sensor
// is then asked for a new one: SL_RESYNC)
#define GW_MAX_SESSIONS 16
typedef struct {
    bool used;
    uint16_t short_addr;
    uint32_t rx_ctr, tx_ctr;  // the last counter heard from it / sent to it
    uint32_t last_ms;
    uint8_t key[16];  // the session key
} gw_session_t;

typedef struct {
    bool used;
    uint32_t order;       // queue order
    uint32_t queued_ms;   // when: it expires GW_PENDING_MS later
    uint8_t kind;         // what to build when the sensor polls (gateway.c)
    uint64_t dst;         // short address, or the extended address of a sensor still associating
    uint16_t short_addr;  // association response: the address given
    uint8_t mac[8];       // association response: the sensor's ASCII MAC (= its extended address)
    uint8_t data[18];     // A4 token, A6 status, keypad reply, one of our messages (seclink.h)
    uint8_t len;          // data length for the keypad reply and our messages
} gw_pending_t;

typedef struct {
    uint64_t ext;              // the gateway's IEEE address
    uint64_t chip_ieee;        // the IEEE address gw_init was given (the chip's): OWS_SUB_CCFG is bound to it
    uint64_t clock_ms;         // Unix ms, set by the host (0x5333)
    uint64_t last_history_ms;  // 0xAD -> 0x5319 monotonic tiebreaker when consecutive records share a timestamp
    size_t n_sensors;
    size_t n_types;
    size_t list_cursor, list_limit;
    nvstore_t *nv;           // the sensor table's flash page, or NULL
    const void *build_info;  // OWS_SUB_BUILD_INFO's answer: the image's block (common/build_info.h), or NULL
    size_t build_info_len;
    gw_io_t io;
    struct {  // GW_FLAG_OTA: update wave staged by the host (up to GW_MAX_OTA_TARGETS MACs or wildcard "********")
        uint64_t wildcard_done;     // bit i: gw->sensors[i] completed (OTA_DONE_OK) in this wildcard wave
        uint64_t wildcard_skip;     // bit i: gw->sensors[i] refused/failed recently (suppresses burst polls)
        uint32_t wildcard_skip_ms;  // when wildcard_skip was last set (expires after 15 s or on next A1)
        uint32_t active_ms;
        uint32_t want;        // the block asked for last: offset, slot build, length, bytes the host sent of it
        uint16_t active_src;  // short_addr currently owning block[OTA_BLOCK] (0 = none)
        bool staged;
        bool wildcard;
        bool started;  // active_src has asked for a block: no more offers (until its next A1)
        uint8_t active_mac[8];
        uint8_t want_slot, want_len, filled;
        struct {
            bool used;
            bool associating;  // it is (re)joining: nothing for it until its A3 (then it is joining)
            uint8_t mac[8];
        } targets[GW_MAX_OTA_TARGETS];
        uint8_t offer[OTA_OFFER_LEN];  // the C0 payload
        uint8_t block[OTA_BLOCK];
    } ota;
    struct {  // async packets sent to the host, resent until it ACKs them
        size_t len;
        uint32_t order;
        uint32_t next_ms;
        bool used;
        uint8_t sends;
        uint8_t pkt[64];
    } unacked[GW_MAX_UNACKED];
    gw_pending_t pending[GW_MAX_PENDING];
    uint32_t pending_order;
    uint32_t unacked_order;
    uint32_t now_ms;        // the last gw_tick()
    uint32_t beacon_at_ms;  // a beacon request answered then (beacon_due), by gw_tick: see gw_rx (#84)
    uint32_t rng;           // xorshift32 state for the beacon delay
    uint32_t next_time_req_ms;
    uint32_t flags;  // GW_FLAG_*, off at reset
    uint32_t ccfg_until, ccfg_nonce;
    struct {
        uint32_t at_ms;
        uint16_t short_addr;
        bool used;
        uint8_t mac[8];
    } joining[GW_MAX_JOINING];
    struct {  // GW_FLAG_DEDUP: reports forwarded lately
        uint32_t crc, at_ms;
        uint16_t short_addr;
        bool used;
    } seen[GW_DEDUP_ENTRIES];
    uint16_t pan, short_addr;
    uint16_t addr_first, addr_last;  // OWS_SUB_ADDR_RANGE: short addresses new sensors get (0x0001-0xFFFD at reset)
    uint16_t history_tie;
    gw_sensor_t sensors[GW_MAX_SENSORS];
    gw_session_t sessions[GW_MAX_SESSIONS];
    uint8_t net_key[16];  // OWS_SUB_NET_KEY, from the host at every start; never in flash
    bool have_net_key;
    struct {
        uint16_t short_addr;
        uint8_t type;
    } types[GW_MAX_SENSORS];  // from each sensor's reports
    uint8_t mac[8];           // the gateway's ASCII MAC (Get MAC)
    uint8_t dsn, bsn;         // sequence numbers: data/command frames, beacons
    bool list_active;         // 0x5330 -> 0x5331 streaming cursor paced by free unacked[] slots
    bool time_req_armed;      // asking the host for the time (0x5332): from its Finish Auth on
    bool permit;              // pairing on (0x531C)
    bool beacon_due;          // a beacon request heard, its beacon not sent yet
    bool ccfg_armed;          // OWS_SUB_CCFG step 1 done, until ccfg_until
    bool history_seen;
    bool usb_hid;  // Wyze USB dongle CH554 bridge: refuses OWS_SUB_ENTER_SBL; CH554 handles 0x4312 in HW
} gateway_t;

void gw_init(gateway_t *gw, uint16_t pan, uint16_t short_addr, uint64_t ext, const uint8_t mac[8], const gw_io_t *io);

// Adds a sensor to the table (as if paired before); false if the table is full. Saved, with an NV store.
bool gw_add_sensor(gateway_t *gw, const uint8_t mac[8], uint16_t short_addr);

// Attaches the flash page that keeps the network and the sensor table, using the stock dongle's item/sub-IDs inside
// our OWNV store (FACTS.md §5: item 1 network, 4 count, 5/n sensors; stock pages are migrated by tools/nv_json.py or
// tools/gwsync.py), and loads them. Returns true if the page holds a network (gw->pan, short_addr, ext are set).
bool gw_load(gateway_t *gw, nvstore_t *nv);

// Saves the network (PAN, short address, IEEE address) as item 1.
bool gw_save_network(gateway_t *gw);

// A frame from the air: MAC frame without FCS (the radio checked it), rssi in dBm.
void gw_rx(gateway_t *gw, const uint8_t *frame, size_t len, int8_t rssi);

// A complete packet from the host.
void gw_host(gateway_t *gw, const uint8_t *pkt, size_t len);

// The passing of time (ms, any origin): resends what the host has not ACKed. Call it often.
void gw_tick(gateway_t *gw, uint32_t now_ms);
