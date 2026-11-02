// hal/boot.h on the CC13x0: the slot is the one this build is linked for (its slot header's address). Flash writes go only to the header words
// of the two slots (confirmed, attempts; each programmed from all-ones to 0 once) and, for an OTA update, to the
// slot not running. boot_board(): soc/cc13x0/board.c (it needs the board's name).
#include "boot.h"
#include "chip.h"

#include <stdint.h>

#include <ti/devices/DeviceFamily.h>
// clang-format off: a TI include path, not a division
#include DeviceFamily_constructPath(driverlib/flash.h)
#include DeviceFamily_constructPath(driverlib/vims.h)
#include DeviceFamily_constructPath(inc/hw_memmap.h)
// clang-format on
#include <ti/drivers/dpl/HwiP.h>

#include "boot_slot.h"

// Flash erase / program with the cache off and interrupts held, as driverlib requires; data must be in RAM
static bool flash_op(uint32_t addr, const uint8_t *data, size_t len)
{
    chip_flash_begin();  // the battery under the operation's load (#75)
    uint32_t key = HwiP_disable();
    VIMSModeSet(VIMS_BASE, VIMS_MODE_DISABLED);
    while (VIMSModeGet(VIMS_BASE) != VIMS_MODE_DISABLED) {
    }
    uint32_t status = data ? FlashProgram((uint8_t *)data, addr, len) : FlashSectorErase(addr);
    VIMSModeSet(VIMS_BASE, VIMS_MODE_ENABLED);
    chip_flash_end();
    HwiP_restore(key);
    return status == FAPI_STATUS_SUCCESS;
}

static bool program_word(void *ctx, uint32_t addr, uint32_t value)
{
    (void)ctx;
    int slot = boot_slot_of(addr);
    // only a slot's own header words after the CRC (confirmed, attempts): nothing else, ever
    if (slot < 0 || addr < boot_slot_base(slot) + 64 || addr >= boot_slot_base(slot) + 80 || addr % 4) {
        return false;
    }
    uint32_t word = value;
    return flash_op(addr, (const uint8_t *)&word, 4);
}

// An update goes only to the other slot: never the running one, never pages 0, 29-31
static bool in_other_slot(uint32_t addr, size_t len)
{
    int other = boot_running_slot() == 0 ? 1 : boot_running_slot() == 1 ? 0 : -1;
    uint32_t base = other < 0 ? 0 : boot_slot_base(other);
    return other >= 0 && addr >= base && addr <= base + SLOT_SIZE && len <= base + SLOT_SIZE - addr;
}

static bool erase_page(void *ctx, uint32_t addr)
{
    (void)ctx;
    return in_other_slot(addr, BOOT_PAGE) && addr % BOOT_PAGE == 0 && flash_op(addr, 0, 0);
}

static bool program(void *ctx, uint32_t addr, const uint8_t *data, size_t len)
{
    (void)ctx;
    return in_other_slot(addr, len) && flash_op(addr, data, len);
}

static boot_flash_t flash(void)
{
    uintptr_t zero;
    __asm__ volatile("" : "=r"(zero) : "0"(0u));  // flash starts at address 0: not a C null pointer
    return (boot_flash_t){(const uint8_t *)zero, program_word, 0, erase_page, program};
}

// The slot header of a slot build (soc/cc13x0/slot_header.c, at its slot's base); NULL in an image linked at 0,
// whose code also lies in slot A's range: that is no slot image, and must never write "its header" (its own code)
extern const uint8_t ows_slot_header[] __attribute__((weak));

int boot_running_slot(void)
{
    uintptr_t h = (uintptr_t)ows_slot_header;
    int slot = h ? boot_slot_of((uint32_t)h) : -1;
    return slot >= 0 && h == boot_slot_base(slot) ? slot : -1;
}

void boot_running_take_attempt(void)
{
    int slot = boot_running_slot();
    boot_flash_t f = flash();
    if (slot >= 0) {
        boot_take_attempt(&f, slot);
    }
}

bool boot_running_confirmed(void)
{
    boot_flash_t f = flash();
    return boot_running_slot() < 0 || boot_is_confirmed(&f, boot_running_slot());
}

bool boot_running_confirm(void)
{
    int slot = boot_running_slot();
    boot_flash_t f = flash();
    return slot < 0 || boot_confirm(&f, slot);
}

uint32_t boot_running_version(void)
{
    boot_flash_t f = flash();
    return boot_running_slot() < 0 ? 0 : boot_slot_version(&f, boot_running_slot());
}

bool boot_update_write(uint32_t off, const uint8_t *data, size_t len)
{
    boot_flash_t f = flash();
    return boot_running_slot() >= 0 && boot_write(&f, 1 - boot_running_slot(), off, data, len);
}

bool boot_update_ok(uint32_t version)
{
    boot_flash_t f = flash();
    return boot_running_slot() >= 0 && boot_written_ok(&f, 1 - boot_running_slot(), boot_board(), version);
}
