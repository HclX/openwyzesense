// board.h for CC13x0 boards: the objects the TI drivers expect, and the board's pins (board_config.h).
#include "board.h"
#include "boot.h"
#include "boot_slot.h"

#include <string.h>

#include <NoRTOS.h>  // needs stdint.h first (board.h)

#include <ti/devices/DeviceFamily.h>
// clang-format off: a TI include path, not a division
#include DeviceFamily_constructPath(driverlib/gpio.h)
#include DeviceFamily_constructPath(driverlib/ioc.h)
#include DeviceFamily_constructPath(driverlib/osc.h)
#include DeviceFamily_constructPath(driverlib/prcm.h)
#include DeviceFamily_constructPath(driverlib/rfc.h)
// clang-format on
#include <ti/drivers/PIN.h>
#include <ti/drivers/dpl/ClockP.h>
#include <ti/drivers/dpl/HwiP.h>
#include <ti/drivers/Power.h>
#include <ti/drivers/pin/PINCC26XX.h>
#include <ti/drivers/power/PowerCC26XX.h>
#include <ti/drivers/rf/RF.h>

#include "input.h"
#include "uart.h"

#ifdef BOARD_LOW_POWER  // the board itself (a test build may keep it awake: OWS_LOW_POWER below)
#define BOARD_BATTERY BOARD_LOW_POWER
#else
#define BOARD_BATTERY 0
#endif
#ifdef OWS_LOW_POWER  // a test build's override (firmware/CMakeLists.txt)
#undef BOARD_LOW_POWER
#define BOARD_LOW_POWER OWS_LOW_POWER
#endif
#ifndef BOARD_LOW_POWER
#define BOARD_LOW_POWER 0
#endif
#ifndef BOARD_MAC_IN_CCFG
#define BOARD_MAC_IN_CCFG 0
#endif
#ifndef BOARD_USB_HID
#define BOARD_USB_HID 0
#endif

#ifndef BOARD_INPUT_EVENT_DIO
#define BOARD_INPUT_EVENT_DIO -1
#endif
#ifndef BOARD_INPUT_EVENT_ALT_DIO
#define BOARD_INPUT_EVENT_ALT_DIO -1
#endif
#ifndef BOARD_INPUT_EVENT_ACTIVE_LOW
#define BOARD_INPUT_EVENT_ACTIVE_LOW 1
#endif
#ifndef BOARD_INPUT_BUTTON_DIO
#define BOARD_INPUT_BUTTON_DIO -1
#endif
#ifndef BOARD_INPUT_BUTTON_ACTIVE_LOW
#define BOARD_INPUT_BUTTON_ACTIVE_LOW 1
#endif
// pulled to the inactive level unless the board says otherwise (a switch to GND that is "active" when open, or a
// push-pull comparator output with BOARD_PULL_NONE)
#ifndef BOARD_INPUT_EVENT_PULL_UP
#define BOARD_INPUT_EVENT_PULL_UP BOARD_INPUT_EVENT_ACTIVE_LOW
#endif
#ifndef BOARD_INPUT_BUTTON_PULL_UP
#define BOARD_INPUT_BUTTON_PULL_UP BOARD_INPUT_BUTTON_ACTIVE_LOW
#endif
#ifndef BOARD_INPUT_EVENT_PULL
#define BOARD_INPUT_EVENT_PULL (BOARD_INPUT_EVENT_PULL_UP ? BOARD_PULL_UP : BOARD_PULL_DOWN)
#endif
#ifndef BOARD_INPUT_BUTTON_PULL
#define BOARD_INPUT_BUTTON_PULL (BOARD_INPUT_BUTTON_PULL_UP ? BOARD_PULL_UP : BOARD_PULL_DOWN)
#endif

void clock_init(void);  // clock.c
void input_init(const int dio[INPUT_COUNT], const bool active_low[INPUT_COUNT], const int pull[INPUT_COUNT],
                int event_alt_dio);

const PINCC26XX_HWAttrs PINCC26XX_hwAttrs = {
    .intPriority = ~0,
    .swiPriority = 0,
};

// A battery board (BOARD_LOW_POWER) goes to standby whenever the app waits; a gateway or a board with a console
// stays awake (mains-powered, always receiving). The RC oscillators stay calibrated either way.
const PowerCC26XX_Config PowerCC26XX_config = {
    .policyInitFxn = NULL,
    .policyFxn = BOARD_LOW_POWER ? &PowerCC26XX_standbyPolicy : NULL,
    .calibrateFxn = &PowerCC26XX_calibrate,
    .enablePolicy = BOARD_LOW_POWER,
    .calibrateRCOSC_LF = true,
    .calibrateRCOSC_HF = true,
};

const RFCC26XX_HWAttrsV2 RFCC26XX_hwAttrs = {
    .hwiPriority = ~0,
    .swiPriority = 0,
    .xoscHfAlwaysNeeded = true,
    .globalCallback = NULL,
    .globalEventMask = 0,
};

// OpenOCD resets the CC13x0 with VECTRESET, which restarts only the CPU. The TI drivers assume power-on state,
// and two leftovers hang the RF driver forever: an RF core that is still powered and booted (its BOOT_DONE has
// come and gone), and an HF clock still on the crystal (the Power driver then never schedules the switch to it,
// and the RF driver waits for one). Put both back as after power-on.
static void undo_cpu_only_reset(void)
{
    // only if it is on: RFCClockDisable() writes inside the RF core, a bus fault when its domain is off
    if (PRCMPowerDomainStatus(PRCM_DOMAIN_RFCORE) != PRCM_DOMAIN_POWER_OFF) {
        RFCClockDisable();
        PRCMPowerDomainOff(PRCM_DOMAIN_RFCORE);
        while (PRCMPowerDomainStatus(PRCM_DOMAIN_RFCORE) != PRCM_DOMAIN_POWER_OFF) {
        }
    }
    if (OSCClockSourceGet(OSC_SRC_CLK_HF) != OSC_RCOSC_HF) {
        OSCHF_SwitchToRcOscTurnOffXosc();
    }
}

static const int led_dio[] = {BOARD_LED_OK_DIO, BOARD_LED_ERROR_DIO};

// A battery board stays out of standby for its first DEBUG_WINDOW_MS: cheap insurance for a board wired by TMS/TCK
// only (no reset line). Standby itself does not lock the debugger out (the LaunchPad, sleeping at once, was reached
// over 2-pin and 4-pin JTAG without a reset, 2026-10-24); a v1 contact that became unreachable after our image went
// on was more likely damaged.
#ifdef OWS_DEBUG_WINDOW_MS
#define DEBUG_WINDOW_MS OWS_DEBUG_WINDOW_MS
#else
#define DEBUG_WINDOW_MS 10000
#endif
#define CONSOLE_AWAKE_MS (5u * 60 * 1000)

static ClockP_Struct debug_window;
static bool debug_window_ready;
static volatile bool console_awake;
static bool permanently_awake;

static uint32_t ms_to_ticks(uint32_t ms)
{
    return (uint32_t)((uint64_t)ms * 1000u / ClockP_getSystemTickPeriod());
}

static void debug_window_over(uintptr_t arg)
{
    (void)arg;
    uintptr_t key = HwiP_disable();
    if (console_awake) {
        console_awake = false;
        if (!permanently_awake) {
            uart_sleep();
        }
        Power_releaseConstraint(PowerCC26XX_DISALLOW_STANDBY);
    }
    HwiP_restore(key);
}

void board_init(void)
{
    static const PIN_Config no_pins[] = {PIN_TERMINATE};  // the PIN driver, for the inputs (input.c)
    static const int input_dio[INPUT_COUNT] = {BOARD_INPUT_EVENT_DIO, BOARD_INPUT_BUTTON_DIO};
    static const bool input_low[INPUT_COUNT] = {BOARD_INPUT_EVENT_ACTIVE_LOW, BOARD_INPUT_BUTTON_ACTIVE_LOW};
    static const int input_pull[INPUT_COUNT] = {BOARD_INPUT_EVENT_PULL, BOARD_INPUT_BUTTON_PULL};
    undo_cpu_only_reset();
    Power_init();
    PIN_init(no_pins);  // init resets every pin: before the LEDs and UART
    Power_setDependency(PowerCC26XX_PERIPH_GPIO);
    for (unsigned i = 0; i < sizeof led_dio / sizeof led_dio[0]; i++) {
        if (led_dio[i] < 0) {
            continue;
        }
        IOCPinTypeGpioOutput((uint32_t)led_dio[i]);
        GPIO_clearDio((uint32_t)led_dio[i]);
    }
    NoRTOS_start();
    debug_window_ready = false;
    console_awake = false;
    permanently_awake = false;
    if (BOARD_LOW_POWER && DEBUG_WINDOW_MS != 0) {
        ClockP_Params cp;
        ClockP_Params_init(&cp);
        cp.startFlag = true;
        console_awake = true;
        Power_setConstraint(PowerCC26XX_DISALLOW_STANDBY);
        ClockP_construct(&debug_window, debug_window_over, ms_to_ticks(DEBUG_WINDOW_MS), &cp);
        debug_window_ready = true;
    }
    clock_init();
#ifdef OWS_NO_CONSOLE
    uart_init(-1, -1);
#else
    uart_init(BOARD_UART_RX, BOARD_UART_TX);
#endif
    input_init(input_dio, input_low, input_pull, BOARD_INPUT_EVENT_ALT_DIO);
}

bool board_battery(void)
{
    return BOARD_BATTERY;
}

bool board_usb_hid(void)
{
    return BOARD_USB_HID;
}

void board_led(int led, bool on)
{
    if (led < 0 || led >= (int)(sizeof led_dio / sizeof led_dio[0]) || led_dio[led] < 0) {
        return;
    }
    GPIO_writeDio((uint32_t)led_dio[led], on ? 1 : 0);
}

void board_keep_awake(void)
{
    if (!permanently_awake) {
        Power_setConstraint(PowerCC26XX_DISALLOW_STANDBY);
        permanently_awake = true;
    }
}

void board_console_touch(void)
{
    if (!debug_window_ready) {
        return;
    }
    uintptr_t key = HwiP_disable();
    if (console_awake) {
        ClockP_Handle h = ClockP_handle(&debug_window);
        ClockP_stop(h);
        ClockP_setTimeout(h, ms_to_ticks(CONSOLE_AWAKE_MS));
        ClockP_start(h);
    }
    HwiP_restore(key);
}

void board_console_sleep(void)
{
    if (!debug_window_ready) {
        return;
    }
    uintptr_t key = HwiP_disable();
    ClockP_stop(ClockP_handle(&debug_window));
    bool sleep = console_awake;
    if (console_awake) {
        console_awake = false;
        Power_releaseConstraint(PowerCC26XX_DISALLOW_STANDBY);
    }
    HwiP_restore(key);
    if (sleep && !permanently_awake) {
        uart_sleep();
    }
}

bool board_console_awake(void)
{
    return !BOARD_LOW_POWER || permanently_awake || console_awake;
}

bool board_mac(uint8_t mac[8])
{
    if (!BOARD_MAC_IN_CCFG) {
        return false;
    }
    uint8_t m[8];
    memcpy(m, (const void *)0x1FFCC, 8);  // CCFG IEEE_MAC_1 + IEEE_BLE_0: the stock sensors' ASCII MAC
    for (int i = 0; i < 8; i++) {         // erased, zeroed (known-issues.md §1) or garbage: not a MAC
        if (!((m[i] >= '0' && m[i] <= '9') || (m[i] >= 'A' && m[i] <= 'F'))) {
            return false;  // mac untouched: the caller's fallback stands
        }
    }
    if (!memcmp(m, "00000000", 8)) {
        return false;
    }
    memcpy(mac, m, 8);
    return true;
}

bool board_expects_mac(void)
{
    return BOARD_MAC_IN_CCFG;
}

uint32_t boot_board(void)
{
    return boot_board_id(BOARD_NAME);
}

uint32_t board_pin_levels(uint32_t mask, int pull)
{
    uint32_t p = pull == BOARD_PULL_UP ? IOC_IOPULL_UP : pull == BOARD_PULL_DOWN ? IOC_IOPULL_DOWN : IOC_NO_IOPULL;
    for (uint32_t d = 0; d < 32; d++) {
        if (mask >> d & 1) {
            IOCPortConfigureSet(d, IOC_PORT_GPIO, (IOC_STD_INPUT & ~IOC_NO_IOPULL) | p);
        }
    }
    return GPIO_readMultiDio(mask);
}
