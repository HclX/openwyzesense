// The Wyze Sense USB dongle (WHSB1): a CC1310F128 behind a CH554 USB-HID bridge (FACTS.md §5, §9).
// From the stock firmware (ground_truth/firmware/gateway_2025-02-08_v1.8): the host link is UART0 on DIO0/DIO1.
#pragma once

#define BOARD_NAME    "Wyze Sense dongle"
#define BOARD_UART_RX 1  // DIO1, from the CH554 (UART hwattrs at flash 0x17328: tx 0, rx 1)
#define BOARD_UART_TX 0  // DIO0
// The stock PIN table (flash 0x174C0) makes DIO10 and DIO13 outputs, low: the dongle's LED(s). Which colour is
// which is not known.
#define BOARD_LED_OK_DIO    10
#define BOARD_LED_ERROR_DIO 13
#define BOARD_USB_HID       1  // UART0 sits behind the CH554 USB-HID bridge: never enter raw SBL behind its back
