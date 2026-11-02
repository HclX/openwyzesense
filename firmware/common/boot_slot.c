#include "boot_slot.h"

#include <stddef.h>

#include "mac154.h"  // crc32_ieee

uint32_t boot_board_id(const char *board_name)
{
    size_t n = 0;
    while (board_name[n]) {
        n++;
    }
    return crc32_ieee((const uint8_t *)board_name, n);
}

uint32_t boot_slot_base(int slot)
{
    return slot == 0 ? SLOT_A_BASE : SLOT_B_BASE;
}

static const slot_header_t *header(const boot_flash_t *f, int slot)
{
    return (const slot_header_t *)(const void *)(f->flash + boot_slot_base(slot));
}

static bool attempts_left(const slot_header_t *h)
{
    if (h->confirmed == 0) {
        return true;
    }
    for (int i = 0; i < SLOT_ATTEMPTS; i++) {
        if (h->attempts[i] != 0) {
            return true;
        }
    }
    return false;
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// Verify ARM Cortex-M3 initial MSP and Thumb reset vector lie strictly inside this slot's linked image range.
static bool vector_table_ok(const uint8_t *image, uint32_t base, uint32_t size)
{
    uint32_t sp = le32(image);
    uint32_t reset = le32(image + 4);
    if (sp <= 0x20000000u || sp > 0x20005000u || (sp & 3u) != 0) {
        return false;
    }
    uint32_t entry = reset & ~1u;
    uint32_t code_start = base + SLOT_IMAGE_OFFSET;
    return (reset & 1u) != 0 && entry >= code_start && entry < code_start + size;
}

// An image for this board and this specific slot, whole (its vector table and CRC check)
static bool slot_intact(const boot_flash_t *f, int slot, uint32_t board_id)
{
    if (slot < 0 || slot > 1) {
        return false;
    }
    uint32_t base = boot_slot_base(slot);
    const slot_header_t *h = header(f, slot);
    if (h->magic != SLOT_MAGIC || h->board_id != board_id) {
        return false;
    }
    if (h->slot != 0xFFFFFFFFu && h->slot != (uint32_t)slot) {
        return false;
    }
    if (h->size < 8 || h->size > SLOT_SIZE - SLOT_IMAGE_OFFSET) {
        return false;
    }
    const uint8_t *image = f->flash + base + SLOT_IMAGE_OFFSET;
    if (!vector_table_ok(image, base, h->size)) {
        return false;
    }
    return crc32_ieee(image, h->size) == h->crc;
}

bool boot_slot_ok(const boot_flash_t *f, int slot, uint32_t board_id)
{
    return slot_intact(f, slot, board_id) && attempts_left(header(f, slot));
}

static int newer(const boot_flash_t *f, bool a, bool b)
{
    return a && b ? (header(f, 1)->version > header(f, 0)->version ? 1 : 0) : a ? 0 : b ? 1 : -1;
}

int boot_choose(const boot_flash_t *f, uint32_t board_id)
{
    int slot = newer(f, boot_slot_ok(f, 0, board_id), boot_slot_ok(f, 1, board_id));
    // Last resort: an intact image that used up its attempts (e.g. one installed by wire on a sensor never
    // paired, so never confirmed) beats none at all.
    if (slot < 0) {
        slot = newer(f, slot_intact(f, 0, board_id), slot_intact(f, 1, board_id));
    }
    return slot;
}

void boot_take_attempt(const boot_flash_t *f, int slot)
{
    const slot_header_t *h = header(f, slot);
    if (h->confirmed == 0) {
        return;
    }
    for (int i = 0; i < SLOT_ATTEMPTS; i++) {
        if (h->attempts[i] == 0) {
            continue;
        }
        f->program_word(f->ctx, boot_slot_base(slot) + (uint32_t)offsetof(slot_header_t, attempts) + 4u * (uint32_t)i,
                        0);
        return;  // if it fails (a weak battery), run all the same
    }
}

bool boot_write(const boot_flash_t *f, int slot, uint32_t off, const uint8_t *data, size_t len)
{
    uint32_t base = boot_slot_base(slot);
    if (slot < 0 || slot > 1 || off > SLOT_SIZE || len > SLOT_SIZE - off) {
        return false;
    }
    for (uint32_t p = (off + BOOT_PAGE - 1) & ~(BOOT_PAGE - 1); p < off + len; p += BOOT_PAGE) {
        if (!f->erase_page(f->ctx, base + p)) {
            return false;
        }
    }
    return len == 0 || f->program(f->ctx, base + off, data, len);
}

bool boot_written_ok(const boot_flash_t *f, int slot, uint32_t board_id, uint32_t version)
{
    return slot_intact(f, slot, board_id) && header(f, slot)->version == version && attempts_left(header(f, slot));
}

uint32_t boot_slot_version(const boot_flash_t *f, int slot)
{
    return header(f, slot)->version;
}

int boot_slot_of(uint32_t addr)
{
    if (addr >= SLOT_A_BASE && addr < SLOT_A_BASE + SLOT_SIZE) {
        return 0;
    }
    if (addr >= SLOT_B_BASE && addr < SLOT_B_BASE + SLOT_SIZE) {
        return 1;
    }
    return -1;
}

bool boot_is_confirmed(const boot_flash_t *f, int slot)
{
    return header(f, slot)->confirmed == 0;
}

bool boot_confirm(const boot_flash_t *f, int slot)
{
    if (header(f, slot)->confirmed == 0) {
        return true;
    }
    return f->program_word(f->ctx, boot_slot_base(slot) + (uint32_t)offsetof(slot_header_t, confirmed), 0);
}
