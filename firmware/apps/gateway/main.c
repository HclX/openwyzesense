// The gateway app on a chip (apps/gateway/gateway_app.c).
#include "board.h"
#include "clock.h"
#include "gateway_app.h"

int main(void)
{
    static gateway_app_t app;
    board_init();
    gateway_app_init(&app);
    for (;;) {
        clock_wait(gateway_app_step(&app));
    }
}
