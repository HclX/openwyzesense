#include "contact.h"

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_private/gpio.h"  // gpio_od_enable
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "settings.h"

#define UART UART_NUM_1

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

void contact_init(void)
{
    uart_lock = xSemaphoreCreateMutex();
    pin_init(CONFIG_OWS_CONTACT_RESET);
    pin_init(CONFIG_OWS_CONTACT_BOOT);
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
    ESP_ERROR_CHECK(uart_set_pin(UART, CONFIG_OWS_CONTACT_UART_TX, CONFIG_OWS_CONTACT_UART_RX, UART_PIN_NO_CHANGE,
                                 UART_PIN_NO_CHANGE));
    // the line idles high: a contact that does not drive its T pad (a half-written image, a chip that is starting)
    // leaves it floating, and noise came in as bytes (00 ff 00 fe ..., 2026-10-25)
    gpio_pullup_en(CONFIG_OWS_CONTACT_UART_RX);
    if (settings.rig) {
        // the battery rig (#95, docs/low-battery.md §3.1): the contact's supply sits below 3.3 V and is measured, so
        // nothing may feed it through its pins: no pull-up on the line from T, and the line to R open-drain (the
        // contact pulls its R pin up itself, to its own supply)
        gpio_pullup_dis(CONFIG_OWS_CONTACT_UART_RX);
        // the pad's open-drain bit only: gpio_set_direction() would detach the UART and leave a plain output, low
        // (2026-11-01: R held low, the console dead and the contact's pull-up and UART drawing all the time)
        gpio_od_enable(CONFIG_OWS_CONTACT_UART_TX);
    }
}

static void pulse_reset(void)
{
    gpio_set_level(CONFIG_OWS_CONTACT_RESET, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(CONFIG_OWS_CONTACT_RESET, 1);
}

void contact_reset(void)
{
    gpio_set_level(CONFIG_OWS_CONTACT_BOOT, 1);
    pulse_reset();
}

void contact_enter_bootloader(void)
{
    gpio_set_level(CONFIG_OWS_CONTACT_BOOT, 0);
    vTaskDelay(pdMS_TO_TICKS(5));
    pulse_reset();
    vTaskDelay(pdMS_TO_TICKS(50));  // the ROM has looked at W by now
    gpio_set_level(CONFIG_OWS_CONTACT_BOOT, 1);
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

void contact_gwsync(void)
{
    gwsync_io_t io = {sbl_write, sbl_read, sbl_flush, gw_reset, gw_bootloader, gw_sleep, gw_image, gw_progress, NULL};
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
