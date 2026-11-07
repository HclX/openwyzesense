// Hands-free 2-pin cJTAG (M/C) background probe task for dumping and flashing CC1310 sensors (issue #69).
// When armed from the web UI, continuously polls both probe GPIO orientations every 150 ms; on contact, dumps or
// flashes 4 KB pages with automatic page-level resume if hand-held probes slip and reconnect.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cjtag.h"

typedef enum {
    CJTAG_MODE_NONE = 0,
    CJTAG_MODE_DUMP = 1,
    CJTAG_MODE_FLASH = 2,
} cjtag_mode_t;

void cjtag_task_start(void);

// Stage a CC1310 image (up to 120 KB, pages 0-29) into the 4 KB page buffers for Hands-Free Flash.
bool cjtag_task_stage_begin(bool wipe_nv, char *err, size_t err_cap);
bool cjtag_task_stage_feed(const uint8_t *data, size_t len);
bool cjtag_task_stage_end(bool rx_ok, char *msg, size_t msg_cap);

// Arm or disarm the hands-free state machine (pin_a / pin_b < 0 keeps current pins).
bool cjtag_task_arm(cjtag_mode_t mode, int pin_a, int pin_b, char *msg, size_t msg_cap);

// Copy out a 4 KB page from the completed dump buffer, or the 1 KB FCFG1 buffer.
bool cjtag_task_get_dump_page(uint32_t page_idx, uint8_t *out_4k);
bool cjtag_task_get_fcfg1(uint8_t *out_1k);
void cjtag_task_note_dump_downloaded(void);
const char *cjtag_task_mac(void);

// Format current hands-free state and progress as JSON for GET /api/cjtag.
size_t cjtag_task_json(char *out, size_t cap);
