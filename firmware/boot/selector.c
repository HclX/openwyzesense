// The boot selector: flash page 0, where the ROM starts the chip. Picks the slot to run (common/boot_slot.c:
// the newest usable image) and jumps to it. Reads only, touches no hardware, uses only its stack; never updated
// over the air (docs/design/ota.md). No slot to run: waits, for JTAG or the ROM bootloader's backdoor.
#include <stdint.h>

#include "board_config.h"
#include "boot_slot.h"

extern uint32_t _stack_end;  // the linker script's
void selector_reset(void);

static void halt(void)
{
    for (;;) {
        __asm__ volatile("wfi");
    }
}

__attribute__((section(".resetVecs"), used)) void (*const selector_vectors[16])(void) = {
    (void (*)(void))(uintptr_t)&_stack_end,
    selector_reset,
    halt,
    halt,
    halt,
    halt,
    halt,
    0,
    0,
    0,
    0,
    halt,
    halt,
    0,
    halt,
    halt,
};

void selector_reset(void)
{
    uintptr_t zero;
    __asm__ volatile("" : "=r"(zero) : "0"(0u));  // flash starts at address 0: not a C null pointer
    boot_flash_t f = {.flash = (const uint8_t *)zero};
    int slot = boot_choose(&f, boot_board_id(BOARD_NAME));
    if (slot < 0) {
        halt();
    }
    const uint32_t *vt = (const uint32_t *)(uintptr_t)(boot_slot_base(slot) + SLOT_IMAGE_OFFSET);
    *(volatile uint32_t *)0xE000ED08u = (uint32_t)(uintptr_t)vt;  // SCB->VTOR
    __asm__ volatile("msr msp, %0\n\tbx %1" : : "r"(vt[0]), "r"(vt[1]));
    halt();
}
