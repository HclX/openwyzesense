#include "contact.h"

#include "cjtag.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "driver/uart.h"
#include "esp_private/gpio.h"  // gpio_od_enable
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "settings.h"

#define UART   UART_NUM_1
#define OFF_MS 300  // a power cycle's off time: the board's capacitors drain, the chip's power-on reset fires

static SemaphoreHandle_t uart_lock;
static volatile uint32_t s_uart_tx, s_uart_rx;

static void pin_init(int gpio)
{
    gpio_config_t c = {
        .pin_bit_mask = 1ULL << gpio,
        .mode = GPIO_MODE_OUTPUT_OD,  // the contact pulls its own pins up (DIO10 externally, RESET_N internally)
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_set_level(gpio, 1);
    gpio_config(&c);
}

// The contact's + pad on a GPIO (#81): a reset is a power cycle. Not with the battery rig, which owns the supply.
static bool power_ctl(void)
{
    return OWS_PIN_CONTACT_POWER >= 0 && !settings.rig;
}

static void line_free(int gpio)  // high impedance, no pull: nothing driven into the contact
{
    if (gpio >= 0) {
        gpio_reset_pin(gpio);
        gpio_set_direction(gpio, GPIO_MODE_DISABLE);
        gpio_pullup_dis(gpio);
        gpio_pulldown_dis(gpio);
    }
}

void contact_init(void)
{
    uart_lock = xSemaphoreCreateMutex();
    if (OWS_PIN_CONTACT_RESET >= 0) {
        pin_init(OWS_PIN_CONTACT_RESET);
    }
    if (OWS_PIN_CONTACT_BOOT >= 0) {
        pin_init(OWS_PIN_CONTACT_BOOT);
    }
    line_free(OWS_PIN_CONTACT_TMS);  // the debug port (M, C) only when gwsync uses it
    line_free(OWS_PIN_CONTACT_TCK);
    if (power_ctl()) {  // supplied from the start: the UART below sets R high (idle) before anything looks at it
        gpio_reset_pin(OWS_PIN_CONTACT_POWER);
        gpio_set_direction(OWS_PIN_CONTACT_POWER, GPIO_MODE_OUTPUT);
        gpio_set_drive_capability(OWS_PIN_CONTACT_POWER, GPIO_DRIVE_CAP_3);
        gpio_set_level(OWS_PIN_CONTACT_POWER, 1);
    }
    uart_config_t u = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(UART, 4096, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART, &u));
    ESP_ERROR_CHECK(uart_set_pin(UART, OWS_PIN_CONTACT_TX, OWS_PIN_CONTACT_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    // the line idles high: a contact that does not drive its T pad (a half-written image, a chip that is starting)
    // leaves it floating, and noise came in as bytes (00 ff 00 fe ..., 2026-10-25)
    gpio_pullup_en(OWS_PIN_CONTACT_RX);
    if (settings.rig) {
        // the battery rig (#95, docs/design/low-battery.md §3.1): the contact's supply sits below 3.3 V and is measured, so
        // nothing may feed it through its pins: no pull-up on the line from T, and the line to R open-drain (the
        // contact pulls its R pin up itself, to its own supply)
        gpio_pullup_dis(OWS_PIN_CONTACT_RX);
        // the pad's open-drain bit only: gpio_set_direction() would detach the UART and leave a plain output, low
        // (2026-11-01: R held low, the console dead and the contact's pull-up and UART drawing all the time)
        gpio_od_enable(OWS_PIN_CONTACT_TX);
    }
}

// A restart of the contact, into its firmware or (bootloader) into its ROM bootloader (#81): a power cycle when its
// + pad is on a GPIO (a cold reset: it also ends a brown-out lock-up, TI errata Advisory 12), else S. Into the
// bootloader, R (DIO1: the stock backdoor, BL_CONFIG 0xC5FE01C5, as the dongle's CH554 does it) and W (DIO10: the
// backdoor our earlier provisioning set) are held low across the start, whichever the unit has. While the power is
// off nothing may feed the chip through its pins: R low, the line from T unpulled, S and W released (open-drain).
static void restart(bool bootloader)
{
    const int tx = OWS_PIN_CONTACT_TX, rx = OWS_PIN_CONTACT_RX;
    const int s = OWS_PIN_CONTACT_RESET, w = OWS_PIN_CONTACT_BOOT;
    bool cycle = power_ctl();
    if (!cycle && s < 0) {
        ESP_LOGW("contact", "no way to reset it: neither a power GPIO nor the S pad");
        return;
    }
    // R as a plain output (the UART detached): low; open-drain on the rig, whose supply nothing may feed
    gpio_reset_pin(tx);
    gpio_pullup_dis(tx);
    gpio_set_direction(tx, settings.rig ? GPIO_MODE_OUTPUT_OD : GPIO_MODE_OUTPUT);
    gpio_set_level(tx, 0);
    if (bootloader && w >= 0) {
        gpio_set_level(w, 0);
    }
    if (s >= 0) {
        gpio_set_level(s, 0);  // held in reset
    }
    if (cycle) {
        gpio_pullup_dis(rx);
        gpio_set_level(OWS_PIN_CONTACT_POWER, 0);
        vTaskDelay(pdMS_TO_TICKS(OFF_MS));
        gpio_set_level(OWS_PIN_CONTACT_POWER, 1);
        if (!bootloader) {
            // R high right after the power, not before (it would feed the unpowered chip, which then missed its
            // power-on reset without S: bench, #81), and long before the ROM looks at its backdoor pin
            gpio_set_level(tx, 1);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    } else {
        if (!bootloader) {
            gpio_set_level(tx, 1);
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (s >= 0) {
        gpio_set_level(s, 1);  // it starts here (or at the power-up, S released then)
    }
    if (bootloader) {
        vTaskDelay(pdMS_TO_TICKS(50));  // the ROM has looked at its backdoor pin by now
        gpio_set_level(tx, 1);
        if (w >= 0) {
            gpio_set_level(w, 1);
        }
    }
    // R back to the UART (idles high), the line from T pulled up again (not on the rig: contact_init)
    uart_set_pin(UART, tx, rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (settings.rig) {
        gpio_od_enable(tx);
    } else {
        gpio_pullup_en(rx);
    }
}

void contact_reset(void)
{
    restart(false);
}

void contact_enter_bootloader(void)
{
    restart(true);
}

bool contact_uart_take(uint32_t wait_ms)
{
    return xSemaphoreTake(uart_lock, pdMS_TO_TICKS(wait_ms)) == pdTRUE;
}

void contact_uart_give(void)
{
    xSemaphoreGive(uart_lock);
}

// up to cap bytes: waits up to timeout_ms for the first, then takes what is buffered. Not uart_read_bytes(cap) alone:
// it waits for all cap bytes or the whole timeout, so every short bootloader reply cost its full timeout (a page took
// 49 s on the LOLIN32, 2026-10-25)
size_t contact_uart_read(uint8_t *buf, size_t cap, uint32_t timeout_ms)
{
    if (!cap || uart_read_bytes(UART, buf, 1, pdMS_TO_TICKS(timeout_ms)) != 1) {
        return 0;
    }
    size_t more = 0;
    uart_get_buffered_data_len(UART, &more);
    more = more < cap - 1 ? more : cap - 1;
    int k = more ? uart_read_bytes(UART, buf + 1, more, 0) : 0;
    size_t n = 1 + (k > 0 ? (size_t)k : 0);
    s_uart_rx += (uint32_t)n;
    return n;
}

bool contact_uart_write(const uint8_t *data, size_t len)
{
    int n = uart_write_bytes(UART, data, len);
    if (n > 0) {
        s_uart_tx += (uint32_t)n;
    }
    return n == (int)len && uart_wait_tx_done(UART, pdMS_TO_TICKS(1000)) == ESP_OK;
}

void contact_uart_flush_input(void)
{
    uart_flush_input(UART);
}

void contact_uart_counts(uint32_t *tx, uint32_t *rx)
{
    *tx = s_uart_tx;
    *rx = s_uart_rx;
}

// --- reflashing through the ROM bootloader ---

#include <stdio.h>
#include <string.h>

#include "sbl.h"

static bool sbl_write(void *ctx, const uint8_t *d, size_t n)
{
    (void)ctx;
    return contact_uart_write(d, n);
}

static size_t sbl_read(void *ctx, uint8_t *buf, size_t cap, uint32_t timeout_ms)
{
    (void)ctx;
    size_t n = contact_uart_read(buf, cap, timeout_ms);
    while (n && n < cap) {  // what follows, until the line is quiet for 20 ms
        size_t k = contact_uart_read(buf + n, cap - n, 20);
        if (!k) {
            break;
        }
        n += k;
    }
    return n;
}

static void sbl_flush(void *ctx)
{
    (void)ctx;
    contact_uart_flush_input();
}

static sbl_t s_sbl;  // shared across contact_gwsync() and contact_flash_*() under uart_lock
static uint8_t s_page[SBL_PAGE];
static size_t s_fill;
static bool s_wipe_nv;

bool contact_flash_begin(const char *expect_mac, bool wipe_nv, char *msg, size_t cap)
{
    sbl_io_t io = {sbl_write, sbl_read, sbl_flush, NULL};
    if (!contact_uart_take(5000)) {
        snprintf(msg, cap, "the UART is busy");
        return false;
    }
    s_fill = 0;
    s_wipe_nv = wipe_nv;
    sbl_init(&s_sbl, &io);
    bool ok = sbl_connect(&s_sbl, expect_mac);
    if (!ok) {
        sbl_init(&s_sbl, &io);
        contact_enter_bootloader();
        ok = sbl_connect(&s_sbl, expect_mac);
    }
    if (!ok || !sbl_stream_begin(&s_sbl)) {
        snprintf(msg, cap, "not flashed: %s", s_sbl.error);
        contact_reset();
        contact_uart_flush_input();
        contact_uart_give();
        return false;
    }
    return true;
}

bool contact_flash_write(const uint8_t *data, size_t len)
{
    for (size_t at = 0; at < len;) {
        size_t n = len - at < SBL_PAGE - s_fill ? len - at : SBL_PAGE - s_fill;
        memcpy(s_page + s_fill, data + at, n);
        s_fill += n;
        at += n;
        if (s_fill == SBL_PAGE) {
            if (!sbl_stream_page(&s_sbl, s_page, SBL_PAGE)) {
                s_fill = 0;
                return false;
            }
            s_fill = 0;
        }
    }
    return true;
}

bool contact_flash_end(bool rx_ok, char *msg, size_t cap)
{
    bool ok = rx_ok;
    if (ok && s_fill > 0) {
        ok = sbl_stream_page(&s_sbl, s_page, s_fill);
        s_fill = 0;
    }
    if (ok) {
        ok = sbl_stream_end(&s_sbl);
    }
    if (ok && s_wipe_nv) {
        memset(s_page, 0xFF, sizeof s_page);
        ok = sbl_write_nv(&s_sbl, s_page);
    }
    if (ok) {
        snprintf(msg, cap, "contact %s (chip %08lx, backdoor DIO%u): %lu bytes written and verified%s; reset",
                 s_sbl.mac, (unsigned long)s_sbl.chip_id, s_sbl.backdoor_dio, (unsigned long)s_sbl.stream_at,
                 s_wipe_nv ? ", NV wiped" : "");
    } else if (!rx_ok && !s_sbl.error[0]) {
        snprintf(msg, cap, "not flashed: the upload broke off or was not a valid image");
    } else {
        snprintf(msg, cap, "not flashed: %s", s_sbl.error);
    }
    contact_reset();  // into its firmware, new or old (the bootloader's own reset would hang it)
    contact_uart_flush_input();
    contact_uart_give();
    return ok;
}

bool contact_flash(const uint8_t *image, size_t len, const char *expect_mac, bool wipe_nv, char *msg, size_t cap)
{
    if (!contact_flash_begin(expect_mac, wipe_nv, msg, cap)) {
        return false;
    }
    bool ok = contact_flash_write(image, len);
    return contact_flash_end(ok, msg, cap);
}

// --- at the start: the contact on the gateway image this firmware carries (esp32/contact/gwsync.c, #76) ---

#include "esp_log.h"
#include "esp_timer.h"
#include "gwsync.h"
#include "settings.h"
#include "pins.h"

extern const uint8_t gw_image_start[] asm("_binary_gateway_contact_v1_app_bin_start");
extern const uint8_t gw_image_end[] asm("_binary_gateway_contact_v1_app_bin_end");

static gwsync_report_t gw_report;
static bool gw_checked;
static char gw_steps[6][120];  // the last steps, for the status page while the check runs and after
static unsigned gw_nsteps;

static void gw_progress(void *ctx, const char *step)
{
    (void)ctx;
    snprintf(gw_steps[gw_nsteps % 6], sizeof gw_steps[0], "%lu ms %s", (unsigned long)(esp_timer_get_time() / 1000),
             step);
    gw_nsteps++;
    ESP_LOGI("contact", "gateway check: %s", step);
}

size_t contact_gwsync_steps(char *out, size_t cap)  // the last steps, oldest first, one per line
{
    size_t n = 0;
    out[0] = 0;
    for (unsigned i = gw_nsteps > 6 ? gw_nsteps - 6 : 0; i < gw_nsteps && n + 1 < cap; i++) {
        n += (size_t)snprintf(out + n, cap - n, "%s\n", gw_steps[i % 6]);
    }
    return n < cap ? n : cap - 1;
}

static void gw_reset(void *ctx)
{
    (void)ctx;
    contact_reset();
}

static void gw_bootloader(void *ctx)
{
    (void)ctx;
    contact_enter_bootloader();
}

static void gw_sleep(void *ctx, uint32_t ms)
{
    (void)ctx;
    vTaskDelay(pdMS_TO_TICKS(ms));
}

static bool gw_image(void *ctx, uint32_t off, uint8_t *buf, size_t len)
{
    (void)ctx;
    if (off + len > (size_t)(gw_image_end - gw_image_start)) {
        return false;
    }
    memcpy(buf, gw_image_start + off, len);
    return true;
}

// --- the contact's debug port (cJTAG on M and C, #81) for gwsync ---

typedef struct {
    int tck, tms;
} cj_pins_t;

static void cj_set_pins(void *ctx, bool tck, bool tms)  // TMSC changes while TCKC is low (cjtag_task.c's order)
{
    const cj_pins_t *p = ctx;
    if (!tck) {
        gpio_set_level(p->tck, 0);
        gpio_set_level(p->tms, tms);
    } else {
        gpio_set_level(p->tms, tms);
        gpio_set_level(p->tck, 1);
    }
}

static void cj_set_tms_dir(void *ctx, bool output)
{
    const cj_pins_t *p = ctx;
    gpio_set_direction(p->tms, output ? GPIO_MODE_INPUT_OUTPUT : GPIO_MODE_INPUT);
}

static bool cj_get_tms(void *ctx)
{
    const cj_pins_t *p = ctx;
    return gpio_get_level(p->tms) != 0;
}

static void cj_delay(void *ctx, uint32_t us)
{
    (void)ctx;
    if (us > 1) {
        esp_rom_delay_us(us);
    }
}

// The carried image's pages 0-29 through the debug port: pages already equal skipped, the CCFG's factory MAC checked
// first (cjtag_flash_page refuses pages 30 and 31 and verifies each page), then a restart into the new firmware.
// Returns the pages written, or -1 (err says why: not wired, not answering, no MAC, a write failed).
static int cjtag_flash_now(uint32_t image_len, char *mac, char *err, size_t cap)
{
    static cjtag_t cj;
    static uint8_t want[CJTAG_PAGE], have[CJTAG_PAGE];
    const int tck = OWS_PIN_CONTACT_TCK, tms = OWS_PIN_CONTACT_TMS;
    if (tck < 0 || tms < 0) {
        snprintf(err, cap, "no debug port wired (M, C)");
        return -1;
    }
    cj_pins_t pins = {tck, tms};
    gpio_reset_pin(tck);
    gpio_set_direction(tck, GPIO_MODE_OUTPUT);
    gpio_set_level(tck, 1);
    gpio_reset_pin(tms);
    gpio_set_pull_mode(tms, GPIO_PULLUP_ONLY);
    gpio_set_direction(tms, GPIO_MODE_INPUT_OUTPUT);
    gpio_set_level(tms, 1);
    cjtag_io_t io = {cj_set_pins, cj_set_tms_dir, cj_get_tms, cj_delay, &pins};
    bool connected = false;
    for (int i = 0; i < 3 && !connected; i++) {
        cjtag_init(&cj, &io);
        connected = cjtag_connect(&cj, true);
        if (!connected) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
    int written = -1;
    if (!connected) {
        snprintf(err, cap, "%s", cj.error);
        goto out;
    }
    memcpy(mac, cj.mac, 9);
    if (!cj.has_ccfg_mac) {
        snprintf(err, cap, "no factory MAC in its CCFG: not written");
        goto release;
    }
    written = 0;
    for (uint32_t p = 0; p * CJTAG_PAGE < image_len; p++) {
        uint32_t n = image_len - p * CJTAG_PAGE < CJTAG_PAGE ? image_len - p * CJTAG_PAGE : CJTAG_PAGE;
        memset(want, 0xFF, sizeof want);
        if (!gw_image(NULL, p * CJTAG_PAGE, want, n) || !cjtag_mem_read_block(&cj, p * CJTAG_PAGE, have, CJTAG_PAGE)) {
            snprintf(err, cap, "page %lu: %s", (unsigned long)p, cj.error[0] ? cj.error : "image read failed");
            written = -1;
            break;
        }
        if (!memcmp(want, have, CJTAG_PAGE)) {
            continue;
        }
        if (!cjtag_flash_page(&cj, p, want, false)) {
            snprintf(err, cap, "page %lu: %s", (unsigned long)p, cj.error);
            written = -1;
            break;
        }
        written++;
    }
release:
    cjtag_release_and_reset(&cj);
out:
    line_free(tck);
    line_free(tms);
    if (written > 0) {
        contact_reset();  // a clean start of the new firmware (VECTRESET restarts the CPU only)
    }
    return written;
}

// cjtag.c keeps 4 KB page buffers on the stack, more than the relay task has (6 KB, which gwsync_run runs on): the
// debug-port work runs in its own task, with room for them, only for as long as it takes (#81: the first bench run
// overflowed the relay task's stack into the Wi-Fi driver's memory).
typedef struct {
    uint32_t image_len;
    char *mac, *err;
    size_t cap;
    int result;
    TaskHandle_t caller;
} cj_job_t;

static void cj_worker(void *arg)
{
    cj_job_t *job = arg;
    job->result = cjtag_flash_now(job->image_len, job->mac, job->err, job->cap);
    xTaskNotifyGive(job->caller);
    vTaskDelete(NULL);
}

static int gw_cjtag_flash(void *ctx, uint32_t image_len, char *mac, char *err, size_t cap)
{
    (void)ctx;
    cj_job_t job = {image_len, mac, err, cap, -1, xTaskGetCurrentTaskHandle()};
    if (xTaskCreate(cj_worker, "gw_cjtag", 12288, &job, uxTaskPriorityGet(NULL), NULL) != pdPASS) {
        snprintf(err, cap, "out of memory for the debug-port task");
        return -1;
    }
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    return job.result;
}

void contact_gwsync(void)
{
    gwsync_io_t io = {sbl_write, sbl_read,    sbl_flush, gw_reset,       gw_bootloader,         gw_sleep,
                      gw_image,  gw_progress, NULL,      gw_cjtag_flash, settings.contact_flash};
    uint32_t img_len = (uint32_t)(gw_image_end - gw_image_start);
    contact_uart_take(portMAX_DELAY);
    if (settings.skip_gwsync) {
        gwsync_probe(&io, img_len, s_page, &gw_report);
    } else {
        gwsync_run(&io, img_len, &s_sbl, s_page, &gw_report);
    }
    contact_uart_flush_input();
    contact_uart_give();
    gw_checked = true;
    ESP_LOGI("contact", "gateway: %s", gw_report.msg);
}

const gwsync_report_t *contact_gwsync_report(void)
{
    if (gw_checked && !gw_report.mac[0] && s_sbl.mac[0]) {
        memcpy(gw_report.mac, s_sbl.mac, sizeof gw_report.mac);
    }
    return gw_checked ? &gw_report : NULL;
}

bool contact_nv_export(char *json_out, size_t json_cap, char *msg, size_t msg_cap)
{
    gwnv_io_t io = {sbl_write, sbl_read, sbl_flush, NULL};
    if (!contact_uart_take(5000)) {
        snprintf(msg, msg_cap, "the UART is busy");
        return false;
    }
    gwnv_doc_t doc;
    bool ok = gwnv_export(&io, &doc, msg, msg_cap);
    contact_uart_flush_input();
    contact_uart_give();
    if (!ok) {
        return false;
    }
    if (!gwnv_format_json(&doc, json_out, json_cap)) {
        snprintf(msg, msg_cap, "NV JSON buffer too small");
        return false;
    }
    return true;
}

bool contact_nv_import(const gwnv_doc_t *doc, char *msg, size_t msg_cap)
{
    gwnv_io_t io = {sbl_write, sbl_read, sbl_flush, NULL};
    if (!contact_uart_take(5000)) {
        snprintf(msg, msg_cap, "the UART is busy");
        return false;
    }
    bool ok = gwnv_import(&io, doc, msg, msg_cap);
    contact_uart_flush_input();
    contact_uart_give();
    if (ok) {
        snprintf(msg, msg_cap, "imported network %04X/%04X and %u sensors", doc->pan, doc->short_addr,
                 (unsigned)doc->n_sensors);
    }
    return ok;
}
