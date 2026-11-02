/* CC1310F128: 128 KB flash, 20 KB SRAM. For the NoRTOS startup in
   third_party/ti_coresdk/kernel/nortos/startup/startup_cc13xx_cc26xx_gcc.c.
   Code ends at 0x1E000: page 30 is the NV page (the sensor table, as on the stock dongle), page 31 ends with
   the CCFG (its first 4008 bytes are left unused, so the CCFG page never holds code). */

STACK_SIZE = 0x1800;
IMAGE_OFFSET = 0;

MEMORY
{
    FLASH (RX)      : ORIGIN = 0x00000000, LENGTH = 0x0001E000
    FLASH_CCFG (R)  : ORIGIN = 0x0001FFA8, LENGTH = 0x00000058
    SRAM (RWX)      : ORIGIN = 0x20000000, LENGTH = 0x00005000
}

ENTRY(resetISR)
INCLUDE cc1310f128_sections.ld
