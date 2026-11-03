// A Wyze sensor: joining a gateway and reporting, as a portable state machine (no hardware). Time comes in
// as now_ms; frames go out through io.tx. One implementation for every sensor type: the per-type differences
// are in sensor_type_t (sensor.c). Protocol: docs/FACTS.md §3-4.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gateway.h"  // gw_io_t-style callbacks

typedef enum {
    SENSOR_IDLE,         // not joined, not trying
    SENSOR_SCANNING,     // beacon request sent, collecting beacons
    SENSOR_ASSOCIATING,  // association request sent, polling for the response
    SENSOR_HANDSHAKE,    // A3 .. A6
    SENSOR_JOINED,
} sensor_state_t;

typedef struct {
    uint32_t board_id, version;
    uint32_t size[2], crc[2];  // of the slot-A and slot-B builds, as their slot headers have them
    uint8_t slot;              // the build to take (ota_offer sets it: the slot not running)
} sensor_ota_offer_t;

typedef struct {
    // a MAC frame to send (the radio adds PHR and FCS); gap_us as in gw_io_t
    void (*tx)(void *ctx, const uint8_t *frame, size_t len, uint32_t gap_us);
    // the join ended: joined (true) or gave up (false)
    void (*joined)(void *ctx, bool ok);
    // the 16-byte A5 response to the gateway's A4 token. The stock sensors use XXTEA under a per-sensor key
    // (FACTS.md §4.3, the key is open); NULL: send the token back unchanged
    void (*join_response)(void *ctx, const uint8_t token[16], uint8_t out[16]);
    // the join changed: keep it (sensor_restore after a reset); may be NULL. The message counter is not kept (#38)
    void (*save)(void *ctx);
    void *ctx;
    // OTA updates (common/ota.h, docs/design/ota.md); all NULL: the sensor ignores offers (as a stock one would).
    // ota_offer: an update offered; OTA_DONE_OK to take it (o->slot set to the build wanted), else the refusal
    uint8_t (*ota_offer)(void *ctx, sensor_ota_offer_t *o);
    // ota_write: the next bytes of the slot image, in order from 0
    bool (*ota_write)(void *ctx, uint32_t off, const uint8_t *data, size_t len);
    // ota_check: all written; OTA_DONE_OK if the slot now holds the offered image, else OTA_DONE_BAD_CRC
    uint8_t (*ota_check)(void *ctx, const sensor_ota_offer_t *o);
    // ota_end: the update is over and the gateway told (or that failed); status OTA_DONE_* (OK: run the new image)
    void (*ota_end)(void *ctx, uint8_t status);
} sensor_io_t;

typedef struct {
    uint8_t type;           // SENSOR_* (wyze_app.h)
    uint8_t mac[8];         // ASCII, also the extended address while associating
    uint8_t fw_version;     // in A3 / A5
    uint8_t a3_extra[4];    // A3 bytes 11-14 (open; 14 changes on every join)
    uint16_t only_pan;      // join only this PAN (0: any)
    uint16_t never_pan;     // never join this PAN (0: none)
    uint32_t heartbeat_ms;  // A1 interval; 0: the stock one for the type (sensor.c)
} sensor_config_t;

#define SENSOR_QUEUE_LEN 4

typedef struct {
    uint8_t msg;      // APP_STATUS / APP_EVENT / APP_CLIMATE
    uint8_t data[3];  // state_byte (1B) or climate[3] (3B) captured when queued
} sensor_queued_t;

typedef struct {
    sensor_config_t cfg;
    sensor_io_t io;
    sensor_state_t state;
    uint16_t pan, coord, short_addr;  // the network, once found / joined
    uint8_t seq;                      // MAC sequence number
    uint16_t counter;                 // message counter (A2/E8 advance it, A1 repeats it)
    uint8_t temp_raw, battery_raw;    // what the reports carry in bytes 10 / 11
    bool low_battery;                 // byte 12 (stock: set after 3 loaded readings below 2.625 V, FACTS.md R5)
    uint8_t state_byte;               // A1/A2 data: open / active
    uint8_t climate[3];               // E8 data: degC, hundredths, humidity %
    int16_t sent_centi_c;             // the last E8 sent, for sensor_climate_sample
    uint8_t sent_humidity;
    bool sent_climate;
    uint32_t next_heartbeat;
    uint64_t clock_ms;  // from the gateway's B0 / A6 (0: never set)

    // MAC: one frame awaiting its ACK
    uint8_t out[48];
    size_t out_len;
    bool awaiting_ack;
    bool awaiting_frame;  // an ACK said frame-pending: the frame is coming
    uint8_t tries;
    uint32_t retry_at;

    // what to do next, and when
    uint8_t step;     // the current action (kept after it runs, for on_ack)
    bool step_armed;  // run it at step_at
    uint32_t step_at;
    uint8_t polls_left;
    sensor_queued_t queued[SENSOR_QUEUE_LEN];  // reports waiting to be sent
    uint8_t n_queued;
    uint8_t token[16];  // A4
    bool got_beacon;
    uint8_t join_tries;
    uint32_t join_deadline;

    struct {  // an OTA update in progress (common/ota.h)
        bool active;
        sensor_ota_offer_t offer;
        uint32_t offset, total;  // bytes of the slot image written / to write (header + image)
        uint8_t rounds;          // block requests in a row without a block
        uint8_t status;          // the C3 to send
    } ota;
    uint32_t acks;         // frames of ours ACKed since the start (the app: has it talked to its gateway)
    bool check_in;         // the A1 queued by sensor_check_in: JOIN_POLLS polls after it, any type
    bool gw_ours;          // the gateway's A6 at the pairing said it is ours (wyze_app.h A6_OURS_*)
    uint16_t gw_features;  // ... and these features
    struct {               // SENSOR_MOTION_V1 PIR: 12 x 10 s pulse buckets (0xAB) + 120 s retriggerable clear
        bool active;
        bool pin_high;
        uint8_t idx;
        uint8_t pulses;
        uint8_t buckets[12];
        uint8_t summary[12];
        uint32_t bucket_at;
        uint32_t summary_at;
        uint32_t clear_at;
    } pir;
} sensor_t;

void sensor_init(sensor_t *s, const sensor_config_t *cfg, const sensor_io_t *io);

// Starts a join (as when the user holds the button): beacon request, association, A3..A6.
void sensor_join(sensor_t *s, uint32_t now_ms);

// Forgets the network (as a sensor reset does).
void sensor_leave(sensor_t *s);

// Restores a join made earlier (from flash): no handshake.
void sensor_restore(sensor_t *s, uint16_t pan, uint16_t coord, uint16_t short_addr, uint16_t counter, uint32_t now_ms);

// Reports. sensor_event: contact open / motion active (A2); sensor_pir: v1 motion PIR pulse (A2=1 on trigger,
// 12 x 10 s buckets -> 0xAB summary, retriggerable 120 s clear -> A2=0); sensor_climate: E8; sensor_status: A1.
void sensor_event(sensor_t *s, bool active, uint32_t now_ms);
void sensor_pir(sensor_t *s, bool active, uint32_t now_ms);
void sensor_climate(sensor_t *s, int16_t centi_c, uint8_t humidity, uint32_t now_ms);

// A climate measurement (the stock sensors take one every 25.86 s): sends an E8 when it has moved by 0.5 degC
// or 3 % humidity from the last one sent (FACTS.md §4.1).
void sensor_climate_sample(sensor_t *s, int16_t centi_c, uint8_t humidity, uint32_t now_ms);
void sensor_status(sensor_t *s, uint32_t now_ms);

// A check-in (ours, on a short press of the button: docs/design/ota.md): an A1, then polls (as many as a v2 contact
// sensor makes after its A1, whatever the type), so the gateway can deliver what it has for the sensor.
void sensor_check_in(sensor_t *s, uint32_t now);

// One data request, if the sensor is joined and idle (nothing being sent, nothing queued); false otherwise.
bool sensor_poll(sensor_t *s, uint32_t now);

// A frame from the air (MAC frame without FCS), and the passing of time. Call sensor_tick often.
void sensor_rx(sensor_t *s, const uint8_t *frame, size_t len, uint32_t now_ms);
void sensor_tick(sensor_t *s, uint32_t now_ms);

// Whether the receiver must be on now. A joined sensor hears its gateway only in answer to its own frames (an
// ACK ~2 ms after them, or the frame a pending ACK announces), so it listens only while joining, while awaiting
// an ACK, and while awaiting an announced frame; the radio can sleep the rest of the time.
bool sensor_listening(const sensor_t *s);

// When sensor_tick has something to do next (ms; now_ms + 0xFFFFFFFF/2 if nothing): the time to wake up.
uint32_t sensor_next_event(const sensor_t *s, uint32_t now_ms);
