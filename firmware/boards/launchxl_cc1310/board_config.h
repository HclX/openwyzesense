// TI LAUNCHXL-CC1310: the development board
#pragma once

#define BOARD_NAME             "LAUNCHXL-CC1310"
#define BOARD_UART_RX          2   // DIO2: the XDS110's application UART (/dev/ttyACM0 on the host)
#define BOARD_UART_TX          3   // DIO3
#define BOARD_LED_OK_DIO       7   // green
#define BOARD_LED_ERROR_DIO    6   // red
#define BOARD_INPUT_EVENT_DIO  13  // BTN-1 (pressed: low): stands in for a reed switch / PIR (pressed = open)
#define BOARD_INPUT_BUTTON_DIO 14  // BTN-2 (pressed: low): the pairing button
