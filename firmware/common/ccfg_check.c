#include "ccfg_check.h"

#include <string.h>

// offsets from the CCFG's start (hw_ccfg.h CCFG_O_* - 0xFA8)
#define BL_CONFIG        0x30
#define ERASE_CONF       0x34
#define CCFG_TAP_DAP_0   0x3C
#define IMAGE_VALID_CONF 0x44
#define CCFG_PROT        0x48  // 4 words: one bit per sector, 0 = write-protected for good

static uint32_t word(const uint8_t *c, unsigned off)
{
    return (uint32_t)c[off] | (uint32_t)c[off + 1] << 8 | (uint32_t)c[off + 2] << 16 | (uint32_t)c[off + 3] << 24;
}

bool ccfg_is_safe(const uint8_t ccfg[CCFG_SIZE], const char **why)
{
    const char *bad = 0;
    uint32_t bl = word(ccfg, BL_CONFIG), erase = word(ccfg, ERASE_CONF), tap = word(ccfg, CCFG_TAP_DAP_0);
    if (bl >> 24 != 0xC5) {
        bad = "ROM bootloader disabled";
    } else if ((bl & 0xFF) != 0xC5) {
        bad = "bootloader backdoor disabled";
    } else if (((bl >> 8) & 0xFF) > 31) {
        bad = "bootloader backdoor pin out of range";
    } else if (((tap >> 16) & 0xFF) != 0xC5) {
        bad = "CPU debug port disabled";
    } else if (!(erase & 0x100) || !(erase & 0x1)) {
        bad = "chip or bank erase disabled";
    } else if (word(ccfg, IMAGE_VALID_CONF) != 0) {
        bad = "flash image not valid";
    } else {
        for (unsigned i = 0; i < 4 && !bad; i++) {
            if (word(ccfg, CCFG_PROT + 4 * i) != 0xFFFFFFFFu) {
                bad = "a flash sector write-protected";
            }
        }
    }
    if (why) {
        *why = bad;
    }
    return !bad;
}

void ccfg_merge(const uint8_t builtin[CCFG_SIZE], const uint8_t current[CCFG_SIZE], uint8_t out[CCFG_SIZE])
{
    memcpy(out, builtin, CCFG_SIZE);
    memcpy(out + CCFG_IDENTITY_OFF, current + CCFG_IDENTITY_OFF, CCFG_IDENTITY_SIZE);
}
