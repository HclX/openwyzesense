// The gateway app: the host protocol (FACTS.md §9) on the console UART, the gateway state machine
// (common/gateway.c) on the radio, its table in the NV page.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "gateway.h"
#include "host_proto.h"
#include "nvstore.h"

typedef struct {
    gateway_t gw;
    host_rx_t host_rx;
    nvstore_t nv;
    uint32_t now_ms;
    bool in_sbl;    // handed over to the ROM bootloader (OWS_SUB_ENTER_SBL): nothing more to do
    bool radio_ok;  // the receiver runs; if not, the host link still does (#70) and the radio is retried
    uint32_t radio_retry_ms;
    bool scan_was_on, scan_led_on;
    uint32_t scan_next_ms;
    bool rx_pulsing;
    uint32_t rx_pulse_until;
} gateway_app_t;

void gateway_app_init(gateway_app_t *a);

// Does what is due; returns how long it may sleep (ms).
uint32_t gateway_app_step(gateway_app_t *a);
