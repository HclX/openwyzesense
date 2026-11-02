#include "nv_page.h"

#include "chip.h"

#include <string.h>

#include <ti/devices/DeviceFamily.h>
// clang-format off: a TI include path, not a division
#include DeviceFamily_constructPath(driverlib/flash.h)
#include DeviceFamily_constructPath(driverlib/vims.h)
#include DeviceFamily_constructPath(inc/hw_memmap.h)
// clang-format on
#include <ti/drivers/dpl/HwiP.h>

#define NV_FLASH_ADDR 0x1E000  // flash page 30

// Flash operations: the ROM routines, with the flash cache off and interrupts held off (code runs from flash)
static uint32_t begin(void)
{
    chip_flash_begin();  // the battery under the operation's load (#75)
    uint32_t key = HwiP_disable();
    VIMSModeSet(VIMS_BASE, VIMS_MODE_DISABLED);
    while (VIMSModeGet(VIMS_BASE) != VIMS_MODE_DISABLED) {
    }
    return key;
}

static void end(uint32_t key)
{
    VIMSModeSet(VIMS_BASE, VIMS_MODE_ENABLED);
    chip_flash_end();
    HwiP_restore(key);
}

static bool program(void *ctx, uint32_t offset, const uint8_t *data, size_t len)
{
    uint8_t buf[64];  // the source must not be in flash while flash is programmed
    (void)ctx;
    if (offset > NV_PAGE_SIZE || len > NV_PAGE_SIZE - offset) {
        return false;  // page 30 only, never the CCFG
    }
    while (len) {
        size_t n = len < sizeof buf ? len : sizeof buf;
        memcpy(buf, data, n);
        uint32_t key = begin();
        uint32_t status = FlashProgram(buf, NV_FLASH_ADDR + offset, n);
        end(key);
        if (status != FAPI_STATUS_SUCCESS) {
            return false;
        }
        data += n, offset += n, len -= n;
    }
    return true;
}

static bool erase(void *ctx)
{
    (void)ctx;
    uint32_t key = begin();
    uint32_t status = FlashSectorErase(NV_FLASH_ADDR);
    end(key);
    return status == FAPI_STATUS_SUCCESS;
}

bool nv_page_open(nvstore_t *nv)
{
    nv->page = (const uint8_t *)NV_FLASH_ADDR;
    nv->program = program;
    nv->erase = erase;
    nv->ctx = NULL;
    return nv_open(nv);
}
