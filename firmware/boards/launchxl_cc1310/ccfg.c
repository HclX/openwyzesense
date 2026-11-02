// Customer configuration (CCFG, flash 0x1FFA8). Like the stock dongle and sensor (docs/FACTS.md §5), it keeps
// the CPU debug port enabled and the ROM bootloader backdoor on, so a device can always be reflashed. On the
// LaunchPad the backdoor is BTN-1 (DIO13) held low at reset; the stock devices use DIO1.
#define CCFG_FORCE_VDDR_HH                   1  // for +14 dBm (the stock txPower 0xA73F)
#define SET_CCFG_BL_CONFIG_BOOTLOADER_ENABLE 0xC5
#define SET_CCFG_BL_CONFIG_BL_LEVEL          0x0
#ifndef OWS_BL_PIN
#define OWS_BL_PIN 13  // -DOWS_BL_PIN=2: the UART RX line instead, as on the dongle
#endif                 // (test_hw_bootloader.py: a UART break enters the bootloader)
#define SET_CCFG_BL_CONFIG_BL_PIN_NUMBER OWS_BL_PIN
#define SET_CCFG_BL_CONFIG_BL_ENABLE     0xC5
// the defaults keep the rest: CPU DAP enabled, LF clock from the 32 kHz crystal (LaunchPad), no write protection

#include <ti/devices/cc13x0/startup_files/ccfg.c>
