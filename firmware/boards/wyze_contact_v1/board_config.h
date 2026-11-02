// The Wyze Sense v1 contact (door) sensor: a CC1310F128, 5x5 package (ground_truth/firmware/contact_v1_777B42DF_2026-10-24).
// Its pins: UART TX DIO0, RX DIO1 (the stock UART hwattrs at flash 0x15280: "00 01 ff ff", as on the dongle; the RX/TX
// pads); the reed switch on chip pin 20 = DIO7 (the user, by continuity, 2026-10-24); the LED DIO9 (the only output
// in the stock PIN table at 0x1532C, lit by our firmware); the W pad DIO10; the S pad RESET_N; the button DIO11
// (pressed: low; found with the sensor app's "watch", 2026-10-24). JTAG is 2-pin only (TMS, TCK on the M / C pads):
// TI UniFlash, not OpenOCD.
#pragma once

#define BOARD_NAME                   "Wyze Sense v1 contact"
#define BOARD_UART_RX                1  // the RX pad
#define BOARD_UART_TX                0  // the TX pad
#define BOARD_LED_OK_DIO             9
#define BOARD_LED_ERROR_DIO          -1
#define BOARD_INPUT_EVENT_DIO        7  // the reed switch: closed (magnet near) pulls it low; open = high = "open"
#define BOARD_INPUT_EVENT_ACTIVE_LOW 0
#define BOARD_INPUT_EVENT_PULL_UP    1   // the reed switch closes to GND (the stock PIN table: pull-ups only)
#define BOARD_INPUT_BUTTON_DIO       11  // the pairing button (pressed: low)
#define BOARD_MAC_IN_CCFG            1   // the sensor's ASCII MAC at 0x1FFCC (CCFG IEEE_MAC_1 + IEEE_BLE_0)
#define BOARD_LOW_POWER              1   // a coin cell: standby between events
