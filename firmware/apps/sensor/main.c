// The sensor app on a chip (apps/sensor/sensor_app.c).
#include "board.h"
#include "clock.h"
#include "sensor_app.h"

int main(void)
{
    static sensor_app_t app;
    board_init();
    sensor_app_init(&app);
    for (;;) {
        clock_wait(sensor_app_step(&app));
    }
}
