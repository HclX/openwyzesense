#include "cjtag_task.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "cjtag";

typedef enum {
    STATE_IDLE = 0,
    STATE_ARMED,
    STATE_RUNNING,
    STATE_SLIPPED,
    STATE_DONE,
    STATE_ERROR,
} task_state_t;

typedef struct {
    int tck_pin;
    int tms_pin;
} esp_pin_ctx_t;

static SemaphoreHandle_t lock;
static uint8_t *page_bufs[CJTAG_TOTAL_PAGES];
static uint8_t fcfg1_buf[CJTAG_FCFG1_SIZE];

static volatile task_state_t state = STATE_IDLE;
static volatile cjtag_mode_t mode = CJTAG_MODE_NONE;
static int cfg_pin_a = CONFIG_OWS_CJTAG_PIN_A;
static int cfg_pin_b = CONFIG_OWS_CJTAG_PIN_B;
static int active_tck = -1;
static int active_tms = -1;

static size_t staged_bytes;
static uint32_t staged_pages;
static bool staged_wipe_nv;

static uint32_t pages_done_mask;
static uint32_t pages_done_count;
static uint32_t total_pages_target;
static uint32_t current_page;
static uint32_t slip_count;
static uint32_t done_seq;
static bool dump_ready;

static char active_mac[9];
static uint32_t active_user_id;
static uint32_t active_bl_config;
static uint64_t active_ieee_mac;
static char status_msg[224] = "Disarmed — select Dump or Flash to arm hands-free probing.";

static bool ensure_page_bufs(void)
{
    for (uint32_t i = 0; i < CJTAG_TOTAL_PAGES; i++) {
        if (!page_bufs[i]) {
            page_bufs[i] = malloc(CJTAG_PAGE);
            if (!page_bufs[i]) {
                return false;
            }
        }
    }
    return true;
}

static void release_pins(int pin_a, int pin_b)
{
    if (pin_a >= 0) {
        gpio_reset_pin((gpio_num_t)pin_a);
        gpio_set_direction((gpio_num_t)pin_a, GPIO_MODE_INPUT);
        gpio_set_pull_mode((gpio_num_t)pin_a, GPIO_PULLUP_ONLY);
    }
    if (pin_b >= 0) {
        gpio_reset_pin((gpio_num_t)pin_b);
        gpio_set_direction((gpio_num_t)pin_b, GPIO_MODE_INPUT);
        gpio_set_pull_mode((gpio_num_t)pin_b, GPIO_PULLUP_ONLY);
    }
}

static void prepare_pins(int tck_pin, int tms_pin)
{
    gpio_reset_pin((gpio_num_t)tck_pin);
    gpio_set_direction((gpio_num_t)tck_pin, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)tck_pin, 1);

    gpio_reset_pin((gpio_num_t)tms_pin);
    gpio_set_pull_mode((gpio_num_t)tms_pin, GPIO_PULLUP_ONLY);
    gpio_set_direction((gpio_num_t)tms_pin, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)tms_pin, 1);
}

static void io_set_pins(void *ctx, bool tck, bool tms)
{
    const esp_pin_ctx_t *p = (const esp_pin_ctx_t *)ctx;
    gpio_set_level((gpio_num_t)p->tck_pin, tck ? 1 : 0);
    gpio_set_level((gpio_num_t)p->tms_pin, tms ? 1 : 0);
}

static void io_set_tms_dir(void *ctx, bool output)
{
    const esp_pin_ctx_t *p = (const esp_pin_ctx_t *)ctx;
    gpio_set_direction((gpio_num_t)p->tms_pin, output ? GPIO_MODE_OUTPUT : GPIO_MODE_INPUT);
}

static bool io_get_tms(void *ctx)
{
    const esp_pin_ctx_t *p = (const esp_pin_ctx_t *)ctx;
    return gpio_get_level((gpio_num_t)p->tms_pin) != 0;
}

static void io_delay_us(void *ctx, uint32_t us)
{
    (void)ctx;
    esp_rom_delay_us(us);
}

static uint32_t popcount32(uint32_t x)
{
    uint32_t c = 0;
    while (x) {
        c += x & 1u;
        x >>= 1;
    }
    return c;
}

static bool probe_with_polarity(cjtag_t *cj, esp_pin_ctx_t *pctx, int pin_a, int pin_b)
{
    int pairs[2][2] = {{pin_a, pin_b}, {pin_b, pin_a}};
    for (int i = 0; i < 2; i++) {
        pctx->tck_pin = pairs[i][0];
        pctx->tms_pin = pairs[i][1];
        prepare_pins(pctx->tck_pin, pctx->tms_pin);
        cjtag_io_t io = {
            .set_pins = io_set_pins,
            .set_tms_dir = io_set_tms_dir,
            .get_tms = io_get_tms,
            .delay_us = io_delay_us,
            .ctx = pctx,
        };
        cjtag_init(cj, &io);
        if (cjtag_connect(cj, true)) {
            // Debounce hand-held contact: wait 40 ms and verify connection holds steady
            vTaskDelay(pdMS_TO_TICKS(40));
            if (cjtag_connect(cj, true)) {
                return true;
            }
        }
        release_pins(pin_a, pin_b);
    }
    return false;
}

static void task(void *arg)
{
    (void)arg;
    static cjtag_t cj;
    esp_pin_ctx_t pctx = {0};

    for (;;) {
        if (state != STATE_ARMED && state != STATE_SLIPPED) {
            vTaskDelay(pdMS_TO_TICKS(150));
            continue;
        }

        int pin_a = cfg_pin_a;
        int pin_b = cfg_pin_b;
        if (!probe_with_polarity(&cj, &pctx, pin_a, pin_b)) {
            vTaskDelay(pdMS_TO_TICKS(150));
            continue;
        }

        xSemaphoreTake(lock, portMAX_DELAY);
        if (state != STATE_ARMED && state != STATE_SLIPPED) {
            xSemaphoreGive(lock);
            release_pins(pin_a, pin_b);
            continue;
        }

        // If resuming from a slip, ensure the user re-touched the same sensor MAC
        if (state == STATE_SLIPPED && active_mac[0] != '\0' && strcmp(active_mac, cj.mac) != 0) {
            snprintf(status_msg, sizeof status_msg, "Sensor MAC changed (%s != %s) — re-touch %s or re-arm", cj.mac,
                     active_mac, active_mac);
            xSemaphoreGive(lock);
            release_pins(pin_a, pin_b);
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        active_tck = pctx.tck_pin;
        active_tms = pctx.tms_pin;
        memcpy(active_mac, cj.mac, sizeof active_mac);
        active_user_id = cj.user_id;
        active_bl_config = cj.bl_config;
        active_ieee_mac = cj.ieee_mac;

        if (mode == CJTAG_MODE_FLASH && !cj.has_ccfg_mac) {
            state = STATE_ERROR;
            snprintf(status_msg, sizeof status_msg, "Refused: sensor %s has no factory MAC in CCFG (page 31)", cj.mac);
            xSemaphoreGive(lock);
            release_pins(pin_a, pin_b);
            continue;
        }

        state = STATE_RUNNING;
        snprintf(status_msg, sizeof status_msg, "Locked %s (TCKC=GPIO%d, TMSC=GPIO%d) — %s…", active_mac, active_tck,
                 active_tms, mode == CJTAG_MODE_DUMP ? "dumping" : "flashing");
        xSemaphoreGive(lock);
        ESP_LOGI(TAG, "%s", status_msg);

        bool slipped = false;
        if (mode == CJTAG_MODE_DUMP) {
            if (!cjtag_dump_fcfg1(&cj, fcfg1_buf)) {
                slipped = true;
            }
            for (uint32_t p = 0; !slipped && p < CJTAG_TOTAL_PAGES; p++) {
                if (state != STATE_RUNNING) {
                    break;
                }
                if ((pages_done_mask & (1u << p)) != 0) {
                    continue;
                }
                current_page = p;
                if (!cjtag_dump_page(&cj, p, page_bufs[p])) {
                    slipped = true;
                    break;
                }
                xSemaphoreTake(lock, portMAX_DELAY);
                pages_done_mask |= (1u << p);
                pages_done_count = popcount32(pages_done_mask);
                snprintf(status_msg, sizeof status_msg, "Dumping %s: page %" PRIu32 " / 32 verified", active_mac,
                         pages_done_count);
                xSemaphoreGive(lock);
            }
        } else if (mode == CJTAG_MODE_FLASH) {
            for (uint32_t p = 0; p < total_pages_target; p++) {
                if (state != STATE_RUNNING) {
                    break;
                }
                if ((pages_done_mask & (1u << p)) != 0) {
                    continue;
                }
                uint32_t page_idx = (p == staged_pages) ? CJTAG_NV_PAGE_IDX : p;
                current_page = page_idx;
                bool allow_nv = (page_idx == CJTAG_NV_PAGE_IDX);
                if (!cjtag_flash_page(&cj, page_idx, page_bufs[page_idx], allow_nv)) {
                    slipped = true;
                    break;
                }
                xSemaphoreTake(lock, portMAX_DELAY);
                pages_done_mask |= (1u << p);
                pages_done_count = popcount32(pages_done_mask);
                snprintf(status_msg, sizeof status_msg, "Flashing %s: page %" PRIu32 " / %" PRIu32 " verified",
                         active_mac, pages_done_count, total_pages_target);
                xSemaphoreGive(lock);
            }
        }

        if (state != STATE_RUNNING) {
            release_pins(pin_a, pin_b);
            continue;
        }

        if (slipped) {
            xSemaphoreTake(lock, portMAX_DELAY);
            slip_count++;
            state = STATE_SLIPPED;
            snprintf(status_msg, sizeof status_msg,
                     "Probe slipped at page %" PRIu32 " (%s) — touch M/C again to resume (%" PRIu32 "/%" PRIu32
                     " done)",
                     current_page, cj.error, pages_done_count, total_pages_target);
            xSemaphoreGive(lock);
            ESP_LOGW(TAG, "%s", status_msg);
            release_pins(pin_a, pin_b);
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }

        cjtag_release_and_reset(&cj);
        release_pins(pin_a, pin_b);

        xSemaphoreTake(lock, portMAX_DELAY);
        if (mode == CJTAG_MODE_DUMP) {
            dump_ready = true;
            snprintf(status_msg, sizeof status_msg,
                     "Dump complete for %s (128 KB flash + 1 KB FCFG1 verified). Lift probes!", active_mac);
        } else {
            snprintf(status_msg, sizeof status_msg,
                     "Flash complete for %s (%" PRIu32 " pages verified, page 31 intact). Lift probes!", active_mac,
                     pages_done_count);
        }
        done_seq++;
        state = STATE_DONE;
        xSemaphoreGive(lock);
        ESP_LOGI(TAG, "%s", status_msg);
    }
}

void cjtag_task_start(void)
{
    lock = xSemaphoreCreateMutex();
    release_pins(cfg_pin_a, cfg_pin_b);
    xTaskCreate(task, "cjtag", 8192, NULL, 6, NULL);
}

bool cjtag_task_stage_begin(bool wipe_nv, char *err, size_t err_cap)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    if (state == STATE_RUNNING) {
        snprintf(err, err_cap, "cJTAG operation in progress");
        xSemaphoreGive(lock);
        return false;
    }
    if (!ensure_page_bufs()) {
        snprintf(err, err_cap, "out of memory allocating 128 KB page buffers");
        xSemaphoreGive(lock);
        return false;
    }
    state = STATE_IDLE;
    mode = CJTAG_MODE_NONE;
    dump_ready = false;
    staged_bytes = 0;
    staged_pages = 0;
    staged_wipe_nv = wipe_nv;
    for (uint32_t i = 0; i < CJTAG_TOTAL_PAGES; i++) {
        memset(page_bufs[i], 0xFF, CJTAG_PAGE);
    }
    xSemaphoreGive(lock);
    return true;
}

bool cjtag_task_stage_feed(const uint8_t *data, size_t len)
{
    if (staged_bytes + len > CJTAG_APP_END) {
        return false;
    }
    size_t rem = len;
    const uint8_t *p = data;
    while (rem > 0) {
        uint32_t pg = (uint32_t)(staged_bytes / CJTAG_PAGE);
        size_t off = staged_bytes % CJTAG_PAGE;
        size_t chunk = (rem < CJTAG_PAGE - off) ? rem : (CJTAG_PAGE - off);
        memcpy(page_bufs[pg] + off, p, chunk);
        staged_bytes += chunk;
        p += chunk;
        rem -= chunk;
    }
    return true;
}

bool cjtag_task_stage_end(bool rx_ok, char *msg, size_t msg_cap)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    if (!rx_ok || staged_bytes == 0 || (staged_bytes % 4u) != 0) {
        staged_bytes = 0;
        staged_pages = 0;
        snprintf(msg, msg_cap, "invalid CC1310 image (must be 1..122880 bytes, multiple of 4)");
        xSemaphoreGive(lock);
        return false;
    }
    // Verify Cortex-M3 vector table at start of page 0
    const uint8_t *p0 = page_bufs[0];
    uint32_t sp = (uint32_t)p0[0] | ((uint32_t)p0[1] << 8) | ((uint32_t)p0[2] << 16) | ((uint32_t)p0[3] << 24);
    uint32_t reset = (uint32_t)p0[4] | ((uint32_t)p0[5] << 8) | ((uint32_t)p0[6] << 16) | ((uint32_t)p0[7] << 24);
    if (!(sp > 0x20000000u && sp <= 0x20005000u && (reset & 1u) != 0 && reset < CJTAG_APP_END)) {
        staged_bytes = 0;
        staged_pages = 0;
        snprintf(msg, msg_cap, "refused: not a CC1310 image (invalid vector table in page 0)");
        xSemaphoreGive(lock);
        return false;
    }

    staged_pages = (uint32_t)((staged_bytes + CJTAG_PAGE - 1u) / CJTAG_PAGE);
    total_pages_target = staged_pages + (staged_wipe_nv ? 1u : 0u);
    pages_done_mask = 0;
    pages_done_count = 0;
    current_page = 0;
    slip_count = 0;
    active_mac[0] = '\0';
    mode = CJTAG_MODE_FLASH;
    state = STATE_ARMED;
    snprintf(status_msg, sizeof status_msg,
             "Armed Hands-Free Flash (%u B, %" PRIu32 " pages%s) — touch probes to M & C pads!", (unsigned)staged_bytes,
             staged_pages, staged_wipe_nv ? " + NV wipe" : "");
    snprintf(msg, msg_cap, "%s", status_msg);
    xSemaphoreGive(lock);
    return true;
}

bool cjtag_task_arm(cjtag_mode_t new_mode, int pin_a, int pin_b, char *msg, size_t msg_cap)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    if (pin_a >= 0 && pin_b >= 0 && pin_a != pin_b) {
        release_pins(cfg_pin_a, cfg_pin_b);
        cfg_pin_a = pin_a;
        cfg_pin_b = pin_b;
    }

    if (new_mode == CJTAG_MODE_NONE) {
        state = STATE_IDLE;
        mode = CJTAG_MODE_NONE;
        release_pins(cfg_pin_a, cfg_pin_b);
        snprintf(status_msg, sizeof status_msg, "Disarmed.");
        snprintf(msg, msg_cap, "%s", status_msg);
        xSemaphoreGive(lock);
        return true;
    }

    if (!ensure_page_bufs()) {
        snprintf(msg, msg_cap, "out of memory allocating 128 KB page buffers");
        xSemaphoreGive(lock);
        return false;
    }

    if (new_mode == CJTAG_MODE_FLASH && staged_pages == 0) {
        snprintf(msg, msg_cap, "upload/stage a CC1310 .bin image first");
        xSemaphoreGive(lock);
        return false;
    }

    pages_done_mask = 0;
    pages_done_count = 0;
    current_page = 0;
    slip_count = 0;
    active_mac[0] = '\0';
    active_tck = -1;
    active_tms = -1;
    mode = new_mode;

    if (new_mode == CJTAG_MODE_DUMP) {
        dump_ready = false;
        total_pages_target = CJTAG_TOTAL_PAGES;
        snprintf(status_msg, sizeof status_msg,
                 "Armed Hands-Free Dump (GPIO%d / GPIO%d, auto-polarity) — touch probes to M & C pads!", cfg_pin_a,
                 cfg_pin_b);
    } else {
        total_pages_target = staged_pages + (staged_wipe_nv ? 1u : 0u);
        snprintf(status_msg, sizeof status_msg,
                 "Armed Hands-Free Flash (%" PRIu32 " pages on GPIO%d / GPIO%d) — touch probes to M & C pads!",
                 total_pages_target, cfg_pin_a, cfg_pin_b);
    }
    state = STATE_ARMED;
    snprintf(msg, msg_cap, "%s", status_msg);
    xSemaphoreGive(lock);
    return true;
}

bool cjtag_task_get_dump_page(uint32_t page_idx, uint8_t *out_4k)
{
    if (page_idx >= CJTAG_TOTAL_PAGES) {
        return false;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    bool ok = dump_ready && page_bufs[page_idx] != NULL;
    if (ok) {
        memcpy(out_4k, page_bufs[page_idx], CJTAG_PAGE);
    }
    xSemaphoreGive(lock);
    return ok;
}

bool cjtag_task_get_fcfg1(uint8_t *out_1k)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    bool ok = dump_ready;
    if (ok) {
        memcpy(out_1k, fcfg1_buf, CJTAG_FCFG1_SIZE);
    }
    xSemaphoreGive(lock);
    return ok;
}

const char *cjtag_task_mac(void)
{
    return active_mac[0] ? active_mac : "UNKNOWN";
}

size_t cjtag_task_json(char *out, size_t cap)
{
    static const char *state_names[] = {"idle", "armed", "running", "slipped", "done", "error"};
    static const char *mode_names[] = {"none", "dump", "flash"};

    xSemaphoreTake(lock, portMAX_DELAY);
    int n =
        snprintf(out, cap,
                 "{\"state\":\"%s\",\"mode\":\"%s\",\"pin_a\":%d,\"pin_b\":%d,\"tck_pin\":%d,\"tms_pin\":%d,"
                 "\"mac\":\"%s\",\"user_id\":\"0x%08" PRIX32 "\",\"bl_config\":\"0x%08" PRIX32 "\","
                 "\"ieee_mac\":\"%016" PRIX64 "\",\"pages_done\":%" PRIu32 ",\"total_pages\":%" PRIu32 ","
                 "\"current_page\":%" PRIu32 ",\"slips\":%" PRIu32 ",\"done_seq\":%" PRIu32 ",\"dump_ready\":%s,"
                 "\"staged_bytes\":%u,\"msg\":\"%s\"}",
                 state_names[state], mode_names[mode], cfg_pin_a, cfg_pin_b, active_tck, active_tms, active_mac,
                 active_user_id, active_bl_config, active_ieee_mac, pages_done_count, total_pages_target, current_page,
                 slip_count, done_seq, dump_ready ? "true" : "false", (unsigned)staged_bytes, status_msg);
    xSemaphoreGive(lock);
    return n < 0 ? 0 : ((size_t)n < cap ? (size_t)n : cap - 1);
}
