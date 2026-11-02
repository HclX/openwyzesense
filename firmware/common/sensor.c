#include "sensor.h"

#include <string.h>

#include "boot_slot.h"
#include "mac154.h"
#include "ota.h"
#include "wyze_app.h"

// Timing, from the captured joins (ground_truth/rf/2026-10-22_18*, FACTS.md §3.1) and reports
#define SCAN_MS             850   // beacon request -> association request
#define POLL_EXT_MS         1050  // association request -> data request for the response
#define HANDSHAKE_MS        100   // association response -> data request, -> A3 (70 ms after)
#define A3_MS               70
#define POLL_MS             700  // between data requests while waiting
#define A5_MS               200  // A4 -> A5
#define REPORT_GAP_MS       130  // between back-to-back reports
#define ACK_TIMEOUT_MS      250  // resend after (hypothesis 4: identical copies 0.25 s apart)
#define MAX_TRIES           8
#define WAIT_FRAME_MS       500  // after an ACK with frame-pending
#define JOIN_POLLS          10
#define JOIN_TIMEOUT_MS     30000               // overall ceiling on SCANNING..HANDSHAKE so a join can never wedge
#define HEARTBEAT_V1_MS     (240u * 60 * 1000)  // A1 every 4 h: v1 motion (7 days of the house dongle's log)
#define HEARTBEAT_MS        (120u * 60 * 1000)  // every 2 h: leak, climate, v2 motion
#define PIR_BUCKET_MS       10000u              // stock motion_v1: twelve 10 s PIR pulse buckets
#define PIR_SUMMARY_MS      120000u             // stock motion_v1: 0xAB summary every 120 s while motion is active
#define PIR_CLEAR_MS        120000u             // stock motion_v1: 0xA2 (0) clear 120 s after last PIR pulse
#define CLIMATE_DELTA_CENTI 50                  // E8 when temperature moves 0.5 degC ...
#define CLIMATE_DELTA_HUM   3                   // ... or humidity 3 % from the last E8 sent
// OTA (common/ota.h): after a block request is ACKed, poll for the block (the gateway fetches it from its host)
#define OTA_POLL_MS     60
#define OTA_POLLS       4
#define OTA_POLL_GAP_MS 150
#define OTA_MAX_ROUNDS  12  // block requests in a row without a block: give up
#define OTA_GAP_MS      10  // block received -> the next request

enum {
    ACT_NONE,
    ACT_ASSOC,
    ACT_POLL_EXT,
    ACT_PRE_A3,
    ACT_A3,
    ACT_POLL,
    ACT_A5,
    ACT_REPORT,
    ACT_RESCAN,
    ACT_OTA_REQ,
    ACT_OTA_DONE
};

static uint32_t heartbeat_ms(const sensor_t *s);

static bool is_v2_contact_or_motion(uint8_t type)
{
    return type == SENSOR_CONTACT_V2 || type == SENSOR_MOTION_V2;
}

static bool is_joining(const sensor_t *s)
{
    return s->state == SENSOR_SCANNING || s->state == SENSOR_ASSOCIATING || s->state == SENSOR_HANDSHAKE;
}

void sensor_init(sensor_t *s, const sensor_config_t *cfg, const sensor_io_t *io)
{
    memset(s, 0, sizeof *s);
    s->cfg = *cfg;
    s->io = *io;
    s->temp_raw = 0x14;     // 20 degC
    s->battery_raw = 0x60;  // 3.0 V
}

void sensor_leave(sensor_t *s)
{
    s->state = SENSOR_IDLE;
    s->pan = s->coord = s->short_addr = 0;
    s->awaiting_ack = s->awaiting_frame = false;
    s->step_armed = false;
    s->step = ACT_NONE;
    s->n_queued = 0;
    s->ota.active = false;
    s->check_in = false;
    s->join_tries = 0;
    s->gw_ours = false;
    s->gw_features = 0;
    memset(&s->pir, 0, sizeof s->pir);
}

void sensor_restore(sensor_t *s, uint16_t pan, uint16_t coord, uint16_t short_addr, uint16_t counter, uint32_t now)
{
    s->pan = pan, s->coord = coord, s->short_addr = short_addr, s->counter = counter;
    s->state = SENSOR_JOINED;
    s->next_heartbeat = now + heartbeat_ms(s);
}

static void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void schedule(sensor_t *s, uint8_t action, uint32_t at)
{
    s->step = action;
    s->step_armed = action != ACT_NONE;
    s->step_at = at;
}

// Sends a frame. With ack: keeps it for resending until its ACK comes.
static void send(sensor_t *s, const uint8_t *f, size_t len, bool ack, uint32_t now)
{
    s->io.tx(s->io.ctx, f, len, 0);
    if (!ack) {
        return;
    }
    memcpy(s->out, f, len);
    s->out_len = len;
    s->awaiting_ack = true;
    s->tries = 1;
    s->retry_at = now + ACK_TIMEOUT_MS;
}

static void send_ack(sensor_t *s, uint8_t seq)
{
    uint8_t ack[3];
    s->io.tx(s->io.ctx, ack, mac_build_ack(ack, seq, false), 0);
}

static void beacon_request(sensor_t *s, uint32_t now)
{
    // FC 0x0803: command, short destination 0xFFFF on PAN 0xFFFF, no source
    uint8_t f[] = {0x03, 0x08, s->seq++, 0xFF, 0xFF, 0xFF, 0xFF, MAC_CMD_BEACON_REQ};
    send(s, f, sizeof f, false, now);
}

static void association_request(sensor_t *s, uint32_t now)
{
    // FC 0xC823: command, ack request, short destination, extended source (the ASCII MAC) on PAN 0xFFFF
    uint8_t f[21] = {0x23, 0xC8, s->seq++};
    put_le16(f + 3, s->pan);
    put_le16(f + 5, s->coord);
    put_le16(f + 7, 0xFFFF);
    memcpy(f + 9, s->cfg.mac, 8);
    f[17] = MAC_CMD_ASSOC_REQ;
    f[18] = 0x80;  // capability: allocate address
    send(s, f, 19, true, now);
}

static void data_request(sensor_t *s, bool ext, uint32_t now)
{
    // FC 0x8863 (short source) / 0xC863 (extended source, before the association): command, ack request,
    // PAN-ID compression, short destination
    uint8_t f[20] = {0x63, ext ? 0xC8 : 0x88, s->seq++};
    size_t n = 7;
    put_le16(f + 3, s->pan);
    put_le16(f + 5, s->coord);
    if (ext) {
        memcpy(f + n, s->cfg.mac, 8), n += 8;
    } else {
        put_le16(f + n, s->short_addr), n += 2;
    }
    f[n++] = MAC_CMD_DATA_REQ;
    send(s, f, n, true, now);
}

static void send_message(sensor_t *s, const uint8_t *pt, size_t n, uint32_t now)
{
    uint8_t ct[APP_MAX_PAYLOAD], f[9 + APP_MAX_PAYLOAD];
    size_t len = app_encrypt(pt, n, ct);
    send(s, f, mac_build_data(f, s->seq++, s->pan, s->coord, s->short_addr, ct, len, true), true, now);
}

// A1 / A2 / E8: type MAC sensor_type temp battery len(2) data counter(2); AB: AB MAC type buckets(12) 0x01
static void send_report(sensor_t *s, const sensor_queued_t *q, uint32_t now)
{
    uint8_t pt[24], n = 0;
    if (q->msg == APP_MOTION_SUMMARY) {
        pt[n++] = APP_MOTION_SUMMARY;
        memcpy(pt + n, s->cfg.mac, 8);
        n += 8;
        pt[n++] = s->cfg.type;
        memcpy(pt + n, s->pir.summary, 12);
        n += 12;
        pt[n++] = 0x01;
        send_message(s, pt, n, now);
        return;
    }
    uint8_t len = q->msg == APP_CLIMATE ? 3 : 1;
    if (q->msg != APP_STATUS) {  // events advance the counter, A1 repeats it (FACTS.md §4.1); RAM only (#38)
        s->counter++;
    }
    pt[n++] = q->msg;
    memcpy(pt + n, s->cfg.mac, 8), n += 8;
    pt[n++] = s->cfg.type;
    pt[n++] = s->temp_raw;
    pt[n++] = s->battery_raw;
    pt[n++] = s->low_battery ? 0x01 : 0x00, pt[n++] = len;
    memcpy(pt + n, q->data, len), n += len;
    pt[n++] = (uint8_t)(s->counter >> 8), pt[n++] = (uint8_t)s->counter;
    send_message(s, pt, n, now);
}

// What comes after a report or an OTA step: reports first, then the update's next block request
static void next_step(sensor_t *s, uint32_t at)
{
    schedule(s, s->n_queued ? ACT_REPORT : s->ota.active ? ACT_OTA_REQ : ACT_NONE, at);
}

static void ota_request(sensor_t *s, uint32_t now)
{
    uint8_t pt[14];
    pt[0] = APP_OTA_REQUEST;
    memcpy(pt + 1, s->cfg.mac, 8);
    pt[9] = s->ota.offer.slot;
    ota_put_be32(pt + 10, s->ota.offset);
    send_message(s, pt, sizeof pt, now);
}

static void ota_send_done(sensor_t *s, uint32_t now)
{
    uint8_t pt[14];
    pt[0] = APP_OTA_DONE;
    memcpy(pt + 1, s->cfg.mac, 8);
    pt[9] = s->ota.status;
    ota_put_be32(pt + 10, s->ota.offer.version);
    send_message(s, pt, sizeof pt, now);
}

// The update ends (status in s->ota.status): tell the gateway (C3), then the app (ota_end, on C3's ACK or failure)
static void ota_finish(sensor_t *s, uint8_t status, uint32_t now)
{
    s->ota.active = false;
    s->ota.status = status;
    s->polls_left = 0;
    schedule(s, ACT_OTA_DONE, now + OTA_GAP_MS);
}

static void ota_ended(sensor_t *s, uint32_t now)
{
    if (s->io.ota_end) {
        s->io.ota_end(s->io.ctx, s->ota.status);
    }
    next_step(s, now + REPORT_GAP_MS);
}

// No block came for a request: ask again, or give up
static void ota_no_block(sensor_t *s, uint32_t now)
{
    if (++s->ota.rounds >= OTA_MAX_ROUNDS) {
        ota_finish(s, OTA_DONE_GAVE_UP, now);
    } else {
        next_step(s, now + OTA_POLL_GAP_MS);
    }
}

static bool ota_offered(sensor_t *s, const uint8_t *pt, size_t n, uint32_t now)
{
    if (n < 1 + OTA_OFFER_LEN || !s->io.ota_offer || s->state != SENSOR_JOINED) {
        return false;
    }
    if (s->ota.active) {
        return false;  // the same offer again (the gateway repeats it until we answer)
    }
    sensor_ota_offer_t o = {ota_be32(pt + 1),
                            ota_be32(pt + 5),
                            {ota_be32(pt + 9), ota_be32(pt + 17)},
                            {ota_be32(pt + 13), ota_be32(pt + 21)},
                            0};
    uint8_t st = s->io.ota_offer(s->io.ctx, &o);
    if (st == OTA_DONE_OK && (o.slot > 1 || !o.size[o.slot] || o.size[o.slot] > SLOT_SIZE - SLOT_IMAGE_OFFSET)) {
        st = OTA_DONE_REFUSED;
    }
    s->ota.offer = o;
    s->polls_left = 0;
    if (st != OTA_DONE_OK) {
        ota_finish(s, st, now);
        return true;
    }
    s->ota.active = true;
    s->ota.offset = 0;
    s->ota.total = SLOT_IMAGE_OFFSET + o.size[o.slot];
    s->ota.rounds = 0;
    next_step(s, now + REPORT_GAP_MS);
    return true;
}

static bool ota_block(sensor_t *s, const uint8_t *pt, size_t n, uint32_t now)
{
    if (n < 6) {
        return false;
    }
    uint32_t off = ota_be32(pt + 1);
    uint8_t len = pt[5];
    if (!s->ota.active || off != s->ota.offset || !len || len > OTA_BLOCK || 6u + len > n || len > s->ota.total - off) {
        return false;
    }
    if (!s->io.ota_write(s->io.ctx, off, pt + 6, len)) {
        ota_finish(s, OTA_DONE_FLASH, now);
        return true;
    }
    s->ota.offset += len;
    s->ota.rounds = 0;
    s->polls_left = 0;
    if (s->ota.offset == s->ota.total) {
        ota_finish(s, s->io.ota_check(s->io.ctx, &s->ota.offer), now);
    } else {
        next_step(s, now + OTA_GAP_MS);
    }
    return true;
}

static void queue_report(sensor_t *s, uint8_t msg, uint32_t now)
{
    if (s->n_queued == SENSOR_QUEUE_LEN) {
        size_t drop = (s->step == ACT_REPORT && s->awaiting_ack) ? 1 : 0;
        memmove(s->queued + drop, s->queued + drop + 1, (SENSOR_QUEUE_LEN - 1 - drop) * sizeof s->queued[0]);
        s->n_queued--;
    }
    sensor_queued_t *q = &s->queued[s->n_queued++];
    q->msg = msg;
    if (msg == APP_CLIMATE) {
        memcpy(q->data, s->climate, 3);
    } else {
        q->data[0] = s->state_byte;
        q->data[1] = q->data[2] = 0;
    }
    if (s->state == SENSOR_JOINED && !s->step_armed && !s->awaiting_ack) {
        schedule(s, ACT_REPORT, now);
    }
}

static void start_scan(sensor_t *s, uint32_t now)
{
    s->state = SENSOR_SCANNING;
    s->got_beacon = false;
    s->awaiting_ack = s->awaiting_frame = false;
    s->polls_left = 0;
    s->n_queued = 0;
    beacon_request(s, now);
    schedule(s, ACT_ASSOC, now + SCAN_MS);
}

void sensor_join(sensor_t *s, uint32_t now)
{
    // the counter restarts at a join for the v2 contact (3 captured joins) and v1 motion (1); climate continues
    // it (2); others: not captured (FACTS.md §4)
    if (s->cfg.type == SENSOR_CONTACT_V2 || s->cfg.type == SENSOR_MOTION_V1) {
        s->counter = 0;
    }
    memset(&s->ota, 0, sizeof s->ota);
    memset(&s->pir, 0, sizeof s->pir);
    s->check_in = false;
    s->join_tries = 0;
    s->join_deadline = now + JOIN_TIMEOUT_MS;
    start_scan(s, now);
}

static void pir_rotate_buckets(sensor_t *s, uint32_t now)
{
    while ((int32_t)(now - s->pir.bucket_at) >= 0) {
        s->pir.buckets[s->pir.idx] = s->pir.pulses;
        s->pir.pulses = 0;
        s->pir.idx = (uint8_t)((s->pir.idx + 1u) % 12u);
        s->pir.bucket_at += PIR_BUCKET_MS;
    }
}

static void pir_tick(sensor_t *s, uint32_t now)
{
    if (!s->pir.active || s->state != SENSOR_JOINED) {
        return;
    }
    pir_rotate_buckets(s, now);
    if ((int32_t)(now - s->pir.summary_at) >= 0) {
        memcpy(s->pir.summary, s->pir.buckets, 12);
        memset(s->pir.buckets, 0, sizeof s->pir.buckets);
        s->pir.summary_at += PIR_SUMMARY_MS;
        queue_report(s, APP_MOTION_SUMMARY, now);
    }
    if ((int32_t)(now - s->pir.clear_at) >= 0) {
        if (s->pir.pin_high) {
            s->pir.clear_at = now + PIR_CLEAR_MS;
        } else {
            s->pir.active = false;
            s->state_byte = 0;
            queue_report(s, APP_EVENT, now);
        }
    }
}

void sensor_event(sensor_t *s, bool active, uint32_t now)
{
    s->state_byte = active;
    queue_report(s, APP_EVENT, now);
}

void sensor_pir(sensor_t *s, bool active, uint32_t now)
{
    s->pir.pin_high = active;
    if (s->state != SENSOR_JOINED || !active) {
        return;
    }
    if (!s->pir.active) {
        s->pir.active = true;
        s->pir.idx = 0;
        s->pir.pulses = 1;
        memset(s->pir.buckets, 0, sizeof s->pir.buckets);
        s->pir.bucket_at = now + PIR_BUCKET_MS;
        s->pir.summary_at = now + PIR_SUMMARY_MS;
        s->pir.clear_at = now + PIR_CLEAR_MS;
        s->state_byte = 1;
        queue_report(s, APP_EVENT, now);
        return;
    }
    pir_rotate_buckets(s, now);
    if (s->pir.pulses < 255u) {
        s->pir.pulses++;
    }
    s->pir.clear_at = now + PIR_CLEAR_MS;
}

static uint32_t heartbeat_ms(const sensor_t *s)
{
    if (s->cfg.heartbeat_ms) {
        return s->cfg.heartbeat_ms;
    }
    // v1 contact: not in the logs; assumed like v1 motion
    return s->cfg.type == SENSOR_MOTION_V1 || s->cfg.type == SENSOR_CONTACT_V1 ? HEARTBEAT_V1_MS : HEARTBEAT_MS;
}

void sensor_climate_sample(sensor_t *s, int16_t centi_c, uint8_t humidity, uint32_t now)
{
    int d_t = centi_c - s->sent_centi_c, d_h = humidity - s->sent_humidity;
    if (s->sent_climate && d_t > -CLIMATE_DELTA_CENTI && d_t < CLIMATE_DELTA_CENTI && d_h > -CLIMATE_DELTA_HUM &&
        d_h < CLIMATE_DELTA_HUM) {
        return;
    }
    sensor_climate(s, centi_c, humidity, now);
}

void sensor_climate(sensor_t *s, int16_t centi_c, uint8_t humidity, uint32_t now)
{
    s->sent_centi_c = centi_c;
    s->sent_humidity = humidity;
    s->sent_climate = true;
    s->climate[0] = (uint8_t)(centi_c / 100);
    s->climate[1] = (uint8_t)(centi_c % 100);
    s->climate[2] = humidity;
    queue_report(s, APP_CLIMATE, now);
}

void sensor_status(sensor_t *s, uint32_t now)
{
    queue_report(s, APP_STATUS, now);
}

void sensor_check_in(sensor_t *s, uint32_t now)
{
    if (s->state != SENSOR_JOINED) {
        return;
    }
    s->check_in = true;
    queue_report(s, APP_STATUS, now);
}

bool sensor_poll(sensor_t *s, uint32_t now)
{
    if (s->state != SENSOR_JOINED || s->step_armed || s->awaiting_ack || s->awaiting_frame || s->n_queued ||
        s->ota.active) {
        return false;
    }
    s->polls_left = 1;
    schedule(s, ACT_POLL, now);
    return true;
}

static void join_done(sensor_t *s, bool ok, uint32_t now)
{
    s->state = ok ? SENSOR_JOINED : SENSOR_IDLE;
    s->awaiting_ack = s->awaiting_frame = false;
    s->join_tries = 0;
    s->next_heartbeat = now + heartbeat_ms(s);
    schedule(s, ACT_NONE, 0);
    if (ok && s->io.save) {
        s->io.save(s->io.ctx);
    }
    if (s->io.joined) {
        s->io.joined(s->io.ctx, ok);
    }
    if (!ok) {
        return;
    }
    // first reports, as the stock sensors send them: v2 contact/motion their state then A1; climate A1 then E8
    if (s->cfg.type == SENSOR_CLIMATE_V2) {
        queue_report(s, APP_STATUS, now);
        queue_report(s, APP_CLIMATE, now);
    } else {
        if (is_v2_contact_or_motion(s->cfg.type)) {
            queue_report(s, APP_EVENT, now);
        }
        queue_report(s, APP_STATUS, now);
    }
}

// --- receiving ---

static void on_ack(sensor_t *s, const mac_frame_t *m, uint32_t now)
{
    uint8_t sent_type = s->out[0] & 7, sent_cmd = 0;
    if (!s->awaiting_ack || m->seq != s->out[2]) {
        return;
    }
    s->awaiting_ack = false;
    s->acks++;
    if (sent_type == MAC_CMD) {
        sent_cmd = s->out[s->out_len - (s->out[0] == 0x23 ? 2 : 1)];
    }
    if (sent_cmd == MAC_CMD_ASSOC_REQ) {
        schedule(s, ACT_POLL_EXT, now + POLL_EXT_MS);
    } else if (sent_cmd == MAC_CMD_DATA_REQ) {
        if (m->pending) {
            s->awaiting_frame = true;
            schedule(s, s->step, now + WAIT_FRAME_MS);  // the frame comes; poll again if it does not
        } else if (s->state == SENSOR_HANDSHAKE && s->step == ACT_PRE_A3) {
            schedule(s, ACT_A3, now + A3_MS);
        } else if (s->polls_left) {
            schedule(s, s->state == SENSOR_ASSOCIATING ? ACT_POLL_EXT : ACT_POLL,
                     now + (s->ota.active ? OTA_POLL_GAP_MS : POLL_MS));
        } else if (s->ota.active) {
            ota_no_block(s, now);
        } else if (s->state == SENSOR_JOINED) {
            next_step(s, now + REPORT_GAP_MS);
        }
    } else if (sent_type == MAC_DATA) {
        uint8_t msg = s->step == ACT_REPORT && s->n_queued ? s->queued[0].msg : 0;
        if (s->state == SENSOR_HANDSHAKE) {  // A3 or A5: poll for the answer
            s->polls_left = JOIN_POLLS;
            schedule(s, ACT_POLL, now + (s->step == ACT_A5 ? 430 : 600));
        } else if (s->step == ACT_OTA_REQ) {  // a block request: poll for the block
            s->polls_left = OTA_POLLS;
            schedule(s, ACT_POLL, now + OTA_POLL_MS);
        } else if (s->step == ACT_OTA_DONE) {
            ota_ended(s, now);
        } else if (msg) {
            memmove(s->queued, s->queued + 1, (size_t)(--s->n_queued) * sizeof s->queued[0]);
            if (msg == APP_STATUS) {  // after A1: poll for the time / for commands
                s->next_heartbeat = now + heartbeat_ms(s);
                bool v2 = is_v2_contact_or_motion(s->cfg.type) || s->check_in;
                s->check_in = false;
                s->polls_left = v2 ? JOIN_POLLS : 1;
                schedule(s, ACT_POLL, now + (v2 ? 560 : 800));
            } else {
                next_step(s, now + REPORT_GAP_MS);
            }
        }
    }
}

static bool on_message(sensor_t *s, const uint8_t *pt, size_t n, uint32_t now)
{
    switch (pt[0]) {
        case APP_JOIN_TOKEN:  // A4 -> A5
            if (n < 17 || s->state != SENSOR_HANDSHAKE) {
                return false;
            }
            memcpy(s->token, pt + 1, 16);
            s->polls_left = 0;
            schedule(s, ACT_A5, now + A5_MS);
            return true;
        case APP_JOIN_DONE:  // A6: joined; the dongle clock in bytes 5-10
            if (n < 11 || s->state != SENSOR_HANDSHAKE) {
                return false;
            }
            s->clock_ms = 0;
            for (int i = 5; i < 11; i++) {
                s->clock_ms = s->clock_ms << 8 | pt[i];
            }
            s->gw_features = 0;
            s->gw_ours = app_is_ours(pt, n, &s->gw_features);
            if (s->awaiting_ack) {
                s->awaiting_ack = false;
                s->acks++;
            }
            join_done(s, true, now);
            return true;
        case APP_TIME_SYNC:  // B0
            if (n < 9 || s->state != SENSOR_JOINED) {
                return false;
            }
            s->clock_ms = 0;
            for (int i = 3; i < 9; i++) {
                s->clock_ms = s->clock_ms << 8 | pt[i];
            }
            if (s->ota.active) {  // our gateway sends none during an update; a stock one: poll on
                s->awaiting_frame = false;
                return false;
            }
            s->polls_left = 0;
            next_step(s, now + REPORT_GAP_MS);
            return true;
        case APP_OTA_OFFER:
            return ota_offered(s, pt, n, now);
        case APP_OTA_BLOCK:
            return ota_block(s, pt, n, now);
        default:
            return false;
    }
}

void sensor_rx(sensor_t *s, const uint8_t *frame, size_t len, uint32_t now)
{
    mac_frame_t m;
    if (!mac_parse(frame, len, &m)) {
        return;
    }
    if (m.type == MAC_ACK) {
        on_ack(s, &m, now);
        return;
    }
    if (m.type == MAC_BEACON && s->state == SENSOR_SCANNING && !s->got_beacon && m.payload_len >= 2) {
        uint16_t sf = (uint16_t)(m.payload[0] | m.payload[1] << 8);
        bool allowed =
            (!s->cfg.only_pan || m.src_pan == s->cfg.only_pan) && (!s->cfg.never_pan || m.src_pan != s->cfg.never_pan);
        if ((sf & 0x8000) && allowed) {  // association permit: the gateway is pairing
            s->got_beacon = true;
            s->pan = m.src_pan;
            s->coord = (uint16_t)m.src;
        }
        return;
    }
    if (m.dst_pan != s->pan || !m.dst_mode) {
        return;
    }
    if (m.dst_mode == MAC_ADDR_EXT) {  // association response, to our extended address
        uint64_t ext = 0;
        for (int i = 7; i >= 0; i--) {
            ext = ext << 8 | s->cfg.mac[i];
        }
        if (m.dst != ext || s->state != SENSOR_ASSOCIATING) {
            return;
        }
        s->awaiting_frame = false;
        if (m.ack_req) {
            send_ack(s, m.seq);
        }
        if (m.cmd == MAC_CMD_ASSOC_RESP && m.payload_len >= 4 && m.payload[3] == 0) {
            if (s->awaiting_ack) {
                s->awaiting_ack = false;
                s->acks++;
            }
            s->short_addr = (uint16_t)(m.payload[1] | m.payload[2] << 8);
            s->state = SENSOR_HANDSHAKE;
            s->polls_left = 0;
            memset(s->token, 0, sizeof s->token);
            schedule(s, ACT_PRE_A3, now + HANDSHAKE_MS);
        }
        return;
    }
    if (m.dst != s->short_addr || s->state < SENSOR_HANDSHAKE) {
        return;
    }
    s->awaiting_frame = false;
    if (m.ack_req) {
        send_ack(s, m.seq);
    }
    if (m.type == MAC_DATA && m.payload_len && m.payload_len % 16 == 0 && m.payload_len <= APP_MAX_PAYLOAD) {
        uint8_t pt[APP_MAX_PAYLOAD];
        aes128_ecb_decrypt(app_key, m.payload, pt, m.payload_len);
        if (on_message(s, pt, m.payload_len, now) && s->awaiting_ack && (!m.src_mode || m.src == s->coord) &&
            ((s->out[0] & 7) == MAC_CMD || s->state != SENSOR_JOINED)) {
            s->awaiting_ack = false;
            s->acks++;
        }
    }
}

// --- time ---

static void act(sensor_t *s, uint32_t now)
{
    uint8_t action = s->step;
    uint8_t pt[32];
    s->step_armed = false;      // keeps the action for on_ack
    s->awaiting_frame = false;  // an announced frame that did not come: poll again, or give up
    switch (action) {
        case ACT_ASSOC:
            if (!s->got_beacon) {  // nobody pairing: try again a few times
                if (++s->join_tries >= 3) {
                    s->join_tries = 0;
                    join_done(s, false, now);
                } else {
                    start_scan(s, now);
                }
                return;
            }
            s->join_tries = 0;
            s->state = SENSOR_ASSOCIATING;
            s->polls_left = JOIN_POLLS;
            association_request(s, now);
            break;
        case ACT_POLL_EXT:
            if (!s->polls_left) {
                join_done(s, false, now);
                return;
            }
            s->polls_left--;
            data_request(s, true, now);
            break;
        case ACT_PRE_A3:
            data_request(s, false, now);
            break;
        case ACT_A3:  // A3: MAC, type, firmware, 4 bytes (open), 0
            pt[0] = APP_JOIN_ANNOUNCE;
            memcpy(pt + 1, s->cfg.mac, 8);
            pt[9] = s->cfg.type;
            pt[10] = s->cfg.fw_version;
            memcpy(pt + 11, s->cfg.a3_extra, 4);
            pt[15] = 0;
            send_message(s, pt, 16, now);
            break;
        case ACT_POLL:
            if (!s->polls_left) {
                if (s->state == SENSOR_HANDSHAKE) {
                    join_done(s, false, now);
                } else if (s->ota.active) {
                    ota_no_block(s, now);
                } else {
                    next_step(s, now);
                }
                return;
            }
            s->polls_left--;
            data_request(s, false, now);
            break;
        case ACT_A5:  // A5: MAC, response(16), type, firmware
            pt[0] = APP_JOIN_RESPONSE;
            memcpy(pt + 1, s->cfg.mac, 8);
            if (s->io.join_response) {
                s->io.join_response(s->io.ctx, s->token, pt + 9);
            } else {
                memcpy(pt + 9, s->token, 16);
            }
            pt[25] = s->cfg.type;
            pt[26] = s->cfg.fw_version;
            send_message(s, pt, 27, now);
            break;
        case ACT_REPORT:
            if (s->n_queued) {
                send_report(s, &s->queued[0], now);
            } else {
                s->step = ACT_NONE;
            }
            break;
        case ACT_OTA_REQ:
            if (s->ota.active) {
                ota_request(s, now);
            } else {
                s->step = ACT_NONE;
            }
            break;
        case ACT_OTA_DONE:
            ota_send_done(s, now);
            break;
        default:
            break;
    }
}

bool sensor_listening(const sensor_t *s)
{
    return s->state == SENSOR_SCANNING || s->state == SENSOR_ASSOCIATING || s->state == SENSOR_HANDSHAKE ||
           s->awaiting_ack || s->awaiting_frame;
}

uint32_t sensor_next_event(const sensor_t *s, uint32_t now)
{
    uint32_t next = now + 0x7FFFFFFFu;
    if (is_joining(s) && (int32_t)(s->join_deadline - next) < 0) {
        next = s->join_deadline;
    }
    if (s->awaiting_ack && (int32_t)(s->retry_at - next) < 0) {
        next = s->retry_at;
    }
    if (s->step_armed && (int32_t)(s->step_at - next) < 0) {
        next = s->step_at;
    }
    if (s->state == SENSOR_JOINED && (int32_t)(s->next_heartbeat - next) < 0) {
        next = s->next_heartbeat;
    }
    if (s->state == SENSOR_JOINED && s->pir.active) {
        if ((int32_t)(s->pir.bucket_at - next) < 0) {
            next = s->pir.bucket_at;
        }
        if ((int32_t)(s->pir.summary_at - next) < 0) {
            next = s->pir.summary_at;
        }
        if ((int32_t)(s->pir.clear_at - next) < 0) {
            next = s->pir.clear_at;
        }
    }
    return (int32_t)(next - now) < 0 ? now : next;
}

void sensor_tick(sensor_t *s, uint32_t now)
{
    if (is_joining(s) && (int32_t)(now - s->join_deadline) >= 0) {
        join_done(s, false, now);
        return;
    }
    pir_tick(s, now);
    if (s->state == SENSOR_JOINED && (int32_t)(now - s->next_heartbeat) >= 0) {
        s->next_heartbeat = now + heartbeat_ms(s);
        queue_report(s, APP_STATUS, now);
    }
    if (s->awaiting_ack) {
        if ((int32_t)(now - s->retry_at) < 0) {
            return;
        }
        if (s->tries < MAX_TRIES) {  // resend, same sequence number
            s->io.tx(s->io.ctx, s->out, s->out_len, 0);
            s->tries++;
            s->retry_at = now + ACK_TIMEOUT_MS;
            return;
        }
        s->awaiting_ack = false;  // gave up on this frame
        if (s->state != SENSOR_JOINED) {
            join_done(s, false, now);
            return;
        }
        if (s->step == ACT_OTA_DONE) {
            ota_ended(s, now);
            return;
        }
        if (s->step == ACT_OTA_REQ || (s->ota.active && s->step == ACT_POLL)) {
            ota_no_block(s, now);
            return;
        }
        if (s->step == ACT_REPORT && s->n_queued) {
            memmove(s->queued, s->queued + 1, (size_t)(--s->n_queued) * sizeof s->queued[0]);
        }
        next_step(s, now + REPORT_GAP_MS);
        return;
    }
    if (s->step_armed && (int32_t)(now - s->step_at) >= 0) {
        act(s, now);
    }
}
