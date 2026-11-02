// The app's side of the two-slot boot (docs/ota.md, common/boot_slot.h): which slot it runs from, using up an
// attempt while on probation, and confirming itself once it has run well.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// 0 / 1: slot A / B; -1: not a slot image (installed at 0x0, no boot selector)
int boot_running_slot(void);

// Call first thing after start-up: an unconfirmed image uses up one of its attempts.
void boot_running_take_attempt(void);

// Whether the running image has confirmed itself (true if not a slot image: nothing to confirm)
bool boot_running_confirmed(void);

// The running image works: no more attempts counted (a no-op if confirmed already, or not a slot image).
bool boot_running_confirm(void);

// --- an OTA update (docs/ota.md): into the slot not running ---

// The running image's version (its slot header); 0 if not a slot image
uint32_t boot_running_version(void);

// This board's ID (boot_board_id(BOARD_NAME)): an update must carry it
uint32_t boot_board(void);

// Writes the update into the other slot, in order from offset 0 (slot header, then image); false if this is not a
// slot image, the data would leave the slot, or the flash fails
bool boot_update_write(uint32_t off, const uint8_t *data, size_t len);

// The other slot now holds a whole image for this board with this version (the boot selector will take it)
bool boot_update_ok(uint32_t version);
