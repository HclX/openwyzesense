// The radio: 906.8 MHz receive and transmit.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RADIO_MAX_FRAME 127  // MAC frame bytes we keep (longest seen: 45)

typedef struct {
    uint8_t frame[RADIO_MAX_FRAME];  // MAC frame, without the FCS (the radio checked it)
    uint8_t len;
    int8_t rssi;        // dBm
    uint32_t rat_time;  // radio timer (4 MHz) at the end of the sync word
} radio_frame_t;

bool radio_init(void);

// Starts continuous receive (if not on). Received frames queue up for radio_poll(), and wake clock_wait().
bool radio_rx_start(void);

// Stops receiving; the RF core then powers down. radio_tx() turns the receiver on again after sending.
void radio_rx_stop(void);

// Takes the oldest received frame, if any. Call from the main loop.
bool radio_poll(radio_frame_t *out);

// Number of frames dropped because radio_poll() fell behind.
uint32_t radio_overruns(void);

// Sends one MAC frame (the radio adds PHR and FCS), starting no earlier than gap_us after the start of the
// previous one (0: now). Blocks until sent, then receives (for the ACK).
bool radio_tx(const uint8_t *frame, size_t len, uint32_t gap_us);

// The lowest supply voltage (mV) measured while the last radio_tx() frame was on the air, 0 if none: the battery
// under its heaviest load, which an old coin cell's idle reading hides (#75, docs/design/low-battery.md).
uint32_t radio_tx_min_mv(void);

// Transmits the radio core never finished since the start, each aborted (#85). A core that ignores even the abort
// restarts the chip.
uint32_t radio_tx_stuck(void);
