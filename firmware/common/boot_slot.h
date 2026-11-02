// Two firmware slots behind a boot selector (docs/ota.md): the slot header, which slot to run, and confirming a new
// image. Portable: flash is read through a pointer and programmed through a callback, so the boot selector
// (firmware/boot/), the apps (confirming) and the native tests share it.
//
// A slot: its header (256 bytes, below), then the image: vector table first, linked for slot base + 0x100.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SLOT_A_BASE       0x01000u     // flash pages 1-14
#define SLOT_B_BASE       0x0F000u     // pages 15-28
#define SLOT_SIZE         0xE000u      // 56 KB
#define SLOT_IMAGE_OFFSET 0x100u       // the vector table (VTOR wants 256-byte alignment for 50 vectors)
#define SLOT_MAGIC        0x4953574Fu  // "OWSI"
#define SLOT_ATTEMPTS     3            // boots an unconfirmed image gets before the selector gives up on it

typedef struct {
    uint32_t magic;
    uint32_t board_id;  // boot_board_id(BOARD_NAME): an image runs only on its board
    uint32_t version;   // higher runs first
    uint32_t size;      // image bytes after the header
    uint32_t crc;       // CRC-32 of those bytes
    uint32_t slot;      // target slot: 0 (A), 1 (B), or 0xFFFFFFFF (legacy unstamped)
    uint32_t reserved[10];
    // Left 0xFFFFFFFF in the image; programmed to 0 later, one word each, never erased (flash only clears bits):
    uint32_t confirmed;                // by the image itself, once it has run well (joined, reported)
    uint32_t attempts[SLOT_ATTEMPTS];  // one taken by the selector per boot of an unconfirmed image
} slot_header_t;                       // 80 bytes; the rest of the 256 stays 0xFF

// Flash access: flash points at flash address 0 (memory-mapped on the chip, an array in tests).
typedef struct {
    const uint8_t *flash;
    bool (*program_word)(void *ctx, uint32_t addr, uint32_t value);  // clear bits of one word
    void *ctx;
    // for boot_write (an OTA update; may be NULL otherwise)
    bool (*erase_page)(void *ctx, uint32_t addr);  // the 4 KB page at addr
    bool (*program)(void *ctx, uint32_t addr, const uint8_t *data, size_t len);
} boot_flash_t;

#define BOOT_PAGE 0x1000u

uint32_t boot_board_id(const char *board_name);

// Whether a slot holds an image for this board and slot whose CRC checks, and that has not used up its attempts.
bool boot_slot_ok(const boot_flash_t *f, int slot, uint32_t board_id);

// The boot selector: the slot to run, the usable one with the higher version (A on a tie); -1 if none. Read-only
// (writing flash needs the device trimmed, which the app's start-up does): program_word may be NULL.
int boot_choose(const boot_flash_t *f, uint32_t board_id);

// The app, first thing after its start-up: if it runs on probation (unconfirmed), it uses up an attempt, so an
// image that keeps failing is dropped after SLOT_ATTEMPTS boots.
void boot_take_attempt(const boot_flash_t *f, int slot);

// The running image says it works (the app, after it has joined / reported): no more attempts counted.
bool boot_confirm(const boot_flash_t *f, int slot);

// An update into a slot (never the running one: the caller's business), in order from offset 0 (the slot header,
// then the image, as a slot-image file has them): each page is erased as the data first reaches it. False if the
// data would leave the slot, or the flash fails.
bool boot_write(const boot_flash_t *f, int slot, uint32_t off, const uint8_t *data, size_t len);

// What boot_write wrote is a whole image for this board, slot, and version: the boot selector will take it
bool boot_written_ok(const boot_flash_t *f, int slot, uint32_t board_id, uint32_t version);

uint32_t boot_slot_version(const boot_flash_t *f, int slot);

bool boot_is_confirmed(const boot_flash_t *f, int slot);

// Which slot an address (e.g. the app's own code) is in; -1 if neither
int boot_slot_of(uint32_t addr);

uint32_t boot_slot_base(int slot);
