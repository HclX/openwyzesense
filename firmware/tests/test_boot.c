// The boot selector's choice (common/boot_slot.c) on a simulated 128 KB flash: newest usable slot, probation and
// fallback for an unconfirmed image, CRC and board checks, the last resort.
#include <stdio.h>
#include <string.h>

#include "boot_slot.h"
#include "mac154.h"

static int failures, checks;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        checks++;                                                                    \
        if (!(cond)) {                                                               \
            failures++;                                                              \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                            \
    } while (0)

static uint8_t flash[0x20000];
static int programs;

static bool program_word(void *ctx, uint32_t addr, uint32_t value)
{
    uint32_t w;
    (void)ctx;
    memcpy(&w, flash + addr, 4);
    CHECK((w & value) == value);  // flash: bits only go from 1 to 0
    w &= value;
    memcpy(flash + addr, &w, 4);
    programs++;
    return true;
}

static int erases;
static uint32_t erased[64];

static bool erase_page(void *ctx, uint32_t addr)
{
    (void)ctx;
    CHECK(addr % BOOT_PAGE == 0);
    memset(flash + addr, 0xFF, BOOT_PAGE);
    if (erases < 64) {
        erased[erases] = addr;
    }
    erases++;
    return true;
}

static bool program(void *ctx, uint32_t addr, const uint8_t *data, size_t len)
{
    (void)ctx;
    for (size_t i = 0; i < len; i++) {
        CHECK((flash[addr + i] & data[i]) == data[i]);  // never over bits already cleared: erased first
        flash[addr + i] &= data[i];
    }
    return true;
}

static const boot_flash_t f = {flash, program_word, NULL, erase_page, program};
static const boot_flash_t selector = {.flash = flash};  // the boot selector only reads
static uint32_t board;

// A boot: the selector picks, the app (if anything was picked) takes an attempt; the slot run
static int boot(void)
{
    int slot = boot_choose(&selector, board);
    if (slot >= 0) {
        boot_take_attempt(&f, slot);
    }
    return slot;
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

// An image in a slot: header + valid ARM vector table + size bytes of pseudo-code
static void install(int slot, uint32_t version, uint32_t board_id, uint32_t size, bool confirmed)
{
    uint32_t slot_base = boot_slot_base(slot);
    uint8_t *base = flash + slot_base;
    memset(base, 0xFF, SLOT_SIZE);
    for (uint32_t i = 0; i < size; i++) {
        base[SLOT_IMAGE_OFFSET + i] = (uint8_t)(i * 31 + version);
    }
    if (size >= 8 && size <= SLOT_SIZE - SLOT_IMAGE_OFFSET) {
        put_le32(base + SLOT_IMAGE_OFFSET, 0x20005000u);
        put_le32(base + SLOT_IMAGE_OFFSET + 4, (slot_base + SLOT_IMAGE_OFFSET + (size > 16 ? 16u : 0u)) | 1u);
    }
    slot_header_t h;
    memset(&h, 0xFF, sizeof h);
    h.magic = SLOT_MAGIC;
    h.board_id = board_id;
    h.version = version;
    h.size = size;
    h.crc = size <= SLOT_SIZE - SLOT_IMAGE_OFFSET ? crc32_ieee(base + SLOT_IMAGE_OFFSET, size) : 0;
    h.slot = (uint32_t)slot;
    if (confirmed) {
        h.confirmed = 0;
    }
    memcpy(base, &h, sizeof h);
}

static void erase(void)
{
    memset(flash, 0xFF, sizeof flash);
}

int main(void)
{
    board = boot_board_id("Wyze Sense v1 contact");
    CHECK(board != boot_board_id("LAUNCHXL-CC1310"));
    CHECK(sizeof(slot_header_t) == 80);
    CHECK(boot_slot_of(0) == -1 && boot_slot_of(SLOT_A_BASE - 1) == -1);
    CHECK(boot_slot_of(SLOT_A_BASE) == 0 && boot_slot_of(SLOT_A_BASE + SLOT_SIZE - 1) == 0);
    CHECK(boot_slot_of(SLOT_B_BASE) == 1 && boot_slot_of(SLOT_B_BASE + SLOT_SIZE - 1) == 1);
    CHECK(boot_slot_of(SLOT_B_BASE + SLOT_SIZE) == -1 && boot_slot_of(0x1FFFFu) == -1);

    erase();
    CHECK(boot() == -1);  // nothing installed

    erase();
    install(0, 1, board, 30000, true);
    programs = 0;
    CHECK(boot() == 0 && programs == 0);  // confirmed: no attempt counted

    // an update in B: newer, on probation for SLOT_ATTEMPTS boots, then back to A
    install(1, 2, board, 31000, false);
    for (int i = 0; i < SLOT_ATTEMPTS; i++) {
        CHECK(boot() == 1);
    }
    CHECK(boot() == 0);
    CHECK(boot() == 0);

    // the same update confirming itself after its first boot: B for good
    install(1, 2, board, 31000, false);
    CHECK(boot() == 1);
    CHECK(boot_confirm(&f, 1));
    for (int i = 0; i < 2 * SLOT_ATTEMPTS; i++) {
        CHECK(boot() == 1);
    }
    programs = 0;
    CHECK(boot_confirm(&f, 1) && programs == 0);  // already confirmed

    // B broken (one bit), for another board, or with a size that cannot be: A
    install(1, 3, board, 31000, true);
    flash[SLOT_B_BASE + SLOT_IMAGE_OFFSET + 1234] ^= 0x04;
    CHECK(boot() == 0);
    install(1, 3, board ^ 1, 31000, true);
    CHECK(boot() == 0);
    install(1, 3, board, SLOT_SIZE, true);
    CHECK(boot() == 0);

    // equal versions: A; an older B: A
    install(1, 1, board, 1000, true);
    CHECK(boot() == 0);

    // last resort: only an unconfirmed image with its attempts used up (a wired install never paired): still run
    erase();
    install(0, 1, board, 30000, false);
    for (int i = 0; i < SLOT_ATTEMPTS + 2; i++) {
        CHECK(boot() == 0);
    }
    // ... but never a broken one
    flash[SLOT_A_BASE + SLOT_IMAGE_OFFSET] ^= 0x80;
    CHECK(boot() == -1);

    // an OTA update written into B in 96-byte blocks: each page erased once, as the data reaches it
    erase();
    install(0, 1, board, 30000, true);
    static uint8_t img[SLOT_IMAGE_OFFSET + 20000];
    memcpy(img, flash + SLOT_A_BASE, sizeof img);  // build a valid slot-B image, version 7
    put_le32(img + SLOT_IMAGE_OFFSET, 0x20005000u);
    put_le32(img + SLOT_IMAGE_OFFSET + 4, (SLOT_B_BASE + SLOT_IMAGE_OFFSET + 16u) | 1u);
    slot_header_t *h = (slot_header_t *)(void *)img;
    h->version = 7;
    h->confirmed = 0xFFFFFFFF;
    h->size = 20000;
    h->slot = 1;
    h->crc = crc32_ieee(img + SLOT_IMAGE_OFFSET, 20000);
    memset(flash + SLOT_B_BASE, 0x00, SLOT_SIZE);  // whatever was there: all bits clear
    erases = 0;
    for (uint32_t off = 0; off < sizeof img; off += 96) {
        uint32_t n = sizeof img - off < 96 ? (uint32_t)(sizeof img - off) : 96;
        CHECK(boot_write(&f, 1, off, img + off, n));
        if (off == 96 * 50) {
            CHECK(!boot_written_ok(&f, 1, board, 7));  // half written: not yet
        }
    }
    CHECK(erases == 5);  // 20256 bytes: pages 0-4 of the slot
    for (int i = 0; i < 5; i++) {
        CHECK(erased[i] == SLOT_B_BASE + BOOT_PAGE * (uint32_t)i);
    }
    CHECK(boot_written_ok(&f, 1, board, 7) && !boot_written_ok(&f, 1, board, 8) &&
          !boot_written_ok(&f, 1, board ^ 1, 7));
    CHECK(boot_slot_version(&f, 1) == 7);
    CHECK(boot() == 1);  // the update runs, on probation

    // cross-slot guard (TASK-08): a slot-A linked image written into slot B (even with valid CRC and unstamped slot)
    // must be rejected by both boot_written_ok() and boot_choose() so a mismatched image never boots
    put_le32(img + SLOT_IMAGE_OFFSET + 4, (SLOT_A_BASE + SLOT_IMAGE_OFFSET + 16u) | 1u);
    h->version = 8;
    h->slot = 0xFFFFFFFFu;
    h->crc = crc32_ieee(img + SLOT_IMAGE_OFFSET, 20000);
    for (uint32_t off = 0; off < sizeof img; off += 96) {
        uint32_t n = sizeof img - off < 96 ? (uint32_t)(sizeof img - off) : 96;
        CHECK(boot_write(&f, 1, off, img + off, n));
    }
    CHECK(!boot_written_ok(&f, 1, board, 8));
    CHECK(!boot_slot_ok(&f, 1, board));
    CHECK(boot() == 0);  // falls back to confirmed slot A, never boots wrong-slot B

    // and a slot-B stamped header with a slot-B reset vector written into slot A is likewise rejected
    erase();
    install(1, 2, board, 20000, true);
    memcpy(flash + SLOT_A_BASE, flash + SLOT_B_BASE, SLOT_IMAGE_OFFSET + 20000);
    slot_header_t *ha = (slot_header_t *)(void *)(flash + SLOT_A_BASE);
    ha->version = 9;
    CHECK(!boot_slot_ok(&f, 0, board));
    CHECK(boot() == 1);

    // restore clean state for the remaining boundary checks
    erase();
    install(0, 1, board, 30000, true);
    put_le32(img + SLOT_IMAGE_OFFSET + 4, (SLOT_B_BASE + SLOT_IMAGE_OFFSET + 16u) | 1u);
    h->version = 7;
    h->slot = 1;
    h->crc = crc32_ieee(img + SLOT_IMAGE_OFFSET, 20000);
    // started again from 0 (the sensor reset during a transfer): the slot is erased again, page by page
    erases = 0;
    CHECK(boot_write(&f, 1, 0, img, 96));
    CHECK(erases == 1 && !boot_written_ok(&f, 1, board, 7));
    CHECK(boot() == 0);
    // nothing outside the slot
    CHECK(!boot_write(&f, 1, SLOT_SIZE - 10, img, 96));
    CHECK(!boot_write(&f, 2, 0, img, 96));
    CHECK(!boot_write(&f, -1, 0, img, 96));

    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
