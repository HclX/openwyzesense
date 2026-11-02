// The sniffer app on a chip (apps/sniffer/sniffer_app.c).
#include "board.h"
#include "clock.h"
#include "sniffer_app.h"

int main(void)
{
    static sniffer_app_t app;
    board_init();
    sniffer_app_init(&app);
    for (;;) {
        clock_wait(sniffer_app_step(&app));
    }
}
