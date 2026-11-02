// The host board (boards/host/host_board.h): a simulated device for native tests of the apps.
#pragma once

#define BOARD_NAME          "host"
#define BOARD_UART_RX       0  // a console: host_console_send() / host_console_take()
#define BOARD_UART_TX       0
#define BOARD_LED_OK_DIO    0
#define BOARD_LED_ERROR_DIO 1
