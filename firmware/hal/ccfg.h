// The chip's customer configuration (common/ccfg_check.h), and putting back the one this firmware was built with:
// the way to revive a chip whose CCFG went wrong (its bootloader unreachable) while our firmware still runs.
#pragma once

#include <stdint.h>

#include "ccfg_check.h"

enum {
    CCFG_RESTORED,       // rewritten and verified
    CCFG_ALREADY_RIGHT,  // the chip's is the built-in one: nothing written
    CCFG_NOT_SAFE,       // the built-in one fails ccfg_is_safe(): nothing written
    CCFG_FAILED,         // flash erase / program / verify failed
};

const uint8_t *ccfg_builtin(void);  // the board's (boards/<board>/ccfg.c), kept in the image as data
const uint8_t *ccfg_current(void);  // the chip's

// Rewrites the CCFG's page with the built-in CCFG in place of the chip's, but for the device's identity fields
// (ccfg_merge: a sensor's MAC stays the chip's), everything else in the page kept (the stock dongle has code
// there); returns CCFG_*. "Already right" and the safety check apply to that merged CCFG. Interrupts are off for
// the ~20 ms it takes: a power loss within them leaves the chip without a CCFG.
int ccfg_restore(void);
