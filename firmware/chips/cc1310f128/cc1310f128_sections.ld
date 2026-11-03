/* CC1310F128: the sections, for every layout (cc1310f128.ld, cc1310f128_slot_{a,b}.ld, the boot selector's). */

SECTIONS
{
    /* a slot image (OTA, docs/design/ota.md): its 256-byte header first, the vector table at IMAGE_OFFSET */
    .ows_header ORIGIN(FLASH) : { KEEP(*(.ows_header)) } > FLASH
    .resetVecs ORIGIN(FLASH) + IMAGE_OFFSET : { KEEP(*(.resetVecs)) } > FLASH
    /* the build info block (common/build_info.h): right after the vectors, in the image's first 1 KB */
    .ows_info : ALIGN(4) { KEEP(*(.ows_info)) } > FLASH

    .text : ALIGN(4)
    {
        *(.text*)
        *(.rodata*)
        KEEP(*(.init))
        KEEP(*(.fini))
        . = ALIGN(4);
        __init_array_start = .;
        KEEP(*(SORT(.init_array.*)))
        KEEP(*(.init_array))
        __init_array_end = .;
    } > FLASH

    .ARM.exidx : { *(.ARM.exidx* .gnu.linkonce.armexidx.*) } > FLASH

    /* RAM interrupt vector tables (VTOR needs 256-byte alignment): driverlib's IntRegister() uses
       vtable_ram, the NoRTOS startup fills .ramVecs */
    vtable_ram (NOLOAD) : ALIGN(256) { KEEP(*(vtable_ram)) } > SRAM
    .ramVecs (NOLOAD) : ALIGN(256) { KEEP(*(.ramVecs)) } > SRAM

    .data : ALIGN(4)
    {
        __data_start__ = .;
        *(.data*)
        . = ALIGN(4);
        __data_end__ = .;
    } > SRAM AT > FLASH
    __data_load__ = LOADADDR(.data);

    .bss (NOLOAD) : ALIGN(4)
    {
        __bss_start__ = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        __bss_end__ = .;
    } > SRAM

    .heap (NOLOAD) : ALIGN(8)
    {
        end = .;
        _end = .;
        . = ORIGIN(SRAM) + LENGTH(SRAM) - STACK_SIZE;
    } > SRAM

    .stack (NOLOAD) : ALIGN(8)
    {
        _stack = .;
        . += STACK_SIZE;
        _stack_end = .;
    } > SRAM

    .ccfg : { KEEP(*(.ccfg)) } > FLASH_CCFG
}
