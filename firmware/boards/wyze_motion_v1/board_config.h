// The Wyze Sense v1 motion sensor: a CC1310F128, 5x5 package (ground_truth/firmware/motion_v1_777A9A46).
// Its pins (stock BoardGpioInitTable at 0x156B0, UART hwattrs at 0x15618, and Sensor_process in stock_sensor.c):
// UART TX DIO0, RX DIO1 (also the stock CCFG ROM bootloader backdoor pin: BL_CONFIG = 0xC5FE01C5); factory test W pad
// DIO2; dual-window PIR comparator outputs on DIO9 (primary) and DIO10 (alt), push-pull active-high; red LED on DIO12
// (active-high); pairing button on DIO13 (active-low, internal pull-up).
#pragma once

#define BOARD_NAME                   "Wyze Sense v1 motion"
#define BOARD_UART_RX                1   // the RX pad (and stock CCFG SBL backdoor, active low)
#define BOARD_UART_TX                0   // the TX pad
#define BOARD_LED_OK_DIO             12  // red LED (active high)
#define BOARD_LED_ERROR_DIO          -1
#define BOARD_INPUT_EVENT_DIO        9   // PIR comparator primary output (active high, push-pull)
#define BOARD_INPUT_EVENT_ALT_DIO    10  // PIR comparator secondary output (ORed with DIO9)
#define BOARD_INPUT_EVENT_ACTIVE_LOW 0
#define BOARD_INPUT_EVENT_PULL       1   // BOARD_PULL_NONE: comparator drives push-pull, no pull resistor current
#define BOARD_INPUT_BUTTON_DIO       13  // pairing button (pressed: low, internal pull-up)
#define BOARD_MAC_IN_CCFG            1   // the sensor's ASCII MAC at 0x1FFCC (CCFG IEEE_MAC_1 + IEEE_BLE_0)
#define BOARD_LOW_POWER              1   // a coin cell: standby between events
