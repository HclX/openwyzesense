// The gateway app on a chip (apps/gateway/gateway_app.c).
#include "board.h"
#include "chip.h"
#include "clock.h"
#include "gateway_app.h"

#define WATCHDOG_MS 3000u

int main(void)
{
    static gateway_app_t app;
    board_init();
    chip_watchdog_start(WATCHDOG_MS);  // #85: a main loop stuck for 2 x WATCHDOG_MS resets the chip
    gateway_app_init(&app);
    for (;;) {
        uint32_t wait = gateway_app_step(&app);
        chip_watchdog_feed();
        clock_wait(wait < WATCHDOG_MS / 2 ? wait : WATCHDOG_MS / 2);  // fed in quiet times too
    }
}
