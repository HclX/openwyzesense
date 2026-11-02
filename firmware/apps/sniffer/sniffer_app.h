// The sniffer app: receive-only, prints every frame on the console as text,
//   "RX <rssi dBm> <rat time> <MAC frame hex>"
// (reference/tests/test_hw.py compares it with an RTL-SDR recording).
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool halted;  // the radio failed
} sniffer_app_t;

void sniffer_app_init(sniffer_app_t *a);
uint32_t sniffer_app_step(sniffer_app_t *a);
