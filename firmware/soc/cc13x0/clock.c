#include "clock.h"

#include <ti/drivers/dpl/ClockP.h>
#include <ti/drivers/dpl/SemaphoreP.h>

#define MAX_WAIT_MS 3600000u  // the system tick counter wraps after ~12 h (10 us ticks): wake well before

static SemaphoreP_Struct wake_obj;
static SemaphoreP_Handle wake;
static uint32_t ticks_per_ms, last_ticks, ticks_left, now_ms;

void clock_init(void)  // board_init(), after NoRTOS_start()
{
    wake = SemaphoreP_constructBinary(&wake_obj, 0);
    ticks_per_ms = 1000 / ClockP_getSystemTickPeriod();
    last_ticks = ClockP_getSystemTicks();
}

uint32_t clock_now_ms(void)
{
    uint32_t ticks = ClockP_getSystemTicks();
    ticks_left += ticks - last_ticks;
    last_ticks = ticks;
    now_ms += ticks_left / ticks_per_ms;
    ticks_left %= ticks_per_ms;
    return now_ms;
}

void clock_wait(uint32_t max_ms)
{
    if (max_ms > MAX_WAIT_MS) {
        max_ms = MAX_WAIT_MS;
    }
    if (max_ms) {
        SemaphoreP_pend(wake, max_ms * ticks_per_ms);
    }
}

void clock_wake(void)
{
    SemaphoreP_post(wake);
}
