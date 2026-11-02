// Time: milliseconds since boot, and sleeping.
#pragma once

#include <stdint.h>

// Milliseconds since board_init(); wraps after 49 days.
uint32_t clock_now_ms(void);

// Sleeps up to max_ms (capped at an hour), or until something wakes it: a received frame, a console byte,
// clock_wake(). A board with BOARD_LOW_POWER goes to standby meanwhile.
void clock_wait(uint32_t max_ms);

// Ends the current or the next clock_wait() (callable from interrupts).
void clock_wake(void);
