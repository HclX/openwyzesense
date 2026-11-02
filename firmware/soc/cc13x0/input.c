#include "input.h"

#include <ti/drivers/PIN.h>
#include <ti/drivers/pin/PINCC26XX.h>

#include "clock.h"

// Matches hal/board.h BOARD_PULL_* without pulling in board_config.h (soc_cc13x0 is board-independent)
enum { PULL_UP = 0, PULL_NONE = 1, PULL_DOWN = 2 };

void input_init(const int dio[INPUT_COUNT], const bool active_low[INPUT_COUNT], const int pull[INPUT_COUNT],
                int event_alt_dio);

static PIN_State pin_state;
static PIN_Handle pin_handle;
static int dios[INPUT_COUNT] = {-1, -1};
static int alt_dio = -1;
static bool base_lows[INPUT_COUNT];
static bool lows[INPUT_COUNT];
static int base_pulls[INPUT_COUNT];
static int pulls[INPUT_COUNT];

static void edge(PIN_Handle h, PIN_Id id)  // software interrupt context
{
    (void)h, (void)id;
    clock_wake();
}

static PIN_Config pin_cfg(int dio, int pull)
{
    uint32_t p = pull == PULL_NONE ? PIN_NOPULL : pull == PULL_DOWN ? PIN_PULLDOWN : PIN_PULLUP;
    return PIN_ID(dio) | PIN_INPUT_EN | PIN_HYSTERESIS | p | PIN_IRQ_BOTHEDGES;
}

void input_init(const int dio[INPUT_COUNT], const bool active_low[INPUT_COUNT], const int pull[INPUT_COUNT],
                int event_alt_dio)
{
    PIN_Config cfg[INPUT_COUNT + 2];
    int n = 0;
    pin_handle = NULL;
    alt_dio = event_alt_dio;
    for (int i = 0; i < INPUT_COUNT; i++) {
        dios[i] = dio[i];
        base_lows[i] = lows[i] = active_low[i];
        base_pulls[i] = pulls[i] = pull[i];
        if (dio[i] < 0) {
            continue;
        }
        // pulled as the board says (a switch to GND: up; push-pull PIR comparator: none); interrupts on both edges,
        // which also wake the chip from standby (PIN_init selects the pad event as a wake-up source)
        cfg[n++] = pin_cfg(dio[i], pull[i]);
        if (i == INPUT_EVENT && event_alt_dio >= 0 && event_alt_dio != dio[i]) {
            cfg[n++] = pin_cfg(event_alt_dio, pull[i]);
        }
    }
    cfg[n] = PIN_TERMINATE;
    if (!n) {
        return;
    }
    pin_handle = PIN_open(&pin_state, cfg);
    if (pin_handle) {
        PIN_registerIntCb(pin_handle, edge);
    }
}

void input_configure_event(bool invert, int pull)
{
    lows[INPUT_EVENT] = invert ? !base_lows[INPUT_EVENT] : base_lows[INPUT_EVENT];
    int p = (pull >= PULL_UP && pull <= PULL_DOWN) ? pull : base_pulls[INPUT_EVENT];
    if (p == pulls[INPUT_EVENT]) {
        return;
    }
    pulls[INPUT_EVENT] = p;
    if (pin_handle && dios[INPUT_EVENT] >= 0) {
        PIN_setConfig(pin_handle, PIN_BM_PULLING, pin_cfg(dios[INPUT_EVENT], p));
        if (alt_dio >= 0 && alt_dio != dios[INPUT_EVENT]) {
            PIN_setConfig(pin_handle, PIN_BM_PULLING, pin_cfg(alt_dio, p));
        }
    }
}

bool input_present(int input)
{
    return input >= 0 && input < INPUT_COUNT && dios[input] >= 0;
}

bool input_level(int input)
{
    if (!input_present(input)) {
        return false;
    }
    bool high = PIN_getInputValue(dios[input]) != 0;
    bool active = lows[input] ? !high : high;
    if (input == INPUT_EVENT && alt_dio >= 0) {
        bool alt_high = PIN_getInputValue(alt_dio) != 0;
        active = active || (lows[input] ? !alt_high : alt_high);
    }
    return active;
}
