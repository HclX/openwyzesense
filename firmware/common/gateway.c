#include "gateway.h"

#include <string.h>

#include "host_proto.h"
#include "boot_slot.h"
#include "build_info.h"
#include "mac154.h"
#include "ota.h"
#include "wyze_app.h"

#define SUPERFRAME        0x4FFF  // non-beacon PAN coordinator, association permit off
#define SUPERFRAME_PERMIT 0xCFFF  // ... permit on (pairing)
// When a beacon request is answered (#84). Stock gateways start their beacon 38-156 ms after the request starts, at
// random (FACTS.md §3: the house dongle, a neighbour's, the spare before ours); the request itself is ~30 ms on the air,
// so that is ~8-126 ms after it ends, when gw_rx sees it. Ours answered at once, so two of ours collided and a sensor
// never saw the farther one's permit. A beacon is on the air ~40-45 ms, so the windows are a beacon apart: a pairing
// gateway answers first and its beacon has ended before any other one starts; both stay within the stock range.
#define BEACON_PERMIT_MS      5  // pairing: 5-20 ms after the request
#define BEACON_PERMIT_SPAN    16
#define BEACON_NO_PERMIT_MS   75  // not pairing: 75-125 ms
#define BEACON_NO_PERMIT_SPAN 51
// The frame that answers a data request starts 33.5 ms (29-38) after the start of the ACK that announced it
// (stock dongle, 34 exchanges in ground_truth/rf): the ACK lasts ~26.7 ms, leaving the sensor ~7 ms to turn round.
#define POLL_REPLY_GAP_US 33500
// An async packet the host does not ACK goes out 4 times in all, ~160 ms apart (stock dongle 77A85A36)
#define HOST_SENDS     4
#define HOST_RESEND_MS 160
// The stock dongle asks the host for the time 0.3 s after its Finish Auth, then every 692 s (house dongle median)
#define TIME_REQ_FIRST_MS 300
#define TIME_REQ_EVERY_MS 692000u

static const char VERSION[] = "0.0.0.47 V1.8 Gateway GW3U";

enum { PEND_ASSOC_RESP, PEND_JOIN_TOKEN, PEND_JOIN_DONE };

void gw_init(gateway_t *gw, uint16_t pan, uint16_t short_addr, uint64_t ext, const uint8_t mac[8], const gw_io_t *io)
{
    memset(gw, 0, sizeof *gw);
    gw->pan = pan;
    gw->short_addr = short_addr;
    gw->ext = ext;
    memcpy(gw->mac, mac, 8);
    gw->io = *io;
    gw->chip_ieee = ext;
    gw->rng = (uint32_t)(ext ^ ext >> 32) | 1;  // differs per chip; never 0 (xorshift)
    gw->addr_first = 0x0001;
    gw->addr_last = 0xFFFD;
}

static uint32_t next_random(gateway_t *gw)
{
    uint32_t x = gw->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return gw->rng = x;
}

// --- tables ---

// --- the NV page (stock records) ---

enum { NV_NETWORK = 1, NV_COUNT = 4, NV_SENSOR = 5 };

static bool nv_save_count(gateway_t *gw)
{
    uint8_t c[4] = {(uint8_t)gw->n_sensors, 0, 0, 0};
    return !gw->nv || nv_write(gw->nv, NV_COUNT, 0, c, 4);
}

static bool nv_save_sensor(gateway_t *gw, const gw_sensor_t *s)
{
    // PAN, short, MAC, then 12 bytes as the stock records have them (meaning open)
    uint8_t r[24] = {0};
    if (!gw->nv) {
        return true;
    }
    r[0] = (uint8_t)gw->pan;
    r[1] = (uint8_t)(gw->pan >> 8);
    r[2] = (uint8_t)s->short_addr;
    r[3] = (uint8_t)(s->short_addr >> 8);
    memcpy(r + 4, s->mac, 8);
    r[17] = 0x01;
    return nv_write(gw->nv, NV_SENSOR, s->nv_sub, r, sizeof r);
}

bool gw_save_network(gateway_t *gw)
{
    uint8_t r[16];
    if (!gw->nv) {
        return false;
    }
    r[0] = (uint8_t)gw->pan, r[1] = (uint8_t)(gw->pan >> 8);
    r[2] = (uint8_t)gw->short_addr, r[3] = (uint8_t)(gw->short_addr >> 8);
    for (int i = 0; i < 8; i++) {
        r[4 + i] = (uint8_t)(gw->ext >> (8 * i));
    }
    r[12] = 0x17, r[13] = 0x00, r[14] = 0xBE, r[15] = 0xBE;  // as on the stock dongles (meaning open)
    return nv_write(gw->nv, NV_NETWORK, 0, r, sizeof r);
}

static bool load_sensor(void *arg, uint16_t item, uint16_t sub, const uint8_t *d, uint16_t len)
{
    gateway_t *gw = arg;
    if (item != NV_SENSOR || len != 24 || gw->n_sensors == GW_MAX_SENSORS) {
        return true;
    }
    gw_sensor_t *s = &gw->sensors[gw->n_sensors++];
    memcpy(s->mac, d + 4, 8);
    s->short_addr = (uint16_t)(d[2] | d[3] << 8);
    s->nv_sub = sub;
    return true;
}

bool gw_load(gateway_t *gw, nvstore_t *nv)
{
    uint16_t len;
    const uint8_t *net = nv_find(nv, NV_NETWORK, 0, &len);
    gw->nv = nv;
    gw->n_sensors = 0;
    nv_each(nv, load_sensor, gw);
    // nv_each goes newest first: put the table in sub-ID order, as the stock dongle lists it
    for (size_t i = 1; i < gw->n_sensors; i++) {
        for (size_t j = i; j > 0 && gw->sensors[j - 1].nv_sub > gw->sensors[j].nv_sub; j--) {
            gw_sensor_t t = gw->sensors[j];
            gw->sensors[j] = gw->sensors[j - 1];
            gw->sensors[j - 1] = t;
        }
    }
    if (!net || len < 12) {
        return false;
    }
    gw->pan = (uint16_t)(net[0] | net[1] << 8);
    gw->short_addr = (uint16_t)(net[2] | net[3] << 8);
    gw->ext = 0;
    for (int i = 7; i >= 0; i--) {
        gw->ext = gw->ext << 8 | net[4 + i];
    }
    return true;
}

static uint16_t free_sub(const gateway_t *gw)
{
    for (uint16_t sub = 0;; sub++) {
        size_t i = 0;
        while (i < gw->n_sensors && gw->sensors[i].nv_sub != sub) {
            i++;
        }
        if (i == gw->n_sensors) {
            return sub;
        }
    }
}

static gw_sensor_t *sensor_by_mac(gateway_t *gw, const uint8_t mac[8])
{
    for (size_t i = 0; i < gw->n_sensors; i++) {
        if (!memcmp(gw->sensors[i].mac, mac, 8)) {
            return &gw->sensors[i];
        }
    }
    return NULL;
}

bool gw_add_sensor(gateway_t *gw, const uint8_t mac[8], uint16_t short_addr)
{
    gw_sensor_t *s = sensor_by_mac(gw, mac);
    bool added = !s;
    if (!s) {
        if (gw->n_sensors == GW_MAX_SENSORS) {
            return false;
        }
        s = &gw->sensors[gw->n_sensors];
        memcpy(s->mac, mac, 8);
        s->nv_sub = free_sub(gw);
        gw->n_sensors++;
    } else if (s->short_addr == short_addr) {
        return true;  // nothing changed: no flash write
    }
    s->short_addr = short_addr;
    nv_save_sensor(gw, s);
    if (added) {
        nv_save_count(gw);
    }
    return true;
}

static void forget_joining_mac(gateway_t *gw, const uint8_t mac[8])
{
    for (size_t i = 0; i < GW_MAX_JOINING; i++) {
        if (gw->joining[i].used && !memcmp(gw->joining[i].mac, mac, 8)) {
            gw->joining[i].used = false;
        }
    }
}

static void forget_joining_short(gateway_t *gw, uint16_t short_addr)
{
    for (size_t i = 0; i < GW_MAX_JOINING; i++) {
        if (gw->joining[i].used && gw->joining[i].short_addr == short_addr) {
            gw->joining[i].used = false;
        }
    }
}

static void expire_joining(gateway_t *gw)
{
    for (size_t i = 0; i < GW_MAX_JOINING; i++) {
        if (gw->joining[i].used && gw->now_ms - gw->joining[i].at_ms >= GW_PENDING_MS) {
            gw->joining[i].used = false;
        }
    }
}

static void forget_type(gateway_t *gw, uint16_t short_addr);

static uint64_t bit_remove(uint64_t m, size_t idx)
{
    uint64_t lo = idx ? (m & ((1ull << idx) - 1ull)) : 0;
    uint64_t hi = idx + 1 < 64 ? ((m >> (idx + 1)) << idx) : 0;
    return lo | hi;
}

static void remove_sensor(gateway_t *gw, const uint8_t mac[8])
{
    forget_joining_mac(gw, mac);
    gw_sensor_t *s = sensor_by_mac(gw, mac);
    if (!s) {
        return;
    }
    size_t idx = (size_t)(s - gw->sensors);
    uint16_t addr = s->short_addr;
    if (gw->nv) {
        nv_delete(gw->nv, NV_SENSOR, s->nv_sub);
    }
    // nothing queued or learned for it stays: with GW_FLAG_ROAM another gateway may own it now (a handover)
    for (size_t i = 0; i < GW_MAX_PENDING; i++) {
        if (gw->pending[i].used && gw->pending[i].dst == addr) {
            gw->pending[i].used = false;
        }
    }
    forget_joining_short(gw, addr);
    if (gw->ota.wildcard && idx < 64) {
        gw->ota.wildcard_done = bit_remove(gw->ota.wildcard_done, idx);
        gw->ota.wildcard_skip = bit_remove(gw->ota.wildcard_skip, idx);
    }
    memmove(s, s + 1, (size_t)(gw->sensors + gw->n_sensors - (s + 1)) * sizeof *s);
    gw->n_sensors--;
    if (gw->list_active) {
        if (idx < gw->list_cursor && gw->list_cursor > 0) {
            gw->list_cursor--;
        }
        if (idx < gw->list_limit && gw->list_limit > 0) {
            gw->list_limit--;
        }
        if (gw->list_cursor >= gw->list_limit || gw->list_cursor >= gw->n_sensors) {
            gw->list_active = false;
        }
    }
    forget_type(gw, addr);
    nv_save_count(gw);
}

static int type_of(const gateway_t *gw, uint16_t short_addr)
{
    for (size_t i = 0; i < gw->n_types; i++) {
        if (gw->types[i].short_addr == short_addr) {
            return gw->types[i].type;
        }
    }
    return -1;
}

static void set_type(gateway_t *gw, uint16_t short_addr, uint8_t type)
{
    for (size_t i = 0; i < gw->n_types; i++) {
        if (gw->types[i].short_addr == short_addr) {
            gw->types[i].type = type;
            return;
        }
    }
    if (gw->n_types < GW_MAX_SENSORS) {
        gw->types[gw->n_types].short_addr = short_addr;
        gw->types[gw->n_types++].type = type;
    }
}

// A sensor (re)associating on short_addr: what was learned from the one before on that address no longer applies
static void forget_type(gateway_t *gw, uint16_t short_addr)
{
    for (size_t i = 0; i < gw->n_types; i++) {
        if (gw->types[i].short_addr == short_addr) {
            gw->types[i] = gw->types[--gw->n_types];
        }
    }
}

static bool joining(const gateway_t *gw, uint16_t short_addr)
{
    for (size_t i = 0; i < GW_MAX_JOINING; i++) {
        if (gw->joining[i].used && gw->joining[i].short_addr == short_addr) {
            return true;
        }
    }
    return false;
}

// GW_FLAG_ROAM: whether the sensor on short_addr is this gateway's to answer (in its table, or joining it)
static bool owns(const gateway_t *gw, uint16_t short_addr)
{
    for (size_t i = 0; i < gw->n_sensors; i++) {
        if (gw->sensors[i].short_addr == short_addr) {
            return true;
        }
    }
    return joining(gw, short_addr);
}

// The short address of a sensor: one that is joining, else one in the table; -1 if neither.
static int32_t short_of(const gateway_t *gw, const uint8_t mac[8])
{
    for (size_t i = 0; i < GW_MAX_JOINING; i++) {
        if (gw->joining[i].used && !memcmp(gw->joining[i].mac, mac, 8)) {
            return gw->joining[i].short_addr;
        }
    }
    for (size_t i = 0; i < gw->n_sensors; i++) {
        if (!memcmp(gw->sensors[i].mac, mac, 8)) {
            return gw->sensors[i].short_addr;
        }
    }
    return -1;
}

// Frames nobody fetched within GW_PENDING_MS (a sensor that gave up, or polls with another address now) are dropped
static void expire_pending(gateway_t *gw)
{
    for (size_t i = 0; i < GW_MAX_PENDING; i++) {
        if (gw->pending[i].used && gw->now_ms - gw->pending[i].queued_ms >= GW_PENDING_MS) {
            gw->pending[i].used = false;
        }
    }
}

// A frame for dst's next data request. One per destination and kind: a second (a retried association request, a
// host command sent again) replaces the first rather than waiting behind it for a poll that may never come.
static gw_pending_t *queue(gateway_t *gw, uint64_t dst, uint8_t kind)
{
    gw_pending_t *p = NULL;
    expire_pending(gw);
    for (size_t i = 0; i < GW_MAX_PENDING && !p; i++) {
        if (gw->pending[i].used && gw->pending[i].dst == dst && gw->pending[i].kind == kind) {
            p = &gw->pending[i];
        }
    }
    for (size_t i = 0; i < GW_MAX_PENDING && !p; i++) {
        if (!gw->pending[i].used) {
            p = &gw->pending[i];
        }
    }
    if (!p) {
        return NULL;  // full of live frames: this one is dropped, the sensor will ask again (join retries)
    }
    memset(p, 0, sizeof *p);
    p->used = true;
    p->dst = dst;
    p->kind = kind;
    p->order = gw->pending_order++;
    p->queued_ms = gw->now_ms;
    return p;
}

// The oldest pending frame for dst
static gw_pending_t *next_pending(gateway_t *gw, uint64_t dst)
{
    gw_pending_t *oldest = NULL;
    expire_pending(gw);  // a stale frame is never delivered
    for (size_t i = 0; i < GW_MAX_PENDING; i++) {
        gw_pending_t *p = &gw->pending[i];
        if (p->used && p->dst == dst && (!oldest || (int32_t)(p->order - oldest->order) < 0)) {
            oldest = p;
        }
    }
    return oldest;
}

// --- frames out ---

static void send_ack(gateway_t *gw, uint8_t seq, bool pending)
{
    uint8_t ack[3];
    gw->io.tx(gw->io.ctx, ack, mac_build_ack(ack, seq, pending), 0);
}

static void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void send_beacon(gateway_t *gw)
{
    // FC 0x8000: beacon, short source address; superframe spec, no GTS, no pending addresses
    uint8_t b[11] = {0x00, 0x80, gw->bsn++};
    put_le16(b + 3, gw->pan);
    put_le16(b + 5, gw->short_addr);
    put_le16(b + 7, gw->permit ? SUPERFRAME_PERMIT : SUPERFRAME);
    gw->io.tx(gw->io.ctx, b, sizeof b, 0);
}

// a data frame answering a data request (the only kind the gateway sends)
static void send_data(gateway_t *gw, uint16_t dst, const uint8_t *pt, size_t n)
{
    uint8_t ct[APP_MAX_PAYLOAD], f[9 + APP_MAX_PAYLOAD];
    size_t len = app_encrypt(pt, n, ct);
    gw->io.tx(gw->io.ctx, f, mac_build_data(f, gw->dsn++, gw->pan, dst, gw->short_addr, ct, len, true),
              POLL_REPLY_GAP_US);
}

static void send_pending(gateway_t *gw, gw_pending_t *p)
{
    uint8_t pt[APP_MAX_PAYLOAD], f[32];
    switch (p->kind) {
        case PEND_ASSOC_RESP:
            // FC 0xCC63: command, ack request, PAN-ID compression, extended -> extended
            f[0] = 0x63, f[1] = 0xCC, f[2] = gw->dsn++;
            put_le16(f + 3, gw->pan);
            memcpy(f + 5, p->mac, 8);  // the sensor's extended address = its MAC
            for (int i = 0; i < 8; i++) {
                f[13 + i] = (uint8_t)(gw->ext >> (8 * i));
            }
            f[21] = MAC_CMD_ASSOC_RESP;
            put_le16(f + 22, p->short_addr);
            f[24] = 0;  // status: success
            gw->io.tx(gw->io.ctx, f, 25, POLL_REPLY_GAP_US);
            break;
        case PEND_JOIN_TOKEN:
            send_data(gw, (uint16_t)p->dst, pt, app_build_join_token(pt, p->data));
            break;
        case PEND_JOIN_DONE: {
            size_t n = app_build_join_done(pt, p->data, gw->clock_ms);
            if (gw->flags & GW_FLAG_ANNOUNCE) {
                n = app_mark_ours(pt, n, gw->flags & GW_FLAG_OTA ? A6_OURS_OTA : 0);
            }
            send_data(gw, (uint16_t)p->dst, pt, n);
            break;
        }
        default:
            break;
    }
    p->used = false;
}

static bool has_free_unacked(const gateway_t *gw)
{
    for (size_t i = 0; i < GW_MAX_UNACKED; i++) {
        if (!gw->unacked[i].used) {
            return true;
        }
    }
    return false;
}

static void host_send(gateway_t *gw, uint16_t cmd, const uint8_t *payload, size_t n)
{
    uint8_t pkt[HOST_MAX_PACKET];
    size_t len = host_build(pkt, cmd, payload, n, true);
    if (!len) {
        return;
    }
    gw->io.host(gw->io.ctx, pkt, len);
    if (cmd >> 8 != HOST_ASYNC || len > sizeof gw->unacked[0].pkt) {
        return;
    }
    for (size_t i = 0; i < GW_MAX_UNACKED; i++) {  // keep it for resending until ACKed
        if (gw->unacked[i].used) {
            continue;
        }
        gw->unacked[i].used = true;
        gw->unacked[i].sends = 1;
        gw->unacked[i].order = gw->unacked_order++;
        gw->unacked[i].next_ms = gw->now_ms + HOST_RESEND_MS;
        gw->unacked[i].len = len;
        memcpy(gw->unacked[i].pkt, pkt, len);
        return;
    }
}

static void ows_send(gateway_t *gw, uint8_t sub_id, const uint8_t *payload, size_t n)
{
    uint8_t buf[OWS_MAX_PAYLOAD];
    if (1 + n > sizeof buf) {
        return;
    }
    buf[0] = sub_id;
    if (n) {
        memcpy(buf + 1, payload, n);
    }
    host_send(gw, HOST_OWS_RSP, buf, 1 + n);
}

static void pump_sensor_list(gateway_t *gw)
{
    while (gw->list_active && gw->list_cursor < gw->list_limit && gw->list_cursor < gw->n_sensors &&
           has_free_unacked(gw)) {
        host_send(gw, HOST_SENSOR_LIST_ITEM, gw->sensors[gw->list_cursor++].mac, 8);
    }
    if (gw->list_cursor >= gw->list_limit || gw->list_cursor >= gw->n_sensors) {
        gw->list_active = false;
    }
}

void gw_tick(gateway_t *gw, uint32_t now_ms)
{
    gw->now_ms = now_ms;
    if (gw->beacon_due && (int32_t)(now_ms - gw->beacon_at_ms) >= 0) {
        gw->beacon_due = false;
        send_beacon(gw);  // with the permit as it is now
    }
    expire_joining(gw);
    if (gw->time_req_armed && (int32_t)(now_ms - gw->next_time_req_ms) >= 0) {
        gw->next_time_req_ms = now_ms + TIME_REQ_EVERY_MS;
        host_send(gw, HOST_TIME_SYNC_REQ, NULL, 0);
    }
    for (;;) {
        size_t oldest = GW_MAX_UNACKED;
        for (size_t i = 0; i < GW_MAX_UNACKED; i++) {
            if (!gw->unacked[i].used || (int32_t)(now_ms - gw->unacked[i].next_ms) < 0) {
                continue;
            }
            if (oldest == GW_MAX_UNACKED || (int32_t)(gw->unacked[i].order - gw->unacked[oldest].order) < 0) {
                oldest = i;
            }
        }
        if (oldest == GW_MAX_UNACKED) {
            break;
        }
        gw->io.host(gw->io.ctx, gw->unacked[oldest].pkt, gw->unacked[oldest].len);
        gw->unacked[oldest].next_ms = now_ms + HOST_RESEND_MS;
        if (++gw->unacked[oldest].sends == HOST_SENDS) {
            gw->unacked[oldest].used = false;
        }
    }
    pump_sensor_list(gw);
}

// --- air ---

static bool short_in_use(const gateway_t *gw, uint16_t addr)
{
    if (addr == 0 || addr > 0xFFFDu || addr == gw->short_addr) {
        return true;
    }
    for (size_t i = 0; i < gw->n_sensors; i++) {
        if (gw->sensors[i].short_addr == addr) {
            return true;
        }
    }
    for (size_t i = 0; i < GW_MAX_JOINING; i++) {
        if (gw->joining[i].used && gw->joining[i].short_addr == addr) {
            return true;
        }
    }
    return false;
}

// The next free address after the highest one in use, within addr_first..addr_last (0x0001-0xFFFD unless the host
// set a range: OWS_SUB_ADDR_RANGE, so that gateways sharing a network never hand out the same one), wrapping round
static int32_t alloc_short_addr(const gateway_t *gw)
{
    uint16_t first = gw->addr_first, last = gw->addr_last;
    uint32_t max_short = 0;
    for (size_t i = 0; i < gw->n_sensors; i++) {
        uint16_t a = gw->sensors[i].short_addr;
        if (a >= first && a <= last && a > max_short) {
            max_short = a;
        }
    }
    uint16_t cand = (uint16_t)(max_short == 0 || max_short >= last ? first : max_short + 1u);
    for (uint32_t tries = 0; tries <= (uint32_t)(last - first); tries++) {
        if (!short_in_use(gw, cand)) {
            return cand;
        }
        cand = cand >= last ? first : (uint16_t)(cand + 1u);
    }
    return -1;
}

static void associate(gateway_t *gw, uint64_t ext)
{
    uint8_t mac[8];
    int32_t s;
    gw_pending_t *p;
    expire_joining(gw);
    for (int i = 0; i < 8; i++) {
        mac[i] = (uint8_t)(ext >> (8 * i));
    }
    s = short_of(gw, mac);
    forget_joining_mac(gw, mac);
    if (s <= 0 || s > 0xFFFD || (uint16_t)s == gw->short_addr) {
        s = -1;
    }
    // its old address, unless another sensor in the table has it too (a stock table can: known-issues.md §2; once
    // two of ours then shared 0x0003, 2026-10-24): then a new one, so no two sensors share an address
    for (size_t i = 0; s >= 0 && i < gw->n_sensors; i++) {
        if (gw->sensors[i].short_addr == s && memcmp(gw->sensors[i].mac, mac, 8) != 0) {
            s = -1;
        }
    }
    if (s < 0) {  // new sensor: next free address in 0x0001..0xFFFD (stock monotonic, wrapping collision-free)
        s = alloc_short_addr(gw);
        if (s < 0) {
            return;
        }
    }
    forget_joining_short(gw, (uint16_t)s);
    if (!gw_add_sensor(gw, mac, (uint16_t)s)) {
        return;
    }
    if (gw->ota.staged) {  // a staged sensor joining again: pause offering to it until its new handshake completes
        for (size_t i = 0; i < GW_MAX_OTA_TARGETS; i++) {
            if (gw->ota.targets[i].used && !memcmp(gw->ota.targets[i].mac, mac, 8)) {
                gw->ota.targets[i].associating = true;
            }
        }
        if (gw->ota.active_src && !memcmp(gw->ota.active_mac, mac, 8)) {
            gw->ota.active_src = 0;
            gw->ota.started = false;
            gw->ota.want_len = gw->ota.filled = 0;
        }
    }
    forget_type(gw, (uint16_t)s);  // a new join: no B0 for it until it reports again (as the stock dongle)
    p = queue(gw, ext, PEND_ASSOC_RESP);
    if (!p) {
        return;
    }
    p->short_addr = (uint16_t)s;
    memcpy(p->mac, mac, 8);
}

// GW_FLAG_DEDUP: whether this report (same sensor, same plaintext) went to the host lately; remembers it if not
static bool seen_lately(gateway_t *gw, uint16_t src, const uint8_t *pt, size_t n)
{
    uint32_t crc = crc32_ieee(pt, n);
    size_t slot = 0;
    for (size_t i = 0; i < GW_DEDUP_ENTRIES; i++) {
        if (gw->seen[i].used && gw->seen[i].short_addr == src && gw->seen[i].crc == crc &&
            gw->now_ms - gw->seen[i].at_ms < GW_DEDUP_MS) {
            return true;
        }
    }
    for (size_t i = 0; i < GW_DEDUP_ENTRIES; i++) {  // a free entry, else the oldest
        if (!gw->seen[i].used) {
            slot = i;
            break;
        }
        if ((int32_t)(gw->seen[i].at_ms - gw->seen[slot].at_ms) < 0) {
            slot = i;
        }
    }
    gw->seen[slot].used = true;
    gw->seen[slot].short_addr = src;
    gw->seen[slot].crc = crc;
    gw->seen[slot].at_ms = gw->now_ms;
    return false;
}

// GW_FLAG_FIX_ZERO_MAC: the MAC of the only sensor in the table on short address src, if there is exactly one
static const uint8_t *only_mac_on(const gateway_t *gw, uint16_t src)
{
    static const uint8_t zero[8];
    const uint8_t *mac = NULL;
    for (size_t i = 0; i < gw->n_sensors; i++) {
        if (gw->sensors[i].short_addr != src) {
            continue;
        }
        if (mac) {
            return NULL;  // two radios on one address (known-issues.md §2)
        }
        mac = gw->sensors[i].mac;
    }
    return mac && memcmp(mac, zero, 8) != 0 ? mac : NULL;
}

// --- OTA (common/ota.h) ---

static bool ota_has_targets(const gateway_t *gw)
{
    for (size_t i = 0; i < GW_MAX_OTA_TARGETS; i++) {
        if (gw->ota.targets[i].used) {
            return true;
        }
    }
    return false;
}

static bool ota_is_wildcard_mac(const uint8_t mac[8])
{
    static const uint8_t zero[8];
    return !memcmp(mac, "********", 8) || !memcmp(mac, zero, 8);
}

static void ota_clear_active(gateway_t *gw)
{
    gw->ota.active_src = 0;
    gw->ota.started = false;
    gw->ota.want_len = gw->ota.filled = 0;
}

// Whether src is an eligible joined sensor in the staged wave; sets mac_out (8 bytes) when matched.
static bool ota_target_for(const gateway_t *gw, uint16_t src, uint8_t mac_out[8])
{
    static const uint8_t zero[8];
    if (!gw->ota.staged || joining(gw, src)) {
        return false;
    }
    for (size_t i = 0; i < GW_MAX_OTA_TARGETS; i++) {
        if (gw->ota.targets[i].used && !gw->ota.targets[i].associating && short_of(gw, gw->ota.targets[i].mac) == src) {
            if (mac_out) {
                memcpy(mac_out, gw->ota.targets[i].mac, 8);
            }
            return true;
        }
    }
    if (!gw->ota.wildcard) {
        return false;
    }
    size_t match = GW_MAX_SENSORS;
    for (size_t i = 0; i < gw->n_sensors; i++) {
        if (gw->sensors[i].short_addr == src) {
            if (match != GW_MAX_SENSORS) {
                return false;  // two sensors sharing one stock short_addr: require explicit MAC target
            }
            match = i;
        }
    }
    uint64_t skip = (gw->now_ms - gw->ota.wildcard_skip_ms < 15000u) ? gw->ota.wildcard_skip : 0;
    if (match >= GW_MAX_SENSORS || !memcmp(gw->sensors[match].mac, zero, 8) ||
        ((gw->ota.wildcard_done | skip) & (1ull << match)) != 0) {
        return false;
    }
    if (mac_out) {
        memcpy(mac_out, gw->sensors[match].mac, 8);
    }
    return true;
}

// Serializes the single 96-byte block buffer to one active sensor at a time; yields if the active sensor goes silent.
static bool ota_acquire_for(gateway_t *gw, uint16_t src, uint8_t mac_out[8])
{
    if (!ota_target_for(gw, src, mac_out)) {
        return false;
    }
    if (gw->ota.active_src != 0 && gw->ota.active_src != src) {
        uint32_t limit = gw->ota.started ? GW_OTA_IDLE_MS : GW_OTA_OFFER_MS;
        if (gw->now_ms - gw->ota.active_ms < limit) {
            return false;
        }
        ota_clear_active(gw);
    }
    return true;
}

static void ota_finish_target(gateway_t *gw, uint16_t src, const uint8_t mac[8], uint8_t status)
{
    for (size_t i = 0; i < GW_MAX_OTA_TARGETS; i++) {
        if (gw->ota.targets[i].used &&
            (!memcmp(gw->ota.targets[i].mac, mac, 8) || short_of(gw, gw->ota.targets[i].mac) == src)) {
            gw->ota.targets[i].used = false;
        }
    }
    if (gw->ota.wildcard) {
        for (size_t i = 0; i < gw->n_sensors && i < 64; i++) {
            if (gw->sensors[i].short_addr == src || !memcmp(gw->sensors[i].mac, mac, 8)) {
                if (status == OTA_DONE_OK) {
                    gw->ota.wildcard_done |= 1ull << i;
                } else {
                    gw->ota.wildcard_skip |= 1ull << i;
                    gw->ota.wildcard_skip_ms = gw->now_ms;
                }
            }
        }
    }
    if (gw->ota.active_src == src || !memcmp(gw->ota.active_mac, mac, 8)) {
        ota_clear_active(gw);
    }
    gw->ota.staged = gw->ota.wildcard || ota_has_targets(gw);
}

// C1: the sensor wants the block at an offset of a slot build; ask the host for it, unless we have it (a repeat)
static void ota_request(gateway_t *gw, uint16_t src, const uint8_t *pt)
{
    uint8_t need[14], mac[8], slot = pt[9];
    uint32_t off = ota_be32(pt + 10);
    if (!ota_acquire_for(gw, src, mac) || slot > 1) {
        return;
    }
    uint32_t total = SLOT_IMAGE_OFFSET + ota_be32(gw->ota.offer + 8 + (size_t)8 * slot);
    if (off >= total) {
        return;
    }
    bool same_sensor = gw->ota.active_src == src && gw->ota.started;
    gw->ota.active_src = src;
    memcpy(gw->ota.active_mac, mac, 8);
    gw->ota.active_ms = gw->now_ms;
    gw->ota.started = true;
    if (same_sensor && off == gw->ota.want && slot == gw->ota.want_slot && gw->ota.want_len &&
        gw->ota.filled == gw->ota.want_len) {
        return;
    }
    gw->ota.want = off;
    gw->ota.want_slot = slot;
    gw->ota.want_len = (uint8_t)(total - off < OTA_BLOCK ? total - off : OTA_BLOCK);
    gw->ota.filled = 0;
    memcpy(need, mac, 8);
    need[8] = slot;
    ota_put_be32(need + 9, off);
    need[13] = gw->ota.want_len;
    ows_send(gw, OWS_SUB_OTA_NEED, need, sizeof need);
}

// A data request from the sensor being updated: its offer, or the block it asked for once the host sent all of it.
// Returns whether a frame went.
static bool ota_poll(gateway_t *gw, uint16_t src, uint8_t seq)
{
    uint8_t pt[6 + OTA_BLOCK], mac[8];
    if (!ota_acquire_for(gw, src, mac)) {
        return false;
    }
    if (gw->ota.active_src != src || !gw->ota.started) {
        gw->ota.active_src = src;
        memcpy(gw->ota.active_mac, mac, 8);
        gw->ota.active_ms = gw->now_ms;
        gw->ota.started = false;
        gw->ota.want_len = gw->ota.filled = 0;
        pt[0] = APP_OTA_OFFER;
        memcpy(pt + 1, gw->ota.offer, OTA_OFFER_LEN);
        send_ack(gw, seq, true);
        send_data(gw, src, pt, 1 + OTA_OFFER_LEN);
        return true;
    }
    if (!gw->ota.want_len || gw->ota.filled != gw->ota.want_len) {
        return false;
    }
    pt[0] = APP_OTA_BLOCK;
    ota_put_be32(pt + 1, gw->ota.want);
    pt[5] = gw->ota.want_len;
    memcpy(pt + 6, gw->ota.block, gw->ota.want_len);
    send_ack(gw, seq, true);
    send_data(gw, src, pt, 6u + gw->ota.want_len);
    return true;
}

// OWS_SUB_OTA_OFFER (0x53F0 0x10): stage an update for one/many sensors (or wildcard "********"), or cancel (size_a=0)
static void ota_offer_command(gateway_t *gw, const uint8_t *payload, size_t len)
{
    uint8_t st = OTA_HOST_REFUSED;
    bool ok = len == 8 + OTA_OFFER_LEN;
    uint32_t size_a = ok ? ota_be32(payload + 16) : 0, size_b = ok ? ota_be32(payload + 24) : 0;
    if ((gw->flags & GW_FLAG_OTA) && ok && size_a <= SLOT_SIZE - SLOT_IMAGE_OFFSET &&
        size_b <= SLOT_SIZE - SLOT_IMAGE_OFFSET) {
        const uint8_t *mac = payload;
        bool wild = ota_is_wildcard_mac(mac);
        if (size_a == 0) {
            if (wild) {
                memset(&gw->ota, 0, sizeof gw->ota);
            } else {
                for (size_t i = 0; i < GW_MAX_OTA_TARGETS; i++) {
                    if (gw->ota.targets[i].used && !memcmp(gw->ota.targets[i].mac, mac, 8)) {
                        gw->ota.targets[i].used = false;
                    }
                }
                if (gw->ota.active_src && !memcmp(gw->ota.active_mac, mac, 8)) {
                    ota_clear_active(gw);
                }
                gw->ota.staged = gw->ota.wildcard || ota_has_targets(gw);
            }
            st = 0;
        } else {
            if (!gw->ota.staged || memcmp(gw->ota.offer, payload + 8, OTA_OFFER_LEN) != 0) {
                memset(&gw->ota, 0, sizeof gw->ota);
                memcpy(gw->ota.offer, payload + 8, OTA_OFFER_LEN);
            }
            if (wild) {
                gw->ota.wildcard = true;
                gw->ota.wildcard_done = 0;
                gw->ota.wildcard_skip = 0;
                gw->ota.staged = true;
                st = 0;
            } else {
                size_t slot = GW_MAX_OTA_TARGETS;
                for (size_t i = 0; i < GW_MAX_OTA_TARGETS; i++) {
                    if (gw->ota.targets[i].used && !memcmp(gw->ota.targets[i].mac, mac, 8)) {
                        slot = i;
                        break;
                    }
                }
                for (size_t i = 0; slot == GW_MAX_OTA_TARGETS && i < GW_MAX_OTA_TARGETS; i++) {
                    if (!gw->ota.targets[i].used) {
                        slot = i;
                    }
                }
                if (slot < GW_MAX_OTA_TARGETS) {
                    gw->ota.targets[slot].used = true;
                    gw->ota.targets[slot].associating = false;
                    memcpy(gw->ota.targets[slot].mac, mac, 8);
                    if (gw->ota.active_src && !memcmp(gw->ota.active_mac, mac, 8)) {
                        ota_clear_active(gw);
                    }
                    gw->ota.staged = true;
                    st = 0;
                }
            }
        }
    }
    ows_send(gw, OWS_SUB_OTA_OFFER, &st, 1);
}

// OWS_SUB_OTA_DATA (0x53F0 0x12): the next piece of the block asked for
static void ota_data_command(gateway_t *gw, const uint8_t *payload, size_t len)
{
    if (!gw->ota.staged || !gw->ota.started || len < 5 || ota_be32(payload) != gw->ota.want + gw->ota.filled) {
        return;
    }
    size_t n = len - 4;
    if (n > (size_t)(gw->ota.want_len - gw->ota.filled)) {
        return;
    }
    memcpy(gw->ota.block + gw->ota.filled, payload + 4, n);
    gw->ota.filled = (uint8_t)(gw->ota.filled + n);
}

static void sensor_message(gateway_t *gw, uint16_t src, const uint8_t *pt, size_t n, int8_t rssi)
{
    static const uint8_t zero[8];
    uint8_t payload[HOST_MAX_PACKET], fixed[APP_MAX_PAYLOAD];
    switch (pt[0]) {
        case APP_STATUS:
        case APP_EVENT:
        case APP_CLIMATE:
        case APP_MOTION_SUMMARY: {
            size_t len;
            // byte 13 is the data length; byte 12 the low-battery flag (FACTS.md: 0x01 once a stock sensor latched
            // it), not a length's high byte: read as one, it dropped exactly the reports of a sensor with a low battery
            size_t need = pt[0] == APP_MOTION_SUMMARY ? 23 : 16 + (size_t)pt[13];
            if (need > n) {
                break;
            }
            // an A1 from a sensor (its check-in after a short press, maybe after refusing a wildcard offer before its
            // button was pressed or resetting half-way): re-enable wildcard eligibility and re-offer on its next poll
            if (pt[0] == APP_STATUS && gw->ota.staged) {
                if (gw->ota.wildcard) {
                    for (size_t i = 0; i < gw->n_sensors && i < 64; i++) {
                        if (gw->sensors[i].short_addr == src) {
                            gw->ota.wildcard_skip &= ~(1ull << i);
                        }
                    }
                }
                if (gw->ota.active_src == src && ota_target_for(gw, src, NULL)) {
                    gw->ota.started = false;
                    gw->ota.want_len = gw->ota.filled = 0;
                }
            }
            if (pt[0] != APP_MOTION_SUMMARY) {
                set_type(gw, src, pt[9]);
            }
            if ((gw->flags & GW_FLAG_DEDUP) && seen_lately(gw, src, pt, n)) {
                break;
            }
            if ((gw->flags & GW_FLAG_FIX_ZERO_MAC) && n >= 9 && n <= sizeof fixed && !memcmp(pt + 1, zero, 8)) {
                const uint8_t *mac = only_mac_on(gw, src);
                if (mac) {
                    memcpy(fixed, pt, n);
                    memcpy(fixed + 1, mac, 8);
                    pt = fixed;
                }
            }
            len = host_alarm_payload(payload, pt, n, gw->clock_ms, (uint8_t)-rssi);
            if (len) {
                host_send(gw, HOST_ALARM, payload, len);
            }
            break;
        }
        case APP_HISTORY_LOG: {  // AD: stock v1 50-entry offline missed-event record -> synthetic A2 HOST_ALARM
            if (n < 22 || pt[18] != APP_EVENT) {
                break;
            }
            set_type(gw, src, pt[9]);
            if ((gw->flags & GW_FLAG_DEDUP) && seen_lately(gw, src, pt, 22)) {
                break;
            }
            const uint8_t *mac = pt + 1;
            if ((gw->flags & GW_FLAG_FIX_ZERO_MAC) && !memcmp(mac, zero, 8)) {
                const uint8_t *m = only_mac_on(gw, src);
                if (m) {
                    mac = m;
                }
            }
            uint64_t ts = 0;
            for (int i = 0; i < 8; i++) {
                ts = ts << 8 | pt[10 + i];
            }
            if (ts < 1000000000000ull || (gw->clock_ms != 0 && ts > gw->clock_ms)) {
                ts = gw->clock_ms;  // sensor clock was unsynced (or ahead of gateway): rebase onto gateway clock
            }
            if (gw->history_seen && ts == gw->last_history_ms) {
                gw->history_tie++;
                ts += gw->history_tie;
            } else {
                gw->history_seen = true;
                gw->history_tie = 0;
                gw->last_history_ms = ts;
            }
            uint8_t ev[17] = {APP_EVENT};
            memcpy(ev + 1, mac, 8);
            ev[9] = pt[9];
            ev[13] = 1;
            ev[14] = pt[19];
            ev[15] = pt[20];
            ev[16] = pt[21];
            size_t len = host_alarm_payload(payload, ev, sizeof ev, ts, (uint8_t)-rssi);
            if (len) {
                host_send(gw, HOST_ALARM, payload, len);
            }
            break;
        }
        case APP_DECOMM_ACK:
            break;
        case APP_JOIN_ANNOUNCE: {  // A3 -> scan result: A3 plaintext bytes 0-10
            // only while pairing, as stock (FACTS.md 0x5320: gated by permitJoinActive): a gateway sharing the PAN
            // with roaming off hears the A3 a pairing gateway's sensor sends, and must not pair it too (#60)
            if (n < 11 || !gw->permit) {
                break;
            }
            expire_joining(gw);
            set_type(gw, src, pt[9]);
            if (gw->ota.staged) {
                for (size_t i = 0; i < GW_MAX_OTA_TARGETS; i++) {
                    if (gw->ota.targets[i].used && !memcmp(gw->ota.targets[i].mac, pt + 1, 8)) {
                        gw->ota.targets[i].associating = false;  // joining now
                    }
                }
            }
            size_t slot = GW_MAX_JOINING;
            for (size_t i = 0; i < GW_MAX_JOINING; i++) {
                if (gw->joining[i].used &&
                    (gw->joining[i].short_addr == src || !memcmp(gw->joining[i].mac, pt + 1, 8))) {
                    slot = i;
                    break;
                }
            }
            for (size_t i = 0; slot == GW_MAX_JOINING && i < GW_MAX_JOINING; i++) {
                if (!gw->joining[i].used) {
                    slot = i;
                }
            }
            if (slot == GW_MAX_JOINING) {  // all 4 active within 30 s: evict oldest so pairing never wedges
                slot = 0;
                for (size_t i = 1; i < GW_MAX_JOINING; i++) {
                    if ((int32_t)(gw->joining[i].at_ms - gw->joining[slot].at_ms) < 0) {
                        slot = i;
                    }
                }
            }
            gw->joining[slot].used = true;
            gw->joining[slot].short_addr = src;
            memcpy(gw->joining[slot].mac, pt + 1, 8);
            gw->joining[slot].at_ms = gw->now_ms;
            host_send(gw, HOST_SCAN_RESULT, pt, 11);
            break;
        }
        case APP_JOIN_RESPONSE:  // A5 -> reply to Get Sensor R1: A5 plaintext bytes 0-26 (FACTS.md §9)
            if (n < 27) {
                break;
            }
            host_send(gw, HOST_GET_R1 + 1, pt, 27);
            break;
        case APP_OTA_REQUEST:
            if (n < 14) {
                break;
            }
            ota_request(gw, src, pt);
            break;
        case APP_OTA_DONE: {
            uint8_t mac[8];
            if (n < 14 || !ota_target_for(gw, src, mac)) {
                break;
            }
            memcpy(payload, pt + 1, 13);  // MAC, status, version
            ows_send(gw, OWS_SUB_OTA_DONE, payload, 13);
            ota_finish_target(gw, src, mac, pt[9]);
            break;
        }
        default:
            break;
    }
}

// GW_FLAG_ROAM: a report from a sensor another gateway owns, heard but not ACKed, for the host to compare signal
// strengths and to fill in what the owner missed. Best effort: sent once, not kept for resending (it would crowd out
// the gateway's own async packets).
static void overheard(gateway_t *gw, uint16_t src, const uint8_t *pt, size_t n, int8_t rssi)
{
    uint8_t buf[OWS_MAX_PAYLOAD], pkt[HOST_MAX_PACKET];
    size_t len;
    if (pt[0] != APP_STATUS && pt[0] != APP_EVENT && pt[0] != APP_CLIMATE && pt[0] != APP_MOTION_SUMMARY) {
        return;
    }
    if ((gw->flags & GW_FLAG_DEDUP) && seen_lately(gw, src, pt, n)) {
        return;
    }
    if (n < 14 || 9 + (pt[0] == APP_MOTION_SUMMARY ? 23 : 16 + (size_t)pt[13]) > sizeof buf - 3) {  // 12: low battery
        return;  // too long for one packet (none of the stock reports is)
    }
    len = host_alarm_payload(buf + 3, pt, n, gw->clock_ms, (uint8_t)-rssi);
    if (!len) {
        return;
    }
    buf[0] = OWS_SUB_OVERHEARD;
    put_le16(buf + 1, src);
    len = host_build(pkt, HOST_OWS_RSP, buf, 3 + len, true);
    if (len) {
        gw->io.host(gw->io.ctx, pkt, len);
    }
}

void gw_rx(gateway_t *gw, const uint8_t *frame, size_t len, int8_t rssi)
{
    mac_frame_t m;
    if (!mac_parse(frame, len, &m)) {
        return;
    }
    if (m.type == MAC_CMD && m.cmd == MAC_CMD_BEACON_REQ) {
        if (!gw->beacon_due) {  // answered by gw_tick, after a random delay (BEACON_*)
            gw->rng ^= (uint32_t)(uint8_t)rssi << 24 ^ gw->now_ms;  // two gateways' sequences drift apart
            uint32_t r = next_random(gw);
            gw->beacon_at_ms = gw->now_ms + (gw->permit ? BEACON_PERMIT_MS + r % BEACON_PERMIT_SPAN
                                                        : BEACON_NO_PERMIT_MS + r % BEACON_NO_PERMIT_SPAN);
            gw->beacon_due = true;
        }
        return;
    }
    if (!m.dst_mode || m.dst_pan != gw->pan || m.dst != gw->short_addr) {
        return;
    }
    // GW_FLAG_ROAM: frames from a sensor that is not ours are another gateway's to ACK and answer (two ACKs at once
    // would collide); associations are the pairing gateway's
    bool roam = gw->flags & GW_FLAG_ROAM;
    bool ours = !roam || (m.src_mode == MAC_ADDR_SHORT && owns(gw, (uint16_t)m.src));

    if (m.type == MAC_CMD && m.cmd == MAC_CMD_ASSOC_REQ) {
        if (roam && !gw->permit) {
            return;
        }
        send_ack(gw, m.seq, false);
        if (gw->permit && m.src_mode == MAC_ADDR_EXT) {
            associate(gw, m.src);
        }
    } else if (m.type == MAC_CMD && m.cmd == MAC_CMD_DATA_REQ) {
        gw_pending_t *p = next_pending(gw, m.src);
        bool short_src = m.src_mode == MAC_ADDR_SHORT;
        if (!ours && !p) {  // (a sensor still associating polls with its extended address: ours if we queued for it)
            return;
        }
        if (!p && short_src && ota_poll(gw, (uint16_t)m.src, m.seq)) {
            return;
        }
        // a sensor we have reports from gets the time, unless it is a v2 contact or motion sensor (FACTS.md §3), or
        // is still joining (its A3 sets its type; a B0 then would break its handshake)
        int type = m.src_mode == MAC_ADDR_SHORT ? type_of(gw, (uint16_t)m.src) : -1;
        bool time_sync = !p && type >= 0 && type != SENSOR_CONTACT_V2 && type != SENSOR_MOTION_V2 &&
                         !joining(gw, (uint16_t)m.src) && !(short_src && ota_target_for(gw, (uint16_t)m.src, NULL));
        send_ack(gw, m.seq, p || time_sync);
        if (p) {
            send_pending(gw, p);
        } else if (time_sync) {
            uint8_t pt[APP_MAX_PAYLOAD];
            send_data(gw, (uint16_t)m.src, pt, app_build_time_sync(pt, gw->clock_ms));
        }
    } else if (m.type == MAC_DATA && m.ack_req) {
        uint8_t pt[APP_MAX_PAYLOAD];
        if (ours) {
            send_ack(gw, m.seq, false);
        }
        if (m.src_mode != MAC_ADDR_SHORT || !m.payload_len || m.payload_len > sizeof pt || m.payload_len % 16) {
            return;
        }
        aes128_ecb_decrypt(app_key, m.payload, pt, m.payload_len);
        if (ours) {
            sensor_message(gw, (uint16_t)m.src, pt, m.payload_len, rssi);
        } else {
            overheard(gw, (uint16_t)m.src, pt, m.payload_len, rssi);
        }
    }
}

// --- host ---

// OWS_SUB_CCFG (gateway.h): two steps, both bound to the magic and this chip's address
static void ccfg_command(gateway_t *gw, const uint8_t *payload, size_t len)
{
    static const uint8_t status_of[] = {GW_CCFG_OK, GW_CCFG_ALREADY_RIGHT, GW_CCFG_NOT_SAFE, GW_CCFG_FAILED};
    uint8_t out[6] = {0, GW_CCFG_REFUSED};
    size_t n = 2;
    uint64_t ieee = 0;
    bool ok = gw->io.ccfg && len >= 25 && !memcmp(payload, GW_CCFG_MAGIC, 16) && (payload[16] == 1 || payload[16] == 2);
    if (ok) {
        for (int i = 0; i < 8; i++) {
            ieee = ieee << 8 | payload[17 + i];
        }
        ok = ieee == gw->chip_ieee;
    }
    if (ok) {
        out[0] = payload[16];
    }
    if (ok && payload[16] == 1) {
        int st = gw->io.ccfg(gw->io.ctx, false);
        if (st) {
            out[1] = status_of[st & 3];
        } else {  // armed: the nonce the confirm must carry
            uint8_t seed[16];
            memcpy(seed, &gw->now_ms, 4);
            memcpy(seed + 4, &gw->chip_ieee, 8);
            memcpy(seed + 12, &gw->ccfg_nonce, 4);
            gw->ccfg_nonce = crc32_ieee(seed, sizeof seed) | 1;
            gw->ccfg_until = gw->now_ms + GW_CCFG_ARM_MS;
            gw->ccfg_armed = true;
            out[1] = GW_CCFG_OK;
            for (int i = 0; i < 4; i++) {
                out[2 + i] = (uint8_t)(gw->ccfg_nonce >> (24 - 8 * i));
            }
            n = 6;
        }
    } else if (ok) {
        uint32_t nonce = 0;
        for (int i = 0; i < 4 && len >= 29; i++) {
            nonce = nonce << 8 | payload[25 + i];
        }
        if (gw->ccfg_armed && len >= 29 && nonce == gw->ccfg_nonce && (int32_t)(gw->ccfg_until - gw->now_ms) > 0) {
            out[1] = status_of[gw->io.ccfg(gw->io.ctx, true) & 3];
        }
        gw->ccfg_armed = false;  // one confirm per arm, whatever it was
    }
    ows_send(gw, OWS_SUB_CCFG, out, n);
}

static void flags_apply(gateway_t *gw, const uint8_t *payload, size_t len, uint8_t out[8])
{
    if (len >= 4) {
        gw->flags = (uint32_t)payload[0] << 24 | (uint32_t)payload[1] << 16 | (uint32_t)payload[2] << 8 | payload[3];
        gw->flags &= GW_FLAGS_SUPPORTED;
        memset(gw->seen, 0, sizeof gw->seen);
        if (!(gw->flags & GW_FLAG_OTA)) {
            memset(&gw->ota, 0, sizeof gw->ota);
        }
    }
    for (int i = 0; i < 4; i++) {
        out[i] = (uint8_t)(gw->flags >> (24 - 8 * i));
        out[4 + i] = (uint8_t)((uint32_t)GW_FLAGS_SUPPORTED >> (24 - 8 * i));
    }
}

// OWS_SUB_BUILD_INFO (0x53F0 0x02): split into <= 55 B pages so each fits CH554 USB-HID (<= 56 B) and unacked[64]
static void build_info_command(gateway_t *gw, const uint8_t *payload, size_t len)
{
    uint8_t part = len ? payload[0] : 0;
    uint8_t out[OWS_MAX_PAYLOAD];
    if (!gw->build_info || gw->build_info_len < sizeof(build_info_t) || part > 2) {
        ows_send(gw, OWS_SUB_BUILD_INFO, NULL, 0);
        return;
    }
    const uint8_t *raw = gw->build_info;
    if (part == 0) {
        memcpy(out, raw, 40);            // header(8) + id(8) + product(24)
        memcpy(out + 40, raw + 64, 12);  // app(12)
        ows_send(gw, OWS_SUB_BUILD_INFO, out, OWS_BUILD_INFO_P0_LEN - 1);
    } else if (part == 1) {
        memcpy(out, raw + 8, 8);         // id(8)
        memcpy(out + 8, raw + 40, 24);   // board(24)
        memcpy(out + 32, raw + 76, 22);  // version[0..21](22)
        ows_send(gw, OWS_SUB_BUILD_INFO, out, OWS_BUILD_INFO_P1_LEN - 1);
    } else {
        memcpy(out, raw + 8, 8);        // id(8)
        memcpy(out + 8, raw + 98, 18);  // version[22..39](18)
        ows_send(gw, OWS_SUB_BUILD_INFO, out, OWS_BUILD_INFO_P2_LEN - 1);
    }
}

static bool normalize_mac(const uint8_t in[8], uint8_t out[8])
{
    for (int i = 0; i < 8; i++) {
        uint8_t c = in[i];
        if (c >= 'a' && c <= 'f') {
            c = (uint8_t)(c - 'a' + 'A');
        }
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) {
            return false;
        }
        out[i] = c;
    }
    return memcmp(out, "00000000", 8) != 0;
}

static uint64_t bit_swap_adj(uint64_t m, size_t lo_idx)
{
    uint64_t a = (m >> lo_idx) & 1ull;
    uint64_t b = (m >> (lo_idx + 1)) & 1ull;
    if (a != b) {
        m ^= 3ull << lo_idx;
    }
    return m;
}

static void sort_sensors(gateway_t *gw)
{
    for (size_t i = 1; i < gw->n_sensors; i++) {
        for (size_t j = i; j > 0 && gw->sensors[j - 1].nv_sub > gw->sensors[j].nv_sub; j--) {
            gw_sensor_t t = gw->sensors[j];
            gw->sensors[j] = gw->sensors[j - 1];
            gw->sensors[j - 1] = t;
            if (gw->ota.wildcard && j < 64) {
                gw->ota.wildcard_done = bit_swap_adj(gw->ota.wildcard_done, j - 1);
                gw->ota.wildcard_skip = bit_swap_adj(gw->ota.wildcard_skip, j - 1);
            }
        }
    }
}

static uint8_t apply_network(gateway_t *gw, uint16_t pan, uint16_t short_addr, uint64_t ext)
{
    if (pan == 0x0000 || pan == 0xFFFF || short_addr >= 0xFFFE) {
        return GW_CCFG_REFUSED;
    }
    for (size_t i = 0; i < gw->n_sensors; i++) {
        if (gw->sensors[i].short_addr == short_addr) {
            return GW_CCFG_REFUSED;
        }
    }
    uint16_t old_pan = gw->pan;
    uint16_t old_short = gw->short_addr;
    uint64_t old_ext = gw->ext;
    gw->pan = pan;
    gw->short_addr = short_addr;
    gw->ext = ext;
    if (gw->nv && !gw_save_network(gw)) {
        gw->pan = old_pan;
        gw->short_addr = old_short;
        gw->ext = old_ext;
        return GW_CCFG_FAILED;
    }
    if (gw->nv && pan != old_pan) {
        for (size_t i = 0; i < gw->n_sensors; i++) {
            if (!nv_save_sensor(gw, &gw->sensors[i])) {
                return GW_CCFG_FAILED;
            }
        }
    }
    gw->permit = false;
    memset(gw->pending, 0, sizeof gw->pending);
    memset(gw->joining, 0, sizeof gw->joining);
    memset(gw->seen, 0, sizeof gw->seen);
    ota_clear_active(gw);
    for (size_t i = 0; i < GW_MAX_OTA_TARGETS; i++) {
        gw->ota.targets[i].associating = false;
    }
    return GW_CCFG_OK;
}

static uint64_t le64_of(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) {
        v = v << 8 | p[i];
    }
    return v;
}

// OWS_SUB_NETWORK (0x53F0 0x04): [] queries, [pan_le16, short_le16, ext_le64] sets coordinator identity in OWNV item 1
static void network_command(gateway_t *gw, const uint8_t *payload, size_t len)
{
    uint8_t out[13];
    uint8_t st = GW_CCFG_REFUSED;
    if (len == 0) {
        st = GW_CCFG_OK;
    } else if (len == 12) {
        uint16_t pan = (uint16_t)(payload[0] | payload[1] << 8);
        uint16_t addr = (uint16_t)(payload[2] | payload[3] << 8);
        st = apply_network(gw, pan, addr, le64_of(payload + 4));
    }
    out[0] = st;
    out[1] = (uint8_t)gw->pan;
    out[2] = (uint8_t)(gw->pan >> 8);
    out[3] = (uint8_t)gw->short_addr;
    out[4] = (uint8_t)(gw->short_addr >> 8);
    for (int i = 0; i < 8; i++) {
        out[5 + i] = (uint8_t)(gw->ext >> (8 * i));
    }
    ows_send(gw, OWS_SUB_NETWORK, out, sizeof out);
}

// OWS_SUB_NV_IMPORT (0x53F0 0x06): zero-ladder sensor-table import & export (CLEAR / SET_NET / ADD_SENSOR / GET_SENSOR)
static void nv_import_command(gateway_t *gw, const uint8_t *payload, size_t len)
{
    uint8_t out[20];
    if (len < 1) {
        out[0] = 0;
        out[1] = GW_CCFG_REFUSED;
        ows_send(gw, OWS_SUB_NV_IMPORT, out, 2);
        return;
    }
    uint8_t op = payload[0];
    out[0] = op;
    if (op == OWS_NV_CLEAR) {
        if (len != 1) {
            out[1] = GW_CCFG_REFUSED;
            ows_send(gw, OWS_SUB_NV_IMPORT, out, 2);
            return;
        }
        gw->permit = false;
        gw->list_active = false;
        gw->n_types = 0;
        memset(gw->pending, 0, sizeof gw->pending);
        memset(gw->joining, 0, sizeof gw->joining);
        memset(gw->seen, 0, sizeof gw->seen);
        memset(&gw->ota, 0, sizeof gw->ota);
        for (size_t i = 0; i < gw->n_sensors; i++) {
            if (gw->nv) {
                nv_delete(gw->nv, NV_SENSOR, gw->sensors[i].nv_sub);
            }
        }
        gw->n_sensors = 0;
        out[1] = nv_save_count(gw) ? GW_CCFG_OK : GW_CCFG_FAILED;
        ows_send(gw, OWS_SUB_NV_IMPORT, out, 2);
        return;
    }
    if (op == OWS_NV_SET_NET) {
        if (len != 13) {
            out[1] = GW_CCFG_REFUSED;
        } else {
            uint16_t pan = (uint16_t)(payload[1] | payload[2] << 8);
            uint16_t addr = (uint16_t)(payload[3] | payload[4] << 8);
            out[1] = apply_network(gw, pan, addr, le64_of(payload + 5));
        }
        ows_send(gw, OWS_SUB_NV_IMPORT, out, 2);
        return;
    }
    if (op == OWS_NV_ADD_SENSOR) {
        uint8_t mac[8];
        if ((len != 12 && len != 14) || !normalize_mac(payload + 1, mac)) {
            out[1] = GW_CCFG_REFUSED;
            out[2] = (uint8_t)gw->n_sensors;
            ows_send(gw, OWS_SUB_NV_IMPORT, out, 3);
            return;
        }
        uint16_t addr = (uint16_t)(payload[9] | payload[10] << 8);
        uint8_t type = payload[11];
        bool have_sub = len == 14;
        uint16_t sub = have_sub ? (uint16_t)(payload[12] | payload[13] << 8) : 0;
        if (addr == 0x0000 || addr >= 0xFFFE || addr == gw->short_addr) {
            out[1] = GW_CCFG_REFUSED;
            out[2] = (uint8_t)gw->n_sensors;
            ows_send(gw, OWS_SUB_NV_IMPORT, out, 3);
            return;
        }
        for (size_t i = 0; i < gw->n_sensors; i++) {
            if (memcmp(gw->sensors[i].mac, mac, 8) != 0 &&
                (gw->sensors[i].short_addr == addr || (have_sub && gw->sensors[i].nv_sub == sub))) {
                out[1] = GW_CCFG_REFUSED;
                out[2] = (uint8_t)gw->n_sensors;
                ows_send(gw, OWS_SUB_NV_IMPORT, out, 3);
                return;
            }
        }
        gw_sensor_t *s = sensor_by_mac(gw, mac);
        bool added = !s;
        uint16_t target_sub = s ? (have_sub ? sub : s->nv_sub) : (have_sub ? sub : free_sub(gw));
        if (!s) {
            if (gw->n_sensors >= GW_MAX_SENSORS) {
                out[1] = 0xE1;
                out[2] = (uint8_t)gw->n_sensors;
                ows_send(gw, OWS_SUB_NV_IMPORT, out, 3);
                return;
            }
            size_t idx = gw->n_sensors++;
            if (gw->ota.wildcard) {
                gw->ota.wildcard_done &= ~(1ull << idx);
                gw->ota.wildcard_skip &= ~(1ull << idx);
            }
            s = &gw->sensors[idx];
            memset(s, 0, sizeof *s);
            memcpy(s->mac, mac, 8);
            s->nv_sub = target_sub;
        } else if (have_sub && s->nv_sub != sub) {
            if (gw->nv) {
                nv_delete(gw->nv, NV_SENSOR, s->nv_sub);
            }
            s->nv_sub = sub;
        }
        s->short_addr = addr;
        bool ok = nv_save_sensor(gw, s) && (!added || nv_save_count(gw));
        sort_sensors(gw);
        if (type) {
            set_type(gw, addr, type);
        } else {
            forget_type(gw, addr);
        }
        out[1] = ok ? GW_CCFG_OK : GW_CCFG_FAILED;
        out[2] = (uint8_t)gw->n_sensors;
        ows_send(gw, OWS_SUB_NV_IMPORT, out, 3);
        return;
    }
    if (op == OWS_NV_GET_SENSOR) {
        if (len != 2 || payload[1] >= gw->n_sensors) {
            out[1] = GW_CCFG_REFUSED;
            out[2] = (uint8_t)gw->n_sensors;
            out[3] = len >= 2 ? payload[1] : 0;
            ows_send(gw, OWS_SUB_NV_IMPORT, out, 4);
            return;
        }
        uint8_t idx = payload[1];
        const gw_sensor_t *s = &gw->sensors[idx];
        int t = type_of(gw, s->short_addr);
        out[1] = GW_CCFG_OK;
        out[2] = (uint8_t)gw->n_sensors;
        out[3] = idx;
        out[4] = (uint8_t)s->nv_sub;
        out[5] = (uint8_t)(s->nv_sub >> 8);
        memcpy(out + 6, s->mac, 8);
        out[14] = (uint8_t)s->short_addr;
        out[15] = (uint8_t)(s->short_addr >> 8);
        out[16] = t >= 0 ? (uint8_t)t : 0;
        ows_send(gw, OWS_SUB_NV_IMPORT, out, 17);
        return;
    }
    out[1] = GW_CCFG_REFUSED;
    ows_send(gw, OWS_SUB_NV_IMPORT, out, 2);
}

// OWS_SUB_ADDR_RANGE (0x53F0 0x23): [] queries, [first_le16, last_le16] sets the addresses new sensors get. Not kept
// across a reset (as the flags): the host sets it again.
static void addr_range_command(gateway_t *gw, const uint8_t *payload, size_t len)
{
    uint8_t out[5];
    uint8_t st = GW_CCFG_REFUSED;
    if (len == 0) {
        st = GW_CCFG_OK;
    } else if (len == 4) {
        uint16_t first = (uint16_t)(payload[0] | payload[1] << 8);
        uint16_t last = (uint16_t)(payload[2] | payload[3] << 8);
        if (first != 0 && first <= last && last <= 0xFFFD) {
            gw->addr_first = first;
            gw->addr_last = last;
            st = GW_CCFG_OK;
        }
    }
    out[0] = st;
    put_le16(out + 1, gw->addr_first);
    put_le16(out + 3, gw->addr_last);
    ows_send(gw, OWS_SUB_ADDR_RANGE, out, sizeof out);
}

static void ows_command(gateway_t *gw, const host_pkt_t *h)
{
    uint8_t out[8];
    // Legacy un-sub-IDed 0x53F0 flags query (len == 0) or 4-byte set (payload[0] == 0: no OWS_SUB_* uses 0x00)
    if (h->len == 0 || (h->len == 4 && h->payload[0] == 0x00)) {
        flags_apply(gw, h->payload, h->len, out);
        host_send(gw, HOST_OWS_RSP, out, 8);
        return;
    }
    const uint8_t *sub = h->payload + 1;
    size_t sub_len = h->len - 1;
    switch (h->payload[0]) {
        case OWS_SUB_FLAGS:
            if (sub_len != 0 && sub_len != 4) {
                break;
            }
            flags_apply(gw, sub, sub_len, out);
            ows_send(gw, OWS_SUB_FLAGS, out, 8);
            break;
        case OWS_SUB_BUILD_INFO:
            build_info_command(gw, sub, sub_len);
            break;
        case OWS_SUB_CCFG:
            ccfg_command(gw, sub, sub_len);
            break;
        case OWS_SUB_NETWORK:
            network_command(gw, sub, sub_len);
            break;
        case OWS_SUB_ENTER_SBL:
            if (!gw->usb_hid && sub_len == 4 && !memcmp(sub, "OSBL", 4) && gw->io.sbl &&
                gw->io.sbl(gw->io.ctx, false)) {
                ows_send(gw, OWS_SUB_ENTER_SBL, (const uint8_t *)"\x00", 1);
                gw->io.sbl(gw->io.ctx, true);
            } else {
                ows_send(gw, OWS_SUB_ENTER_SBL, (const uint8_t *)"\xE0", 1);
            }
            break;
        case OWS_SUB_NV_IMPORT:
            nv_import_command(gw, sub, sub_len);
            break;
        case OWS_SUB_OTA_OFFER:
            ota_offer_command(gw, sub, sub_len);
            break;
        case OWS_SUB_OTA_DATA:
            ota_data_command(gw, sub, sub_len);
            break;
        case OWS_SUB_ADDR_RANGE:
            addr_range_command(gw, sub, sub_len);
            break;
        default:
            break;
    }
}

void gw_host(gateway_t *gw, const uint8_t *pkt, size_t len)
{
    host_pkt_t h;
    uint8_t ack[8], out[16];
    if (!host_parse(pkt, len, &h) || h.to_host) {
        return;
    }
    if (h.cmd == HOST_ACK) {  // the host has it: stop resending the oldest matching packet
        size_t match = GW_MAX_UNACKED;
        for (size_t i = 0; i < GW_MAX_UNACKED; i++) {
            const uint8_t *p = gw->unacked[i].pkt;
            if (!gw->unacked[i].used || (uint16_t)(p[2] << 8 | p[4]) != h.acked) {
                continue;
            }
            if (match == GW_MAX_UNACKED || (int32_t)(gw->unacked[i].order - gw->unacked[match].order) < 0) {
                match = i;
            }
        }
        if (match < GW_MAX_UNACKED) {
            gw->unacked[match].used = false;
            pump_sensor_list(gw);
        }
        return;
    }
    if (h.cmd >> 8 == HOST_ASYNC) {
        gw->io.host(gw->io.ctx, ack, host_build_ack(ack, h.cmd, true));
    }

    switch (h.cmd) {
        case HOST_UPDATE_MODE:  // stock answers 0x4313; dongle CH554 resets in HW, direct-UART gateways jump to SBL
            host_send(gw, h.cmd + 1, NULL, 0);
            if (!gw->usb_hid && gw->io.sbl && gw->io.sbl(gw->io.ctx, false)) {
                gw->io.sbl(gw->io.ctx, true);
            }
            break;
        case HOST_INQUIRY:
            host_send(gw, h.cmd + 1, (const uint8_t *)"\x01", 1);
            break;
        case HOST_GET_ENR:  // stock: a per-dongle function of the token (open); the host checks the length
            memset(out, 0, 16);
            host_send(gw, h.cmd + 1, out, 16);
            break;
        case HOST_GET_MAC:
            host_send(gw, h.cmd + 1, gw->mac, 8);
            break;
        case HOST_GET_VERSION:
            host_send(gw, h.cmd + 1, (const uint8_t *)VERSION, sizeof VERSION - 1);
            break;
        case HOST_FINISH_AUTH:
            host_send(gw, h.cmd + 1, NULL, 0);
            gw->time_req_armed = true;
            gw->next_time_req_ms = gw->now_ms + TIME_REQ_FIRST_MS;
            break;
        case HOST_SET_SCAN:
            gw->permit = h.len && h.payload[0] == 1;
            host_send(gw, h.cmd + 1, NULL, 0);  // empty, as the stock dongle answers
            break;
        case HOST_GET_R1: {  // MAC + 16-byte token -> A4 on that sensor's next poll
            int32_t s = h.len >= 24 ? short_of(gw, h.payload) : -1;
            gw_pending_t *p = s >= 0 ? queue(gw, (uint64_t)s, PEND_JOIN_TOKEN) : NULL;
            if (p) {
                memcpy(p->data, h.payload + 8, 16);
            }
            break;
        }
        case HOST_VERIFY_SENSOR: {  // MAC + ff 04 -> A6 (ff 04 00 00 + clock)
            int32_t s = h.len >= 10 ? short_of(gw, h.payload) : -1;
            gw_pending_t *p = s >= 0 ? queue(gw, (uint64_t)s, PEND_JOIN_DONE) : NULL;
            if (p) {
                memcpy(p->data, h.payload + 8, 2);
            }
            if (h.len >= 8) {
                forget_joining_mac(gw, h.payload);
            }
            if (s >= 0) {
                forget_joining_short(gw, (uint16_t)s);
            }
            host_send(gw, h.cmd + 1, NULL, 0);
            break;
        }
        case HOST_GET_SENSOR_COUNT:
            out[0] = (uint8_t)gw->n_sensors;
            host_send(gw, h.cmd + 1, out, 1);
            break;
        case HOST_GET_SENSOR_LIST: {
            size_t n = h.len ? h.payload[0] : gw->n_sensors;
            if (n > gw->n_sensors) {
                n = gw->n_sensors;
            }
            gw->list_cursor = 0;
            gw->list_limit = n;
            gw->list_active = n > 0;
            pump_sensor_list(gw);
            break;
        }
        case HOST_DELETE_SENSOR:
            if (h.len < 8) {
                break;
            }
            remove_sensor(gw, h.payload);
            memcpy(out, h.payload, 8);
            out[8] = 0xFF;
            host_send(gw, h.cmd + 1, out, 9);
            break;
        case HOST_DELETE_ALL:
            memset(gw->joining, 0, sizeof gw->joining);
            gw->list_active = false;
            while (gw->n_sensors) {
                remove_sensor(gw, gw->sensors[0].mac);
            }
            host_send(gw, h.cmd + 1, (const uint8_t *)"\xff", 1);
            break;
        case HOST_TIME_SYNC:
            if (h.len < 8) {
                break;
            }
            gw->clock_ms = 0;
            for (int i = 0; i < 8; i++) {
                gw->clock_ms = gw->clock_ms << 8 | h.payload[i];
            }
            break;
        case HOST_OWS_BUILD_INFO_LEGACY:  // temporary read-only alias (-> 0x53FB) for rolling ESP updates
            host_send(gw, h.cmd + 1, gw->build_info, gw->build_info ? gw->build_info_len : 0);
            break;
        case HOST_OWS_CMD:
            ows_command(gw, &h);
            break;
        default:
            break;
    }
}
