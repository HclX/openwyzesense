// The 256-byte header at the start of an OTA slot image (common/boot_slot.h), as linked: erased flash (0xFF);
// tools/slot_image.py fills in magic, board ID, version, size and CRC after the link. The words for "confirmed"
// and the attempts stay 0xFF: the app programs them later.
#include <stdint.h>

__attribute__((section(".ows_header"), used)) const uint8_t ows_slot_header[256] = {[0 ... 255] = 0xFF};
