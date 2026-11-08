// The hardware interface (hal/) for the host board: see host_board.h.
#include "host_board.h"

#include <string.h>

#include "board.h"
#include "boot.h"
#include "boot_slot.h"
#include "ccfg.h"
#include "chip.h"
#include "clock.h"
#include "input.h"
#include "nv_page.h"
#include "radio.h"
#include "uart.h"

#define MAX_WAIT_MS 3600000u  // clock_wait's cap on the chip
#define IN_FLIGHT   64

// BL_CONFIG C5FE01C5 (bootloader, backdoor on DIO1 low), TAP_DAP_0 FFC500C5, IMAGE_VALID 0, the rest erased
const uint8_t host_safe_ccfg[CCFG_SIZE] = {
    [0x00 ... 0x2F] = 0xFF,
    [0x30] = 0xC5,
    0x01,
    0xFE,
    0xC5,
    [0x34 ... 0x3B] = 0xFF,
    [0x3C] = 0xC5,
    0x00,
    0xC5,
    0xFF,
    [0x40 ... 0x43] = 0xFF,
    [0x44 ... 0x47] = 0x00,
    [0x48 ... 0x57] = 0xFF,
};

static host_dev_t devs[HOST_MAX_DEVICES];
static size_t n_devs;
static uint64_t now_us;
static host_dev_t *cur;  // the device whose code is running
static host_air_t in_flight[IN_FLIGHT];
static size_t n_in_flight;

host_air_t host_air[HOST_AIR_LOG];
size_t host_n_air;
void (*host_on_tx)(const host_air_t *f);

void host_reset(void)
{
    memset(devs, 0, sizeof devs);
    n_devs = n_in_flight = host_n_air = 0;
    now_us = 0;
    cur = NULL;
}

host_dev_t *host_add(const char *name, void *app, host_app_t fn)
{
    if (n_devs == HOST_MAX_DEVICES) {
        return NULL;
    }
    host_dev_t *d = &devs[n_devs];
    d->name = name;
    d->app = app;
    d->fn = fn;
    d->ieee = 0x00124B0000A10000ull + 0x01000203ull * (n_devs + 1);
    d->temperature_c = 21;
    d->battery_mv = 3000;
    d->rssi = -50;
    memset(d->page, 0xFF, sizeof d->page);
    memset(d->ccfg_page, 0xFF, sizeof d->ccfg_page);
    memcpy(d->ccfg_page + HOST_CCFG_OFF, host_safe_ccfg, CCFG_SIZE);
    memcpy(d->ccfg_builtin, host_safe_ccfg, CCFG_SIZE);
    memset(d->flash, 0xFF, sizeof d->flash);
    d->event_pull = INPUT_PULL_DEFAULT;
    d->slot = -1;
    n_devs++;
    return d;
}

void host_boot(host_dev_t *d)
{
    host_dev_t *was = cur;
    memset(d->led, 0, sizeof d->led);
    d->radio_ready = d->rx_on = false;
    d->rx_head = d->rx_tail = 0;
    d->in_head = d->in_tail = 0;
    d->reset_pending = false;
    d->in_sbl = false;
    d->event_invert = false;
    d->event_pull = INPUT_PULL_DEFAULT;
    d->slot = -1;
    if (d->slots) {  // the boot selector
        boot_flash_t f = {.flash = d->flash};
        d->slot = boot_choose(&f, boot_board_id(BOARD_NAME));
        if (d->slot < 0) {  // nothing to run: it waits (for JTAG, on a chip)
            d->booted = false;
            return;
        }
    }
    d->booted = true;
    d->boot_us = d->wake_us = now_us;
    d->kept_awake = false;
    d->console_until_us = d->battery ? now_us + 10000000ull : UINT64_MAX;
    d->woken = false;
    memset(d->app, 0, d->fn.size);
    cur = d;
    d->fn.init(d->app);
    cur = was;
}

uint64_t host_now_us(void)
{
    return now_us;
}

// --- the air ---

static uint64_t airtime_us(size_t len)
{
    return 15400 + 1600 * (uint64_t)len;
}

static int index_of(const host_dev_t *d)
{
    return (int)(d - devs);
}

static bool collides(const host_air_t *f)
{
    for (size_t i = 0; i < n_in_flight; i++) {
        const host_air_t *o = &in_flight[i];
        if (o->from != f->from && o->start_us < f->end_us && f->start_us < o->end_us) {
            return true;
        }
    }
    size_t n = host_n_air < HOST_AIR_LOG ? host_n_air : HOST_AIR_LOG;
    for (size_t i = 0; i < n; i++) {
        const host_air_t *o = &host_air[i];
        if (o->from != f->from && o->start_us < f->end_us && f->start_us < o->end_us) {
            return true;
        }
    }
    return false;
}

static void deliver(const host_air_t *f)
{
    if (collides(f)) {
        return;
    }
    for (size_t i = 0; i < n_devs; i++) {
        host_dev_t *d = &devs[i];
        if ((int)i == f->from || !d->booted || !d->radio_ready || !d->rx_on || d->rx_since_us > f->start_us) {
            continue;
        }
        if (d->miss_frames) {
            d->miss_frames--;
            continue;
        }
        if (d->rx_head - d->rx_tail == sizeof d->rxq / sizeof d->rxq[0]) {
            d->overruns++;
            continue;
        }
        radio_frame_t *r = &d->rxq[d->rx_head++ % (sizeof d->rxq / sizeof d->rxq[0])];
        memcpy(r->frame, f->frame, f->len);
        r->len = f->len;
        r->rssi = f->rssi;
        r->rat_time = (uint32_t)(f->start_us * 4);
        d->woken = true;
    }
}

static void deliver_due(void)
{
    for (size_t i = 0; i < n_in_flight;) {
        if (in_flight[i].end_us <= now_us) {
            host_air_t f = in_flight[i];
            in_flight[i] = in_flight[--n_in_flight];
            deliver(&f);
            host_air[host_n_air++ % HOST_AIR_LOG] = f;
        } else {
            i++;
        }
    }
}

// --- running ---

static void step(host_dev_t *d)
{
    cur = d;
    d->woken = false;
    uint32_t wait = d->fn.step(d->app);
    cur = NULL;
    if (d->reset_pending) {  // chip_reset(): the next boot right away
        host_boot(d);
        return;
    }
    if (!d->booted) {
        return;
    }
    if (wait > MAX_WAIT_MS) {
        wait = MAX_WAIT_MS;
    }
    d->wake_us = now_us + (wait ? wait : 1) * 1000ull;
}

// When something is next due: a frame ending, a device's wake-up
static uint64_t next_due(void)
{
    uint64_t t = UINT64_MAX;
    for (size_t i = 0; i < n_in_flight; i++) {
        if (in_flight[i].end_us < t) {
            t = in_flight[i].end_us;
        }
    }
    for (size_t i = 0; i < n_devs; i++) {
        if (!devs[i].booted) {
            continue;
        }
        uint64_t w = devs[i].woken ? now_us : devs[i].wake_us;
        if (w < t) {
            t = w;
        }
    }
    return t;
}

static bool step_due(void)
{
    bool ran = false;
    deliver_due();
    for (size_t i = 0; i < n_devs; i++) {
        host_dev_t *d = &devs[i];
        if (d->booted && (d->woken || d->wake_us <= now_us)) {
            step(d), ran = true;
        }
    }
    return ran;
}

bool host_run_until(bool (*cond)(void *arg), void *arg, uint32_t ms)
{
    uint64_t end = now_us + ms * 1000ull;
    for (;;) {
        uint64_t t = next_due();
        if (t > end) {
            t = end;
        }
        if (t > now_us) {
            now_us = t;
        }
        bool ran = step_due();
        if (cond && cond(arg)) {
            return true;
        }
        if (!ran && now_us >= end) {
            return false;
        }
    }
}

void host_set_now_us(uint64_t t)
{
    if (t > now_us) {
        now_us = t;
    }
}

uint64_t host_poll(void)
{
    step_due();
    return next_due();
}

void host_air_receive(const host_air_t *f)
{
    if (n_in_flight < IN_FLIGHT && f->len <= RADIO_MAX_FRAME) {
        in_flight[n_in_flight++] = *f;
    }
}

void host_run(uint32_t ms)
{
    host_run_until(NULL, NULL, ms);
}

// --- console ---

bool host_console_awake(const host_dev_t *d)
{
    return !d->battery || d->kept_awake || now_us < d->console_until_us;
}

void host_console_send(host_dev_t *d, const void *data, size_t n)
{
    if (!host_console_awake(d)) {
        return;  // UART RX is off in standby on a battery board
    }
    const uint8_t *b = data;
    for (size_t i = 0; i < n && d->in_head - d->in_tail < sizeof d->con_in; i++) {
        d->con_in[d->in_head++ % sizeof d->con_in] = b[i];
    }
    d->woken = true;
}

size_t host_console_take(host_dev_t *d, uint8_t *out, size_t max)
{
    size_t n = d->n_out < max ? d->n_out : max;
    memcpy(out, d->con_out, n);
    memmove(d->con_out, d->con_out + n, d->n_out - n);
    d->n_out -= n;
    return n;
}

void uart_init(int rx_dio, int tx_dio)
{
    (void)rx_dio, (void)tx_dio;
}

void uart_write(const uint8_t *data, size_t n)
{
    if (n > sizeof cur->con_out - cur->n_out) {
        n = sizeof cur->con_out - cur->n_out;
    }
    memcpy(cur->con_out + cur->n_out, data, n);
    cur->n_out += n;
}

void uart_flush(void)
{
}

void uart_sleep(void)
{
}

void uart_puts(const char *s)
{
    uart_write((const uint8_t *)s, strlen(s));
}

bool uart_getc(uint8_t *c)
{
    if (cur->in_tail == cur->in_head) {
        return false;
    }
    *c = cur->con_in[cur->in_tail++ % sizeof cur->con_in];
    return true;
}

// --- board, chip, clock ---

void board_init(void)
{
}

void board_led(int led, bool on)
{
    if (led >= 0 && led < 2) {
        cur->led[led] = on;
    }
}

uint32_t board_pin_levels(uint32_t mask, int pull)
{
    (void)pull;
    return cur->dio_levels & mask;
}

bool board_battery(void)
{
    return cur->battery;
}

bool board_usb_hid(void)
{
    return cur->usb_hid;
}

void board_keep_awake(void)
{
    cur->kept_awake = true;
}

void board_console_touch(void)
{
    if (cur->battery && now_us < cur->console_until_us) {
        cur->console_until_us = now_us + 300000000ull;  // 5-minute sliding console window
    }
}

void board_console_sleep(void)
{
    if (cur->battery) {
        cur->console_until_us = now_us;
    }
}

bool board_console_awake(void)
{
    return host_console_awake(cur);
}

bool board_mac(uint8_t mac[8])
{
    if (!cur->has_mac) {
        return false;
    }
    for (int i = 0; i < 8; i++) {  // as the CC1310 board checks its CCFG copy: 8 ASCII hex digits, else not a MAC
        if (!((cur->mac[i] >= '0' && cur->mac[i] <= '9') || (cur->mac[i] >= 'A' && cur->mac[i] <= 'F'))) {
            return false;
        }
    }
    memcpy(mac, cur->mac, 8);
    return true;
}

bool board_expects_mac(void)
{
    return cur->has_mac;
}

uint64_t chip_ieee(void)
{
    return cur->ieee;
}

int chip_temperature_c(void)
{
    return cur->temperature_c;
}

void chip_flash_begin(void)
{
}

void chip_flash_end(void)  // the host's flash operations record their load themselves (flash_load)
{
}

uint32_t chip_flash_mv(void)
{
    return cur->flash_mv;
}

uint32_t chip_flash_min_mv(void)
{
    return cur->flash_min_mv;
}

uint32_t radio_tx_min_mv(void)
{
    return cur->tx_min_mv;
}

uint32_t chip_battery_mv(void)
{
    return cur->battery_mv;
}

bool chip_can_enter_sbl(void)
{
    return cur->ccfg_page[HOST_CCFG_OFF + 0x33] == 0xC5;
}

bool chip_enter_sbl(void)
{
    if (!chip_can_enter_sbl()) {
        return false;
    }
    if (cur->rx_on) {
        if (now_us > cur->rx_since_us) {
            cur->rx_on_us += now_us - cur->rx_since_us;
        }
        cur->rx_on = false;
    }
    cur->radio_ready = false;
    cur->booted = false;
    cur->in_sbl = true;
    cur->sbl_entries++;
    return true;
}

uint32_t clock_now_ms(void)
{
    return (uint32_t)((now_us - cur->boot_us) / 1000);
}

void clock_wait(uint32_t max_ms)  // host_run_until() does the waiting: the apps return how long to sleep
{
    (void)max_ms;
}

void clock_wake(void)
{
    if (cur) {
        cur->woken = true;
    }
}

// --- NV page: flash, which only clears bits until erased ---

// A flash operation's load on the battery (chip_flash_mv, #75)
static void flash_load(host_dev_t *d)
{
    d->flash_mv = d->battery_mv > d->flash_sag_mv ? d->battery_mv - d->flash_sag_mv : 0;
    if (!d->flash_min_mv || d->flash_mv < d->flash_min_mv) {
        d->flash_min_mv = d->flash_mv;
    }
}

static bool page_program(void *ctx, uint32_t off, const uint8_t *data, size_t len)
{
    host_dev_t *d = ctx;
    if (off > HOST_PAGE_SIZE || len > HOST_PAGE_SIZE - off) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if ((d->page[off + i] & data[i]) != data[i]) {
            d->nv_bad_writes++;
        }
        d->page[off + i] &= data[i];
    }
    d->nv_dirty = true;
    flash_load(d);
    return true;
}

static bool page_erase(void *ctx)
{
    host_dev_t *d = ctx;
    memset(d->page, 0xFF, sizeof d->page);
    d->nv_dirty = true;
    flash_load(d);
    return true;
}

void host_nv(host_dev_t *d, nvstore_t *nv)
{
    memset(nv, 0, sizeof *nv);
    nv->page = d->page;
    nv->program = page_program;
    nv->erase = page_erase;
    nv->ctx = d;
}

bool nv_page_open(nvstore_t *nv)
{
    host_nv(cur, nv);
    return nv_open(nv);
}

// --- radio ---

bool radio_init(void)
{
    cur->radio_ready = !cur->radio_broken;
    return cur->radio_ready;
}

bool radio_rx_start(void)
{
    if (!cur->radio_ready) {
        return false;
    }
    if (!cur->rx_on) {
        cur->rx_on = true, cur->rx_since_us = now_us;
    }
    return true;
}

static void rx_off(host_dev_t *d)
{
    if (!d->rx_on) {
        return;
    }
    if (now_us > d->rx_since_us) {
        d->rx_on_us += now_us - d->rx_since_us;
    }
    d->rx_on = false;
}

bool radio_rx_check(void)
{
    radio_rx_stop();
    return radio_rx_start();
}

void radio_rx_stop(void)
{
    rx_off(cur);
}

bool radio_poll(radio_frame_t *out)
{
    if (cur->rx_tail == cur->rx_head) {
        return false;
    }
    *out = cur->rxq[cur->rx_tail++ % (sizeof cur->rxq / sizeof cur->rxq[0])];
    return true;
}

uint32_t radio_overruns(void)
{
    return cur->overruns;
}

// As on the chip: starts no earlier than gap_us after the previous frame's start, and not before that frame has
// ended; the receiver is off meanwhile and on again after it (for the ACK).
bool radio_tx(const uint8_t *frame, size_t len, uint32_t gap_us)
{
    if (!cur->radio_ready || len > RADIO_MAX_FRAME || n_in_flight == IN_FLIGHT) {
        return false;
    }
    uint64_t start = now_us;
    if (gap_us && cur->last_start_us + gap_us > start) {
        start = cur->last_start_us + gap_us;
    }
    if (cur->tx_free_us > start) {
        start = cur->tx_free_us;
    }
    host_air_t *f = &in_flight[n_in_flight++];
    f->from = index_of(cur);
    f->start_us = start;
    f->end_us = start + airtime_us(len);
    f->rssi = cur->rssi;
    f->len = (uint8_t)len;
    memcpy(f->frame, frame, len);
    if (host_on_tx) {
        host_on_tx(f);
    }
    cur->last_start_us = start;
    cur->tx_free_us = f->end_us;
    cur->tx_min_mv = cur->battery_mv > cur->tx_sag_mv ? cur->battery_mv - cur->tx_sag_mv : 0;
    cur->tx_frames++;
    rx_off(cur);
    cur->rx_on = true;
    cur->rx_since_us = f->end_us;
    return true;
}

uint64_t host_rx_on_us(const host_dev_t *d)
{
    return d->rx_on_us + (d->rx_on && now_us > d->rx_since_us ? now_us - d->rx_since_us : 0);
}

// --- inputs ---

void host_input(host_dev_t *d, int input, bool level)
{
    if (input < 0 || input >= INPUT_COUNT) {
        return;
    }
    d->input[input] = level;
    d->woken = true;
}

bool input_present(int input)
{
    return input >= 0 && input < INPUT_COUNT && !cur->no_input[input];
}

bool input_level(int input)
{
    if (!input_present(input)) {
        return false;
    }
    bool v = cur->input[input];
    return input == INPUT_EVENT && cur->event_invert ? !v : v;
}

void input_configure_event(bool invert, int pull)
{
    cur->event_invert = invert;
    cur->event_pull = pull;
}

// --- CCFG (hal/ccfg.h) ---

const uint8_t *ccfg_builtin(void)
{
    return cur->ccfg_builtin;
}

const uint8_t *ccfg_current(void)
{
    return cur->ccfg_page + HOST_CCFG_OFF;
}

int ccfg_restore(void)
{
    uint8_t want[CCFG_SIZE];
    ccfg_merge(ccfg_builtin(), ccfg_current(), want);
    if (!ccfg_is_safe(want, NULL)) {
        return CCFG_NOT_SAFE;
    }
    if (!memcmp(ccfg_current(), want, CCFG_SIZE)) {
        return CCFG_ALREADY_RIGHT;
    }
    cur->ccfg_rewrites++;
    memcpy(cur->ccfg_page + HOST_CCFG_OFF, want, CCFG_SIZE);  // erase + program, the rest of the page kept
    if (cur->ccfg_fail) {
        cur->ccfg_fail = false;
        cur->ccfg_page[HOST_CCFG_OFF] ^= 1;
        return CCFG_FAILED;
    }
    return CCFG_RESTORED;
}

// --- two-slot boot (hal/boot.h) ---

void host_install_slot(host_dev_t *d, int slot, const uint8_t *img, size_t len)
{
    uint8_t *base = d->flash + boot_slot_base(slot);
    memset(base, 0xFF, SLOT_SIZE);
    memcpy(base, img, len < SLOT_SIZE ? len : SLOT_SIZE);
}

static bool flash_word(void *ctx, uint32_t addr, uint32_t value)
{
    host_dev_t *d = ctx;
    for (int i = 0; i < 4; i++) {
        d->flash[addr + i] &= (uint8_t)(value >> (8 * i));
    }
    return true;
}

// As soc/cc13x0/boot_hal.c: an update goes only to the slot not running
static bool other_slot(const host_dev_t *d, uint32_t addr, size_t len)
{
    uint32_t base = d->slot < 0 ? 0 : boot_slot_base(1 - d->slot);
    return d->slot >= 0 && addr >= base && addr <= base + SLOT_SIZE && len <= base + SLOT_SIZE - addr;
}

static bool flash_erase(void *ctx, uint32_t addr)
{
    host_dev_t *d = ctx;
    if (!other_slot(d, addr, BOOT_PAGE) || addr % BOOT_PAGE) {
        return false;
    }
    memset(d->flash + addr, 0xFF, BOOT_PAGE);
    d->flash_erases++;
    flash_load(d);
    return true;
}

static bool flash_program(void *ctx, uint32_t addr, const uint8_t *data, size_t len)
{
    host_dev_t *d = ctx;
    if (!other_slot(d, addr, len)) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        d->flash[addr + i] &= data[i];
    }
    d->flash_programs++;
    flash_load(d);
    return true;
}

static boot_flash_t flash_of(host_dev_t *d)
{
    return (boot_flash_t){d->flash, flash_word, d, flash_erase, flash_program};
}

int boot_running_slot(void)
{
    return cur->slot;
}

void boot_running_take_attempt(void)
{
    boot_flash_t f = flash_of(cur);
    if (cur->slot >= 0) {
        boot_take_attempt(&f, cur->slot);
    }
}

bool boot_running_confirmed(void)
{
    boot_flash_t f = flash_of(cur);
    return cur->slot < 0 || boot_is_confirmed(&f, cur->slot);
}

bool boot_running_confirm(void)
{
    boot_flash_t f = flash_of(cur);
    return cur->slot < 0 || boot_confirm(&f, cur->slot);
}

uint32_t boot_running_version(void)
{
    boot_flash_t f = flash_of(cur);
    return cur->slot < 0 ? 0 : boot_slot_version(&f, cur->slot);
}

uint32_t boot_board(void)
{
    return boot_board_id(BOARD_NAME);
}

bool boot_update_write(uint32_t off, const uint8_t *data, size_t len)
{
    boot_flash_t f = flash_of(cur);
    return cur->slot >= 0 && boot_write(&f, 1 - cur->slot, off, data, len);
}

bool boot_update_ok(uint32_t version)
{
    boot_flash_t f = flash_of(cur);
    return cur->slot >= 0 && boot_written_ok(&f, 1 - cur->slot, boot_board(), version);
}

void chip_watchdog_start(uint32_t timeout_ms)
{
    (void)timeout_ms;
}

void chip_watchdog_feed(void)
{
}

uint32_t radio_errors(void)
{
    return 0;
}

uint32_t radio_tx_stuck(void)
{
    return 0;
}

void chip_reset(void)
{
    cur->reset_pending = true;
    cur->resets++;
}
