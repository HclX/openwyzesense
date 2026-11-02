#include "ccfg.h"

#include <string.h>

#include <ti/devices/DeviceFamily.h>
// clang-format off: a TI include path, not a division
#include DeviceFamily_constructPath(driverlib/flash.h)
#include DeviceFamily_constructPath(driverlib/vims.h)
#include DeviceFamily_constructPath(inc/hw_memmap.h)
// clang-format on
#include <ti/drivers/dpl/HwiP.h>

#define CCFG_PAGE 0x1F000  // flash page 31
#define CCFG_ADDR 0x1FFA8
#define PAGE_SIZE 0x1000

extern const uint8_t ows_ccfg_builtin[];  // ccfg_builtin.c

const uint8_t *ccfg_builtin(void)
{
    return ows_ccfg_builtin;
}

const uint8_t *ccfg_current(void)
{
    return (const uint8_t *)CCFG_ADDR;
}

__attribute__((noinline)) int ccfg_restore(void)
{
    uint8_t want[CCFG_SIZE];
    ccfg_merge(ccfg_builtin(), ccfg_current(), want);
    if (!ccfg_is_safe(want, NULL)) {
        return CCFG_NOT_SAFE;
    }
    if (!memcmp(ccfg_current(), want, CCFG_SIZE)) {
        return CCFG_ALREADY_RIGHT;
    }
    uint8_t page[PAGE_SIZE];  // on the 6 KB main stack only during restore (saves 4 KB permanent .bss)
    memcpy(page, (const void *)CCFG_PAGE, PAGE_SIZE);
    memcpy(page + (CCFG_ADDR - CCFG_PAGE), want, CCFG_SIZE);
    uint32_t key = HwiP_disable();
    VIMSModeSet(VIMS_BASE, VIMS_MODE_DISABLED);
    while (VIMSModeGet(VIMS_BASE) != VIMS_MODE_DISABLED) {
    }
    uint32_t erased = FlashSectorErase(CCFG_PAGE);
    uint32_t written = erased == FAPI_STATUS_SUCCESS ? FlashProgram(page, CCFG_PAGE, PAGE_SIZE) : erased;
    VIMSModeSet(VIMS_BASE, VIMS_MODE_ENABLED);
    HwiP_restore(key);
    if (written != FAPI_STATUS_SUCCESS || memcmp((const void *)CCFG_PAGE, page, PAGE_SIZE)) {
        return CCFG_FAILED;
    }
    return CCFG_RESTORED;
}
