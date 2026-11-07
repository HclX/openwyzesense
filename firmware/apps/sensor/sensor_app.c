// The sensor app: the sensor state machine (common/sensor.c) on the radio, driven by text commands on the
// console (one per line), for development, DIY board repurposing, and tests:
//   type <hex|name>            sensor type (contact|motion|leak|climate|contact2|motion2|default or hex), persisted in
//                              NV_CFG (0x21) before joining; default is the product's (OWS_SENSOR_TYPE)
//   invert <0|1>               flip INPUT_EVENT active polarity (e.g. 1 for a water-leak probe across DIO7 and GND)
//   pull <up|none|down|default> internal resistor pull on INPUT_EVENT (persisted in NV_CFG 0x21)
//   debounce <ms>              INPUT_EVENT debounce in ms (0: default 40 ms, up to 5000 ms; persisted in NV_CFG 0x21)
//   profile [default|contact|contact-no|leak|motion]   print or apply a complete DIY pin/type profile in NV_CFG (0x21)
//   cfg                        print the current NV_CFG (0x21) profile settings
//   join                       pair with whichever gateway is pairing
//   leave                      forget the network
//   event <0|1>                contact open / motion active (A2)
//   climate <centi-degC> <%>   E8
//   sample <centi-degC> <%>    a climate measurement: E8 if it moved 0.5 degC / 3 % from the last E8
//   status                     A1
//   info                       state, network, counter (and slot, version: a slot image)
//   bat                        "bat idle <mV> loaded <mV> low <0|1> last <mV> flash <mV> min <mV>": the battery at
//                              rest, its lowest during the last exchange's frames (0: none yet), the reports'
//                              low-battery flag (#75), what the last frame alone measured, and at the end of the last
//                              flash erase / write and the lowest such since boot (0: none)
//   sleep                      end the battery console awake window immediately and allow standby
//   reset                      a full system reset (as after an OTA update)
//   ota                        as a short press of the button: the OTA window, and a check-in (the console is
//                              physical access too: on a sensor its pads are inside the case)
//   watch <s> [<hex mask> [up|none|down]]   for s seconds, the levels of the DIOs in mask (default 2-8, 10-14; made
//                              inputs, pulled up unless said) on every change: "dio <hex>" (bit n: DIO n), to find a
//                              new board's switches, and (none) whether a pin has a pull-up of its own
// It prints "ok", "joined <pan> <coordinator> <short>", "join failed", and "RX <rssi> <frame hex>" lines.
// The board's inputs (hal/input.h), debounced: the switch (reed / PIR) reports like "event"; each prints
// "input <event|button> <0|1>". The button held LONG_PRESS_MS pairs like "join" (a joined sensor pairs again, and
// keeps its network if that fails), as the stock sensors do ("button long"); a shorter press ("button short", ours:
// what stock does then is open, docs/stock-unknowns.md B1) opens the OTA window and checks in (an A1, then polls).
// OTA updates (docs/design/ota.md), on a slot image only: in the window (OTA_WINDOW_MS after a short press) the sensor
// also polls every OTA_POLL_EVERY_MS, and an update our gateway offers is taken if it is for this board, newer,
// and the battery is good;
// "ota <status>" lines, then a reset into the new image. A new image confirms itself once it has run CONFIRM_MS
// and, if it was joined, its gateway has ACKed a frame (it sends an A1 at start-up for that).
#include "sensor_app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board.h"
#include "boot.h"
#include "chip.h"
#include "clock.h"
#include "input.h"
#include "nv_page.h"
#include "ota.h"
#include "radio.h"
#include "uart.h"
#include "wyze_app.h"

#ifndef OWS_SENSOR_TYPE
#define OWS_SENSOR_TYPE SENSOR_CONTACT_V2  // the product's (firmware/products.cmake)
#endif

#ifndef OWS_VERSION
#define OWS_VERSION "unknown"
#endif
#ifndef OWS_PAN
#define OWS_PAN 0  // for the sensor: join only this PAN (0: any)
#endif
#ifndef OWS_PASSIVE
#define OWS_PASSIVE 0
#endif

#define DEBOUNCE_MS        40     // stock Ssf_buttonClockCb confirms 4 x 10 ms ticks (40 ms)
#define MAX_DEBOUNCE_MS    5000u  // upper bound for custom INPUT_EVENT debounce in NV_CFG (0x21)
#define SHORT_PRESS_MAX_MS 1000   // short press (40..1000 ms): check-in + 5-min OTA window
#define LONG_PRESS_MS      3040   // stock Ssf_buttonClockCb: 40 ms debounce + 300 x 10 ms ticks while held
#define CONFIRM_MS         60000  // an OTA image that runs this long confirms itself (docs/design/ota.md)
#define OTA_WINDOW_MS      (5u * 60 * 1000)
#define OTA_POLL_EVERY_MS  10000  // in the window: an update staged after the press is still found within this
#define BOOT_LOW_BAT_MV    2590   // stock 0x53 (83/32 V = 2.59 V): 5 x 200 ms low-battery blink at boot
#define DIO_BIT(d)         ((d) >= 0 && (d) < 32 ? (1u << ((d) & 31)) : 0u)
#define WATCH_DIOS \
    (0x7FFFu &     \
     ~(DIO_BIT(BOARD_UART_RX) | DIO_BIT(BOARD_UART_TX) | DIO_BIT(BOARD_LED_OK_DIO) | DIO_BIT(BOARD_LED_ERROR_DIO)))
#define OTA_MIN_MV 2500  // no flash erase / write below this (the zero-MAC failure: a write at low battery)
// The battery under load (#75, docs/design/low-battery.md): an old coin cell reads ~2.9 V at rest and sags hundreds of mV
// while a frame goes out, where it browns out. Reports carry the loaded reading (stock samples after each frame,
// FACTS.md R5); the low-battery flag (byte 12) as stock sets it, but cleared again by a fresh cell
#define LOW_BAT_WARN_MV  2625  // stock 0x54: loaded readings below this, LOW_BAT_STREAK in a row, set the flag
#define LOW_BAT_STREAK   3
#define LOW_BAT_CLEAR_MV 2800   // a loaded reading at or above this clears it (a new cell; stock latches it forever)
#define LOADED_WINDOW_MS 10000  // frames this close are one exchange: its lowest reading is the one reported

// Persistent items in Page 30 OWNV:
// - NV_SENSOR_JOIN (0x20, 12 B): type, flags (bit 0: gateway ours), PAN, coordinator, short address, 00 00, gw_features
// - NV_CFG (0x21, 8 B): type (0: board default), flags (bit 0: invert INPUT_EVENT), pull (0=up, 1=none, 2=down,
//   0xFF=board default), 0, debounce_ms (LE16, 0: default 40 ms), 00 00
// - NV_SENSOR_KEY (0x22, 16 B): the secured link's key from the gateway at the pairing (seclink.h; prototype, #28)
#define NV_SENSOR_JOIN 0x20
#define NV_CFG         0x21
#define NV_SENSOR_KEY  0x22

static void save(void *ctx)
{
    sensor_app_t *a = ctx;
    const sensor_t *s = &a->sensor;
    uint8_t r[12] = {s->cfg.type,
                     s->gw_ours,
                     (uint8_t)s->pan,
                     (uint8_t)(s->pan >> 8),
                     (uint8_t)s->coord,
                     (uint8_t)(s->coord >> 8),
                     (uint8_t)s->short_addr,
                     (uint8_t)(s->short_addr >> 8),
                     0,  // the message counter: no longer kept (RAM only, from 0 at each start: #38)
                     0,
                     (uint8_t)s->gw_features,
                     (uint8_t)(s->gw_features >> 8)};
    if (a->nv_ok) {
        nv_write(&a->nv, NV_SENSOR_JOIN, 0, r, sizeof r);
        uint16_t len;
        if (s->sl.have_key) {
            const uint8_t *k = nv_find(&a->nv, NV_SENSOR_KEY, 0, &len);
            if (!k || len != 16 || memcmp(k, s->sl.key, 16) != 0) {  // written only when it changes: a pairing
                nv_write(&a->nv, NV_SENSOR_KEY, 0, s->sl.key, 16);
            }
        } else if (nv_find(&a->nv, NV_SENSOR_KEY, 0, &len)) {
            nv_delete(&a->nv, NV_SENSOR_KEY, 0);
        }
    }
}

static void save_cfg(sensor_app_t *a)
{
    if (!a->nv_ok) {
        return;
    }
    if (!a->cfg_type && !a->event_invert && a->event_pull == INPUT_PULL_DEFAULT && !a->event_debounce_ms) {
        uint16_t len;
        if (nv_find(&a->nv, NV_CFG, 0, &len)) {
            nv_delete(&a->nv, NV_CFG, 0);
        }
        return;
    }
    uint8_t pull =
        (a->event_pull >= BOARD_PULL_UP && a->event_pull <= BOARD_PULL_DOWN) ? (uint8_t)a->event_pull : 0xFFu;
    uint8_t r[8] = {a->cfg_type,
                    a->event_invert ? 1u : 0u,
                    pull,
                    0,
                    (uint8_t)a->event_debounce_ms,
                    (uint8_t)(a->event_debounce_ms >> 8),
                    0,
                    0};
    nv_write(&a->nv, NV_CFG, 0, r, sizeof r);
}

// What a stock sensor of the type puts in A3/A5 (FACTS.md §4.3): firmware version, A3 bytes 11-13. Seen:
// v2 contact 0x21 / 2c 01 00, climate 0x0F / 66 00 00, v1 motion 0x19 (its binary). Others: not seen.
static void set_type(sensor_t *s, uint8_t type)
{
    static const uint8_t a3[3][3] = {{0x2c, 0x01, 0x00}, {0x66, 0x00, 0x00}, {0x00, 0x00, 0x00}};
    int k = type == SENSOR_CONTACT_V2 ? 0 : type == SENSOR_CLIMATE_V2 ? 1 : 2;
    s->cfg.type = type;
    s->cfg.fw_version = type == SENSOR_CONTACT_V2   ? 0x21
                        : type == SENSOR_CLIMATE_V2 ? 0x0F
                        : type == SENSOR_MOTION_V1  ? 0x19
                                                    : 0x10;
    memcpy(s->cfg.a3_extra, a3[k], 3);
}

static void apply_event_cfg(sensor_app_t *a)
{
    input_configure_event(a->event_invert, a->event_pull);
    if (input_present(INPUT_EVENT)) {
        bool v = input_level(INPUT_EVENT);
        a->in[INPUT_EVENT].level = v;
        a->in[INPUT_EVENT].seen = v;
        a->in[INPUT_EVENT].since = a->now_ms;
        a->sensor.state_byte = v;
    }
}

static void restore_cfg(sensor_app_t *a)
{
    uint16_t len;
    const uint8_t *r = nv_find(&a->nv, NV_CFG, 0, &len);
    if (!r || len != 8) {
        return;
    }
    a->cfg_type = r[0];
    a->event_invert = (r[1] & 1u) != 0;
    a->event_pull = r[2] <= BOARD_PULL_DOWN ? (int)r[2] : INPUT_PULL_DEFAULT;
    uint16_t db = (uint16_t)(r[4] | ((uint16_t)r[5] << 8));
    a->event_debounce_ms = db <= MAX_DEBOUNCE_MS ? db : 0;
    input_configure_event(a->event_invert, a->event_pull);
    if (a->cfg_type) {
        set_type(&a->sensor, a->cfg_type);
    }
}

static void restore(sensor_app_t *a)
{
    uint16_t len;
    const uint8_t *r = nv_find(&a->nv, NV_SENSOR_JOIN, 0, &len);
    if (!r || len != 12) {
        return;
    }
    uint16_t pan = (uint16_t)(r[2] | r[3] << 8);
    if (pan == a->sensor.cfg.never_pan || (a->sensor.cfg.only_pan && pan != a->sensor.cfg.only_pan)) {
        return;  // not a PAN this build refuses, nor another PAN when pinned by only_pan
    }
    set_type(&a->sensor, a->cfg_type ? a->cfg_type : r[0]);
    // r[8..9], the counter of older images, is ignored: it starts from 0 at each start (#38)
    sensor_restore(&a->sensor, pan, (uint16_t)(r[4] | r[5] << 8), (uint16_t)(r[6] | r[7] << 8), 0, a->now_ms);
    a->sensor.gw_ours = r[1] & 1;
    a->sensor.gw_features = (uint16_t)(r[10] | r[11] << 8);
    const uint8_t *k = nv_find(&a->nv, NV_SENSOR_KEY, 0, &len);
    if (k && len == 16) {  // the secured link: a new session at its first report (seclink.h)
        memcpy(a->sensor.sl.key, k, 16);
        a->sensor.sl.have_key = true;
    }
}

static uint8_t mv_to_raw(uint32_t mv)  // the reports' battery byte: 1/32 V
{
    uint32_t r = mv * 32 / 1000;
    return (uint8_t)(r > 255 ? 255 : r);
}

// After a frame: what the battery read under its load
static void battery_loaded(sensor_app_t *a, uint32_t mv)
{
    if (!mv) {
        return;
    }
    uint32_t now = clock_now_ms();
    bool new_exchange = !a->have_loaded || now - a->loaded_at_ms > LOADED_WINDOW_MS;
    if (new_exchange || mv < a->loaded_mv) {
        a->loaded_mv = mv;
    }
    a->have_loaded = true;
    a->loaded_at_ms = now;
    a->last_loaded_mv = mv;
    a->sensor.battery_raw = mv_to_raw(a->loaded_mv);
    if (mv < LOW_BAT_WARN_MV) {
        if (a->low_streak < LOW_BAT_STREAK) {
            a->low_streak++;
        }
        if (a->low_streak >= LOW_BAT_STREAK) {
            a->sensor.low_battery = true;
        }
    } else {
        a->low_streak = 0;
        if (mv >= LOW_BAT_CLEAR_MV) {
            a->sensor.low_battery = false;
        }
    }
}

// The battery for decisions (the OTA gate): the lower of idle and the last frame's loaded reading
static uint32_t battery_mv(const sensor_app_t *a)
{
    uint32_t idle = chip_battery_mv();
    return a->have_loaded && a->last_loaded_mv < idle ? a->last_loaded_mv : idle;
}

static void radio_send(void *ctx, const uint8_t *frame, size_t len, uint32_t gap_us)
{
#if OWS_PASSIVE
    (void)ctx, (void)frame, (void)len, (void)gap_us;
#else
    radio_tx(frame, len, gap_us);
    battery_loaded(ctx, radio_tx_min_mv());
#endif
}

// Non-blocking LED patterns on battery boards (DESIGN.md §9.1, matching stock Ssf_ledBlink(period_ms, count));
// mains/bench boards keep their OK LED steadily on.
static void led_blink(sensor_app_t *a, uint16_t half_ms, uint8_t count)
{
    if (!board_battery() || !count || !half_ms) {
        return;
    }
    board_led(BOARD_LED_OK, 1);
    a->led_on = true;
    a->led_half_ms = half_ms;
    a->led_edges = (uint8_t)(2u * count - 1u);
    a->led_at = a->now_ms + half_ms;
}

static void on_joined(void *ctx, bool ok)
{
    sensor_app_t *a = ctx;
    sensor_t *s = &a->sensor;
    char m[64];
    if (ok) {
        snprintf(m, sizeof m, "joined %04X %04X %04X%s\r\n", s->pan, s->coord, s->short_addr,
                 s->gw_ours ? ", gateway ours" : "");
        led_blink(a, 400, 3);
    } else {
        snprintf(m, sizeof m, "join failed\r\n");
        led_blink(a, 100, 3);
    }
    uart_puts(m);
    if (!ok && a->was.active) {  // a re-pairing nobody answered: the network it had
        sensor_restore(s, a->was.pan, a->was.coord, a->was.short_addr, a->was.counter, a->now_ms);
        save(a);
        uart_puts("back on the network it had\r\n");
    }
    a->was.active = false;
}

// --- OTA (docs/design/ota.md) ---

static void ota_open_window(sensor_app_t *a)
{
    a->ota_window = true;
    a->ota_window_until = a->now_ms + OTA_WINDOW_MS;
    a->next_ota_poll = a->now_ms + OTA_POLL_EVERY_MS;
}

static void ota_close_window(sensor_app_t *a)
{
    a->ota_window = false;
}

static uint8_t ota_offer(void *ctx, sensor_ota_offer_t *o)
{
    sensor_app_t *a = ctx;
    char m[96];
    uint32_t mv = battery_mv(a);  // the lower of idle and loaded (#75)
    int slot = boot_running_slot();
    o->slot = slot == 0 ? 1 : 0;  // the build for the slot not running
    const char *why = slot < 0                               ? "not a slot image"
                      : !a->ota_window && !o->sealed         ? "no window"  // sealed: from our own gateway (#28)
                      : o->board_id != boot_board()          ? "another board"
                      : o->version <= boot_running_version() ? "not newer"
                      : mv < OTA_MIN_MV                      ? "battery low"
                                                             : NULL;
    snprintf(m, sizeof m, "ota offer %lu, slot %c, %lu bytes: %s\r\n", (unsigned long)o->version, "AB"[o->slot],
             (unsigned long)o->size[o->slot], why ? why : "taken");
    uart_puts(m);
    if (!why) {
        led_blink(a, 100, 2);
    } else if (mv < OTA_MIN_MV && (a->ota_window || o->sealed) && slot >= 0 && o->board_id == boot_board() &&
               o->version > boot_running_version()) {
        led_blink(a, 200, 5);
    }
    return why ? OTA_DONE_REFUSED : OTA_DONE_OK;
}

static bool ota_write(void *ctx, uint32_t off, const uint8_t *data, size_t len)
{
    const sensor_app_t *a = ctx;
    if (battery_mv(a) < OTA_MIN_MV) {
        uart_puts("ota stopped: battery low\r\n");
        return false;
    }
    return boot_update_write(off, data, len);
}

static uint8_t ota_check(void *ctx, const sensor_ota_offer_t *o)
{
    (void)ctx;
    return boot_update_ok(o->version) ? OTA_DONE_OK : OTA_DONE_BAD_CRC;
}

static void ota_end(void *ctx, uint8_t status)
{
    sensor_app_t *a = ctx;
    char m[48];
    snprintf(m, sizeof m, "ota %s\r\n",
             status == OTA_DONE_OK        ? "done, restarting"
             : status == OTA_DONE_BAD_CRC ? "failed: bad CRC"
             : status == OTA_DONE_REFUSED ? "refused"
             : status == OTA_DONE_GAVE_UP ? "failed: no blocks"
                                          : "failed: flash");
    uart_puts(m);
    if (status != OTA_DONE_REFUSED) {
        ota_close_window(a);
    }
    if (status != OTA_DONE_OK) {
        if (status != OTA_DONE_REFUSED) {
            led_blink(a, 200, 5);
        }
        return;
    }
    uart_flush();
    chip_reset();  // the boot selector takes the new image, on probation
}

static void print_frame(const radio_frame_t *f)
{
    static const char hex[] = "0123456789abcdef";
    char line[16 + 2 * RADIO_MAX_FRAME + 2];
    int n = snprintf(line, 16, "RX %d ", f->rssi);
    for (int i = 0; i < f->len; i++) {
        line[n++] = hex[f->frame[i] >> 4], line[n++] = hex[f->frame[i] & 15];
    }
    line[n++] = '\r', line[n++] = '\n';
    uart_write((const uint8_t *)line, (size_t)n);
}

// Report bytes 10 / 11 from the chip's battery monitor: temperature in degC, battery in 1/32 V (FACTS.md §4.1)
// The temperature, and the battery at rest: reported only until a frame has measured it under load
static void read_batmon(sensor_app_t *a)
{
    int t = chip_temperature_c();
    a->sensor.temp_raw = (uint8_t)(t < 0 ? 0 : t > 255 ? 255 : t);
    a->idle_mv = chip_battery_mv();
    if (!a->have_loaded) {
        a->sensor.battery_raw = mv_to_raw(a->idle_mv);
    }
}

// A join, by a long press or "join". A joined sensor pairs again; if that fails, it goes back to the network it had
// (on_joined). Re-triggering join while already scanning/handshaking restarts the scan cleanly without losing `was`.
static void join(sensor_app_t *a)
{
    sensor_t *s = &a->sensor;
    if (s->state == SENSOR_JOINED) {
        a->was.active = true;
        a->was.pan = s->pan;
        a->was.coord = s->coord;
        a->was.short_addr = s->short_addr;
        a->was.counter = s->counter;
    } else if (s->state == SENSOR_IDLE) {
        a->was.active = false;
    }
    s->cfg.a3_extra[3] = (uint8_t)a->now_ms;  // changes on every join, like the stock sensors
    sensor_join(s, a->now_ms);
}

static void on_input(sensor_app_t *a, int input, bool level)
{
    static const char *const names[INPUT_COUNT] = {"event", "button"};
    char m[32];
    snprintf(m, sizeof m, "input %s %d\r\n", names[input], level);
    uart_puts(m);
    if (input == INPUT_EVENT && a->sensor.state == SENSOR_JOINED) {
        if (a->sensor.cfg.type == SENSOR_MOTION_V1) {
            sensor_pir(&a->sensor, level, a->now_ms);
        } else {
            sensor_event(&a->sensor, level, a->now_ms);
        }
    }
    if (input != INPUT_BUTTON) {
        return;
    }
    if (level) {
        a->button_down_ms = a->in[INPUT_BUTTON].since;
        a->long_press_done = false;
    } else if (!a->long_press_done && a->in[INPUT_BUTTON].since - a->button_down_ms <= SHORT_PRESS_MAX_MS) {
        led_blink(a, 100, 1);
        uart_puts("button short\r\n");
        ota_open_window(a);
        sensor_check_in(&a->sensor, a->now_ms);
    }
}

// The button held down LONG_PRESS_MS (3.04 s): blinks 3 x 400 ms and starts pairing immediately while held.
static uint32_t check_long_press(sensor_app_t *a)
{
    const sensor_input_t *b = &a->in[INPUT_BUTTON];
    if (!input_present(INPUT_BUTTON) || !b->level || a->long_press_done) {
        return UINT32_MAX;
    }
    uint32_t held = a->now_ms - a->button_down_ms;
    if (held < LONG_PRESS_MS) {
        return LONG_PRESS_MS - held;
    }
    a->long_press_done = true;
    led_blink(a, 400, 3);
    uart_puts("button long\r\n");
    join(a);
    return UINT32_MAX;
}

// Reads the inputs; acts on the ones that have settled. Returns how soon to look again (ms).
static uint32_t poll_inputs(sensor_app_t *a)
{
    uint32_t wait = UINT32_MAX;
    for (int i = 0; i < INPUT_COUNT; i++) {
        sensor_input_t *in = &a->in[i];
        if (!input_present(i)) {
            continue;
        }
        bool v = input_level(i);
        if (v != in->seen) {
            in->seen = v;
            in->since = a->now_ms;
        }
        if (in->seen == in->level) {
            continue;
        }
        uint32_t db = (i == INPUT_EVENT && a->event_debounce_ms) ? a->event_debounce_ms : DEBOUNCE_MS;
        uint32_t held = a->now_ms - in->since;
        if (held >= db) {
            in->level = in->seen;
            on_input(a, i, in->level);
        } else if (db - held < wait) {
            wait = db - held;
        }
    }
    return wait;
}

static bool parse_type_arg(const char *arg, uint8_t *out)
{
    if (!strcmp(arg, "default")) {
        *out = 0;
        return true;
    }
    if (!strcmp(arg, "contact")) {
        *out = SENSOR_CONTACT_V1;
        return true;
    }
    if (!strcmp(arg, "contact2")) {
        *out = SENSOR_CONTACT_V2;
        return true;
    }
    if (!strcmp(arg, "motion")) {
        *out = SENSOR_MOTION_V1;
        return true;
    }
    if (!strcmp(arg, "motion2")) {
        *out = SENSOR_MOTION_V2;
        return true;
    }
    if (!strcmp(arg, "leak")) {
        *out = SENSOR_LEAK_V2;
        return true;
    }
    if (!strcmp(arg, "climate")) {
        *out = SENSOR_CLIMATE_V2;
        return true;
    }
    char *end = NULL;
    unsigned long v = strtoul(arg, &end, 16);
    if (end == arg || *end != 0 || v < 1 || v > 0xFF) {
        return false;
    }
    *out = (uint8_t)v;
    return true;
}

static const char *pull_name(int pull)
{
    return pull == BOARD_PULL_UP     ? "up"
           : pull == BOARD_PULL_NONE ? "none"
           : pull == BOARD_PULL_DOWN ? "down"
                                     : "default";
}

static void print_cfg(const sensor_app_t *a)
{
    char m[96];
    uint16_t db = a->event_debounce_ms ? a->event_debounce_ms : DEBOUNCE_MS;
    if (a->cfg_type) {
        snprintf(m, sizeof m, "cfg type %02X invert %d pull %s debounce %u\r\n", a->cfg_type, a->event_invert ? 1 : 0,
                 pull_name(a->event_pull), db);
    } else {
        snprintf(m, sizeof m, "cfg type default(%02X) invert %d pull %s debounce %u\r\n", a->sensor.cfg.type,
                 a->event_invert ? 1 : 0, pull_name(a->event_pull), db);
    }
    uart_puts(m);
}

static bool set_profile(sensor_app_t *a, const char *name)
{
    uint8_t t = 0;
    bool inv = false;
    int pull = INPUT_PULL_DEFAULT;
    uint16_t db = 0;
    if (!strcmp(name, "default") || !strcmp(name, "reset")) {
        // board defaults (deletes NV_CFG 0x21)
    } else if (!strcmp(name, "contact")) {
        t = SENSOR_CONTACT_V1;
        inv = false;
        pull = BOARD_PULL_UP;
        db = DEBOUNCE_MS;
    } else if (!strcmp(name, "contact-no")) {
        t = SENSOR_CONTACT_V1;
        inv = true;
        pull = BOARD_PULL_UP;
        db = DEBOUNCE_MS;
    } else if (!strcmp(name, "leak")) {
        t = SENSOR_LEAK_V2;
        inv = true;
        pull = BOARD_PULL_UP;
        db = 100;
    } else if (!strcmp(name, "motion")) {
        t = SENSOR_MOTION_V1;
        inv = false;
        pull = BOARD_PULL_NONE;
        db = DEBOUNCE_MS;
    } else {
        return false;
    }
    uint8_t eff = t ? t : OWS_SENSOR_TYPE;
    if (a->sensor.state != SENSOR_IDLE && eff != a->sensor.cfg.type) {
        return false;
    }
    a->cfg_type = t;
    a->event_invert = inv;
    a->event_pull = pull;
    a->event_debounce_ms = db;
    set_type(&a->sensor, eff);
    apply_event_cfg(a);
    save_cfg(a);
    return true;
}

static bool command(sensor_app_t *a, char *line)
{
    sensor_t *s = &a->sensor;
    uint32_t now = a->now_ms;
    char *arg = strchr(line, ' '), m[160];
    if (arg) {
        *arg++ = 0;
    }
    if (!strcmp(line, "type") && arg && s->state == SENSOR_IDLE) {
        uint8_t t = 0;
        if (!parse_type_arg(arg, &t)) {
            uart_puts("error\r\n");
            return true;
        }
        a->cfg_type = t;
        set_type(s, t ? t : OWS_SENSOR_TYPE);
        save_cfg(a);
    } else if (!strcmp(line, "invert") && arg) {
        if (strcmp(arg, "0") != 0 && strcmp(arg, "1") != 0) {
            uart_puts("error\r\n");
            return true;
        }
        a->event_invert = arg[0] == '1';
        apply_event_cfg(a);
        save_cfg(a);
    } else if (!strcmp(line, "pull") && arg) {
        int p = !strcmp(arg, "up")        ? BOARD_PULL_UP
                : !strcmp(arg, "none")    ? BOARD_PULL_NONE
                : !strcmp(arg, "down")    ? BOARD_PULL_DOWN
                : !strcmp(arg, "default") ? INPUT_PULL_DEFAULT
                                          : -2;
        if (p == -2) {
            uart_puts("error\r\n");
            return true;
        }
        a->event_pull = p;
        apply_event_cfg(a);
        save_cfg(a);
    } else if (!strcmp(line, "debounce") && arg) {
        char *end = NULL;
        long db = strtol(arg, &end, 10);
        if (end == arg || *end != 0 || db < 0 || db > (long)MAX_DEBOUNCE_MS) {
            uart_puts("error\r\n");
            return true;
        }
        a->event_debounce_ms = (uint16_t)db;
        save_cfg(a);
    } else if (!strcmp(line, "cfg") && !arg) {
        print_cfg(a);
        return true;
    } else if (!strcmp(line, "profile")) {
        if (!arg) {
            print_cfg(a);
            return true;
        }
        if (!set_profile(a, arg)) {
            uart_puts("error\r\n");
            return true;
        }
    } else if (!strcmp(line, "leave")) {
        a->was.active = false;
        sensor_leave(s);
        if (a->nv_ok) {
            nv_delete(&a->nv, NV_SENSOR_JOIN, 0);
            nv_delete(&a->nv, NV_SENSOR_KEY, 0);
        }
    } else if (!strcmp(line, "join")) {
        join(a);
    } else if (!strcmp(line, "event") && arg) {
        sensor_event(s, (int)strtol(arg, NULL, 10) != 0, now);
    } else if (!strcmp(line, "pir") && arg) {
        sensor_pir(s, (int)strtol(arg, NULL, 10) != 0, now);
    } else if ((!strcmp(line, "climate") || !strcmp(line, "sample")) && arg) {
        char *hum = strchr(arg, ' ');
        if (!hum) {
            uart_puts("error\r\n");
            return true;
        }
        if (line[0] == 'c') {
            sensor_climate(s, (int16_t)strtol(arg, NULL, 10), (uint8_t)strtol(hum + 1, NULL, 10), now);
        } else {
            sensor_climate_sample(s, (int16_t)strtol(arg, NULL, 10), (uint8_t)strtol(hum + 1, NULL, 10), now);
        }
    } else if (!strcmp(line, "status")) {
        sensor_status(s, now);
    } else if (!strcmp(line, "watch") && arg) {
        char *mask = strchr(arg, ' '), *pull = mask ? strchr(mask + 1, ' ') : NULL;
        a->watch_mask = mask ? (uint32_t)strtoul(mask + 1, NULL, 16) & WATCH_DIOS : WATCH_DIOS;
        a->watch_pull = !pull                       ? BOARD_PULL_UP
                        : !strcmp(pull + 1, "none") ? BOARD_PULL_NONE
                        : !strcmp(pull + 1, "down") ? BOARD_PULL_DOWN
                                                    : BOARD_PULL_UP;
        a->watch_until = now + 1000u * (uint32_t)strtol(arg, NULL, 10);
        a->watch_last = ~0u;
    } else if (!strcmp(line, "ota")) {
        ota_open_window(a);
        sensor_check_in(s, now);
    } else if (!strcmp(line, "bat")) {  // the battery at rest and under load (#75), and the low-battery flag
        char out[96];
        snprintf(out, sizeof out, "bat idle %lu loaded %lu low %d last %lu flash %lu min %lu\r\n",
                 (unsigned long)chip_battery_mv(), (unsigned long)(a->have_loaded ? a->loaded_mv : 0),
                 a->sensor.low_battery ? 1 : 0, (unsigned long)radio_tx_min_mv(), (unsigned long)chip_flash_mv(),
                 (unsigned long)chip_flash_min_mv());
        uart_puts(out);
        return true;
    } else if (!strcmp(line, "sleep")) {
        uart_puts("ok\r\n");
        uart_flush();
        board_console_sleep();
        return true;
    } else if (!strcmp(line, "sbl")) {
        if (board_usb_hid() || !chip_can_enter_sbl()) {
            uart_puts("error\r\n");
            return true;
        }
        uart_puts("ok\r\n");
        uart_flush();
        chip_enter_sbl();
        return false;
    } else if (!strcmp(line, "reset")) {
        uart_puts("ok\r\n");
        uart_flush();
        chip_reset();
        return false;
    } else if (!strcmp(line, "info")) {
        int n = snprintf(m, sizeof m, "info %.8s type %02X state %d pan %04X coord %04X short %04X counter %u",
                         (const char *)s->cfg.mac, s->cfg.type, s->state, s->pan, s->coord, s->short_addr, s->counter);
        if (s->gw_ours && n > 0 && n < (int)sizeof m) {
            n += snprintf(m + n, sizeof m - (size_t)n, " gw ours %04X", s->gw_features);
        }
        if (s->sl.have_key && n > 0 && n < (int)sizeof m) {
            n += snprintf(m + n, sizeof m - (size_t)n, " secure %s", s->sl.session ? "session" : "key");
        }
        if (boot_running_slot() >= 0 && n > 0 && n < (int)sizeof m) {
            n += snprintf(m + n, sizeof m - (size_t)n, " slot %c v%lu", "AB"[boot_running_slot()],
                          (unsigned long)boot_running_version());
        }
        if (a->event_invert && n > 0 && n < (int)sizeof m) {
            n += snprintf(m + n, sizeof m - (size_t)n, " invert 1");
        }
        if (a->event_pull != INPUT_PULL_DEFAULT && n > 0 && n < (int)sizeof m) {
            n += snprintf(m + n, sizeof m - (size_t)n, " pull %s", pull_name(a->event_pull));
        }
        if (a->event_debounce_ms && n > 0 && n < (int)sizeof m) {
            snprintf(m + n, sizeof m - (size_t)n, " debounce %u", a->event_debounce_ms);
        }
        uart_puts(m);
        uart_puts("\r\n");
        return true;
    } else {
        uart_puts("error\r\n");
        return true;
    }
    uart_puts("ok\r\n");
    return true;
}

void sensor_app_init(sensor_app_t *a)
{
    static const char hex[] = "0123456789ABCDEF";
    sensor_io_t io = {radio_send, on_joined, NULL, save, a, ota_offer, ota_write, ota_check, ota_end};
    sensor_config_t cfg = {OWS_SENSOR_TYPE, {'7', '7', 'F', 'E'}, 0, {0}, OWS_PAN, 0, 0};
    uint64_t ieee = chip_ieee();

    memset(a, 0, sizeof *a);
    a->event_pull = INPUT_PULL_DEFAULT;
    boot_running_take_attempt();  // a new OTA image on probation: one of its boots used up
    a->now_ms = clock_now_ms();
    // the MAC: where the board keeps it (a stock sensor: in the CCFG), else from the chip's IEEE address
    bool got_mac = board_mac(cfg.mac);
    if (!got_mac) {
        for (int i = 0; i < 4; i++) {
            cfg.mac[4 + i] = (uint8_t)hex[(ieee >> (12 - 4 * i)) & 15];
        }
    }
    sensor_init(&a->sensor, &cfg, &io);
    set_type(&a->sensor, OWS_SENSOR_TYPE);
    read_batmon(a);
    a->nv_ok = nv_page_open(&a->nv);
    if (a->nv_ok) {
        restore_cfg(a);
        restore(a);
    }
    for (int i = 0; i < INPUT_COUNT; i++) {
        if (input_present(i)) {
            a->in[i].level = a->in[i].seen = input_level(i);  // as they are: no event at boot
        }
    }
    a->sensor.state_byte = a->in[INPUT_EVENT].level;
    a->joined_at_boot = a->sensor.state == SENSOR_JOINED;
    uart_puts("\r\nOpenWyzeSense sensor " OWS_VERSION " on " BOARD_NAME ", MAC ");
    uart_write(cfg.mac, 8);
    uart_puts(OWS_PASSIVE ? ", RX only" : ", TX path built in");
    if (boot_running_slot() >= 0) {
        char m[32];
        snprintf(m, sizeof m, ", slot %c v%lu", "AB"[boot_running_slot()], (unsigned long)boot_running_version());
        uart_puts(m);
    }
    uart_puts("\r\n");
    if (!radio_init()) {
        uart_puts("radio init FAILED\r\n");
        board_led(BOARD_LED_ERROR, 1);
        a->halted = true;
        return;
    }
    a->confirmed = boot_running_confirmed();
    if (board_battery()) {
        if (chip_battery_mv() <= BOOT_LOW_BAT_MV || (board_expects_mac() && !got_mac)) {
            led_blink(a, 200, 5);
        } else if (!a->confirmed) {
            led_blink(a, 250, 3);
        } else {
            led_blink(a, 250, 1);
        }
    } else {
        board_led(BOARD_LED_OK, 1);
    }
    if (!a->confirmed && a->joined_at_boot) {
        sensor_status(&a->sensor, a->now_ms);  // an ACK to confirm on
    }
}

uint32_t sensor_app_step(sensor_app_t *a)
{
    sensor_t *s = &a->sensor;
    radio_frame_t f;
    uint8_t c;

    if (a->halted) {
        return UINT32_MAX;
    }
    uint32_t before = a->now_ms;
    a->now_ms = clock_now_ms();
    if (before / 1000 != a->now_ms / 1000) {
        read_batmon(a);
    }
    while (uart_getc(&c)) {
        if (c == '\r' || c == '\n' || (c >= 0x20 && c <= 0x7E)) {
            board_console_touch();
        }
        if (c == '\r' || c == '\n') {
            a->line[a->n_line] = 0;
            a->n_line = 0;
            if (a->line[0] && !command(a, a->line)) {
                return 0;
            }
        } else if (c >= 0x20 && c <= 0x7E && a->n_line < sizeof a->line - 1) {
            a->line[a->n_line++] = (char)c;
        }
    }
    if (a->ota_window && (int32_t)(a->now_ms - a->ota_window_until) >= 0) {
        ota_close_window(a);
    }
    while (a->led_edges && (int32_t)(a->now_ms - a->led_at) >= 0) {
        a->led_on = !a->led_on;
        board_led(BOARD_LED_OK, a->led_on);
        a->led_edges--;
        a->led_at += a->led_half_ms;
    }
    if (a->ota_window && (int32_t)(a->now_ms - a->next_ota_poll) >= 0) {
        a->next_ota_poll = a->now_ms + OTA_POLL_EVERY_MS;
        sensor_poll(s, a->now_ms);  // (busy: the next one; an update in progress polls anyway)
    }
    while (radio_poll(&f)) {
        sensor_rx(s, f.frame, f.len, a->now_ms);
        print_frame(&f);
    }
    if (a->watch_until && (int32_t)(a->now_ms - a->watch_until) < 0) {
        uint32_t v = board_pin_levels(a->watch_mask, a->watch_pull);
        if (v != a->watch_last) {
            char m[24];
            snprintf(m, sizeof m, "dio %04lX\r\n", (unsigned long)v);
            uart_puts(m);
            a->watch_last = v;
        }
    } else if (a->watch_until) {
        a->watch_until = 0;
        uart_puts("watch done\r\n");
    }
    uint32_t input_wait = poll_inputs(a);
    uint32_t long_wait = check_long_press(a);
    if (a->watch_until) {
        uint32_t rem = a->watch_until - a->now_ms;
        uint32_t w = rem < 20 ? rem : 20;
        if (w < input_wait) {
            input_wait = w;
        }
    }
    if (long_wait < input_wait) {
        input_wait = long_wait;
    }
    sensor_tick(s, a->now_ms);
    // confirmed once it has run a while and, if it was on a network, talked to its gateway (an ACK)
    if (!a->confirmed && a->now_ms >= CONFIRM_MS && (!a->joined_at_boot || s->acks)) {
        a->confirmed = boot_running_confirm();
        if (a->confirmed) {
            uart_puts("image confirmed\r\n");
        }
    }
    // The receiver only while the sensor expects something (sensor_listening), then sleep until its next event:
    // a frame or a console byte wakes it earlier. A battery board (BOARD_LOW_POWER) is in standby meanwhile.
    if (sensor_listening(s)) {
        radio_rx_start();
    } else {
        radio_rx_stop();
    }
    uint32_t wait = sensor_next_event(s, a->now_ms) - a->now_ms;
    if (!a->confirmed && a->now_ms < CONFIRM_MS && CONFIRM_MS - a->now_ms < wait) {
        wait = CONFIRM_MS - a->now_ms;
    }
    if (a->ota_window && a->ota_window_until - a->now_ms < wait) {
        wait = a->ota_window_until - a->now_ms;
    }
    if (a->ota_window && a->next_ota_poll - a->now_ms < wait) {
        wait = a->next_ota_poll - a->now_ms;
    }
    if (a->led_edges && a->led_at - a->now_ms < wait) {
        wait = a->led_at - a->now_ms;
    }
    return input_wait < wait ? input_wait : wait;
}
