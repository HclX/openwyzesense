/* CC1310F128, an image for OTA slot A (docs/ota.md, common/boot_slot.h): flash 0x01000 + 56 KB, its header
   first, the vector table 0x100 in; no CCFG (the chip's stays). The boot selector in page 0 starts it. */

STACK_SIZE = 0x1800;
IMAGE_OFFSET = 0x100;

MEMORY
{
    FLASH (RX)      : ORIGIN = 0x00001000, LENGTH = 0x0000E000
    FLASH_CCFG (R)  : ORIGIN = 0x0001FFA8, LENGTH = 0x00000058
    SRAM (RWX)      : ORIGIN = 0x20000000, LENGTH = 0x00005000
}

ENTRY(resetISR)
INCLUDE cc1310f128_sections.ld
