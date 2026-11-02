// The sensor app: the sensor state machine (common/sensor.c) on the radio, its join in the NV page, driven by
// text commands on the console (sensor_app.c).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "input.h"
#include "nvstore.h"
#include "sensor.h"

typedef struct {
    bool level;      // debounced
    bool seen;       // as last read
    uint32_t since;  // when it last changed
} sensor_input_t;

typedef struct {
    sensor_t sensor;
    nvstore_t nv;
    bool nv_ok;
    uint32_t now_ms;
    // the battery (#75): idle (BATMON at rest) and loaded (the lowest during a frame, radio_tx_min_mv): the loaded
    // one, the lowest of the current exchange (frames LOADED_WINDOW_MS apart at most), goes into the reports
    uint32_t idle_mv, loaded_mv, loaded_at_ms;
    uint32_t last_loaded_mv;  // the last frame's alone: for decisions (the OTA gate)
    bool have_loaded;
    uint8_t low_streak;  // loaded readings in a row below LOW_BAT_WARN_MV
    char line[64];
    size_t n_line;
    sensor_input_t in[INPUT_COUNT];
    bool halted;          // the radio failed
    bool confirmed;       // a slot image that has run well (docs/ota.md): no longer on probation
    bool joined_at_boot;  // its join restored from flash at start-up
    // OTA (docs/ota.md): updates are taken only for OTA_WINDOW_MS after a press of the button (or "join")
    bool ota_window;
    uint32_t ota_window_until;
    uint32_t next_ota_poll;  // while the window is open: a poll every OTA_POLL_EVERY_MS
    bool long_press_done;
    uint32_t button_down_ms;
    bool led_on;                       // current OK LED level during a non-blocking blink sequence (battery boards)
    uint8_t led_edges;                 // remaining LED transitions (odd -> ends OFF)
    uint16_t led_half_ms;              // half-period (ms) of the active blink pattern
    uint32_t led_at;                   // next LED transition timestamp (ms)
    uint32_t watch_until, watch_last;  // "watch": DIO levels, printed on every change until then
    uint32_t watch_mask;
    int watch_pull;              // BOARD_PULL_*
    uint8_t cfg_type;            // NV_CFG (0x21): overridden SENSOR_* type (0: board default OWS_SENSOR_TYPE)
    bool event_invert;           // NV_CFG (0x21): flip INPUT_EVENT active level
    int event_pull;              // NV_CFG (0x21): BOARD_PULL_* or INPUT_PULL_DEFAULT (-1)
    uint16_t event_debounce_ms;  // NV_CFG (0x21): INPUT_EVENT debounce in ms (0: default DEBOUNCE_MS)
    struct {                     // a re-pairing of a joined sensor: its network, back if the new join fails
        bool active;
        uint16_t pan, coord, short_addr, counter;
    } was;
} sensor_app_t;

void sensor_app_init(sensor_app_t *a);

// Does what is due; returns how long it may sleep (ms).
uint32_t sensor_app_step(sensor_app_t *a);
