/* CC1310F128, the boot selector (firmware/boot/selector.c): flash page 0, no CCFG; RAM only for its stack. */

STACK_SIZE = 0x400;
IMAGE_OFFSET = 0;

MEMORY
{
    FLASH (RX)      : ORIGIN = 0x00000000, LENGTH = 0x00001000
    FLASH_CCFG (R)  : ORIGIN = 0x0001FFA8, LENGTH = 0x00000058
    SRAM (RWX)      : ORIGIN = 0x20000000, LENGTH = 0x00005000
}

ENTRY(selector_reset)
INCLUDE cc1310f128_sections.ld
