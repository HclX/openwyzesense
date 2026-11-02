// AES-128-ECB on the CC13x0 crypto engine, for wyze_app.c.
#include <string.h>

#include "wyze_app.h"

#include <ti/devices/DeviceFamily.h>
// clang-format off: a TI include path, not a division
#include DeviceFamily_constructPath(driverlib/crypto.h)
// clang-format on
#include <ti/drivers/Power.h>
#include <ti/drivers/power/PowerCC26XX.h>

static void ecb(const uint8_t key[16], const uint8_t *in, uint8_t *out, size_t n, bool encrypt)
{
    uint32_t k[4], block[4];
    memcpy(k, key, 16);
    Power_setDependency(PowerCC26XX_PERIPH_CRYPTO);
    CRYPTOAesLoadKey(k, CRYPTO_KEY_AREA_0);
    for (size_t b = 0; b < n; b += 16) {
        memcpy(block, in + b, 16);
        CRYPTOAesEcb(block, block, CRYPTO_KEY_AREA_0, encrypt, false);
        while (CRYPTOAesEcbStatus() == AES_DMA_BSY) {
        }
        CRYPTOAesEcbFinish();
        memcpy(out + b, block, 16);
    }
    Power_releaseDependency(PowerCC26XX_PERIPH_CRYPTO);
}

void aes128_ecb_encrypt(const uint8_t key[16], const uint8_t *in, uint8_t *out, size_t n)
{
    ecb(key, in, out, n, true);
}

void aes128_ecb_decrypt(const uint8_t key[16], const uint8_t *in, uint8_t *out, size_t n)
{
    ecb(key, in, out, n, false);
}
