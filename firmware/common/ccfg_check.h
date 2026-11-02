// The CC13x0 customer configuration (CCFG: the last 88 bytes of flash, 0x1FFA8 on a CC1310F128), checked before
// our firmware ever writes one (hal/ccfg.h): what keeps a chip reachable after it.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define CCFG_SIZE 0x58

// Whether ccfg keeps the chip recoverable: ROM bootloader enabled with its backdoor pin on, CPU debug port
// enabled, chip and bank erase not disabled, no flash sector write-protected (that is permanent), the flash image
// valid. *why: what is wrong, if not.
bool ccfg_is_safe(const uint8_t ccfg[CCFG_SIZE], const char **why);

// The device's own identity in a CCFG, not the firmware's: IEEE_MAC_0/1 and IEEE_BLE_0/1 (a stock v1 motion sensor
// keeps its ASCII MAC in IEEE_MAC_1 + IEEE_BLE_0, FACTS.md §5). A restore keeps these from the chip.
#define CCFG_IDENTITY_OFF  0x20
#define CCFG_IDENTITY_SIZE 0x10

// out = built-in, with the identity fields of current: what a restore writes
void ccfg_merge(const uint8_t builtin[CCFG_SIZE], const uint8_t current[CCFG_SIZE], uint8_t out[CCFG_SIZE]);
