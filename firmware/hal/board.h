// The board: power-on init, LEDs, the MAC it keeps. With board_config.h, the board's pins and features.
// (hal/README.md: the hardware interface the apps use.)
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "board_config.h"  // BOARD_NAME, BOARD_UART_RX/TX, BOARD_LED_OK_DIO / BOARD_LED_ERROR_DIO (-1: none)

enum { BOARD_LED_OK, BOARD_LED_ERROR };

// Power, clocks, pins and the console (uart.h), as after a power-on (also after a debugger's CPU-only reset).
void board_init(void);

// A LED on or off; nothing on a board without it.
void board_led(int led, bool on);

// Never standby from now on (a gateway on a coin-cell board: its host link must keep receiving).
void board_keep_awake(void);

// On a coin-cell board (BOARD_LOW_POWER): extends the power-up console window to 5 minutes from the last valid
// console character, or ends the console window immediately so the board can enter standby.
void board_console_touch(void);
void board_console_sleep(void);
bool board_console_awake(void);

// A coin-cell board (BOARD_LOW_POWER in its board_config.h): its LED must not stay on.
bool board_battery(void);

// A board whose UART0 sits behind the Wyze dongle's CH554 USB-HID bridge (BOARD_USB_HID): raw software SBL entry is
// refused because the CH554 discards SBL replies unless it intercepted 0x4312 itself.
bool board_usb_hid(void);

// The sensor MAC (8 ASCII characters) the board keeps, as the stock sensors do in their CCFG; false if none or not
// 8 hex digits (erased, zeroed), and then mac is left as it was.
bool board_mac(uint8_t mac[8]);

// True on boards that store their factory sensor MAC in CCFG (BOARD_MAC_IN_CCFG).
bool board_expects_mac(void);

// Debugging a new board: the DIOs in mask become inputs (pull: BOARD_PULL_*); their levels (bit n: DIO n). The host
// board: 0. Not for pins in use (the console, LEDs).
enum { BOARD_PULL_UP, BOARD_PULL_NONE, BOARD_PULL_DOWN };
uint32_t board_pin_levels(uint32_t mask, int pull);
