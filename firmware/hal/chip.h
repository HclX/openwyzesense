// The chip: its factory address and its battery monitor.
#pragma once

#include <stdbool.h>
#include <stdint.h>

// The IEEE 802.15.4 address programmed at the factory.
uint64_t chip_ieee(void);

// Die temperature, degC.
int chip_temperature_c(void);

// Supply (battery) voltage, mV.
uint32_t chip_battery_mv(void);

// The supply under a flash erase / program (#75): an old cell sags then too, and a write at low battery is how the
// stock v1 lost its MAC (known-issues.md §1). chip_flash_begin() just before the operation, chip_flash_end() just
// after it (with interrupts still held): the battery as it read at its end. chip_flash_mv(): the last operation's,
// chip_flash_min_mv(): the lowest since boot; 0: none yet.
void chip_flash_begin(void);
void chip_flash_end(void);
uint32_t chip_flash_mv(void);
uint32_t chip_flash_min_mv(void);

// A full system reset (the boot selector picks the slot to run again).
void chip_reset(void);

// The hardware watchdog (#85): started once, then fed at least every timeout_ms; not fed for twice that, the chip
// resets (a system reset). It pauses while a debugger halts the CPU. Not for apps that sleep in standby (it stops).
void chip_watchdog_start(uint32_t timeout_ms);
void chip_watchdog_feed(void);

// True when the chip's CCFG enables the ROM serial bootloader ((BL_CONFIG >> 24) == 0xC5).
bool chip_can_enter_sbl(void);

// Enters the CC13x0 ROM serial bootloader over UART0 without toggling backdoor or reset pins (DESIGN.md §6.6).
// Returns false if the CCFG disables the bootloader (never returns on hardware when enabled).
bool chip_enter_sbl(void);
