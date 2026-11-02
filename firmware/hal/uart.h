// UART0, 115200 8N1, like the stock dongle's host link (docs/FACTS.md §5). On the LaunchPad it is the XDS110's
// application UART (/dev/ttyACM0 on the host).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Pins < 0: the board has no UART, and the functions below do nothing. board_init() calls it.
void uart_init(int rx_dio, int tx_dio);
void uart_write(const uint8_t *data, size_t n);
void uart_puts(const char *s);

// Waits until everything written has left the UART (before a reset).
void uart_flush(void);

// Flushes TX, disables the UART and releases its power domain (before standby on a battery board).
void uart_sleep(void);

// Takes one received byte, if any. Received bytes wake clock_wait().
bool uart_getc(uint8_t *c);
